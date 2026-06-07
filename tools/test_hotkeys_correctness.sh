#!/usr/bin/env bash
#
# Correctness tests for HOTKEYS GET across different access patterns.
# Each scenario flushes state, drives a workload, and verifies the response
# against the expected top-K.
#
# Usage:
#   ./tools/test_hotkeys_correctness.sh [PORT]
#
# Requires: valkey-server and valkey-cli built and on PATH (or in src/).

set -uo pipefail

PORT="${1:-7777}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$HERE/src/valkey-server"
CLI="$HERE/src/valkey-cli"

# K (top-K capacity).
K=16
# Sampling ratio for tests (100% = exact counts within window).
SAMPLING=100
# Window long enough that no cron reset fires mid-test.
WINDOW=60

PASS=0
FAIL=0
SCENARIOS_RUN=0

c() { "$CLI" -p "$PORT" "$@"; }

server_pid=""
cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        c shutdown nosave >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- pretty printing -----------------------------------------------------
# Colors (skip if stdout isn't a TTY).
if [[ -t 1 ]]; then
    C_RST=$'\033[0m'  ; C_BOLD=$'\033[1m'  ; C_DIM=$'\033[2m'
    C_GRN=$'\033[32m' ; C_RED=$'\033[31m'  ; C_YLW=$'\033[33m'
    C_CYN=$'\033[36m' ; C_BLU=$'\033[34m'  ; C_MAG=$'\033[35m'
else
    C_RST=; C_BOLD=; C_DIM=; C_GRN=; C_RED=; C_YLW=; C_CYN=; C_BLU=; C_MAG=;
fi

log()  { printf '%s\n' "$*"; }
ok()   { printf '  %s✔%s %s\n' "$C_GRN" "$C_RST" "$*"; PASS=$((PASS+1)); }
bad()  { printf '  %s✘%s %s\n' "$C_RED" "$C_RST" "$*"; FAIL=$((FAIL+1)); }
note() { printf '  %s•%s %s\n' "$C_YLW" "$C_RST" "$*"; }
hdr()  {
    SCENARIOS_RUN=$((SCENARIOS_RUN+1))
    printf '\n%s━━━ Scenario %d: %s%s%s\n' "$C_BOLD$C_BLU" "$SCENARIOS_RUN" "$*" "" "$C_RST"
}

section() { printf '\n%s%s%s\n' "$C_BOLD" "$*" "$C_RST"; }

