#!/bin/bash
#
# lmbench_freq_sweep.sh — Cross-validate memfreq_bench with lmbench
#
# Wraps `bw_mem` (memory bandwidth, ~stride workload) and
# `lat_mem_rd` (memory read latency, ~chase workload) with a cpufreq
# frequency sweep. Outputs TSV aligned with memfreq_bench's format so
# sweet spots can be compared directly.
#
# Workflow:
#   1. (one-time) Build lmbench3 and copy bw_mem / lat_mem_rd to PATH:
#        git clone https://github.com/intel/lmbench.git /tmp/lmbench
#        cd /tmp/lmbench/src && make
#        sudo cp bw_mem lat_mem_rd /usr/local/bin/
#   2. Run memfreq_bench baseline (for later comparison):
#        sudo ./run_full_sweep.sh --quick --yes --force
#   3. Run this script (parallel, independent measurement):
#        sudo ./lmbench_freq_sweep.sh --yes
#   4. Compare sweet spots:
#        awk -F'\t' 'NR>1' output/lmbench_sweep_*/bw_mem_rd.tsv \
#            | sort -k1 -n | awk -v t=0.95 '...'  # custom
#
# Usage:
#   sudo ./lmbench_freq_sweep.sh [OPTIONS]
#
# Options:
#   --freqs "F F F"   Frequencies in MHz (default: 800 1000 1200 1500 1800 2100 2400 2700 3000)
#   --array-mb N      Working set size in MB (default: 1024, must exceed L3)
#   --warmup N        bw_mem warmup iterations (default: 2)
#   --reps N          bw_mem / lat_mem_rd measurement reps (default: 5)
#   --cpu N           CPU to pin and control (default: 0)
#   --output DIR      Output directory (default: output/lmbench_sweep_YYYYMMDD_HHMMSS)
#   --yes             Skip confirmation prompt
#   --help            Show this help
#
# Requires:
#   - Root (cpufreq sysfs writes)
#   - bw_mem and lat_mem_rd in PATH
#   - cpufreq userspace or performance governor
#   - DRAM ≥ array-mb (otherwise latency measures cache, not DRAM)
#
# Output TSV format (matches memfreq_bench column order):
#   target_MHz  actual_MHz  bw_mem_MBps  lat_mem_rd_ns

set -uo pipefail

# ============================================================================
# Defaults
# ============================================================================

FREQ_LIST_DEFAULT="800 1000 1200 1500 1800 2100 2400 2700 3000"
FREQ_LIST="$FREQ_LIST_DEFAULT"
ARRAY_MB=1024
WARMUP=2
REPS=5
CPU=0
OUTPUT_DIR=""
NO_PROMPT=0

# Color (empty if not TTY)
RED='' GREEN='' YELLOW='' BLUE='' CYAN='' NC=''
if [[ -t 1 ]]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'
fi

log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
log_error() { echo -e "${RED}[ERR]${NC} $*" >&2; }
log_ok()    { echo -e "${GREEN}[ OK]${NC} $*"; }

# ============================================================================
# Help
# ============================================================================

show_help() {
    sed -n '2,40p' "$0" | sed 's/^# *//'
}

# ============================================================================
# Argument parsing
# ============================================================================

while [[ $# -gt 0 ]]; do
    case $1 in
        --freqs)    FREQ_LIST="$2"; shift 2 ;;
        --array-mb) ARRAY_MB="$2"; shift 2 ;;
        --warmup)   WARMUP="$2"; shift 2 ;;
        --reps)     REPS="$2"; shift 2 ;;
        --cpu)      CPU="$2"; shift 2 ;;
        --output)   OUTPUT_DIR="$2"; shift 2 ;;
        --yes)      NO_PROMPT=1; shift ;;
        --help|-h)  show_help; exit 0 ;;
        *)          log_error "Unknown option: $1"; exit 1 ;;
    esac
done

# ============================================================================
# Preflight
# ============================================================================

if [[ $EUID -ne 0 ]]; then
    log_error "Must run as root (sudo)"; exit 1
fi

