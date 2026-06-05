start_server {tags {"hotkey"}} {
    test "Enable hotkey functionality" {
        r config set hotkey-enabled yes
        r config set hotkey-half-life-seconds 60
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
        r config set hotkey-top-k 16

        r set "hot_read_key" "value"
        for {set j 1} {$j <= 1000} {incr j} {
            r get "hot_read_key"
            r set "hot_write_key" "value_$j"
        }

        set all_hotkeys [r hotkeys get]
        assert {[llength $all_hotkeys] > 0}

        # Verify 10-field reply (key, type, db, slot, qps)
        set first [lindex $all_hotkeys 0]
        assert_equal [llength $first] 10
        assert_equal [lindex $first 0] "key"
        assert_equal [lindex $first 2] "type"
        assert_equal [lindex $first 4] "db"
        assert_equal [lindex $first 6] "slot"
        assert_equal [lindex $first 8] "qps"
    }

    test "HOTKEYS GET with TYPE filter" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16

        r set "type_filter_key" "val"
        for {set i 0} {$i < 500} {incr i} {
            r get "type_filter_key"
            r set "type_filter_key" "val_$i"
        }

        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        set all_hotkeys [r hotkeys get TYPE all]

        assert {[llength $read_hotkeys] > 0}
        assert {[llength $write_hotkeys] > 0}
        assert_equal [llength $all_hotkeys] [expr {[llength $read_hotkeys] + [llength $write_hotkeys]}]

        foreach entry $read_hotkeys {
            assert_equal [lindex $entry 3] "read"
        }
        foreach entry $write_hotkeys {
            assert_equal [lindex $entry 3] "write"
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
        r config set hotkey-top-k 16

        r set "hot_string" "value"
        r hset "hot_hash" "field_1" "value"
        r rpush "hot_list" "item"
        r sadd "hot_set" "member"
        r zadd "hot_zset" 1.0 "member"

        for {set i 1} {$i <= 1000} {incr i} {
            r get "hot_string"
            r hget "hot_hash" "field_1"
            r lrange "hot_list" 0 -1
            r smembers "hot_set"
            r zrange "hot_zset" 0 -1
        }

        set all_hotkeys [r hotkeys get]
        assert {[llength $all_hotkeys] > 0}
    }

    test "Invalid HOTKEYS command syntax" {
        catch {r hotkeys invalid} err
        assert_match "*nknown*subcommand*" $err
        catch {r hotkeys get TYPE invalid} err
        assert_match "*Invalid type*" $err
        catch {r hotkeys get INVALID param} err
        assert_match "*Syntax error*" $err
        catch {r hotkeys} err
        assert_match "*wrong number of arguments*" $err
    }

    test "Hotkey configuration parameters" {
        r config set hotkey-sampling-ratio 50
        assert_equal [lindex [r config get hotkey-sampling-ratio] 1] "50"

        r config set hotkey-top-k 8
        assert_equal [lindex [r config get hotkey-top-k] 1] "8"

        r config set hotkey-half-life-seconds 2
        assert_equal [lindex [r config get hotkey-half-life-seconds] 1] "2"

        # Restore defaults for subsequent tests
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16
        r config set hotkey-half-life-seconds 60
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
        r config set hotkey-top-k 16

        for {set i 1} {$i <= 1200} {incr i} {
            r set "super_hot_write" "value_$i"
            r get "super_hot_read"
        }

        set all_hotkeys [r hotkeys get]
        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        assert {[llength $all_hotkeys] > 0}
        assert {[llength $read_hotkeys] > 0}
        assert {[llength $write_hotkeys] > 0}
    }

    test "HOTKEYS GET returns sorted by QPS descending" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16

        r set "low_freq" "val"
        r set "mid_freq" "val"
        r set "high_freq" "val"

        for {set i 0} {$i < 100} {incr i} { r get "low_freq" }
        for {set i 0} {$i < 500} {incr i} { r get "mid_freq" }
        for {set i 0} {$i < 1000} {incr i} { r get "high_freq" }

        set hotkeys [r hotkeys get TYPE read]
        assert {[llength $hotkeys] >= 2}

        # QPS is at field index 9
        set prev_qps [lindex [lindex $hotkeys 0] 9]
        for {set i 1} {$i < [llength $hotkeys]} {incr i} {
            set cur_qps [lindex [lindex $hotkeys $i] 9]
            assert {$prev_qps >= $cur_qps}
            set prev_qps $cur_qps
        }
    }

    test "HOTKEYS GET limits results to top-k" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 3

        foreach key {k1 k2 k3 k4 k5 k6 k7 k8} {
            r set $key "val"
            for {set i 0} {$i < 200} {incr i} { r get $key }
        }

        set hotkeys [r hotkeys get]
        assert {[llength $hotkeys] <= 3}

        r config set hotkey-top-k 16
    }

    test "Hotkey entries include db field" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16

        r select 0
        r set "db0_key" "val"
        for {set i 0} {$i < 500} {incr i} { r get "db0_key" }

        set hotkeys [r hotkeys get]
        assert {[llength $hotkeys] > 0}
        set first [lindex $hotkeys 0]
        assert_equal [lindex $first 4] "db"
        assert_equal [lindex $first 5] 0
    }

    test "FLUSHALL purges all hotkey state" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16

        r set "flush_key" "val"
        for {set i 0} {$i < 500} {incr i} { r get "flush_key" }

        set hotkeys_before [r hotkeys get]
        assert {[llength $hotkeys_before] > 0}

        r flushall

        set hotkeys_after [r hotkeys get]
        assert_equal [llength $hotkeys_after] 0
    }

    test "Test memory cleanup on manager recreation" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16

        for {set i 1} {$i <= 10} {incr i} {
            for {set j 1} {$j <= 350} {incr j} {
                r get "memory_test_key_$i"
            }
        }

        set hotkeys_before [r hotkeys get]
        assert {[llength $hotkeys_before] > 0}

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
        r config set hotkey-top-k 16

        set info_before [r info hotkey]
        set initial_total_sampled 0
        foreach line [split $info_before "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set initial_total_sampled [lindex [split $line ":"] 1]
            }
        }

        for {set i 1} {$i <= 600} {incr i} { r get "metrics_test_read_key" }
        for {set i 1} {$i <= 400} {incr i} { r set "metrics_test_write_key" "value_$i" }

        set info_after [r info hotkey]
        set final_total_sampled 0
        foreach line [split $info_after "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set final_total_sampled [lindex [split $line ":"] 1]
            }
        }

        assert {$final_total_sampled > $initial_total_sampled}
        assert_match "*hotkey_top_k:*" $info_after
        assert_match "*hotkey_half_life_seconds:*" $info_after
    }

    test "QPS reported as a positive decayed rate" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16
        r config set hotkey-half-life-seconds 60

        r set "qps_key" "val"
        for {set i 0} {$i < 1200} {incr i} { r get "qps_key" }

        set hotkeys [r hotkeys get TYPE read]
        assert {[llength $hotkeys] > 0}
        set first [lindex $hotkeys 0]
        assert_equal [lindex $first 1] "qps_key"
        # Under exponential decay the QPS is a recency-weighted rate that
        # depends on real elapsed time, so we assert a sane positive value
        # rather than an exact deterministic figure.
        set qps [lindex $first 9]
        assert {$qps > 0}
    }

    test "Cold keys fade out under decay" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 16
        # Short half-life so the entry decays below the eviction floor quickly.
        r config set hotkey-half-life-seconds 1

        r set "fading_key" "val"
        for {set i 0} {$i < 40} {incr i} { r get "fading_key" }

        set before [r hotkeys get TYPE read]
        assert {[llength $before] > 0}

        # Wait many half-lives with no access. The decayed count (~40) falls
        # under the 0.5 eviction floor after ~7s and is dropped on the next
        # query's lazy decay.
        after 7000
        set after_decay [r hotkeys get TYPE read]
        assert_equal [llength $after_decay] 0

        r config set hotkey-half-life-seconds 60
    }

    test "Max top-k tracks the hottest keys" {
        r hotkeys reset
        r config set hotkey-sampling-ratio 100
        r config set hotkey-top-k 3

        r set "cold_key" "val"
        r set "warm_key" "val"
        r set "hot_key" "val"

        for {set i 0} {$i < 100} {incr i} { r get "cold_key" }
        for {set i 0} {$i < 500} {incr i} { r get "warm_key" }
        for {set i 0} {$i < 1000} {incr i} { r get "hot_key" }

        set hotkeys [r hotkeys get TYPE read]
        assert {[llength $hotkeys] <= 3}
        assert {[llength $hotkeys] > 0}

        r config set hotkey-top-k 16
    }
}