# Print a list of "label : value" lines indented, for "Workload" sections.
print_kv() {
    while [[ $# -ge 2 ]]; do
        printf '    %s%-26s%s %s\n' "$C_DIM" "$1" "$C_RST" "$2"
        shift 2
    done
}

# Print the actual HOTKEYS GET output as a formatted table.
# Args: TYPE arg passed to HOTKEYS GET (read|write|all)
print_hotkeys_table() {
    local type_arg="${1:-all}"
    local raw
    raw=$(c hotkeys get TYPE "$type_arg")

    # Convert flat array into columns: KEY, TYPE, DB, SLOT, QPS.
    local rows
    rows=$(echo "$raw" | awk '
        BEGIN { OFS="\t" }
        /^key$/    { getline v; key=v; next }
        /^type$/   { getline v; type=v; next }
        /^db$/     { getline v; db=v; next }
        /^slot$/   { getline v; slot=v; next }
        /^qps$/    { getline v; qps=v; print key, type, db, slot, qps }
    ')

    if [[ -z "$rows" ]]; then
        printf '    %s(empty response)%s\n' "$C_DIM" "$C_RST"
        return
    fi

    # Compute column widths.
    local key_w=3 type_w=4
    while IFS=$'\t' read -r key type db slot qps; do
        (( ${#key}  > key_w ))  && key_w=${#key}
        (( ${#type} > type_w )) && type_w=${#type}
    done <<< "$rows"

    # Header.
    printf '    %s%-*s  %-*s  %4s  %5s  %8s%s\n' "$C_DIM" \
        "$key_w" "KEY" "$type_w" "TYPE" "DB" "SLOT" "QPS" "$C_RST"
    printf '    %s' "$C_DIM"
    printf '%.0s─' $(seq 1 "$key_w"); printf '  '
    printf '%.0s─' $(seq 1 "$type_w"); printf '  ────  ─────  ────────'
    printf '%s\n' "$C_RST"

    # Rows.
    while IFS=$'\t' read -r key type db slot qps; do
        local color="$C_RST"
        [[ "$type" == "read"  ]] && color="$C_CYN"
        [[ "$type" == "write" ]] && color="$C_MAG"
        printf '    %-*s  %s%-*s%s  %4s  %5s  %8s\n' \
            "$key_w" "$key" "$color" "$type_w" "$type" "$C_RST" "$db" "$slot" "$qps"
    done <<< "$rows"
}

# Expectation banner. Pass description as a single string.
print_workload() { section "Workload"; print_kv "$@"; }
print_expect()   { section "Expected"; print_kv "$@"; }
print_actual()   {
    local type_arg="${1:-all}"
    section "Actual HOTKEYS GET TYPE $type_arg"
    print_hotkeys_table "$type_arg"
}
print_why()      { section "Why it passes"; printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RST"; }
print_checks()   { section "Checks"; }

start_server() {
    "$SERVER" --port "$PORT" --daemonize yes --logfile "" \
        --protected-mode no \
        --loglevel warning \
        --appendonly no \
        --save "" \
        --commandlog-request-larger-than -1 \
        --commandlog-reply-larger-than -1 \
        --io-threads 1 \
        --io-threads-do-reads no \
        --hotkey-enabled yes \
        --hotkey-sampling-ratio "$SAMPLING" \
        --hotkey-window-seconds "$WINDOW" \
        --hotkey-top-k "$K" >/dev/null
        #--hotkey-max-keys "$K" >/dev/null

        
    sleep 1
    server_pid=$(c info server | grep '^process_id:' | tr -d '\r' | cut -d: -f2)
    [[ -z "$server_pid" ]] && { log "Failed to start server"; exit 1; }
    log "Server started, pid=$server_pid"
}

reset_state() {
    c flushall >/dev/null
    c hotkeys reset >/dev/null
}

# Drive a workload from stdin (one command per line) via valkey-cli pipe mode.
pipe_load() {
    "$CLI" -p "$PORT" --pipe >/dev/null 2>&1
}

# Get all reported reads as "key qps" lines, sorted.
report_reads() {
    c hotkeys get TYPE read | awk '
        /^key$/    { getline k; next }
        /^qps$/    { getline q; print k, q; k="" }
    ' | sort
}

# Count of distinct entries reported in the read summary.
report_read_count() {
    report_reads | wc -l | tr -d ' '
}

# Get the QPS reported for a key (read type). Empty if not present.
report_qps_for() {
    local target="$1"
    c hotkeys get TYPE read | awk -v t="$target" '
        /^key$/    { getline k; if (k==t) found=1; next }
        /^qps$/    { getline q; if (found) { print q; exit } }'
}

# Assert: a key is present in the reads response.
assert_present() {
    local key="$1"
    local got
    got=$(report_qps_for "$key")
    if [[ -n "$got" ]]; then
        ok "key '$key' present (qps=$got)"
    else
        bad "key '$key' should be present in response, but is missing"
    fi
}

# Assert: a key is NOT present.
assert_absent() {
    local key="$1"
    local got
    got=$(report_qps_for "$key")
    if [[ -z "$got" ]]; then
        ok "key '$key' correctly absent"
    else
        bad "key '$key' should be absent, but reported qps=$got"
    fi
}

# Assert: total reported entries <= K.
assert_at_most_k() {
    local n
    n=$(report_read_count)
    if (( n <= K )); then
        ok "reported $n entries (<= K=$K)"
    else
        bad "reported $n entries (exceeds K=$K)"
    fi
}

# Assert: entries are sorted by qps descending.
assert_sorted() {
    local sorted
    sorted=$(c hotkeys get TYPE read | awk '/^qps$/ {getline; print}')
    local prev=99999999999
    local i=0
    while read -r line; do
        if (( line > prev )); then
            bad "entries not sorted descending by qps (saw $line after $prev)"
            return
        fi
        prev=$line
        i=$((i+1))
    done <<< "$sorted"
    ok "entries are sorted by qps descending ($i entries)"
}

# Assert: the top-N reported keys are exactly the given list (in any order).
assert_top_n_exact() {
    local n="$1"
    shift
    local expected
    expected=$(printf '%s\n' "$@" | sort)
    local actual
    actual=$(c hotkeys get TYPE read | awk '/^key$/{getline; print}' | head -n "$n" | sort)
    if [[ "$expected" == "$actual" ]]; then
        ok "top-$n keys match expected set"
    else
        bad "top-$n keys differ. expected=[$(echo $expected | tr '\n' ' ')] actual=[$(echo $actual | tr '\n' ' ')]"
    fi
}

# Drive N reads of a single key via pipeline.
do_reads() {
    local key="$1"
    local count="$2"
    yes "GET $key" | head -n "$count" | pipe_load
}

#------------------------------------------------------------------------------
# Scenarios
#------------------------------------------------------------------------------

scenario_uniform() {
    hdr "Uniform access (no hot keys)"
    reset_state

    # 50 keys, each accessed 100 times. No outliers.
    for i in $(seq 1 50); do
        c set "u_$i" v >/dev/null
    done
    {
        for j in $(seq 1 100); do
            for i in $(seq 1 50); do
                echo "GET u_$i"
            done
        done
    } | pipe_load

    print_workload \
        "Distinct keys"        "50 (u_1 .. u_50)" \
        "Reads per key"        "100" \
        "Total accesses"       "5,000" \
        "Hot keys"             "none (all uniform)"

    print_expect \
        "Reported entries"     "<= K ($K)" \
        "QPS values"           "<= 5 (no key dominates)" \
        "Order"                "descending by QPS"

    print_actual read

    print_why \
        "Misra-Gries cannot identify a winner when all keys have the same frequency. \
With f = 100 < N/(K+1) = 5000/17 = 294, no key meets the formal detection threshold, \
so reported counts are noisy and no single key should dominate."

    print_checks
    assert_at_most_k
    assert_sorted

    local max_qps
    max_qps=$(c hotkeys get TYPE read | awk '/^qps$/{getline; print}' | head -n1)
    if (( max_qps <= 5 )); then
        ok "no single key dominates (max qps=$max_qps)"
    else
        bad "expected uniform: max qps should be ~1-2, got $max_qps"
    fi
}

scenario_single_hot() {
    hdr "Single very hot key amid cold traffic"
    reset_state

    c set hot v >/dev/null
    for i in $(seq 1 30); do c set "cold_$i" v >/dev/null; done

    {
        for j in $(seq 1 6000); do echo "GET hot"; done
        for i in $(seq 1 30); do
            for j in $(seq 1 50); do echo "GET cold_$i"; done
        done
    } | pipe_load

    print_workload \
        "Hot key 'hot'"        "6,000 reads" \
        "Cold keys"            "30 keys (cold_1 .. cold_30) x 50 reads" \
        "Total accesses"       "7,500" \
        "Hot share of total"   "80%"

    print_expect \
        "'hot' present at top" "yes (must be the #1 entry)" \
        "Exact QPS for 'hot'"  "100  (= 6000 / 60s window)" \
        "Domination ratio"     "hot >> any cold key (>=5x)"

    print_actual read

    print_why \
        "'hot' has frequency 6000 / 7500 = 80%, far above the MG bound 1/(K+1) = 5.9%. \
The algorithm guarantees it is in the summary. With 100% sampling and a 60s window, \
the reported QPS equals the exact observed count divided by the window."

    print_checks
    assert_at_most_k
    assert_sorted
    assert_present "hot"

    local hot_qps top2
    hot_qps=$(report_qps_for hot)
    top2=$(c hotkeys get TYPE read | awk '/^qps$/{getline; print}' | sed -n '2p')
    if [[ -n "$top2" ]] && (( hot_qps > top2 * 5 )); then
        ok "hot key dominates (hot=$hot_qps, runner-up=$top2)"
    else
        bad "hot did not dominate (hot=$hot_qps, runner-up=$top2)"
    fi

    if (( hot_qps == 100 )); then
        ok "hot qps exactly matches expected (6000 / 60 = 100)"
    else
        bad "hot qps=$hot_qps, expected 100"
    fi
}

scenario_few_hot() {
    hdr "Few hot keys (m=4 < K=$K) above the threshold"
    reset_state

    c set h1 v >/dev/null; c set h2 v >/dev/null
    c set h3 v >/dev/null; c set h4 v >/dev/null
    for i in $(seq 1 100); do c set "c_$i" v >/dev/null; done

    {
        for j in $(seq 1 6000); do echo "GET h1"; done
        for j in $(seq 1 3000); do echo "GET h2"; done
        for j in $(seq 1 1500); do echo "GET h3"; done
        for j in $(seq 1 600);  do echo "GET h4"; done
        for i in $(seq 1 100); do
            for j in $(seq 1 30); do echo "GET c_$i"; done
        done
    } | pipe_load

    print_workload \
        "h1"                    "6,000 reads" \
        "h2"                    "3,000 reads" \
        "h3"                    "1,500 reads" \
        "h4"                    "  600 reads" \
        "Cold keys"             "100 keys x 30 = 3,000 reads" \
        "Total accesses"        "14,100" \
        "MG threshold N/(K+1)"  "830 (any key above this is guaranteed present)"

    print_expect \
        "All four hot keys"     "h1, h2, h3, h4 are present" \
        "Top-4 by QPS"          "exactly {h1, h2, h3, h4}" \
        "Exact QPS for h1"      "100  (= 6000 / 60s window)" \
        "QPS order"             "h1 (100) > h2 (50) > h3 (25) > h4 (10)"

    print_actual read

    print_why \
        "h1 (6000), h2 (3000), h3 (1500) all exceed the MG threshold of 830, so they \
are formally guaranteed to be present. h4 (600) is below the threshold but well above \
the noise floor of cold keys (each ~30), so it is reliably retained in practice. The \
top-4 are entirely the hot set."

    print_checks
    assert_at_most_k
    assert_sorted

    for k in h1 h2 h3 h4; do
        assert_present "$k"
    done

    assert_top_n_exact 4 h1 h2 h3 h4

    local h1_qps
    h1_qps=$(report_qps_for h1)
    if (( h1_qps == 100 )); then
        ok "h1 qps exact (6000 / 60 = 100)"
    else
        bad "h1 qps=$h1_qps, expected 100"
    fi
}

# Helper for the "below threshold" placement scenarios.
#
# Workload design:
#   - 4 hot keys (h1..h4) each accessed 200 times.
#   - 300 cold keys (c_1..c_300) each accessed 50 times.
#   - Total = 800 + 15000 = 15,800. Threshold = N/(K+1) = 929.
#   - All 4 hot keys are BELOW the formal MG detection threshold (200 < 929).
#
# What changes between scenarios is *when* the hot accesses appear in the stream.
# This explores how MG's eviction dynamics interact with timing — the algorithm
# makes no formal promise here, so we report observations rather than failing.

below_threshold_seed_keys() {
    for n in h1 h2 h3 h4; do c set "$n" v >/dev/null; done
    for i in $(seq 1 300); do c set "c_$i" v >/dev/null; done
}

below_threshold_print_intro() {
    print_workload \
        "Hot keys (h1..h4)"      "200 reads each = 800" \
        "Cold keys (c_1..c_300)" "50 reads each   = 15,000" \
        "Total accesses"         "15,800" \
        "MG threshold N/(K+1)"   "929  (all hot freqs 200 are BELOW threshold)" \
        "Hot placement"          "$1"

    print_expect \
        "Formal MG guarantee"    "NONE (all hot frequencies below threshold)" \
        "True hottest keys"      "h1..h4 (4 reads per cold key but 200 per hot)" \
        "Test requirement"       "all 4 hottest keys must appear; top-1 must be hot" \
        "Note"                   "if a placement causes MG to miss a true heavy hitter, the test FAILS"
}

below_threshold_observe() {
    print_checks
    assert_at_most_k
    assert_sorted

    # The 4 hot keys are the *hottest* in the workload (200 reads each) even
    # though they are below the formal MG threshold. We expect all 4 to appear
    # in the response — if fewer do, MG missed a true heavy hitter and the
    # test must fail.
    local hot_kept=0 missing=()
    for k in h1 h2 h3 h4; do
        if [[ -n "$(report_qps_for "$k")" ]]; then
            hot_kept=$((hot_kept+1))
        else
            missing+=("$k")
        fi
    done

    if (( hot_kept == 4 )); then
        ok "all 4 hottest keys retained in the response"
    else
        bad "$hot_kept of 4 hottest keys retained — missing: ${missing[*]}"
    fi

    local top
    top=$(c hotkeys get TYPE read | awk '/^key$/{getline; print; exit}')
    case "$top" in
        h1|h2|h3|h4) ok "top-1 is one of the hot keys ('$top')" ;;
        *)           bad "top-1 is a cold key ('$top'), expected one of h1..h4" ;;
    esac
}

scenario_below_threshold_beginning() {
    hdr "Few hot keys (m=4 < K=$K) below threshold — hot at BEGINNING"
    reset_state
    below_threshold_seed_keys

    {
        # All hot accesses first.
        for j in $(seq 1 200); do echo "GET h1"; done
        for j in $(seq 1 200); do echo "GET h2"; done
        for j in $(seq 1 200); do echo "GET h3"; done
        for j in $(seq 1 200); do echo "GET h4"; done
        # Then 300 distinct cold keys, each 50 times.
        for i in $(seq 1 300); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
    } | pipe_load

    below_threshold_print_intro "all 4 hot keys at the start, then 15,000 cold reads"
    print_actual read
    print_why \
        "Hot keys arrive first and seed the K=16 slots. The flood of 300 distinct cold keys \
that follows triggers many decrement-on-collision rounds. Whether the hot keys survive \
depends on whether their counters (200 each) outlast the decrement pressure from cold-key \
churn (which is ~300 distinct keys * 50 accesses minus the available slots). Expect most \
hot keys to be EVICTED — cold churn dominates."

    below_threshold_observe
}

scenario_below_threshold_middle() {
    hdr "Few hot keys (m=4 < K=$K) below threshold — hot in MIDDLE"
    reset_state
    below_threshold_seed_keys

    {
        # First half of cold reads.
        for i in $(seq 1 150); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
        # All hot accesses in the middle.
        for j in $(seq 1 200); do echo "GET h1"; done
        for j in $(seq 1 200); do echo "GET h2"; done
        for j in $(seq 1 200); do echo "GET h3"; done
        for j in $(seq 1 200); do echo "GET h4"; done
        # Second half of cold reads.
        for i in $(seq 151 300); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
    } | pipe_load

    below_threshold_print_intro "150 cold keys, then 4 hot bursts, then 150 more cold keys"
    print_actual read
    print_why \
        "Hot keys land in slots that may already be partially occupied by surviving cold \
keys. Their 200 accesses build up counters before the second cold flood arrives. Some \
hot keys are likely evicted by post-burst cold churn; others may survive depending on \
which slot they ended up in and how decrements propagate."

    below_threshold_observe
}

scenario_below_threshold_end() {
    hdr "Few hot keys (m=4 < K=$K) below threshold — hot at END"
    reset_state
    below_threshold_seed_keys

    {
        # All cold reads first.
        for i in $(seq 1 300); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
        # Hot accesses last.
        for j in $(seq 1 200); do echo "GET h1"; done
        for j in $(seq 1 200); do echo "GET h2"; done
        for j in $(seq 1 200); do echo "GET h3"; done
        for j in $(seq 1 200); do echo "GET h4"; done
    } | pipe_load

    below_threshold_print_intro "all 15,000 cold reads first, then 4 hot bursts at the end"
    print_actual read
    print_why \
        "Cold-key churn beats up the summary first; whatever K survivors remain at the end \
of the cold stream are mostly cold keys with low counters. When the hot bursts arrive, \
each hot key's first 16 accesses fill empty/decremented slots. The remaining ~184 \
accesses build up the counter unopposed (no further decrement pressure). Hot keys are \
LIKELY to dominate the final response — this placement is the most favorable."

    below_threshold_observe
}

scenario_below_threshold_interleaved() {
    hdr "Few hot keys (m=4 < K=$K) below threshold — INTERLEAVED"
    reset_state
    below_threshold_seed_keys

    # Build a stream that interleaves a cold key and a hot key at roughly the
    # right ratio: 300*50=15000 cold accesses vs 4*200=800 hot accesses,
    # so ~19 cold per 1 hot. We round-robin through hot keys.
    {
        local cold_i=1
        local hot_remain=800
        local cold_remain=15000
        local hot_idx=1
        # Drive hot/cold accesses interleaved in the right ratio.
        while (( cold_remain > 0 || hot_remain > 0 )); do
            # 19 cold accesses (advancing through cold keys), then 1 hot.
            for k in $(seq 1 19); do
                if (( cold_remain > 0 )); then
                    echo "GET c_$cold_i"
                    cold_i=$((cold_i + 1)); if (( cold_i > 300 )); then cold_i=1; fi
                    cold_remain=$((cold_remain - 1))
                fi
            done
            if (( hot_remain > 0 )); then
                echo "GET h$hot_idx"
                hot_idx=$((hot_idx + 1)); if (( hot_idx > 4 )); then hot_idx=1; fi
                hot_remain=$((hot_remain - 1))
            fi
        done
    } | pipe_load

    below_threshold_print_intro "1 hot access per ~19 cold accesses, round-robin across h1..h4"
    print_actual read
    print_why \
        "Hot and cold accesses are uniformly mixed, so hot keys never get a chance to build \
a counter lead over cold churn. Each hot access lands in a heavily-contested summary, \
likely competing for the slot the previous hot access already occupied (or one that's \
been decremented away). Hot key retention here is roughly the same as for cold keys at \
the same access rate — they may or may not appear, with no formal guarantee."

    below_threshold_observe
}

# Helper for the "more-than-K hot keys, all below threshold" scenarios.
#
# Workload design:
#   - 20 hot keys (h_1..h_20) each accessed 300 times.
#   - 200 cold keys (c_1..c_200) each accessed 50 times.
#   - Total = 6,000 + 10,000 = 16,000. Threshold = N/(K+1) = 941.
#   - All 20 hot keys are BELOW the formal MG threshold (300 < 941).
#   - Hot count (20) exceeds K (16), so we cannot fit them all even ideally.
#
# These scenarios stress how each algorithm balances "more hot candidates than
# slots" with "individual frequencies below the formal guarantee" — placement
# becomes a tiebreaker for which subset of hot keys survives.

mtk_below_threshold_seed_keys() {
    for i in $(seq 1 20); do c set "h_$i" v >/dev/null; done
    for i in $(seq 1 200); do c set "c_$i" v >/dev/null; done
}

mtk_below_threshold_print_intro() {
    print_workload \
        "Hot keys (h_1..h_20)"   "300 reads each = 6,000  (m=20 > K=$K)" \
        "Cold keys (c_1..c_200)" "50 reads each  = 10,000" \
        "Total accesses"         "16,000" \
        "MG threshold N/(K+1)"   "941  (all hot freqs 300 are BELOW threshold)" \
        "Hot placement"          "$1"

    print_expect \
        "Formal MG guarantee"    "NONE (all hot frequencies below threshold)" \
        "Capacity vs hot count"  "20 hot but only $K slots — at most $K can survive" \
        "Test requirement"       "top-$K must all be from the hot set; no cold-key contamination at top" \
        "Note"                   "we cannot demand a specific subset of the 20 hot keys; only that the survivors are hot"
}

mtk_below_threshold_observe() {
    print_checks
    assert_at_most_k
    assert_sorted

    # Count how many of the K reported entries are hot (h_*) vs cold (c_*).
    local raw
    raw=$(c hotkeys get TYPE read | awk '/^key$/{getline; print}')
    local hot_count=0 cold_count=0
    while read -r k; do
        case "$k" in
            h_*) hot_count=$((hot_count+1)) ;;
            c_*) cold_count=$((cold_count+1)) ;;
        esac
    done <<< "$raw"

    if (( cold_count == 0 )); then
        ok "all $hot_count reported entries are hot keys (no cold contamination)"
    else
        bad "$cold_count of $((hot_count + cold_count)) reported entries are COLD keys (expected zero)"
    fi

    # The top-1 entry must be a hot key.
    local top
    top=$(echo "$raw" | head -n1)
    case "$top" in
        h_*) ok "top-1 is a hot key ('$top')" ;;
        c_*) bad "top-1 is a cold key ('$top'), expected one of h_*" ;;
        *)   bad "top-1 is unexpected key '$top'" ;;
    esac
}

scenario_mtk_below_threshold_beginning() {
    hdr "More than K=$K hot keys, below threshold — hot at BEGINNING"
    reset_state
    mtk_below_threshold_seed_keys

    {
        # All hot accesses first (h_1 burst, then h_2 burst, ...).
        for i in $(seq 1 20); do
            for j in $(seq 1 300); do echo "GET h_$i"; done
        done
        # Then 200 distinct cold keys, each 50 times.
        for i in $(seq 1 200); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
    } | pipe_load

    mtk_below_threshold_print_intro "20 hot bursts at the start, then 10,000 cold reads"
    print_actual read
    print_why \
        "Hot keys arrive first and seed the K=$K slots (4 hot keys won't even fit). \
The flood of 200 distinct cold keys that follows triggers many decrement-on-collision \
rounds (MG) or count-inheritance evictions (Space-Saving). Whether any hot keys survive \
depends on whether their counters (300 each) outlast the cold-key churn."

    mtk_below_threshold_observe
}

scenario_mtk_below_threshold_middle() {
    hdr "More than K=$K hot keys, below threshold — hot in MIDDLE"
    reset_state
    mtk_below_threshold_seed_keys

    {
        # First half of cold reads.
        for i in $(seq 1 100); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
        # All hot accesses in the middle.
        for i in $(seq 1 20); do
            for j in $(seq 1 300); do echo "GET h_$i"; done
        done
        # Second half of cold reads.
        for i in $(seq 101 200); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
    } | pipe_load

    mtk_below_threshold_print_intro "100 cold keys, then 20 hot bursts, then 100 more cold keys"
    print_actual read
    print_why \
        "Hot keys arrive after the summary is partially populated by cold keys. They \
build up some counter advantage but face a second cold flood that re-churns the slots. \
Survival is roughly a coin-flip across hot vs latest-cold-keys."

    mtk_below_threshold_observe
}

scenario_mtk_below_threshold_end() {
    hdr "More than K=$K hot keys, below threshold — hot at END"
    reset_state
    mtk_below_threshold_seed_keys

    {
        # All cold reads first.
        for i in $(seq 1 200); do
            for j in $(seq 1 50); do echo "GET c_$i"; done
        done
        # Hot accesses last.
        for i in $(seq 1 20); do
            for j in $(seq 1 300); do echo "GET h_$i"; done
        done
    } | pipe_load

    mtk_below_threshold_print_intro "all 10,000 cold reads first, then 20 hot bursts at the end"
    print_actual read
    print_why \
        "Cold churn happens first and beats the K survivors down to low counts. When hot \
bursts arrive, each hot key's accesses can dominate the (now-stable) summary. Even \
though there are 20 hot keys competing for $K slots, the late-arriving hot keys with \
300 accesses each should displace the earlier ones whose counters haven't grown as much, \
and the final response should be entirely hot."

    mtk_below_threshold_observe
}

scenario_mtk_below_threshold_interleaved() {
    hdr "More than K=$K hot keys, below threshold — INTERLEAVED"
    reset_state
    mtk_below_threshold_seed_keys

    # Total 16,000 accesses: 6,000 hot vs 10,000 cold = 1 hot per ~1.67 cold.
    # Round to 1 hot per 2 cold for stream construction.
    {
        local cold_i=1 hot_i=1
        local hot_remain=6000 cold_remain=10000
        while (( hot_remain > 0 || cold_remain > 0 )); do
            # 2 cold accesses (advancing through cold keys).
            for k in 1 2; do
                if (( cold_remain > 0 )); then
                    echo "GET c_$cold_i"
                    cold_i=$((cold_i + 1)); if (( cold_i > 200 )); then cold_i=1; fi
                    cold_remain=$((cold_remain - 1))
                fi
            done
            # 1 hot access, round-robin across h_1..h_20.
            if (( hot_remain > 0 )); then
                echo "GET h_$hot_i"
                hot_i=$((hot_i + 1)); if (( hot_i > 20 )); then hot_i=1; fi
                hot_remain=$((hot_remain - 1))
            fi
        done
    } | pipe_load

    mtk_below_threshold_print_intro "1 hot access per ~2 cold accesses, round-robin across h_1..h_20"
    print_actual read
    print_why \
        "Hot and cold accesses are uniformly mixed. Each hot access lands in a constantly- \
churning summary, fighting both other hot keys (only 16 slots for 20) and persistent cold \
churn. Hot key survival depends on which keys happen to occupy slots when the stream \
finishes — no algorithmic guarantee."

    mtk_below_threshold_observe
}

scenario_more_than_k_hot() {
    hdr "More than K=$K hot keys (m=20)"
    reset_state

    for i in $(seq 1 20); do c set "h_$i" v >/dev/null; done

    {
        for j in $(seq 1 1000); do
            for i in $(seq 1 20); do echo "GET h_$i"; done
        done
    } | pipe_load

    print_workload \
        "Hot keys"              "20 (h_1 .. h_20), all interleaved" \
        "Reads per key"         "1,000" \
        "Total accesses"        "20,000" \
        "MG threshold N/(K+1)"  "1,176 (per-key freq 1,000 is BELOW this)"

    print_expect \
        "Reported entries"      "<= K ($K)" \
        "All reported keys"     "from the hot set (no cold-key contamination)" \
        "Exact membership"      "no formal guarantee (frequencies below threshold)"

    print_actual read

    print_why \
        "All 20 keys have the same frequency below the formal MG threshold. The \
algorithm makes no promise about *which* K survive — they fight for slots and \
some end up evicted with counter==decrements. We only verify (a) the response \
respects the K cap and (b) no key from outside the hot set leaks in."

    print_checks
    local n
    n=$(report_read_count)
    assert_at_most_k
    assert_sorted

    local non_hot_reported
    non_hot_reported=$(c hotkeys get TYPE read | awk '/^key$/{getline; print}' | grep -vc '^h_' || true)
    if (( non_hot_reported == 0 )); then
        ok "all $n reported entries are from the hot set"
    else
        bad "$non_hot_reported reported entries were not in the hot set"
    fi
}

scenario_zipfian() {
    hdr "Zipfian-like distribution (sharp dropoff)"
    reset_state

    for i in $(seq 1 10); do c set "z_$i" v >/dev/null; done
    {
        local count=10000
        for i in $(seq 1 10); do
            for j in $(seq 1 "$count"); do echo "GET z_$i"; done
            count=$((count / 2))
        done
    } | pipe_load

    print_workload \
        "z_1"                   "10,000 reads" \
        "z_2"                   " 5,000 reads" \
        "z_3"                   " 2,500 reads" \
        "z_4 .. z_10"           "halving each step (1250, 625, ...)" \
        "Total accesses"        "~19,960"

    print_expect \
        "Top key"               "z_1 with QPS=166 (= 10000 / 60)" \
        "Top-3 ordering"        "z_1, z_2, z_3 (descending)" \
        "All entries"           "<= K, sorted by QPS"

    print_actual read

    print_why \
        "All ten keys have distinct frequencies, so the top-K ranking is deterministic. \
z_1 (10000), z_2 (5000), z_3 (2500) all exceed the MG threshold N/(K+1) ~= 1175 and \
are guaranteed present in the correct order."

    print_checks
    assert_at_most_k
    assert_sorted

    local top_key top_qps
    top_key=$(c hotkeys get TYPE read | awk '/^key$/{getline; print; exit}')
    top_qps=$(report_qps_for "$top_key")
    if [[ "$top_key" == "z_1" ]] && (( top_qps == 166 )); then
        ok "top key is z_1 with qps=166"
    else
        bad "expected top=z_1 qps=166, got top=$top_key qps=$top_qps"
    fi

    local top3
    top3=$(c hotkeys get TYPE read | awk '/^key$/{getline; print}' | head -n3 | tr '\n' ' ')
    if [[ "$top3" == "z_1 z_2 z_3 " ]]; then
        ok "top-3 ordering is z_1, z_2, z_3"
    else
        bad "top-3 ordering wrong: '$top3'"
    fi
}

scenario_mixed_read_write() {
    hdr "Mixed read/write: separate type tracking"
    reset_state

    c set rwhot v >/dev/null

    {
        for j in $(seq 1 3000); do echo "GET rwhot"; done
        for j in $(seq 1 1500); do echo "SET rwhot v"; done
    } | pipe_load

    print_workload \
        "GETs on 'rwhot'"       "3,000" \
        "SETs on 'rwhot'"       "1,500 (plus 1 initial SET = 1,501)" \
        "Same key for both"     "yes"

    print_expect \
        "TYPE read entry"       "rwhot with QPS=50 (= 3000 / 60)" \
        "TYPE write entry"      "rwhot with QPS=25 or 26 (= 1501 / 60)" \
        "TYPE all"              "both entries present"

    print_actual all

    print_why \
        "Read and write summaries are independent. The same logical key appears in \
both with frequencies derived from each access type separately, so workloads with \
mixed read/write hotness don't blur into a single view."

    print_checks
    local read_qps write_qps
    read_qps=$(c hotkeys get TYPE read  | awk '/^key$/{getline; if($0=="rwhot"){found=1; next} } /^qps$/{getline; if(found){print; exit}}')
    write_qps=$(c hotkeys get TYPE write | awk '/^key$/{getline; if($0=="rwhot"){found=1; next} } /^qps$/{getline; if(found){print; exit}}')

    if [[ "$read_qps" == "50" ]]; then
        ok "rwhot appears in TYPE read with qps=50"
    else
        bad "expected rwhot read qps=50, got '$read_qps'"
    fi
    if [[ "$write_qps" == "26" ]] || [[ "$write_qps" == "25" ]]; then
        ok "rwhot appears in TYPE write with qps=$write_qps (~25)"
    else
        bad "expected rwhot write qps=25, got '$write_qps'"
    fi

    local all_count
    all_count=$(c hotkeys get TYPE all | awk '/^key$/{getline; print}' | sort -u | wc -l | tr -d ' ')
    if (( all_count >= 1 )); then
        ok "TYPE all returned at least one rwhot entry"
    fi
}

scenario_db_isolation() {
    hdr "Multi-DB: same key name in different databases"
    reset_state

    c -n 0 set k v >/dev/null
    c -n 3 set k v >/dev/null

    {
        for j in $(seq 1 1200); do echo "SELECT 0"; echo "GET k"; done
        for j in $(seq 1 600);  do echo "SELECT 3"; echo "GET k"; done
    } | pipe_load

    print_workload \
        "Key name"              "'k' in db0 and db3" \
        "GETs in db0"           "1,200" \
        "GETs in db3"           "  600"

    print_expect \
        "Distinct entries"      "2 (one per (key, db) pair)" \
        "db0 entry QPS"         "20  (= 1200 / 60)" \
        "db3 entry QPS"         "10  (= 600 / 60)"

    print_actual read

    print_why \
        "Identity in the summary is (key, dbid), not key alone. Same key name in \
different logical databases is tracked as two distinct entries with independent \
counters and reported QPS values."

    print_checks
    local entries
    entries=$(c hotkeys get TYPE read | awk '
        /^key$/  { getline k }
        /^db$/   { getline d; print k, d }
    ' | grep '^k ' | sort -k2,2n)

    local lines
    lines=$(echo "$entries" | wc -l | tr -d ' ')
    if (( lines == 2 )); then
        ok "key 'k' tracked separately for db0 and db3"
    else
        bad "expected 2 entries for 'k' (db0, db3), got $lines"
    fi

    local db0_qps db3_qps
    db0_qps=$(c hotkeys get TYPE read | awk '
        BEGIN{state=0}
        /^key$/{getline k; state=1; next}
        /^db$/ {getline d; if(k=="k" && d==0) found=1; next}
        /^qps$/{getline q; if(found){print q; exit}}')
    db3_qps=$(c hotkeys get TYPE read | awk '
        /^key$/{getline k; next}
        /^db$/ {getline d; if(k=="k" && d==3) found=1; next}
        /^qps$/{getline q; if(found){print q; exit}}')

    if [[ "$db0_qps" == "20" ]]; then
        ok "db0 qps=20 (1200 / 60)"
    else
        bad "db0 qps=$db0_qps, expected 20"
    fi
    if [[ "$db3_qps" == "10" ]]; then
        ok "db3 qps=10 (600 / 60)"
    else
        bad "db3 qps=$db3_qps, expected 10"
    fi
}

scenario_reset_clears_state() {
    hdr "RESET clears state"
    reset_state

    c set foo v >/dev/null
    yes "GET foo" | head -n 500 | pipe_load

    print_workload \
        "GETs on 'foo'"         "500" \
        "Then"                  "HOTKEYS RESET"

    print_expect \
        "Before RESET"          "at least 1 entry present" \
        "After RESET"           "0 entries"

    section "State before RESET"
    print_hotkeys_table all

    print_why \
        "HOTKEYS RESET wipes all summary state immediately. The next HOTKEYS GET \
returns an empty array even though the underlying keys still exist in the database."

    print_checks
    local before
    before=$(report_read_count)
    if (( before > 0 )); then
        ok "before reset: have $before entries"
    else
        bad "expected at least 1 entry before reset, got 0"
    fi

    c hotkeys reset >/dev/null

    section "State after RESET"
    print_hotkeys_table all

    local after
    after=$(report_read_count)
    if (( after == 0 )); then
        ok "after reset: 0 entries"
    else
        bad "expected 0 entries after reset, got $after"
    fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

start_server

scenario_uniform
scenario_single_hot
scenario_few_hot
scenario_below_threshold_beginning
scenario_below_threshold_middle
scenario_below_threshold_end
scenario_below_threshold_interleaved
scenario_mtk_below_threshold_beginning
scenario_mtk_below_threshold_middle
scenario_mtk_below_threshold_end
scenario_mtk_below_threshold_interleaved
scenario_more_than_k_hot
scenario_zipfian
scenario_mixed_read_write
scenario_db_isolation
scenario_reset_clears_state

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
log
log "=========================================="
log "Scenarios: $SCENARIOS_RUN"
log "PASS: $PASS"
log "FAIL: $FAIL"
log "=========================================="

if (( FAIL > 0 )); then
    exit 1
fi
exit 0