for tool in bw_mem lat_mem_rd; do
    if ! command -v "$tool" &>/dev/null; then
        log_error "$tool not found in PATH."
        log_error "Build lmbench and install:"
        log_error "  git clone https://github.com/intel/lmbench.git /tmp/lmbench"
        log_error "  cd /tmp/lmbench/src && make"
        log_error "  sudo cp bw_mem lat_mem_rd /usr/local/bin/"
        exit 1
    fi
done

CPUFREQ="/sys/devices/system/cpu/cpu${CPU}/cpufreq"
if [[ ! -d "$CPUFREQ" ]]; then
    log_error "cpufreq not available at $CPUFREQ"; exit 1
fi

HW_MAX_KHZ=$(cat $CPUFREQ/cpuinfo_max_freq)
HW_MIN_KHZ=$(cat $CPUFREQ/cpuinfo_min_freq)
HW_MAX_MHZ=$((HW_MAX_KHZ / 1000))
HW_MIN_MHZ=$((HW_MIN_KHZ / 1000))

# Clamp FREQ_LIST to hardware range
CLAMPED=""
for f in $FREQ_LIST; do
    if [[ $f -lt $HW_MIN_MHZ ]]; then
        log_warn "freq $f MHz below hw min $HW_MIN_MHZ MHz, skipping"
        continue
    fi
    if [[ $f -gt $HW_MAX_MHZ ]]; then
        log_warn "freq $f MHz above hw max $HW_MAX_MHZ MHz, skipping"
        continue
    fi
    CLAMPED="$CLAMPED $f"
done
FREQ_LIST="$CLAMPED"
if [[ -z "$FREQ_LIST" ]]; then
    log_error "No valid frequencies in [$HW_MIN_MHZ, $HW_MAX_MHZ] MHz"
    exit 1
fi

# ============================================================================
# Output setup
# ============================================================================

if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="output/lmbench_sweep_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUTPUT_DIR"
BW_OUT="$OUTPUT_DIR/bw_mem_rd.tsv"
LAT_OUT="$OUTPUT_DIR/lat_mem_rd.tsv"
LOG="$OUTPUT_DIR/run.log"

# Tee all output to log
exec > >(tee -a "$LOG") 2>&1

log_info "CPU:        $CPU"
log_info "Array:      ${ARRAY_MB} MB (must exceed L3)"
log_info "Frequencies:$FREQ_LIST (hw range: $HW_MIN_MHZ - $HW_MAX_MHZ MHz)"
log_info "Warmup:     $WARMUP, reps: $REPS"
log_info "Output:     $OUTPUT_DIR"
log_info ""

if [[ $NO_PROMPT -eq 0 ]]; then
    read -r -p "Press Enter to start (Ctrl+C to cancel)..." < /dev/tty
fi

# ============================================================================
# Frequency control (three-step write, mirrors memfreq_bench set_freq)
# ============================================================================

ORIG_MAX_KHZ=$(cat $CPUFREQ/scaling_max_freq)
ORIG_MIN_KHZ=$(cat $CPUFREQ/scaling_min_freq)
ORIG_GOVERNOR=$(cat $CPUFREQ/scaling_governor)

set_freq() {
    local khz=$1
    # 1. widen max to hw max (must come first; otherwise writing min > max rejected)
    echo $HW_MAX_KHZ > $CPUFREQ/scaling_max_freq
    # 2. set min to target
    echo $khz       > $CPUFREQ/scaling_min_freq
    # 3. tighten max to target
    echo $khz       > $CPUFREQ/scaling_max_freq
}

restore_freq() {
    if [[ -n "${ORIG_MAX_KHZ:-}" ]]; then
        echo $HW_MAX_KHZ > $CPUFREQ/scaling_max_freq
        echo $ORIG_MIN_KHZ > $CPUFREQ/scaling_min_freq
        echo $ORIG_MAX_KHZ > $CPUFREQ/scaling_max_freq
    fi
    if [[ -n "${ORIG_GOVERNOR:-}" && -w $CPUFREQ/scaling_governor ]]; then
        echo $ORIG_GOVERNOR > $CPUFREQ/scaling_governor
    fi
}
trap restore_freq EXIT INT TERM

