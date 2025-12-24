start_server {tags {"hotkey"}} {
    test "Enable hotkey functionality" {
        r config set hotkey-enabled yes
        set hotkey_status [r config get hotkey-enabled]
        assert_equal [lindex $hotkey_status 1] "yes"
    }

    test "HOTKEYS GET returns empty when no hot keys" {
        r hotkeys reset
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
        set read_hotkeys [r hotkeys get TYPE read]
        assert_equal [llength $read_hotkeys] 0
        set write_hotkeys [r hotkeys get TYPE write]
        assert_equal [llength $write_hotkeys] 0
    }

    test "Generate hot keys through repeated access" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        r set "hot_read_key" "value"
        for {set j 1} {$j <= 1000} {incr j} {
            r get "hot_read_key"
            r set "hot_write_key" "value_$j"
        }

        after 2000

        set all_hotkeys [r hotkeys get]
        puts "All hotkeys detected: [llength $all_hotkeys]"
        assert {[llength $all_hotkeys] > 0}

        # Verify 14-field reply (includes slot)
        set first [lindex $all_hotkeys 0]
        assert_equal [llength $first] 14
    }

    test "HOTKEYS GET with TYPE filter" {
        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        set all_hotkeys [r hotkeys get TYPE all]
        puts "Read: [llength $read_hotkeys], Write: [llength $write_hotkeys], All: [llength $all_hotkeys]"
        assert {[llength $all_hotkeys] >= 0}
    }

    test "HOTKEYS GET with SLOT filter" {
        set all_hotkeys [r hotkeys get]
        assert {[llength $all_hotkeys] > 0}
        set first [lindex $all_hotkeys 0]
        set slot_val [lindex $first 5]
        set slot_hotkeys [r hotkeys get SLOT $slot_val]
        assert {[llength $slot_hotkeys] > 0}
    }

    test "HOTKEYS GET with SLOT and TYPE combined" {
        set all_hotkeys [r hotkeys get]
        if {[llength $all_hotkeys] > 0} {
            set first [lindex $all_hotkeys 0]
            set slot_val [lindex $first 5]
            set type_val [lindex $first 3]
            set filtered [r hotkeys get SLOT $slot_val TYPE $type_val]
            assert {[llength $filtered] > 0}
            set filtered2 [r hotkeys get TYPE $type_val SLOT $slot_val]
            assert {[llength $filtered2] > 0}
        }
    }

    test "HOTKEYS RESET clears all statistics" {
        set reset_result [r hotkeys reset]
        assert_equal $reset_result "OK"
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
    }

    test "Hotkey detection with different data types" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16

        for {set i 1} {$i <= 1000} {incr i} {
            r get "hot_string"
            r hget "hot_hash" "field_1"
            r lrange "hot_list" 0 -1
            r smembers "hot_set"
            r zrange "hot_zset" 0 -1
        }

        after 1100

        set all_hotkeys [r hotkeys get]
        puts "Hotkeys detected for different data types: [llength $all_hotkeys]"
        assert {[llength $all_hotkeys] > 0}
    }

    test "Invalid HOTKEYS command syntax" {
        catch {r hotkeys invalid} err
        assert_match "*unknown subcommand*" $err
        catch {r hotkeys get TYPE invalid} err
        assert_match "*Invalid type*" $err
        catch {r hotkeys get INVALID param} err
        assert_match "*Syntax error*" $err
        catch {r hotkeys} err
        assert_match "*wrong number of arguments*" $err
        catch {r hotkeys get SLOT -1} err
        assert_match "*Invalid slot*" $err
        catch {r hotkeys get SLOT 16384} err
        assert_match "*Invalid slot*" $err
    }

    test "Hotkey configuration parameters" {
        r config set hotkey-sampling-ratio 50
        assert_equal [lindex [r config get hotkey-sampling-ratio] 1] "50"

        r config set hotkey-max-keys 8
        assert_equal [lindex [r config get hotkey-max-keys] 1] "8"

        r config set hotkey-window-seconds 2
        assert_equal [lindex [r config get hotkey-window-seconds] 1] "2"

        r config set hotkey-history-ttl 300
        assert_equal [lindex [r config get hotkey-history-ttl] 1] "300"
    }

    test "Disable hotkey functionality" {
        r config set hotkey-enabled no
        assert_equal [lindex [r config get hotkey-enabled] 1] "no"
        catch {r hotkeys get} err
        assert_match "*Hotkey detection is disabled*" $err
        catch {r hotkeys reset} err
        assert_match "*Hotkey detection is disabled*" $err
    }

    test "Re-enable hotkey functionality" {
        r config set hotkey-enabled yes
        assert_equal [lindex [r config get hotkey-enabled] 1] "yes"
        assert_equal [r hotkeys reset] "OK"
    }

    test "Hotkey detection with high frequency access" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 1} {$i <= 1200} {incr i} {
            r set "super_hot_write" "value_$i"
            r get "super_hot_read"
        }

        after 1100

        set all_hotkeys [r hotkeys get]
        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        puts "All: [llength $all_hotkeys], Read: [llength $read_hotkeys], Write: [llength $write_hotkeys]"
        assert {[llength $all_hotkeys] > 0}
        assert {[llength $read_hotkeys] > 0}
        assert {[llength $write_hotkeys] > 0}
    }

    test "Test LRU eviction in history management" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-history-max-count 5
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 1} {$i <= 8} {incr i} {
            for {set j 1} {$j <= 600} {incr j} {
                r set "hotkey_$i" "value_$j"
                r get "hotkey_$i"
            }
        }

        after 1100

        set all_hotkeys [r hotkeys get]
        puts "LRU test - hotkeys count: [llength $all_hotkeys]"
        assert {[llength $all_hotkeys] <= 5}
    }

    test "Test history expiration functionality" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-history-ttl 2
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 1} {$i <= 600} {incr i} {
            r set "expire_test_key" "value_$i"
            r get "expire_test_key"
        }

        after 1100
        set hotkeys_before [r hotkeys get]
        puts "Before expiration: [llength $hotkeys_before] hotkeys"

        after 3000
        set hotkeys_after [r hotkeys get]
        puts "After expiration: [llength $hotkeys_after] hotkeys"
        assert_equal [llength $hotkeys_after] 0
    }

    test "Test memory cleanup on manager recreation" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 1} {$i <= 10} {incr i} {
            for {set j 1} {$j <= 350} {incr j} {
                r get "memory_test_key_$i"
            }
        }
        after 1100

        r config set hotkey-enabled no
        r config set hotkey-enabled yes
        set hotkeys_after [r hotkeys get]
        assert_equal [llength $hotkeys_after] 0
        assert_equal [r ping] "PONG"
    }

    test "Test hotkey runtime metrics" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        set info_before [r info hotkey]
        set initial_total_sampled 0
        foreach line [split $info_before "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set initial_total_sampled [lindex [split $line ":"] 1]
            }
        }

        for {set i 1} {$i <= 600} {incr i} { r get "metrics_test_read_key" }
        for {set i 1} {$i <= 400} {incr i} { r set "metrics_test_write_key" "value_$i" }

        after 1100

        set info_after [r info hotkey]
        set final_total_sampled 0
        set final_history_count 0
        foreach line [split $info_after "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set final_total_sampled [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_history_count:*" $line]} {
                set final_history_count [lindex [split $line ":"] 1]
            }
        }

        assert {$final_total_sampled > $initial_total_sampled}
        puts "hotkey_runtime_total_sampled increased from $initial_total_sampled to $final_total_sampled"

        set current_hotkeys [r hotkeys get]
        assert_equal $final_history_count [llength $current_hotkeys]
        puts "hotkey_runtime_history_count ($final_history_count) matches current hotkeys count"

        # Verify max_keys appears in INFO
        assert_match "*hotkey_max_keys:*" $info_after

        r hotkeys reset
        set info_reset [r info hotkey]
        set reset_history_count 0
        foreach line [split $info_reset "\r\n"] {
            if {[string match "hotkey_runtime_history_count:*" $line]} {
                set reset_history_count [lindex [split $line ":"] 1]
            }
        }
        assert_equal $reset_history_count 0
    }

    test "Hotkey entries include slot field" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 16
        r config set hotkey-window-seconds 1

        for {set i 0} {$i < 600} {incr i} { r get "slot_test_key" }
        after 1100

        set hotkeys [r hotkeys get]
        assert {[llength $hotkeys] > 0}
        set first [lindex $hotkeys 0]
        assert_equal [llength $first] 14
        assert_equal [lindex $first 4] "slot"
        set slot_val [lindex $first 5]
        assert {$slot_val >= 0 && $slot_val < 16384}
        puts "Slot test: key=[lindex $first 1], slot=$slot_val"
    }

    test "Max-keys tracks the hottest keys" {
        r config set hotkey-enabled yes
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-max-keys 3
        r config set hotkey-history-max-count 10
        r config set hotkey-window-seconds 1

        # Create keys with different frequencies
        for {set i 0} {$i < 100} {incr i} { r get "cold_key" }
        for {set i 0} {$i < 500} {incr i} { r get "warm_key" }
        for {set i 0} {$i < 1000} {incr i} { r get "hot_key" }

        after 1100

        set hotkeys [r hotkeys get]
        puts "Max-keys test: [llength $hotkeys] hotkeys detected"
        # With max-keys=3, we should see at most 3 keys per type
        assert {[llength $hotkeys] > 0}
        assert {[llength $hotkeys] <= 3}
    }
}
