#!/bin/bash
#
# run_full_sweep.sh — Exhaustive memfreq_bench sweep for deep analysis
#
# Runs a comprehensive grid of tests:
#   - 7 stride values (1,2,4,8,16,32,64)
#   - 11 multi-core counts (1,2,4,8,12,16,24,32,48,64,96)
#   - Random permutation + cache flush
#   - Full NUMA matrix (local/remote × each node)
#   - Combined modes
#
# Expected duration: ~3-4 hours at -t 5 -n 5
#
# Usage:
#   sudo ./run_full_sweep.sh
#   sudo ./run_full_sweep.sh --quick       # ~30 min subset
#   sudo ./run_full_sweep.sh --yes          # skip prompt
#

set -uo pipefail

# ============================================================================
# Config
# ============================================================================

QUICK_MODE=0
NO_PROMPT=0
SELECTED_SUITES=""   # --suite N[,N...]: run only the listed suite numbers
RESUME_MODE=0        # --resume: skip tests whose output file already has a sweet-spot line
OUTPUT_DIR=""
TEST_DURATION=5
TEST_SAMPLES=5
CPU_PIN=0

# Thresholds to evaluate in the per-test sensitivity sweep. Comma-separated,
# passed to memfreq_bench via -L. Override with SENS_THRESHOLDS=0.85,0.90 env var.
SENS_THRESHOLDS="${SENS_THRESHOLDS:-0.80,0.90,0.95,0.99}"

# Flags that unlock the new statistical blocks (sensitivity, sweet-spot CI
# via bootstrap, raw_samples). Always on in lazy mode — the user said
# "事后汇总看结果" so richer per-test data is the point.
STATS_FLAGS=(-r -L "$SENS_THRESHOLDS")

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="${SCRIPT_DIR}/memfreq_bench"

TESTS_RUN=0
TESTS_OK=0
TESTS_FAIL=0

# ============================================================================
# Helpers
# ============================================================================

log_info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
log_success() { echo -e "${GREEN}[ OK]${NC} $*"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error()   { echo -e "${RED}[ERR]${NC} $*" >&2; }
log_suite()   { echo -e "\n${CYAN}━━━ $* ━━━${NC}\n"; }

run_test() {
    local test_name="$1"; shift
    local output_file="$OUTPUT_DIR/${test_name}.txt"
    TESTS_RUN=$((TESTS_RUN + 1))

    # --resume: skip tests whose output file already has a valid sweet-spot line
    if [[ $RESUME_MODE -eq 1 && -f "$output_file" ]] \
        && grep -q "^# stride  sweet spot:" "$output_file" 2>/dev/null; then
        log_info "[$TESTS_RUN] $test_name  (cached, skipping)"
        TESTS_OK=$((TESTS_OK + 1))
        return 0
    fi

    log_info "[$TESTS_RUN] $test_name"

    {
        echo "# Test: $test_name"
        echo "# Command: $BENCH $*"
        echo "# Timestamp: $(date -Iseconds)"
        echo "#"
    } > "$output_file"

    if "$BENCH" "$@" >> "$output_file" 2>&1; then
        TESTS_OK=$((TESTS_OK + 1))
        local sweet
        sweet=$(grep "sweet spot" "$output_file" | head -1 | sed 's/^#\s*//' || true)
        log_success "  ✓ ${sweet:-$test_name}"
    else
        TESTS_FAIL=$((TESTS_FAIL + 1))
        log_warn "  ✗ $test_name"
    fi
}

# Suite selector for --suite N[,N...]. Returns 0 (run it) if the suite number
# is in $SELECTED_SUITES, or if $SELECTED_SUITES is empty (run all).
should_run_suite() {
    local n="$1"
    if [[ -z "$SELECTED_SUITES" ]]; then
        return 0
    fi
    [[ ",$SELECTED_SUITES," == *",$n,"* ]]
}

detect_topology() {
    local cores_per_socket sockets
    cores_per_socket=$(lscpu | grep "Core(s) per socket:" | awk '{print $NF}')
    sockets=$(lscpu | grep "Socket(s):" | awk '{print $NF}')
    NUM_PHYSICAL=$((cores_per_socket * sockets))
    NUM_LOGICAL=$(nproc)
    NUM_SMT=$((NUM_LOGICAL / NUM_PHYSICAL))

    if command -v numactl &>/dev/null; then
        NUM_NUMA=$(numactl -H 2>/dev/null | grep "available:" | awk '{print $2}' || true)
    fi
    NUM_NUMA=${NUM_NUMA:-1}

    FMIN=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq 2>/dev/null || echo 0)
    FMAX=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo 0)
    FMIN_MHZ=$((FMIN / 1000))
    FMAX_MHZ=$((FMAX / 1000))

    log_success "Topology: ${NUM_PHYSICAL}P/${NUM_LOGICAL}T, SMT=${NUM_SMT}, NUMA=${NUM_NUMA}, ${FMIN_MHZ}-${FMAX_MHZ} MHz"
}

