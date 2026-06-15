#!/bin/bash
#
# run_all_tests.sh — One-click comprehensive memfreq_bench test suite
#
# Automatically runs all test combinations:
#   1. Single-core latency-bound (baseline)
#   2. Multi-core bandwidth saturation (various core counts)
#   3. Different stride patterns
#   4. Random permutation (Fisher-Yates)
#   5. Cache flush (forced L3 miss)
#   6. NUMA binding (local vs remote)
#   7. Cache hierarchy sweep (½×L2, 2×L2, ½×L3, 2×L3, 4×L3)
#
# Usage:
#   sudo ./run_all_tests.sh              # Run all tests
#   sudo ./run_all_tests.sh --quick      # Quick mode (1s, 1 sample)
#   sudo ./run_all_tests.sh --yes        # Skip confirmation prompt
#   sudo ./run_all_tests.sh --suite 1,4  # Run only suites 1 and 4
#   sudo ./run_all_tests.sh --help       # Show options
#

set -uo pipefail
# Note: -e removed because grep returning no match would kill the script.
# We handle errors explicitly.

# ============================================================================
# Configuration
# ============================================================================

QUICK_MODE=0
NO_PROMPT=0
OUTPUT_DIR=""
TEST_DURATION=3
TEST_SAMPLES=3
CPU_PIN=0
MAX_MULTICORE=0       # 0 = auto (half physical cores)
SUITES=""             # empty = run all

# Colors (default to empty so set -u doesn't crash when stdout is not a TTY)
RED='' GREEN='' YELLOW='' BLUE='' CYAN='' NC=''
if [[ -t 1 ]]; then
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color
fi

# Resolve script directory (so we can find memfreq_bench regardless of cwd)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="${SCRIPT_DIR}/memfreq_bench"

# Test counters
TESTS_RUN=0
TESTS_OK=0
TESTS_FAIL=0

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
    echo -e "${GREEN}[ OK]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERR]${NC} $*" >&2
}

log_suite() {
    echo -e "\n${CYAN}━━━ $* ━━━${NC}\n"
}

