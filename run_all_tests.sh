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
#
# Usage:
#   sudo ./run_all_tests.sh           # Run all tests
#   sudo ./run_all_tests.sh --quick   # Quick mode (shorter duration)
#   sudo ./run_all_tests.sh --help    # Show options
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

QUICK_MODE=0
OUTPUT_DIR=""
TEST_DURATION=3
TEST_SAMPLES=3
CPU_PIN=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

detect_topology() {
    log_info "Detecting system topology..."
    
    # CPU count (physical cores, not SMT siblings)
    NUM_PHYSICAL_CORES=$(lscpu -p=CPU,CORE | grep -v '^#' | awk -F, '{print $2}' | sort -u | wc -l)
    NUM_LOGICAL_CORES=$(nproc)
    NUM_SMT_THREADS=$((NUM_LOGICAL_CORES / NUM_PHYSICAL_CORES))
    
    # NUMA nodes
    NUM_NUMA_NODES=$(numactl -H 2>/dev/null | grep "available:" | awk '{print $2}' || echo 1)
    
    # L3 cache size (MB) — total across all sharing cores
    # lscpu reports total L3 (e.g. "L3 cache: 273 MB")
    L3_SIZE_MB=$(lscpu | grep "L3 cache" | awk '{print $3}' | sed 's/M//')
    if [[ -z "$L3_SIZE_MB" || "$L3_SIZE_MB" == "0" ]]; then
        # Fallback: per-core slice from sysfs × number of sharing cores
        local slice_bytes=$(cat /sys/devices/system/cpu/cpu0/cache/index3/size 2>/dev/null || echo 0)
        # Handle K suffix
        if [[ "$slice_bytes" == *K ]]; then
            slice_bytes=$(( ${slice_bytes%K} * 1024 ))
        fi
        local shared=$(cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list 2>/dev/null || echo "0")
        # Count CPUs in shared list (handles "0-47" or "0,1,2" etc.)
        local n_sharing=$(echo "$shared" | tr ',' '\n' | while read range; do
            if [[ "$range" == *-* ]]; then
                local lo=${range%-*} hi=${range#*-}
                echo $((hi - lo + 1))
            else
                echo 1
            fi
        done | awk '{s+=$1} END {print s}')
        n_sharing=${n_sharing:-1}
        L3_SIZE_MB=$(( slice_bytes * n_sharing / 1024 / 1024 ))
    fi
    
    # Array size (2x L3)
    ARRAY_SIZE_MB=$((L3_SIZE_MB * 2))
    if [[ $ARRAY_SIZE_MB -lt 128 ]]; then
        ARRAY_SIZE_MB=128
    fi
    
    log_success "Topology detected:"
    log_info "  Physical cores: $NUM_PHYSICAL_CORES"
    log_info "  SMT threads:  $NUM_SMT_THREADS (total logical: $NUM_LOGICAL_CORES)"
    log_info "  NUMA nodes:   $NUM_NUMA_NODES"
    log_info "  L3 cache:     ${L3_SIZE_MB} MB"
    log_info "  Array size:   ${ARRAY_SIZE_MB} MB (2x L3)"
}

create_output_dir() {
    if [[ -z "$OUTPUT_DIR" ]]; then
        OUTPUT_DIR="memfreq_results_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "$OUTPUT_DIR"
    log_success "Output directory: $OUTPUT_DIR"
}

run_test() {
    local test_name="$1"
    shift
    local output_file="$OUTPUT_DIR/${test_name}.txt"
    
    log_info "Running: $test_name"
    echo "Command: ./memfreq_bench $*" >> "$output_file"
    echo "Timestamp: $(date)" >> "$output_file"
    echo "---" >> "$output_file"
    
    if ./memfreq_bench "$@" >> "$output_file" 2>&1; then
        log_success "  ✓ $test_name completed"
    else
        log_warn "  ⚠ $test_name failed (see $output_file)"
    fi
}

# ============================================================================
# Test Suites
# ============================================================================

test_single_core_baseline() {
    log_info "=== Test Suite 1: Single-Core Baseline ==="
    
    # Stride = 8 (cache line)
    run_test "single_stride8" -c "$CPU_PIN" -m "$ARRAY_SIZE_MB" \
        -s 8 -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    
    # Stride = 1 (prefetcher-friendly)
    run_test "single_stride1" -c "$CPU_PIN" -m "$ARRAY_SIZE_MB" \
        -s 1 -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    
    # Stride = 64 (extreme)
    run_test "single_stride64" -c "$CPU_PIN" -m "$ARRAY_SIZE_MB" \
        -s 64 -t "$TEST_DURATION" -n "$TEST_SAMPLES"
}

test_random_permutation() {
    log_info "=== Test Suite 2: Random Permutation (Fisher-Yates) ==="
    
    run_test "random_perm" -c "$CPU_PIN" -m "$ARRAY_SIZE_MB" \
        -R -t "$TEST_DURATION" -n "$TEST_SAMPLES"
}

test_cache_flush() {
    log_info "=== Test Suite 3: Cache Flush (Forced L3 Miss) ==="
    
    run_test "cache_flush" -c "$CPU_PIN" -m "$ARRAY_SIZE_MB" \
        -f -t "$TEST_DURATION" -n "$TEST_SAMPLES"
}

test_multi_core_bandwidth() {
    log_info "=== Test Suite 4: Multi-Core Bandwidth Saturation ==="
    
    # Calculate core counts for testing
    # Test: 1, 2, 4, 8, half, all (up to physical cores)
    local core_counts=(1 2 4 8)
    local half_cores=$((NUM_PHYSICAL_CORES / 2))
    local all_cores=$NUM_PHYSICAL_CORES
    
    # Add half and all if not already in list
    [[ ! " ${core_counts[@]} " =~ " ${half_cores} " ]] && core_counts+=($half_cores)
    [[ ! " ${core_counts[@]} " =~ " ${all_cores} " ]] && core_counts+=($all_cores)
    
    # Remove duplicates and sort
    core_counts=($(echo "${core_counts[@]}" | tr ' ' '\n' | sort -un))
    
    for num_cores in "${core_counts[@]}"; do
        # Skip if more cores than available
        [[ $num_cores -gt $NUM_PHYSICAL_CORES ]] && continue
        
        local test_name="multicore_${num_cores}cores"
        log_info "Running: $test_name"
        
        # Build command with -N for multi-core
        local cmd=(-N "$num_cores" -m "$ARRAY_SIZE_MB" -t "$TEST_DURATION" -n "$TEST_SAMPLES")
        
        run_test "$test_name" "${cmd[@]}"
    done
}

test_numa_binding() {
    log_info "=== Test Suite 5: NUMA Binding (Local vs Remote) ==="
    
    if [[ $NUM_NUMA_NODES -lt 2 ]]; then
        log_warn "Only 1 NUMA node detected, skipping NUMA binding tests"
        return
    fi
    
    # Test on each NUMA node
    for node in $(seq 0 $((NUM_NUMA_NODES - 1))); do
        # Find first CPU on this NUMA node
        local first_cpu=$(numactl -H | grep "node $node cpus:" | awk '{print $4}')
        if [[ -z "$first_cpu" ]]; then
            log_warn "No CPUs found on NUMA node $node"
            continue
        fi
        
        # Test with memory bound to local node
        run_test "numa${node}_local" -c "$first_cpu" -m "$ARRAY_SIZE_MB" \
            -B "$node" -t "$TEST_DURATION" -n "$TEST_SAMPLES"
        
        # Test with memory bound to remote node (different from CPU's node)
        local remote_node=$(((node + 1) % NUM_NUMA_NODES))
        run_test "numa${node}_remote" -c "$first_cpu" -m "$ARRAY_SIZE_MB" \
            -B "$remote_node" -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    done
}

test_combined_modes() {
    log_info "=== Test Suite 6: Combined Modes ==="
    
    # Multi-core + random permutation
    run_test "multicore_random" -N 4 -m "$ARRAY_SIZE_MB" \
        -R -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    
    # Multi-core + cache flush
    run_test "multicore_flush" -N 4 -m "$ARRAY_SIZE_MB" \
        -f -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    
    # NUMA binding + cache flush
    if [[ $NUM_NUMA_NODES -ge 2 ]]; then
        local first_cpu=$(numactl -H | grep "node 0 cpus:" | awk '{print $4}')
        run_test "numa0_flush" -c "$first_cpu" -m "$ARRAY_SIZE_MB" \
            -B 0 -f -t "$TEST_DURATION" -n "$TEST_SAMPLES"
    fi
}

generate_summary() {
    log_info "=== Generating Summary Report ==="
    
    local summary_file="$OUTPUT_DIR/SUMMARY.txt"
    {
        echo "========================================"
        echo " memfreq_bench Test Summary"
        echo "========================================"
        echo ""
        echo "Timestamp: $(date)"
        echo "Hostname: $(hostname)"
        echo ""
        echo "System Topology:"
        echo "  Physical cores: $NUM_PHYSICAL_CORES"
        echo "  SMT threads:    $NUM_SMT_THREADS"
        echo "  NUMA nodes:     $NUM_NUMA_NODES"
        echo "  L3 cache:       ${L3_SIZE_MB} MB"
        echo "  Array size:     ${ARRAY_SIZE_MB} MB"
        echo ""
        echo "Test Configuration:"
        echo "  Duration per test: ${TEST_DURATION}s"
        echo "  Samples per test:  $TEST_SAMPLES"
        echo "  Quick mode:        $([[ $QUICK_MODE -eq 1 ]] && echo 'Yes' || echo 'No')"
        echo ""
        echo "========================================"
        echo " Test Results"
        echo "========================================"
        echo ""
        
        # Extract sweet spots from each test
        for result_file in "$OUTPUT_DIR"/*.txt; do
            local test_name=$(basename "$result_file" .txt)
            [[ "$test_name" == "SUMMARY" ]] && continue
            
            echo "--- $test_name ---"
            
            # Extract sweet spot information
            if grep -q "Sweet spot" "$result_file"; then
                grep "Sweet spot" "$result_file" | sed 's/^/  /'
            else
                echo "  (No sweet spot detected or test failed)"
            fi
            
            # Extract baseline throughput (highest frequency)
            local baseline=$(grep -E "^[0-9]+\s+[0-9]+\." "$result_file" | tail -1)
            if [[ -n "$baseline" ]]; then
                local freq=$(echo "$baseline" | awk '{print $1}')
                local throughput=$(echo "$baseline" | awk '{print $4}')
                echo "  Baseline: ${throughput} MB/s @ ${freq} MHz"
            fi
            
            echo ""
        done
        
        echo "========================================"
        echo " Interpretation Guide"
        echo "========================================"
        echo ""
        echo "1. Single-Core Tests (Stride)"
        echo "   - Latency-bound sweet spot: lowest freq where throughput >= 95% of max"
        echo "   - Stride=8: Cache line access (realistic)"
        echo "   - Stride=1: Sequential (prefetcher-friendly)"
        echo "   - Stride=64: Extreme memory-bound"
        echo ""
        echo "2. Random Permutation"
        echo "   - Tests true random access latency"
        echo "   - Sweet spot should be very low (memory-bound)"
        echo ""
        echo "3. Cache Flush"
        echo "   - Forces L3 miss on every access"
        echo "   - Sweet spot should be low (pure DRAM latency)"
        echo ""
        echo "4. Multi-Core Tests"
        echo "   - Bandwidth-bound sweet spot: where MC saturates"
        echo "   - More cores = higher sweet spot (bandwidth contention)"
        echo ""
        echo "5. NUMA Binding"
        echo "   - Local: Memory on same NUMA node as CPU"
        echo "   - Remote: Memory on different NUMA node"
        echo "   - Remote should show higher latency, possibly different sweet spot"
        echo ""
    } > "$summary_file"
    
    log_success "Summary report: $summary_file"
    
    # Display summary on screen
    echo ""
    cat "$summary_file"
}

show_help() {
    cat << EOF
Usage: sudo $0 [OPTIONS]

One-click comprehensive memfreq_bench test suite.

Options:
  --quick         Quick mode (1s duration, 1 sample)
  --duration SEC  Test duration in seconds (default: 3)
  --samples N     Number of samples (default: 3)
  --cpu N         Pin to CPU N (default: 0)
  --output DIR    Output directory (default: auto-generated)
  --help          Show this help message

Examples:
  sudo $0                          # Run all tests (default settings)
  sudo $0 --quick                  # Quick mode for validation
  sudo $0 --duration 5 --samples 5 # Higher precision
  sudo $0 --output my_results      # Custom output directory

Test Suites:
  1. Single-core baseline (stride=1,8,64)
  2. Random permutation (Fisher-Yates)
  3. Cache flush (forced L3 miss)
  4. Multi-core bandwidth saturation (1,2,4,8,half,all cores)
  5. NUMA binding (local vs remote)
  6. Combined modes (multi-core+random, multi-core+flush, etc.)

Output:
  Results saved to timestamped directory with individual test files
  and a SUMMARY.txt report.

EOF
}

# ============================================================================
# Main
# ============================================================================

main() {
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --quick)
                QUICK_MODE=1
                TEST_DURATION=1
                TEST_SAMPLES=1
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
    
    # Verify memfreq_bench exists
    if [[ ! -x ./memfreq_bench ]]; then
        log_error "memfreq_bench not found or not executable"
        log_info "Please compile first: gcc -O2 -o memfreq_bench memfreq_bench.c"
        exit 1
    fi
    
    # Detect topology and create output directory
    detect_topology
    create_output_dir
    
    # Show test plan
    echo ""
    log_info "=========================================="
    log_info " Starting Comprehensive Test Suite"
    log_info "=========================================="
    if [[ $QUICK_MODE -eq 1 ]]; then
        log_warn "Quick mode enabled (1s duration, 1 sample)"
    fi
    log_info "This will run ~15-20 tests"
    log_info "Estimated time: $((TEST_DURATION * 20)) seconds"
    echo ""
    read -p "Press Enter to continue (Ctrl+C to cancel)..."
    
    # Run test suites
    local start_time=$(date +%s)
    
    test_single_core_baseline
    test_random_permutation
    test_cache_flush
    test_multi_core_bandwidth
    test_numa_binding
    test_combined_modes
    
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    
    # Generate summary
    generate_summary
    
    # Final message
    echo ""
    log_success "=========================================="
    log_success " All Tests Completed!"
    log_success "=========================================="
    log_info "Total time: ${elapsed}s"
    log_info "Results saved to: $OUTPUT_DIR"
    log_info "Summary report: $OUTPUT_DIR/SUMMARY.txt"
    echo ""
    log_info "Next steps:"
    log_info "  1. Review $OUTPUT_DIR/SUMMARY.txt for sweet spot analysis"
    log_info "  2. Compare single-core vs multi-core sweet spots"
    log_info "  3. Check NUMA local vs remote performance"
    log_info "  4. Use these results to tune your DVFS governor"
    echo ""
}

main "$@"