get_numa_cpu() {
    local node=$1
    numactl -H 2>/dev/null | grep "node $node cpus:" | \
        sed 's/node [0-9]* cpus: //' | awk '{print $1}' || true
}

# ============================================================================
# Parse a test output file, extract key data points
# ============================================================================

extract_data() {
    local file=$1
    local test_name
    test_name=$(basename "$file" .txt)

    # Extract max throughput (last data line = lowest freq in high→low sweep...
    # actually first line = highest freq)
    local max_line
    max_line=$(grep -E "^[0-9]" "$file" 2>/dev/null | head -1 || true)
    local max_freq max_mbs
    max_freq=$(echo "$max_line" | awk '{print $1}' || true)
    max_mbs=$(echo "$max_line" | awk '{print $4}' || true)

    # Extract sweet spots
    local stride_sweet chase_sweet
    stride_sweet=$(grep "stride.*sweet spot" "$file" 2>/dev/null | grep -oP '\d+ MHz' | head -1 | awk '{print $1}' || true)
    chase_sweet=$(grep "chase.*sweet spot" "$file" 2>/dev/null | grep -oP '\d+ MHz' | head -1 | awk '{print $1}' || true)

    # Extract CI low/high (sweet-spot CI block; only present with -r)
    # Block format: "# workload  sweet_MHz  low_MHz  high_MHz  method"
    # Lines start with "# " (hash + space), so awk's default whitespace
    # splitting makes $1="#" and $2=workload — match on $2.
    # Em-dash values (no_plateau) come through as the literal "—" which we
    # want to preserve as "?" in the report so it doesn't look like a number.
    local stride_ci_low stride_ci_high chase_ci_low chase_ci_high
    local ci_block
    ci_block=$(awk '/^# --- sweet-spot CI ---/{f=1; next} /^# ---/{f=0} f' "$file" 2>/dev/null)
    stride_ci_low=$(echo "$ci_block"  | awk '$2=="stride" {print $3}')
    stride_ci_high=$(echo "$ci_block" | awk '$2=="stride" {print $4}')
    chase_ci_low=$(echo "$ci_block"   | awk '$2=="chase"  {print $3}')
    chase_ci_high=$(echo "$ci_block"  | awk '$2=="chase"  {print $4}')

    # Extract plateau breakpoint (always present unless suppressed by -P).
    # Format: "# stride   plateau_breakpoint: 2050 MHz  (slope ratio ..., ...)"
    local stride_bp chase_bp
    stride_bp=$(grep "stride.*plateau_breakpoint" "$file" 2>/dev/null | grep -oP '\d+ MHz' | head -1 | awk '{print $1}')
    chase_bp=$(grep  "chase.*plateau_breakpoint"  "$file" 2>/dev/null | grep -oP '\d+ MHz' | head -1 | awk '{print $1}')

    # 11-field pipe-delimited row. Order is significant — see report code.
    # Fields 1-5 are the original baseline; 6-11 are the new statistical add-ons.
    echo "${test_name}|${max_freq:-?}|${max_mbs:-?}|${stride_sweet:-?}|${chase_sweet:-?}|${stride_ci_low:-?}|${stride_ci_high:-?}|${chase_ci_low:-?}|${chase_ci_high:-?}|${stride_bp:-?}|${chase_bp:-?}"
}