check_numeric() {
    if ! [[ "$1" =~ ^[0-9]+$ ]]; then
        echo "ERROR: Expected a number, got: $1" >&2
        exit 1
    fi
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

check_cpufreq() {
    if [[ ! -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
        log_error "cpufreq not available on cpu0"
        log_info "  Check: ls /sys/devices/system/cpu/cpu0/cpufreq/"
        log_info "  Ensure a cpufreq driver is loaded (e.g. cppc_cpufreq)"
        exit 1
    fi

    # Check if we can actually set frequency
    if [[ ! -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq ]]; then
        log_error "Cannot write to cpufreq sysfs (need root)"
        exit 1
    fi

    log_success "cpufreq available"
}

should_run_suite() {
    local suite_num="$1"
    if [[ -z "$SUITES" ]]; then
        return 0  # run all
    fi
    echo "$SUITES" | grep -q "\b${suite_num}\b" 2>/dev/null
}

detect_topology() {
    log_info "Detecting system topology..."

    # CPU count — use lscpu summary for accuracy across sockets
    local cores_per_socket sockets
    cores_per_socket=$(lscpu | grep "Core(s) per socket:" | awk '{print $NF}')
    sockets=$(lscpu | grep "Socket(s):" | awk '{print $NF}')
    if [[ -n "$cores_per_socket" && -n "$sockets" ]]; then
        NUM_PHYSICAL_CORES=$((cores_per_socket * sockets))
    else
        # Fallback
        NUM_PHYSICAL_CORES=$(nproc)
    fi

    NUM_LOGICAL_CORES=$(nproc)
    if [[ $NUM_PHYSICAL_CORES -gt 0 ]]; then
        NUM_SMT_THREADS=$((NUM_LOGICAL_CORES / NUM_PHYSICAL_CORES))
    else
        NUM_SMT_THREADS=1
        NUM_PHYSICAL_CORES=$NUM_LOGICAL_CORES
    fi

    # NUMA nodes
    if command -v numactl &>/dev/null; then
        NUM_NUMA_NODES=$(numactl -H 2>/dev/null | grep "available:" | awk '{print $2}' || true)
    fi
    NUM_NUMA_NODES=${NUM_NUMA_NODES:-1}

    # Frequency range
    local fmin_khz fmax_khz
    fmin_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq 2>/dev/null || echo 0)
    fmax_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo 0)
    FREQ_MIN_MHZ=$((fmin_khz / 1000))
    FREQ_MAX_MHZ=$((fmax_khz / 1000))

    # Max multi-core test count
    if [[ $MAX_MULTICORE -le 0 ]]; then
        MAX_MULTICORE=$((NUM_PHYSICAL_CORES / 2))
        if [[ $MAX_MULTICORE -lt 4 ]]; then
            MAX_MULTICORE=$NUM_PHYSICAL_CORES
        fi
    fi

    # Cache hierarchy (L1d, L2, L3) — read from sysfs
    CACHE_L1D_MB=0
    CACHE_L2_MB=0
    CACHE_L3_MB=0
    for idx in $(seq 0 9); do
        local level_file="/sys/devices/system/cpu/cpu0/cache/index${idx}/level"
        local size_file="/sys/devices/system/cpu/cpu0/cache/index${idx}/size"
        local type_file="/sys/devices/system/cpu/cpu0/cache/index${idx}/type"
        [[ -r "$level_file" && -r "$size_file" ]] || break
        local level size_str size_mb
        level=$(cat "$level_file")
        size_str=$(cat "$size_file")
        # Parse "128K", "1M", "32M" etc.
        local num unit
        num=$(echo "$size_str" | sed 's/[^0-9]//g')
        unit=$(echo "$size_str" | sed 's/[0-9]//g')
        if [[ "$unit" == "K" || "$unit" == "k" ]]; then
            size_mb=0  # < 1MB, treat as 0 for our purposes
        elif [[ "$unit" == "M" || "$unit" == "m" ]]; then
            size_mb=$num
        else
            size_mb=0
        fi
        case $level in
            1) [[ "$size_mb" -gt "$CACHE_L1D_MB" ]] && CACHE_L1D_MB=$size_mb ;;
            2) [[ "$size_mb" -gt "$CACHE_L2_MB" ]] && CACHE_L2_MB=$size_mb ;;
            3) [[ "$size_mb" -gt "$CACHE_L3_MB" ]] && CACHE_L3_MB=$size_mb ;;
        esac
    done

    log_success "Topology detected:"
    log_info "  Physical cores : $NUM_PHYSICAL_CORES (${cores_per_socket:-?}/socket × ${sockets:-?})"
    log_info "  SMT threads    : $NUM_SMT_THREADS (total logical: $NUM_LOGICAL_CORES)"
    log_info "  NUMA nodes     : $NUM_NUMA_NODES"
    log_info "  Cache          : L1d=${CACHE_L1D_MB}MB, L2=${CACHE_L2_MB}MB, L3=${CACHE_L3_MB}MB"
    log_info "  Frequency range: ${FREQ_MIN_MHZ} – ${FREQ_MAX_MHZ} MHz"
    log_info "  Max multi-core : $MAX_MULTICORE cores"
}

create_output_dir() {
    if [[ -z "$OUTPUT_DIR" ]]; then
        OUTPUT_DIR="${SCRIPT_DIR}/output/results_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "${SCRIPT_DIR}/output"
    mkdir -p "$OUTPUT_DIR"
    log_success "Output directory: $OUTPUT_DIR"
}

run_test() {
    local test_name="$1"
    shift
    local output_file="$OUTPUT_DIR/${test_name}.txt"

    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "[$TESTS_RUN] $test_name: $BENCH $*"

    {
        echo "# Test: $test_name"
        echo "# Command: $BENCH $*"
        echo "# Timestamp: $(date -Iseconds)"
        echo "#"
    } > "$output_file"

    if "$BENCH" "$@" >> "$output_file" 2>&1; then
        TESTS_OK=$((TESTS_OK + 1))
        # Extract sweet spot from output
        local sweet
        sweet=$(grep "sweet spot" "$output_file" | head -1 | sed 's/^#\s*//' || true)
        if [[ -n "$sweet" ]]; then
            log_success "  ✓ $test_name: $sweet"
        else
            log_success "  ✓ $test_name"
        fi
    else
        TESTS_FAIL=$((TESTS_FAIL + 1))
        log_warn "  ✗ $test_name failed (see $output_file)"
    fi
}

# ============================================================================
# Test Suites
# ============================================================================