# Switch to userspace governor and disable turbo
log_info "Setting governor to userspace, disabling turbo..."
echo userspace > $CPUFREQ/scaling_governor
if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
elif [[ -f $CPUFREQ/boost ]]; then
    echo 0 > $CPUFREQ/boost
fi

# Write headers
echo -e "target_MHz\tactual_MHz\tbw_mem_MBps\tlat_mem_rd_ns" > $BW_OUT
echo -e "target_MHz\tactual_MHz\tlat_mem_rd_ns"               > $LAT_OUT

# ============================================================================
# Main sweep
# ============================================================================

for freq in $FREQ_LIST; do
    khz=$((freq * 1000))
    set_freq $khz
    sleep 0.15   # let frequency settle

    actual_mhz=$(($(cat $CPUFREQ/scaling_cur_freq) / 1000))
    log_info "─── freq target=${freq} MHz actual=${actual_mhz} MHz ───"

    # ---- bw_mem: read bandwidth ----
    # Output: rows of "<size_MB> <MB/s>". We want the row where size = ARRAY_MB.
    BW_VAL=$(bw_mem -W $WARMUP -N $REPS ${ARRAY_MB}m rd 2>/dev/null | \
             awk -v target="$ARRAY_MB" '
                 $1 ~ /^[0-9]/ && $1+0 == target+0 { print $2; exit }
             ')
    if [[ -z "$BW_VAL" ]]; then
        log_warn "bw_mem: no row for ${ARRAY_MB} MB, capturing last numeric row"
        BW_VAL=$(bw_mem -W $WARMUP -N $REPS ${ARRAY_MB}m rd 2>/dev/null | \
                 awk '$1 ~ /^[0-9]/ {bw=$2} END{print bw+0}')
    fi

    # ---- lat_mem_rd: random-access read latency ----
    # Output: rows of "<size_MB> <ns>". Same extraction.
    LAT_VAL=$(lat_mem_rd -N $REPS ${ARRAY_MB}m 2>/dev/null | \
              awk -v target="$ARRAY_MB" '
                  $1 ~ /^[0-9]/ && $1+0 == target+0 { print $2; exit }
              ')
    if [[ -z "$LAT_VAL" ]]; then
        log_warn "lat_mem_rd: no row for ${ARRAY_MB} MB, capturing last numeric row"
        LAT_VAL=$(lat_mem_rd -N $REPS ${ARRAY_MB}m 2>/dev/null | \
                  awk '$1 ~ /^[0-9]/ {lat=$2} END{print lat+0}')
    fi

    echo -e "$freq\t$actual_mhz\t${BW_VAL:-?}\t${LAT_VAL:-?}" >> $BW_OUT
    echo -e "$freq\t$actual_mhz\t${LAT_VAL:-?}"                  >> $LAT_OUT

    log_ok "  bw_mem=${BW_VAL:-?} MB/s,  lat_mem_rd=${LAT_VAL:-?} ns"
done

# ============================================================================
# Cleanup + summary
# ============================================================================

restore_freq
trap - EXIT INT TERM

echo ""
log_info "═══════════════════════════════════════════════════════════════"
log_info "Done. Outputs:"
log_info "  $BW_OUT  (target + actual freq, bw_mem bandwidth)"
log_info "  $LAT_OUT (target + actual freq, lat_mem_rd latency)"
log_info "  $LOG     (full run log)"
log_info ""
log_info "Sweet spot detection (95% threshold) — examples:"
log_info "  awk -F'\\t' 'NR>1 {print \$2,\$3}' $BW_OUT | \\"
log_info "      awk '\$1==max{bw_max=\$2} \$2>=bw_max*0.95{print \$1; exit}' max=\$(awk -F'\\t' 'NR>1{mw=\$2>max?\$2:max}END{print mw}' $BW_OUT)"
log_info ""
log_info "Compare with memfreq_bench (must be in same freq range):"
log_info "  grep -E '^# (stride|chase).*sweet spot' output/full_sweep_*/FULL_REPORT.txt"
log_info ""
log_info "═══════════════════════════════════════════════════════════════"