# Per-threshold sensitivity for a given workload. Returns pipe-separated
# values aligned with $SENS_THRESHOLDS order, one per threshold. Em-dash
# for "no plateau at this threshold" (or for workloads that never plateau,
# like compute).
extract_sensitivity() {
    local file=$1
    local workload=$2

    # Block is delimited by "# --- sensitivity (workload) ---" ... next "# ---"
    local block
    block=$(awk -v w="$workload" '
        $0 ~ "^# --- sensitivity \\(" w "\\) ---" {f=1; next}
        /^# ---/ && f {f=0}
        f
    ' "$file" 2>/dev/null)

    local IFS=','
    local -a ts
    read -ra ts <<< "$SENS_THRESHOLDS"

    local out=""
    for t in "${ts[@]}"; do
        # Lines look like "# 0.80\t800" or "# 1.00\t—". awk's default FS
        # makes $1="#" and $2=threshold, so match on $2 and grab $3.
        local v
        v=$(echo "$block" | awk -v t="$t" '$2==t {print $3}')
        out+="${out:+|}${v:-—}"
    done
    echo "$out"
}

# ============================================================================
# Test Suites
# ============================================================================

suite_stride_grid() {
    log_suite "Suite A: Stride Grid (7 values × full sweep)"
    log_info "Testing how stride size affects frequency sensitivity"

    local common=(-c "$CPU_PIN" -A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    for stride in 1 2 4 8 16 32 64; do
        run_test "s${stride}" "${common[@]}" -s "$stride"
    done
}

suite_random_flush() {
    log_suite "Suite B: Random + Flush Modes"

    local common=(-c "$CPU_PIN" -A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    run_test "random"    "${common[@]}" -R
    run_test "flush"     "${common[@]}" -f
    run_test "randflush" "${common[@]}" -R -f
}

suite_multicore_sweep() {
    log_suite "Suite C: Multi-Core Sweep (11 core counts)"
    log_info "Testing how core count affects bandwidth-bound sweet spot"

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    local -a counts=(1 2 4 8 12 16 24 32 48 64 96)

    for n in "${counts[@]}"; do
        [[ $n -gt $NUM_PHYSICAL ]] && continue
        run_test "mc${n}" -N "$n" "${common[@]}"
    done
}

suite_multicore_modes() {
    log_suite "Suite D: Multi-Core + Access Modes"

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    # Multi-core × stride
    for n in 2 4 8 16; do
        [[ $n -gt $NUM_PHYSICAL ]] && continue
        run_test "mc${n}_s1"  -N "$n" -s 1  "${common[@]}"
        run_test "mc${n}_s64" -N "$n" -s 64 "${common[@]}"
    done

    # Multi-core × random
    for n in 2 4 8; do
        [[ $n -gt $NUM_PHYSICAL ]] && continue
        run_test "mc${n}_R" -N "$n" -R "${common[@]}"
    done

    # Multi-core × flush
    for n in 2 4 8; do
        [[ $n -gt $NUM_PHYSICAL ]] && continue
        run_test "mc${n}_f" -N "$n" -f "${common[@]}"
    done
}

suite_numa_matrix() {
    log_suite "Suite E: Full NUMA Matrix"

    if [[ $NUM_NUMA -lt 2 ]]; then
        log_warn "Single NUMA node, skipping"
        return 0
    fi

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    # Every CPU×Memory combination
    for cpu_node in $(seq 0 $((NUM_NUMA - 1))); do
        local cpu
        cpu=$(get_numa_cpu "$cpu_node")
        [[ -z "$cpu" ]] && continue

        for mem_node in $(seq 0 $((NUM_NUMA - 1))); do
            local locality="local"
            [[ "$cpu_node" != "$mem_node" ]] && locality="remote"
            run_test "n${cpu_node}c_m${mem_node}mem_${locality}" \
                -c "$cpu" -B "$mem_node" "${common[@]}"
        done
    done

    # NUMA × multi-core
    for n in 2 4 8; do
        [[ $n -gt $NUM_PHYSICAL ]] && continue
        local per_node=$((n / NUM_NUMA))
        [[ $per_node -lt 1 ]] && per_node=1

        for mem_node in $(seq 0 $((NUM_NUMA - 1))); do
            run_test "mc${n}_B${mem_node}" -N "$n" -B "$mem_node" "${common[@]}"
        done
    done
}

suite_stress_comparison() {
    log_suite "Suite F: Stress-NG Style Comparison"
    log_info "Comparing different memory access patterns at scale"

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES" "${STATS_FLAGS[@]}")

    # Half-system with all access modes
    local half=$((NUM_PHYSICAL / 2))
    run_test "half_s1"  -N "$half" -s 1  "${common[@]}"
    run_test "half_s8"  -N "$half" -s 8  "${common[@]}"
    run_test "half_s64" -N "$half" -s 64 "${common[@]}"
    run_test "half_R"   -N "$half" -R    "${common[@]}"
    run_test "half_f"   -N "$half" -f    "${common[@]}"

    # Full system with key modes
    run_test "full_s8"  -N "$NUM_PHYSICAL" -s 8  "${common[@]}"
    run_test "full_R"   -N "$NUM_PHYSICAL" -R    "${common[@]}"
}

# ============================================================================
# Report Generation
# ============================================================================

generate_full_report() {
    log_suite "Generating Full Analysis Report"

    local report="$OUTPUT_DIR/FULL_REPORT.txt"

    {
        echo "================================================================"
        echo "  memfreq_bench Full Sweep Analysis"
        echo "================================================================"
        echo ""
        echo "Timestamp : $(date -Iseconds)"
        echo "Hostname  : $(hostname)"
        echo "Kernel    : $(uname -r)"
        echo "Topology  : ${NUM_PHYSICAL}P/${NUM_LOGICAL}T, SMT=${NUM_SMT}, NUMA=${NUM_NUMA}"
        echo "Frequency : ${FMIN_MHZ} – ${FMAX_MHZ} MHz"
        echo "Duration  : ${TEST_DURATION}s × ${TEST_SAMPLES} samples"
        echo ""
        echo "Results: ${TESTS_RUN} tests (${TESTS_OK} ok, ${TESTS_FAIL} failed)"
        echo ""

        # ---- Table 1: Stride Grid ----
        echo "================================================================"
        echo "  TABLE 1: Stride Grid — How stride affects sweet spot"
        echo "================================================================"
        echo ""
        printf "%-12s %10s %12s %15s %12s %15s %12s\n" \
            "Test" "Max MB/s" "Stride SP" "Stride 95% CI" "Chase SP" "Chase 95% CI" "Plateau BP"
        printf "%-12s %10s %12s %15s %12s %15s %12s\n" \
            "----" "--------" "---------" "---------------" "---------" "---------------" "-----------"

        for stride in 1 2 4 8 16 32 64; do
            local file="$OUTPUT_DIR/s${stride}.txt"
            [[ ! -f "$file" ]] && continue
            local data
            data=$(extract_data "$file")
            local name mbs s_sp c_sp s_lo s_hi c_lo c_hi bp
            name=$(echo "$data"  | cut -d'|' -f1)
            mbs=$(echo "$data"   | cut -d'|' -f3)
            s_sp=$(echo "$data"  | cut -d'|' -f4)
            c_sp=$(echo "$data"  | cut -d'|' -f5)
            s_lo=$(echo "$data"  | cut -d'|' -f6)
            s_hi=$(echo "$data"  | cut -d'|' -f7)
            c_lo=$(echo "$data"  | cut -d'|' -f8)
            c_hi=$(echo "$data"  | cut -d'|' -f9)
            bp=$(echo "$data"    | cut -d'|' -f10)
            printf "%-12s %10s %12s %15s %12s %15s %12s\n" \
                "$name" "$mbs" "$s_sp" "${s_lo}-${s_hi}" "$c_sp" "${c_lo}-${c_hi}" "$bp"
        done

        echo ""
        echo "  Analysis:"
        echo "    stride=1:  HW prefetcher effective → higher bandwidth → freq-sensitive"
        echo "    stride=8:  1 cache line/access → realistic memory-bound"
        echo "    stride=64: Extreme miss → pure DRAM latency → freq-insensitive"
        echo "    Gap between stride=1 and stride=64 sweet spots shows"
        echo "    prefetcher's contribution to frequency sensitivity."
        echo "    CI = 95% bootstrap range on the 95% sweet spot (wider = noisier)"
        echo "    BP  = plateau breakpoint MHz (elbow where DRAM starts binding)"
        echo ""

        # ---- Table 2: Multi-Core Sweep ----
        echo "================================================================"
        echo "  TABLE 2: Multi-Core — How core count affects bandwidth"
        echo "================================================================"
        echo ""
        printf "%-12s %10s %10s %12s %15s %12s %15s %12s\n" \
            "Cores" "Max MB/s" "MB/s/core" "Stride SP" "Stride 95% CI" "Chase SP" "Chase 95% CI" "Plateau BP"
        printf "%-12s %10s %10s %12s %15s %12s %15s %12s\n" \
            "-----" "--------" "---------" "---------" "---------------" "---------" "---------------" "-----------"

        local -a mc_counts=(1 2 4 8 12 16 24 32 48 64 96)
        for n in "${mc_counts[@]}"; do
            local file="$OUTPUT_DIR/mc${n}.txt"
            [[ ! -f "$file" ]] && continue
            local data
            data=$(extract_data "$file")
            local mbs s_sp c_sp s_lo s_hi c_lo c_hi bp mbs_per_core
            mbs=$(echo "$data"  | cut -d'|' -f3)
            s_sp=$(echo "$data" | cut -d'|' -f4)
            c_sp=$(echo "$data" | cut -d'|' -f5)
            s_lo=$(echo "$data" | cut -d'|' -f6)
            s_hi=$(echo "$data" | cut -d'|' -f7)
            c_lo=$(echo "$data" | cut -d'|' -f8)
            c_hi=$(echo "$data" | cut -d'|' -f9)
            bp=$(echo "$data"   | cut -d'|' -f10)
            if [[ "$mbs" != "?" && "$mbs" != "" ]]; then
                mbs_per_core=$(awk "BEGIN {printf \"%.1f\", $mbs / $n}")
            else
                mbs_per_core="?"
            fi
            printf "%-12s %10s %10s %12s %15s %12s %15s %12s\n" \
                "${n} cores" "$mbs" "$mbs_per_core" "$s_sp" "${s_lo}-${s_hi}" \
                "$c_sp" "${c_lo}-${c_hi}" "$bp"
        done

        echo ""
        echo "  Analysis:"
        echo "    MB/s should increase linearly until MC bandwidth saturates."
        echo "    MB/s/core should DECREASE as cores compete for bandwidth."
        echo "    Sweet spot should INCREASE with core count (more BW pressure)."
        echo "    The knee point = optimal core count for this memory subsystem."
        echo "    CI widens as cores compete (more variance under load)."
        echo ""

        # ---- Table 3: NUMA Matrix ----
        if [[ $NUM_NUMA -ge 2 ]]; then
            echo "================================================================"
            echo "  TABLE 3: NUMA Matrix — CPU node vs Memory node"
            echo "================================================================"
            echo ""

            for cpu_node in $(seq 0 $((NUM_NUMA - 1))); do
                for mem_node in $(seq 0 $((NUM_NUMA - 1))); do
                    local locality="LOCAL"
                    [[ "$cpu_node" != "$mem_node" ]] && locality="REMOTE"
                    local file="$OUTPUT_DIR/n${cpu_node}c_m${mem_node}mem_${locality,,}.txt"
                    [[ ! -f "$file" ]] && continue
                    local data
                    data=$(extract_data "$file")
                    local mbs s_sp c_sp s_lo s_hi c_lo c_hi bp
                    mbs=$(echo "$data"  | cut -d'|' -f3)
                    s_sp=$(echo "$data" | cut -d'|' -f4)
                    c_sp=$(echo "$data" | cut -d'|' -f5)
                    s_lo=$(echo "$data" | cut -d'|' -f6)
                    s_hi=$(echo "$data" | cut -d'|' -f7)
                    c_lo=$(echo "$data" | cut -d'|' -f8)
                    c_hi=$(echo "$data" | cut -d'|' -f9)
                    bp=$(echo "$data"   | cut -d'|' -f10)
                    printf "  CPU node %d → Mem node %d (%s): %s MB/s, stride=%s [CI %s-%s], chase=%s [CI %s-%s], BP=%s\n" \
                        "$cpu_node" "$mem_node" "$locality" "$mbs" \
                        "$s_sp" "$s_lo" "$s_hi" "$c_sp" "$c_lo" "$c_hi" "$bp"
                done
            done

            echo ""
            echo "  Analysis:"
            echo "    Local should have lower latency (higher chase Mops)."
            echo "    Remote adds interconnect latency (~50-150ns extra)."
            echo "    If remote sweet spot is LOWER → latency dominates (less freq-sensitive)."
            echo "    If remote sweet spot is HIGHER → BW dominates (interconnect bottleneck)."
            echo ""
        fi

        # ---- Table 4: Mode Combinations ----
        echo "================================================================"
        echo "  TABLE 4: Access Mode × Scale Combinations"
        echo "================================================================"
        echo ""
        printf "%-16s %10s %12s %15s %12s %15s %12s\n" \
            "Test" "Max MB/s" "Stride SP" "Stride 95% CI" "Chase SP" "Chase 95% CI" "Plateau BP"
        printf "%-16s %10s %12s %15s %12s %15s %12s\n" \
            "----" "--------" "---------" "---------------" "---------" "---------------" "-----------"

        for file in "$OUTPUT_DIR"/mc*_s*.txt "$OUTPUT_DIR"/mc*_R.txt "$OUTPUT_DIR"/mc*_f.txt \
                    "$OUTPUT_DIR"/half_*.txt "$OUTPUT_DIR"/full_*.txt; do
            [[ ! -f "$file" ]] && continue
            local data
            data=$(extract_data "$file")
            local name mbs s_sp c_sp s_lo s_hi c_lo c_hi bp
            name=$(echo "$data"  | cut -d'|' -f1)
            mbs=$(echo "$data"   | cut -d'|' -f3)
            s_sp=$(echo "$data"  | cut -d'|' -f4)
            c_sp=$(echo "$data"  | cut -d'|' -f5)
            s_lo=$(echo "$data"  | cut -d'|' -f6)
            s_hi=$(echo "$data"  | cut -d'|' -f7)
            c_lo=$(echo "$data"  | cut -d'|' -f8)
            c_hi=$(echo "$data"  | cut -d'|' -f9)
            bp=$(echo "$data"    | cut -d'|' -f10)
            printf "%-16s %10s %12s %15s %12s %15s %12s\n" \
                "$name" "$mbs" "$s_sp" "${s_lo}-${s_hi}" "$c_sp" "${c_lo}-${c_hi}" "$bp"
        done

        echo ""

        # ---- Table 5: Threshold Sensitivity ----
        # Per-workload sweet spot at each threshold in $SENS_THRESHOLDS. Two
        # sub-tables: stride (always run) and chase (skipped if chase
        # workload was disabled, but our suites always run chase).
        echo "================================================================"
        echo "  TABLE 5: Threshold Sensitivity — sweet spot at each threshold"
        echo "  (How aggressive can the 95% threshold be pushed? Lower = more savings.)"
        echo "================================================================"
        echo ""

        # Build the @threshold column count from $SENS_THRESHOLDS so the
        # number of columns follows whatever the user configured.
        local IFS=','
        local -a ts
        read -ra ts <<< "$SENS_THRESHOLDS"

        for workload in stride chase; do
            echo "  --- $workload workload ---"
            # Header row: "Test" in 12-char slot, then one @threshold
            # per column in 7-char slot (fits 4-digit MHz or "—").
            printf "  %-12s" "Test"
            for t in "${ts[@]}"; do printf "  %-7s" "@${t}"; done
            echo
            # Separator row, same column widths.
            printf "  %-12s" "----"
            for t in "${ts[@]}"; do printf "  %-7s" "-------"; done
            echo

            # Iterate over all .txt files; print sensitivity for any that have it
            for file in "$OUTPUT_DIR"/*.txt; do
                local bn
                bn=$(basename "$file" .txt)
                [[ "$bn" == "SUMMARY" || "$bn" == "COMPARISON" || "$bn" == "FULL_REPORT" ]] && continue
                local sens
                sens=$(extract_sensitivity "$file" "$workload")
                [[ -z "$sens" ]] && continue
                # Skip if all values are em-dashes (workload didn't run or no plateau)
                [[ "$sens" == "$(printf '—%.0s' $(seq 1 ${#ts[@]}))" ]] && continue
                local IFS='|'
                local -a vs
                read -ra vs <<< "$sens"
                printf "  %-12s" "$bn"
                for v in "${vs[@]}"; do printf "  %-7s" "$v"; done
                echo
            done
            echo ""
        done

        # ---- Summary: DVFS Governor Recommendations ----
        echo "================================================================"
        echo "  DVFS GOVERNOR RECOMMENDATIONS"
        echo "================================================================"
        echo ""

        # Find single-core stride8 sweet spot
        local sc_sweet
        sc_sweet=$(grep "stride.*sweet spot" "$OUTPUT_DIR/s8.txt" 2>/dev/null | \
                   grep -oP '\d+ MHz' | head -1 | awk '{print $1}' || true)
        # Find mc4 stride8 sweet spot
        local mc4_sweet
        mc4_sweet=$(grep "stride.*sweet spot" "$OUTPUT_DIR/mc4.txt" 2>/dev/null | \
                    grep -oP '\d+ MHz' | head -1 | awk '{print $1}' || true)
        # Find half-system sweet spot
        local half_sweet
        half_sweet=$(grep "stride.*sweet spot" "$OUTPUT_DIR/half_s8.txt" 2>/dev/null | \
                     grep -oP '\d+ MHz' | head -1 | awk '{print $1}' || true)

        echo "  Based on test results:"
        echo ""
        if [[ -n "$sc_sweet" ]]; then
            echo "  Latency-bound workloads (single task, DB queries, compilers):"
            echo "    → Safe frequency floor: $sc_sweet MHz"
            echo "    → Below this, DRAM latency dominates and freq barely matters"
            echo ""
        fi
        if [[ -n "$mc4_sweet" ]]; then
            echo "  Moderate multi-task (4 cores, web server, build server):"
            echo "    → Safe frequency floor: $mc4_sweet MHz"
            echo "    → Memory controller under moderate pressure"
            echo ""
        fi
        if [[ -n "$half_sweet" ]]; then
            echo "  Heavy multi-task ($(( NUM_PHYSICAL / 2 )) cores, HPC, AI training):"
            echo "    → Safe frequency floor: $half_sweet MHz"
            echo "    → Memory controller near saturation"
            echo ""
        fi
        echo "  Governor strategy:"
        echo "    - If compute_ratio > 0.7: use full frequency (compute-bound)"
        echo "    - If mem_ratio > 0.7 and cores_active ≤ 4: drop to $sc_sweet MHz"
        echo "    - If mem_ratio > 0.7 and cores_active > 4: drop to $mc4_sweet MHz"
        echo "    - If both high: interpolate between compute and memory sweet spots"
        echo ""
        echo "================================================================"

    } > "$report"

    log_success "Full report: $report"
    echo ""
    cat "$report"

    # Also generate CSV for spreadsheet analysis
    local csv="$OUTPUT_DIR/data.csv"
    {
        # Build sensitivity column header from $SENS_THRESHOLDS.
        local IFS=','
        local -a ts
        read -ra ts <<< "$SENS_THRESHOLDS"
        local sens_hdr=""
        for t in "${ts[@]}"; do sens_hdr+=",stride_at_${t}"; done
        for t in "${ts[@]}"; do sens_hdr+=",chase_at_${t}"; done

        echo "test,max_MHz,max_MBs,stride_sweet_MHz,chase_sweet_MHz,stride_ci_low_MHz,stride_ci_high_MHz,chase_ci_low_MHz,chase_ci_high_MHz,stride_plateau_bp_MHz,chase_plateau_bp_MHz${sens_hdr}"
        for file in "$OUTPUT_DIR"/*.txt; do
            local bn
            bn=$(basename "$file" .txt)
            [[ "$bn" == "SUMMARY" || "$bn" == "COMPARISON" || "$bn" == "FULL_REPORT" ]] && continue
            local data
            data=$(extract_data "$file")
            local name max_f mbs s_sp c_sp s_lo s_hi c_lo c_hi bp_s bp_c
            name=$(echo "$data"  | cut -d'|' -f1)
            max_f=$(echo "$data" | cut -d'|' -f2)
            mbs=$(echo "$data"   | cut -d'|' -f3)
            s_sp=$(echo "$data"  | cut -d'|' -f4)
            c_sp=$(echo "$data"  | cut -d'|' -f5)
            s_lo=$(echo "$data"  | cut -d'|' -f6)
            s_hi=$(echo "$data"  | cut -d'|' -f7)
            c_lo=$(echo "$data"  | cut -d'|' -f8)
            c_hi=$(echo "$data"  | cut -d'|' -f9)
            bp_s=$(echo "$data"  | cut -d'|' -f10)
            bp_c=$(echo "$data"  | cut -d'|' -f11)

            # Per-threshold sensitivity for stride + chase (pipe-separated,
            # one value per threshold in $SENS_THRESHOLDS order)
            local s_sens c_sens
            s_sens=$(extract_sensitivity "$file" "stride")
            c_sens=$(extract_sensitivity "$file" "chase")
            local s_vals="" c_vals=""
            if [[ -n "$s_sens" ]]; then
                local IFS='|'
                local -a sa
                read -ra sa <<< "$s_sens"
                for v in "${sa[@]}"; do s_vals+=",${v}"; done
            else
                for _ in "${ts[@]}"; do s_vals+=","; done
            fi
            if [[ -n "$c_sens" ]]; then
                local IFS='|'
                local -a ca
                read -ra ca <<< "$c_sens"
                for v in "${ca[@]}"; do c_vals+=",${v}"; done
            else
                for _ in "${ts[@]}"; do c_vals+=","; done
            fi

            echo "${name},${max_f},${mbs},${s_sp},${c_sp},${s_lo},${s_hi},${c_lo},${c_hi},${bp_s},${bp_c}${s_vals}${c_vals}"
        done
    } > "$csv"
    log_success "CSV data: $csv (includes CI, plateau BP, and sensitivity columns)"

    # Per-test raw data CSV (all frequency points)
    local raw_csv="$OUTPUT_DIR/raw_data.csv"
    {
        echo "test,target_MHz,actual_MHz,stride_Mops,stride_MBs,stride_pct,chase_Mops,chase_pct,compute_Mops,compute_pct"
        for file in "$OUTPUT_DIR"/*.txt; do
            local bn
            bn=$(basename "$file" .txt)
            [[ "$bn" == "SUMMARY" || "$bn" == "COMPARISON" || "$bn" == "FULL_REPORT" ]] && continue
            while IFS= read -r line; do
                echo "${bn},${line}" | tr '\t' ','
            done < <(grep -E "^[0-9]" "$file" 2>/dev/null || true)
        done
    } > "$raw_csv"
    log_success "Raw data CSV: $raw_csv (all frequency points, importable to spreadsheet)"
}

# ============================================================================
# Help
# ============================================================================

show_help() {
    cat << 'EOF'
Usage: sudo ./run_full_sweep.sh [OPTIONS]

Exhaustive memfreq_bench sweep for deep analysis.

Options:
  --quick      Run a reduced subset (~30 min instead of ~3-4 hours)
  --yes        Skip confirmation prompt
  --duration N Seconds per test point (default: 5)
  --samples N  Samples per point (default: 5)
  --cpu N      Pin single-core tests to CPU N (default: 0)
  --output DIR Output directory
  --suite LIST Comma-separated suite numbers to run, e.g. 1,3,5 (default: all)
  --resume     Skip tests whose output file already has a sweet-spot line
  --help       Show this help

Environment:
  SENS_THRESHOLDS  Comma-separated list of thresholds for the per-test
                   sensitivity sweep (default: 0.80,0.90,0.95,0.99).
                   Always-on flag: -r (raw samples → bootstrap CI).

Test Suites (--suite N picks one or more):
  1  Stride grid (1,2,4,8,16,32,64) — 7 tests
  2  Random + Flush + RandomFlush — 3 tests
  3  Multi-core sweep (1,2,4,8,12,16,24,32,48,64,96) — 11 tests
  4  Multi-core × access modes — 14 tests
  5  Full NUMA matrix + multi-core NUMA — ~10 tests
  6  Half/full system with all modes — 7 tests

Output:
  FULL_REPORT.txt  — Analysis tables + DVFS recommendations
  data.csv         — Per-test summary (importable to spreadsheet)
  raw_data.csv     — All frequency points from all tests
  *.txt            — Individual test output files (include raw_samples,
                     sweet-spot CI, sensitivity, plateau blocks)
EOF
}

# ============================================================================
# Main
# ============================================================================

main() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --quick)    QUICK_MODE=1; shift ;;
            --yes)      NO_PROMPT=1; shift ;;
            --duration) TEST_DURATION="$2"; shift 2 ;;
            --samples)  TEST_SAMPLES="$2"; shift 2 ;;
            --cpu)      CPU_PIN="$2"; shift 2 ;;
            --output)   OUTPUT_DIR="$2"; shift 2 ;;
            --suite)    SELECTED_SUITES="$2"; shift 2 ;;
            --resume)   RESUME_MODE=1; shift ;;
            --help|-h)  show_help; exit 0 ;;
            *)          log_error "Unknown option: $1"; show_help; exit 1 ;;
        esac
    done

    # Pre-flight
    if [[ $EUID -ne 0 ]]; then
        log_error "Must run as root (sudo)"; exit 1
    fi
    if [[ ! -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
        log_error "cpufreq not available"; exit 1
    fi
    if [[ ! -x "$BENCH" ]]; then
        log_error "memfreq_bench not found at $BENCH"
        log_info "Run: cd $SCRIPT_DIR && make"
        exit 1
    fi

    detect_topology

    if [[ -z "$OUTPUT_DIR" ]]; then
        OUTPUT_DIR="${SCRIPT_DIR}/full_sweep_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "$OUTPUT_DIR"

    # In quick mode, reduce parameters
    if [[ $QUICK_MODE -eq 1 ]]; then
        TEST_DURATION=2
        TEST_SAMPLES=2
        log_warn "Quick mode: reduced duration=${TEST_DURATION}s, samples=${TEST_SAMPLES}"
    fi

    # Estimate
    local est_tests
    if [[ $QUICK_MODE -eq 1 ]]; then
        est_tests=15  # reduced subset
    else
        est_tests=52  # full sweep
    fi
    local est_min=$(( est_tests * TEST_DURATION * 4 / 60 ))

    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║  memfreq_bench Full Sweep                    ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "Tests    : ~$est_tests"
    log_info "Duration : ~${est_min} minutes"
    log_info "Output   : $OUTPUT_DIR"
    echo ""

    if [[ $NO_PROMPT -eq 0 ]]; then
        read -r -p "Press Enter to start (Ctrl+C to cancel, --yes to skip)... " < /dev/tty
    fi

    local start_time
    start_time=$(date +%s)

    # Run suites
    if [[ $QUICK_MODE -eq 1 ]]; then
        # Quick: stride grid + mc sweep + NUMA
        should_run_suite 1 && suite_stride_grid
        should_run_suite 3 && suite_multicore_sweep
        should_run_suite 5 && [[ $NUM_NUMA -ge 2 ]] && suite_numa_matrix
    else
        should_run_suite 1 && suite_stride_grid
        should_run_suite 2 && suite_random_flush
        should_run_suite 3 && suite_multicore_sweep
        should_run_suite 4 && suite_multicore_modes
        should_run_suite 5 && [[ $NUM_NUMA -ge 2 ]] && suite_numa_matrix
        should_run_suite 6 && suite_stress_comparison
    fi

    local end_time
    end_time=$(date +%s)
    local elapsed=$(( end_time - start_time ))

    # Generate reports
    generate_full_report

    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  Full Sweep Complete                         ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════╝${NC}"
    echo ""
    log_info "Elapsed : $((elapsed / 60))m $((elapsed % 60))s"
    log_info "Tests   : $TESTS_RUN run, $TESTS_OK ok, $TESTS_FAIL failed"
    log_info "Output  : $OUTPUT_DIR/"
    echo ""
    log_info "Key files:"
    log_info "  FULL_REPORT.txt  — analysis tables + DVFS recommendations"
    log_info "  data.csv         — per-test summary"
    log_info "  raw_data.csv     — all frequency points (for spreadsheet)"
    echo ""
}

main "$@"