test_single_core_baseline() {
    should_run_suite 1 || return 0
    log_suite "Suite 1: Single-Core Baseline"

    # Use -A for L3-aware auto-sizing (more accurate than shell calculation)
    local common=(-c "$CPU_PIN" -A -t "$TEST_DURATION" -n "$TEST_SAMPLES")

    # Stride = 8 (cache line, default)
    run_test "stride8" "${common[@]}" -s 8

    # Stride = 1 (prefetcher-friendly)
    run_test "stride1" "${common[@]}" -s 1

    # Stride = 64 (extreme memory-bound)
    run_test "stride64" "${common[@]}" -s 64
}

test_random_permutation() {
    should_run_suite 2 || return 0
    log_suite "Suite 2: Random Permutation (Fisher-Yates)"

    run_test "random" -c "$CPU_PIN" -A -R \
        -t "$TEST_DURATION" -n "$TEST_SAMPLES"
}

test_cache_flush() {
    should_run_suite 3 || return 0
    log_suite "Suite 3: Cache Flush (Forced L3 Miss)"

    run_test "flush" -c "$CPU_PIN" -A -f \
        -t "$TEST_DURATION" -n "$TEST_SAMPLES"
}

test_multi_core_bandwidth() {
    should_run_suite 4 || return 0
    log_suite "Suite 4: Multi-Core Bandwidth Saturation"

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES")

    # Calculate core counts: 1, 2, 4, ..., up to MAX_MULTICORE
    local -a core_counts=()
    local n=1
    while [[ $n -le $MAX_MULTICORE ]]; do
        core_counts+=($n)
        if [[ $n -lt 4 ]]; then
            n=$((n + 1))
        else
            n=$((n * 2))
        fi
    done
    # Always include MAX_MULTICORE
    local last=${core_counts[-1]:-0}
    if [[ $last -ne $MAX_MULTICORE ]]; then
        core_counts+=($MAX_MULTICORE)
    fi

    log_info "Core counts to test: ${core_counts[*]}"

    for num_cores in "${core_counts[@]}"; do
        [[ $num_cores -gt $NUM_PHYSICAL_CORES ]] && continue
        run_test "mc_${num_cores}" -N "$num_cores" "${common[@]}"
    done
}

test_numa_binding() {
    should_run_suite 5 || return 0
    log_suite "Suite 5: NUMA Binding (Local vs Remote)"

    if [[ $NUM_NUMA_NODES -lt 2 ]]; then
        log_warn "Only 1 NUMA node, skipping NUMA binding tests"
        return 0
    fi

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES")

    for node in $(seq 0 $((NUM_NUMA_NODES - 1))); do
        # Find first CPU on this NUMA node
        local first_cpu
        first_cpu=$(numactl -H 2>/dev/null | grep "node $node cpus:" | \
                    sed 's/node [0-9]* cpus: //' | awk '{print $1}' || true)
        if [[ -z "$first_cpu" ]]; then
            log_warn "No CPUs found on NUMA node $node, skipping"
            continue
        fi

        # Local binding
        run_test "numa${node}_local" -c "$first_cpu" -B "$node" "${common[@]}"

        # Remote binding
        local remote_node=$(((node + 1) % NUM_NUMA_NODES))
        run_test "numa${node}_remote" -c "$first_cpu" -B "$remote_node" "${common[@]}"
    done
}

test_combined_modes() {
    should_run_suite 6 || return 0
    log_suite "Suite 6: Combined Modes"

    local common=(-A -t "$TEST_DURATION" -n "$TEST_SAMPLES")

    # Multi-core + random permutation
    run_test "mc4_random" -N 4 -R "${common[@]}"

    # Multi-core + cache flush
    run_test "mc4_flush" -N 4 -f "${common[@]}"

    # NUMA + flush (if multi-node)
    if [[ $NUM_NUMA_NODES -ge 2 ]]; then
        local first_cpu
        first_cpu=$(numactl -H 2>/dev/null | grep "node 0 cpus:" | \
                    sed 's/node [0-9]* cpus: //' | awk '{print $1}' || true)
        if [[ -n "$first_cpu" ]]; then
            run_test "numa0_flush" -c "$first_cpu" -B 0 -f "${common[@]}"
        fi
    fi
}

