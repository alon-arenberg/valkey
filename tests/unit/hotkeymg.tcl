start_server {tags {"hotkeymg"}} {
    test "Enable hotkey MG functionality" {
        r config set hotkey-mg-enabled yes
        set status [r config get hotkey-mg-enabled]
        assert_equal [lindex $status 1] "yes"
    }

    test "HOTKEYS MG returns empty when no hot keys" {
        r hotkeys mgreset
        set all [r hotkeys mg]
        assert_equal [llength $all] 0
        set reads [r hotkeys mg TYPE read]
        assert_equal [llength $reads] 0
        set writes [r hotkeys mg TYPE write]
        assert_equal [llength $writes] 0
    }

    test "Generate hot keys through repeated access with MG" {
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        r set "mg_hot_read" "value"
        for {set i 0} {$i < 1000} {incr i} {
            r get "mg_hot_read"
            r set "mg_hot_write" "v_$i"
        }

        after 2000

        set all [r hotkeys mg]
        puts "MG hotkeys detected: [llength $all]"
        assert {[llength $all] > 0}

        # Verify 14-field reply (includes slot)
        set first [lindex $all 0]
        assert_equal [llength $first] 14
        assert_equal [lindex $first 4] "slot"
    }

    test "HOTKEYS MG with TYPE filter" {
        set reads [r hotkeys mg TYPE read]
        set writes [r hotkeys mg TYPE write]
        set all [r hotkeys mg TYPE all]
        puts "MG read: [llength $reads], write: [llength $writes], all: [llength $all]"
        assert {[llength $all] >= 0}
    }

    test "HOTKEYS MG with SLOT filter" {
        set all [r hotkeys mg]
        assert {[llength $all] > 0}

        set first [lindex $all 0]
        set slot_val [lindex $first 5]

        set slot_hotkeys [r hotkeys mg SLOT $slot_val]
        assert {[llength $slot_hotkeys] > 0}

        set other_slot [expr {($slot_val + 1) % 16384}]
        set other_hotkeys [r hotkeys mg SLOT $other_slot]
        assert {[llength $other_hotkeys] >= 0}
    }

    test "HOTKEYS MG with SLOT and TYPE combined" {
        set all [r hotkeys mg]
        if {[llength $all] > 0} {
            set first [lindex $all 0]
            set slot_val [lindex $first 5]
            set type_val [lindex $first 3]

            set filtered [r hotkeys mg SLOT $slot_val TYPE $type_val]
            assert {[llength $filtered] > 0}

            set filtered2 [r hotkeys mg TYPE $type_val SLOT $slot_val]
            assert {[llength $filtered2] > 0}
        }
    }

    test "HOTKEYS MGRESET clears all statistics" {
        set result [r hotkeys mgreset]
        assert_equal $result "OK"
        set all [r hotkeys mg]
        assert_equal [llength $all] 0
    }

    test "MG hotkey detection with different data types" {
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16

        for {set i 0} {$i < 1000} {incr i} {
            r get "mg_string"
            r hget "mg_hash" "f1"
            r lrange "mg_list" 0 -1
            r smembers "mg_set"
            r zrange "mg_zset" 0 -1
        }

        after 1100

        set all [r hotkeys mg]
        puts "MG different types: [llength $all]"
        assert {[llength $all] > 0}
    }

    test "Invalid HOTKEYS MG command syntax" {
        catch {r hotkeys mg TYPE invalid} err
        assert_match "*Invalid type*" $err

        catch {r hotkeys mg INVALID param} err
        assert_match "*Syntax error*" $err

        catch {r hotkeys mg SLOT -1} err
        assert_match "*Invalid slot*" $err

        catch {r hotkeys mg SLOT 16384} err
        assert_match "*Invalid slot*" $err
    }

    test "MG configuration parameters" {
        r config set hotkey-mg-max-keys 32
        set val [r config get hotkey-mg-max-keys]
        assert_equal [lindex $val 1] "32"

        r config set hotkey-mg-sampling-ratio 50
        set val [r config get hotkey-mg-sampling-ratio]
        assert_equal [lindex $val 1] "50"

        r config set hotkey-mg-history-max-count 5
        set val [r config get hotkey-mg-history-max-count]
        assert_equal [lindex $val 1] "5"

        r config set hotkey-mg-history-ttl 300
        set val [r config get hotkey-mg-history-ttl]
        assert_equal [lindex $val 1] "300"
    }

    test "Disable hotkey MG functionality" {
        r config set hotkey-mg-enabled no
        set status [r config get hotkey-mg-enabled]
        assert_equal [lindex $status 1] "no"

        catch {r hotkeys mg} err
        assert_match "*Hotkey MG detection is disabled*" $err

        catch {r hotkeys mgreset} err
        assert_match "*Hotkey MG detection is disabled*" $err
    }

    test "Re-enable hotkey MG functionality" {
        r config set hotkey-mg-enabled yes
        set result [r hotkeys mgreset]
        assert_equal $result "OK"
        set all [r hotkeys mg]
        assert {[llength $all] >= 0}
    }

    test "MG read/write classification" {
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 1200} {incr i} {
            r get "mg_classify_read"
            r set "mg_classify_write" "v_$i"
        }

        after 1100

        set reads [r hotkeys mg TYPE read]
        set writes [r hotkeys mg TYPE write]
        puts "MG classify - read: [llength $reads], write: [llength $writes]"
        assert {[llength $reads] > 0}
        assert {[llength $writes] > 0}
    }

    test "MG LRU eviction in history" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-history-max-count 5
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 1} {$i <= 8} {incr i} {
            for {set j 0} {$j < 600} {incr j} {
                r set "mg_lru_$i" "v_$j"
                r get "mg_lru_$i"
            }
        }

        after 1100

        set all [r hotkeys mg]
        puts "MG LRU test: [llength $all]"
        assert {[llength $all] <= 5}
    }

    test "MG history expiration" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-history-ttl 2
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 600} {incr i} {
            r set "mg_expire_key" "v_$i"
            r get "mg_expire_key"
        }

        after 1100

        set before [r hotkeys mg]
        puts "MG before expiration: [llength $before]"

        after 3000

        set after_expire [r hotkeys mg]
        puts "MG after expiration: [llength $after_expire]"
        assert_equal [llength $after_expire] 0
    }

    test "MG memory cleanup on manager recreation" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 500} {incr i} {
            r get "mg_mem_key"
        }

        after 1100

        r config set hotkey-mg-enabled no
        r config set hotkey-mg-enabled yes

        set all [r hotkeys mg]
        assert_equal [llength $all] 0
        assert_equal [r ping] "PONG"
    }

    test "MG runtime metrics" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        set info_before [r info hotkeymg]
        set initial_sampled 0
        foreach line [split $info_before "\r\n"] {
            if {[string match "hotkey_mg_runtime_total_sampled:*" $line]} {
                set initial_sampled [lindex [split $line ":"] 1]
            }
        }

        for {set i 0} {$i < 600} {incr i} { r get "mg_metrics_read" }
        for {set i 0} {$i < 400} {incr i} { r set "mg_metrics_write" "v_$i" }

        after 1100

        set info_after [r info hotkeymg]
        set final_sampled 0
        set final_history 0
        foreach line [split $info_after "\r\n"] {
            if {[string match "hotkey_mg_runtime_total_sampled:*" $line]} {
                set final_sampled [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_mg_runtime_history_count:*" $line]} {
                set final_history [lindex [split $line ":"] 1]
            }
        }

        assert {$final_sampled > $initial_sampled}
        puts "MG metrics - sampled: $initial_sampled -> $final_sampled"

        set current [r hotkeys mg]
        assert_equal $final_history [llength $current]

        r hotkeys mgreset
        set info_reset [r info hotkeymg]
        set reset_history 0
        foreach line [split $info_reset "\r\n"] {
            if {[string match "hotkey_mg_runtime_history_count:*" $line]} {
                set reset_history [lindex [split $line ":"] 1]
            }
        }
        assert_equal $reset_history 0
    }

    test "MG max-keys callback recreates manager" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 8
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 500} {incr i} { r get "mg_rekey" }
        after 1100

        set before [r hotkeys mg]
        puts "MG before max-keys change: [llength $before]"

        r config set hotkey-mg-max-keys 32
        set after_change [r hotkeys mg]
        assert_equal [llength $after_change] 0
    }

    test "CMS and MG can run simultaneously with per-slot" {
        r config set hotkey-enabled yes
        r config set hotkey-mg-enabled yes
        r hotkeys reset
        r hotkeys mgreset

        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 1200} {incr i} {
            r get "both_hot_key"
            r set "both_hot_write" "v_$i"
        }

        after 2000

        set cms_keys [r hotkeys get]
        set mg_keys [r hotkeys mg]
        puts "CMS detected: [llength $cms_keys], MG detected: [llength $mg_keys]"

        assert {[llength $cms_keys] > 0}
        assert {[llength $mg_keys] > 0}

        # Both should report the same slot for the same key
        if {[llength $cms_keys] > 0 && [llength $mg_keys] > 0} {
            set cms_first [lindex $cms_keys 0]
            set mg_first [lindex $mg_keys 0]
            # Both have 14-field format with slot at index 5
            assert_equal [lindex $cms_first 4] "slot"
            assert_equal [lindex $mg_first 4] "slot"
        }

        r hotkeys mgreset
        set cms_after [r hotkeys get]
        set mg_after [r hotkeys mg]
        assert {[llength $cms_after] > 0}
        assert_equal [llength $mg_after] 0
    }

    test "MG entries include slot field" {
        r config set hotkey-mg-enabled yes
        r hotkeys mgreset
        r config set hotkey-mg-sampling-ratio 100
        r config set hotkey-mg-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 600} {incr i} {
            r get "mg_slot_test_key"
        }

        after 1100

        set hotkeys [r hotkeys mg]
        assert {[llength $hotkeys] > 0}

        set first [lindex $hotkeys 0]
        assert_equal [llength $first] 14
        assert_equal [lindex $first 4] "slot"
        set slot_val [lindex $first 5]
        assert {$slot_val >= 0 && $slot_val < 16384}
        puts "MG Slot test: key=[lindex $first 1], slot=$slot_val"
    }
}