test_cache_hierarchy() {
    should_run_suite 7 || return 0
    log_suite "Suite 7: Cache Hierarchy Sweep"

    local common=(-c "$CPU_PIN" -t "$TEST_DURATION" -n "$TEST_SAMPLES")

    # Calculate array sizes at different cache boundaries
    # We want: ½×L2, 2×L2, ½×L3, 2×L3, 4×L3
    local -a sizes_mb=()
    local -a labels=()

    # ½× L2 (if L2 detected)
    if [[ $CACHE_L2_MB -gt 0 ]]; then
        local half_l2=$((CACHE_L2_MB / 2))
        if [[ $half_l2 -ge 1 ]]; then
            sizes_mb+=($half_l2)
            labels+=("half_L2_${half_l2}MB")
        fi
    fi

    # 2× L2 (if L2 detected)
    if [[ $CACHE_L2_MB -gt 0 ]]; then
        local double_l2=$((CACHE_L2_MB * 2))
        sizes_mb+=($double_l2)
        labels+=("double_L2_${double_l2}MB")
    fi

    # ½× L3 (if L3 detected)
    if [[ $CACHE_L3_MB -gt 0 ]]; then
        local half_l3=$((CACHE_L3_MB / 2))
        if [[ $half_l3 -ge 1 ]]; then
            sizes_mb+=($half_l3)
            labels+=("half_L3_${half_l3}MB")
        fi
    fi

    # 2× L3 (if L3 detected) — this is what -A would do
    if [[ $CACHE_L3_MB -gt 0 ]]; then
        local double_l3=$((CACHE_L3_MB * 2))
        sizes_mb+=($double_l3)
        labels+=("double_L3_${double_l3}MB")
    fi

    # 4× L3 (if L3 detected) — deep DRAM-bound
    if [[ $CACHE_L3_MB -gt 0 ]]; then
        local quad_l3=$((CACHE_L3_MB * 4))
        sizes_mb+=($quad_l3)
        labels+=("quad_L3_${quad_l3}MB")
    fi

    # Fallback if no cache info
    if [[ ${#sizes_mb[@]} -eq 0 ]]; then
        log_warn "Cache sizes not detected, using default sizes: 4MB, 16MB, 64MB, 256MB, 512MB"
        sizes_mb=(4 16 64 256 512)
        labels=("4MB" "16MB" "64MB" "256MB" "512MB")
    fi

    log_info "Cache hierarchy test sizes: ${sizes_mb[*]} MB"
    log_info "Labels: ${labels[*]}"

    for i in "${!sizes_mb[@]}"; do
        local size=${sizes_mb[$i]}
        local label=${labels[$i]}
        run_test "cache_${label}" -m "$size" "${common[@]}"
    done
}

# ============================================================================
# Summary Generation
# ============================================================================

generate_summary() {
    log_suite "Generating Summary Report"

    local summary_file="$OUTPUT_DIR/SUMMARY.txt"
    local comparison_file="$OUTPUT_DIR/COMPARISON.txt"

    # ---- SUMMARY.txt ----
    {
        echo "========================================"
        echo " memfreq_bench Test Summary"
        echo "========================================"
        echo ""
        echo "Timestamp : $(date -Iseconds)"
        echo "Hostname  : $(hostname)"
        echo "Kernel    : $(uname -r)"
        echo ""
        echo "System Topology:"
        echo "  Physical cores : $NUM_PHYSICAL_CORES"
        echo "  SMT threads    : $NUM_SMT_THREADS"
        echo "  NUMA nodes     : $NUM_NUMA_NODES"
        echo "  Frequency range: ${FREQ_MIN_MHZ} – ${FREQ_MAX_MHZ} MHz"
        echo ""
        echo "Test Configuration:"
        echo "  Duration per test: ${TEST_DURATION}s"
        echo "  Samples per test : $TEST_SAMPLES"
        echo "  Quick mode       : $([[ $QUICK_MODE -eq 1 ]] && echo 'Yes' || echo 'No')"
        echo ""
        echo "Results: $TESTS_RUN tests ($TESTS_OK ok, $TESTS_FAIL failed)"
        echo ""
        echo "========================================"
        echo " Per-Test Results"
        echo "========================================"
        echo ""

        for result_file in "$OUTPUT_DIR"/*.txt; do
            local test_name
            test_name=$(basename "$result_file" .txt)
            [[ "$test_name" == "SUMMARY" || "$test_name" == "COMPARISON" ]] && continue

            echo "--- $test_name ---"

            # Extract sweet spot (grep || true to avoid set -e exit)
            local sweet
            sweet=$(grep "sweet spot" "$result_file" 2>/dev/null | head -2 || true)
            if [[ -n "$sweet" ]]; then
                echo "$sweet" | sed 's/^#\s*/  /'
            else
                echo "  (no sweet spot or test failed)"
            fi

            # Extract max throughput line (highest freq = last data row)
            local max_line
            max_line=$(grep -E "^[0-9]" "$result_file" 2>/dev/null | tail -1 || true)
            if [[ -n "$max_line" ]]; then
                local freq mops mbs
                freq=$(echo "$max_line" | awk '{print $1}')
                mops=$(echo "$max_line" | awk '{print $3}')
                mbs=$(echo "$max_line" | awk '{print $4}')
                echo "  Max throughput: ${mbs:-?} MB/s (${mops:-?} Mops) @ ${freq:-?} MHz"
            fi
            echo ""
        done

    } > "$summary_file"

    log_success "Summary: $summary_file"

    # ---- COMPARISON.txt — cross-test sweet spot table ----
    {
        echo "========================================"
        echo " Sweet Spot Comparison"
        echo "========================================"
        echo ""
        printf "%-20s %12s %12s %12s\n" "Test" "Stride(MHz)" "Chase(MHz)" "Max MB/s"
        printf "%-20s %12s %12s %12s\n" "----" "-----------" "----------" "--------"

        for result_file in "$OUTPUT_DIR"/*.txt; do
            local test_name
            test_name=$(basename "$result_file" .txt)
            [[ "$test_name" == "SUMMARY" || "$test_name" == "COMPARISON" ]] && continue

            local stride_sweet chase_sweet max_mbs
            stride_sweet=$(grep "stride.*sweet spot" "$result_file" 2>/dev/null | \
                           grep -oE '[0-9]+ MHz' | head -1 | awk '{print $1}' || true)
            chase_sweet=$(grep "chase.*sweet spot" "$result_file" 2>/dev/null | \
                          grep -oE '[0-9]+ MHz' | head -1 | awk '{print $1}' || true)

            local max_line
            max_line=$(grep -E "^[0-9]" "$result_file" 2>/dev/null | tail -1 || true)
            max_mbs=$(echo "$max_line" | awk '{print $4}' || true)

            printf "%-20s %12s %12s %12s\n" \
                "$test_name" \
                "${stride_sweet:-—}" \
                "${chase_sweet:-—}" \
                "${max_mbs:-—}"
        done

        echo ""
        echo "========================================"
        echo " DVFS Governor Guidance"
        echo "========================================"
        echo ""
        echo "Single-core sweet spot (low) → latency-bound workloads"
        echo "  → DVFS can aggressively drop frequency for DB queries, compilers, etc."
        echo ""
        echo "Multi-core sweet spot (higher) → bandwidth-bound workloads"
        echo "  → DVFS should be conservative for HPC, AI training, etc."
        echo ""
        echo "Gap between single vs multi-core sweet spots indicates:"
        echo "  - Small gap  → memory controller not easily saturated"
        echo "  - Large gap  → MC bandwidth is the bottleneck under multi-core load"
        echo ""
        echo "NUMA remote vs local sweet spot:"
        echo "  - Remote sweet spot ≤ local → interconnect latency dominates"
        echo "  - Remote sweet spot > local → interconnect bandwidth dominates"
        echo ""

    } > "$comparison_file"

    log_success "Comparison: $comparison_file"

    # Display comparison on screen
    echo ""
    cat "$comparison_file"
}

# ============================================================================
# Help
# ============================================================================

show_help() {
    cat << 'EOF'
Usage: sudo ./run_all_tests.sh [OPTIONS]

One-click comprehensive memfreq_bench test suite.

Options:
  --quick            Quick mode (1s duration, 1 sample)
  --yes              Skip confirmation prompt (for automation)
  --duration SEC     Test duration in seconds (default: 3)
  --samples N        Number of samples per point (default: 3)
  --cpu N            Pin single-core tests to CPU N (default: 0)
  --max-cores N      Cap multi-core tests at N cores (default: half physical)
  --suite LIST       Run only specific suites, comma-separated (default: all)
  --output DIR       Output directory (default: results_TIMESTAMP)
  --help             Show this help message

Test Suites (use --suite to select):
  1  Single-core baseline (stride=1,8,64)
  2  Random permutation (Fisher-Yates, -R)
  3  Cache flush (forced L3 miss, -f)
  4  Multi-core bandwidth saturation (1,2,4,...,N cores)
  5  NUMA binding (local vs remote, requires ≥2 NUMA nodes)
  6  Combined modes (multi-core+random, multi-core+flush, NUMA+flush)
  7  Cache hierarchy sweep (½×L2, 2×L2, ½×L3, 2×L3, 4×L3)

Examples:
  sudo ./run_all_tests.sh                    # Run everything
  sudo ./run_all_tests.sh --quick            # Quick validation
  sudo ./run_all_tests.sh --yes --quick      # Non-interactive quick run
  sudo ./run_all_tests.sh --suite 1,4        # Only single-core + multi-core
  sudo ./run_all_tests.sh --max-cores 8      # Cap multi-core at 8
  sudo ./run_all_tests.sh --duration 5       # Higher precision (5s per test)
EOF
}

# ============================================================================
# Main
# ============================================================================

main() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --quick)
                QUICK_MODE=1
                TEST_DURATION=1
                TEST_SAMPLES=1
                shift
                ;;
            --yes)
                NO_PROMPT=1
                shift
                ;;
            --duration)
                TEST_DURATION="$2"
                shift 2
                ;;
            --samples)
                TEST_SAMPLES="$2"
                shift 2
                ;;
            --cpu)
                CPU_PIN="$2"
                shift 2
                ;;
            --max-cores)
                MAX_MULTICORE="$2"
                shift 2
                ;;
            --suite)
                SUITES="$2"
                shift 2
                ;;
            --output)
                OUTPUT_DIR="$2"
                shift 2
                ;;
            --help|-h)
                show_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done

    # Pre-flight checks
    check_root
    check_cpufreq

    # Verify memfreq_bench exists and is executable
    if [[ ! -x "$BENCH" ]]; then
        log_error "memfreq_bench not found at: $BENCH"
        log_info "Compile with: cd $SCRIPT_DIR && make"
        exit 1
    fi

    # Detect topology
    detect_topology
    create_output_dir

    # Estimate test count
    local est_tests=0
    if should_run_suite 1; then est_tests=$((est_tests + 3)); fi
    if should_run_suite 2; then est_tests=$((est_tests + 1)); fi
    if should_run_suite 3; then est_tests=$((est_tests + 1)); fi
    if should_run_suite 4; then
        local n=1; while [[ $n -le $MAX_MULTICORE ]]; do
            est_tests=$((est_tests + 1)); n=$((n * 2))
        done
    fi
    if should_run_suite 5 && [[ $NUM_NUMA_NODES -ge 2 ]]; then
        est_tests=$((est_tests + NUM_NUMA_NODES * 2))
    fi
    if should_run_suite 6; then est_tests=$((est_tests + 3)); fi
    if should_run_suite 7; then
        local n=0
        [[ $CACHE_L2_MB -gt 0 ]] && n=$((n + 2))
        [[ $CACHE_L3_MB -gt 0 ]] && n=$((n + 3))
        [[ $n -eq 0 ]] && n=5
        est_tests=$((est_tests + n))
    fi

    local est_secs=$((est_tests * TEST_DURATION * 4))
    local est_min=$((est_secs / 60))

    echo ""
    log_info "=========================================="
    log_info " memfreq_bench Comprehensive Test Suite"
    log_info "=========================================="
    if [[ $QUICK_MODE -eq 1 ]]; then
        log_warn "Quick mode: 1s duration, 1 sample"
    fi
    log_info "Estimated: ~$est_tests tests, ~${est_min} minutes"
    if [[ -n "$SUITES" ]]; then
        log_info "Selected suites: $SUITES"
    fi
    echo ""

    if [[ $NO_PROMPT -eq 0 ]]; then
        read -r -p "Press Enter to continue (Ctrl+C to cancel, or use --yes to skip)..." < /dev/tty
    fi

    # Run test suites
    local start_time
    start_time=$(date +%s)

    test_single_core_baseline
    test_random_permutation
    test_cache_flush
    test_multi_core_bandwidth
    test_numa_binding
    test_combined_modes
    test_cache_hierarchy

    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    # Generate summary
    generate_summary

    # Final report
    echo ""
    log_success "=========================================="
    log_success " All Tests Completed"
    log_success "=========================================="
    log_info "Total time : ${elapsed}s"
    log_info "Tests      : $TESTS_RUN run, $TESTS_OK ok, $TESTS_FAIL failed"
    log_info "Results    : $OUTPUT_DIR/"
    log_info "Summary    : $OUTPUT_DIR/SUMMARY.txt"
    log_info "Comparison : $OUTPUT_DIR/COMPARISON.txt"
    echo ""

    if [[ $TESTS_RUN -eq 0 ]]; then log_warn "no tests selected"; exit 1; fi
    [[ $TESTS_FAIL -gt 0 ]] && exit 1
    exit 0
}

main "$@"
