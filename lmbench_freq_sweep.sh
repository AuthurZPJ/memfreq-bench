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
#        # Source build:
#        git clone https://github.com/intel/lmbench.git /tmp/lmbench
#        cd /tmp/lmbench/src && make
#        sudo cp bw_mem lat_mem_rd /usr/local/bin/
#        # Debian/Ubuntu package (auto-detected at /opt/lmbench/bin):
#        sudo apt install lmbench
#        # Or symlink if installed elsewhere:
#        sudo ln -s /opt/lmbench/bin/bw_mem /usr/local/bin/bw_mem
#        sudo ln -s /opt/lmbench/bin/lat_mem_rd /usr/local/bin/lat_mem_rd
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
#   --freqs "F F F"   Frequencies in MHz (default: auto-detected from cpufreq,
#                      matches memfreq_bench's read_freqs: tries
#                      scaling_available_frequencies, falls back to
#                      cpuinfo_min/max with --step-khz step)
#   --step-khz N      Step size in kHz for range mode (default: 25000 = 25 MHz;
#                      matches memfreq_bench -S default). Only used when
#                      scaling_available_frequencies is absent.
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

FREQ_LIST_DEFAULT=""
FREQ_LIST="$FREQ_LIST_DEFAULT"
STEP_KHZ=25000     # range-mode step (matches memfreq_bench -S default)
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

# Detect the frequency list that memfreq_bench would use, in MHz.
# Mirrors memfreq_bench's read_freqs() logic (memfreq_bench.c):
#   1. Read /sys/.../scaling_available_frequencies (discrete list) — most
#      systems have this; the values are in kHz.
#   2. Fall back to cpuinfo_min/max with a stepped range (matches memfreq_bench
#      -S STEP_KHZ, default 25000 = 25 MHz). Used when the driver is in
#      CPPC range mode and only exposes min/max.
# ACPI CPPC abstract-performance translation is not implemented here —
# in practice, scaling_available_frequencies is present on >95% of
# production systems, and cpuinfo_min/max covers the rest.
# Returns space-separated MHz list, or empty string on failure.
detect_freqs() {
    local cpu_path="/sys/devices/system/cpu/cpu${CPU}/cpufreq"
    local avail_file="${cpu_path}/scaling_available_frequencies"
    local out=""

    # 1. Discrete list (most common)
    if [[ -r "$avail_file" ]]; then
        read -r line < "$avail_file"
        for khz in $line; do
            [[ -z "$khz" ]] && continue
            out="$out $((khz / 1000))"
        done
        if [[ -n "$out" ]]; then
            echo "$out"
            return 0
        fi
    fi

    # 2. Range mode with step
    local min_khz max_khz
    min_khz=$(cat "${cpu_path}/cpuinfo_min_freq" 2>/dev/null || echo 0)
    max_khz=$(cat "${cpu_path}/cpuinfo_max_freq" 2>/dev/null || echo 0)
    if [[ $min_khz -gt 0 && $max_khz -gt $min_khz ]]; then
        local step_khz=${STEP_KHZ:-25000}
        local khz=$min_khz
        while [[ $khz -le $max_khz ]]; do
            out="$out $((khz / 1000))"
            khz=$((khz + step_khz))
        done
        echo "$out"
        return 0
    fi

    return 1
}
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
        --step-khz) STEP_KHZ="$2"; shift 2 ;;
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

# Auto-detect lmbench in common system install path /opt/lmbench/bin
# (Debian/Ubuntu package, or source build installed to /opt). The user
# reported `which lmbench` -> /opt/lmbench/bin/lmbench, so the binaries
# are likely next to it. Add to PATH only if not already in PATH.
if ! command -v bw_mem &>/dev/null && [[ -x /opt/lmbench/bin/bw_mem ]] \
   && ! command -v lat_mem_rd &>/dev/null && [[ -x /opt/lmbench/bin/lat_mem_rd ]]; then
    export PATH="/opt/lmbench/bin:$PATH"
    log_info "Auto-detected lmbench at /opt/lmbench/bin, added to PATH"
fi

for tool in bw_mem lat_mem_rd; do
    if ! command -v "$tool" &>/dev/null; then
        log_error "$tool not found in PATH."
        log_error "Build lmbench and install (one of):"
        log_error "  # Source build:"
        log_error "  git clone https://github.com/intel/lmbench.git /tmp/lmbench"
        log_error "  cd /tmp/lmbench/src && make"
        log_error "  sudo cp bw_mem lat_mem_rd /usr/local/bin/"
        log_error "  # Debian/Ubuntu package (installs to /opt/lmbench/bin):"
        log_error "  sudo apt install lmbench"
        log_error "  # Or symlink:"
        log_error "  sudo ln -s /opt/lmbench/bin/bw_mem /usr/local/bin/bw_mem"
        log_error "  sudo ln -s /opt/lmbench/bin/lat_mem_rd /usr/local/bin/lat_mem_rd"
        exit 1
    fi
done

# Pin the benchmark process to the CPU we control the frequency for.
# Without this, on systems with per-cluster or per-CPU freq domains
# (e.g., ARM big.LITTLE), the OS scheduler may run bw_mem on a
# different CPU whose frequency we did NOT set, making the measurement
# invalid. On unified-domain systems this is a no-op for correctness
# but still cleaner. taskset is in util-linux (universal on Linux).
if command -v taskset &>/dev/null; then
    TASKSET_PREFIX=(taskset -c "$CPU")
    log_info "Will pin benchmark to CPU $CPU via taskset"
else
    TASKSET_PREFIX=()
    log_warn "taskset not found — benchmark may run on a different CPU than the one we control"
    log_warn "On per-cluster / per-CPU freq domains (big.LITTLE), this gives wrong results"
fi

CPUFREQ="/sys/devices/system/cpu/cpu${CPU}/cpufreq"
if [[ ! -d "$CPUFREQ" ]]; then
    log_error "cpufreq not available at $CPUFREQ"; exit 1
fi

HW_MAX_KHZ=$(cat $CPUFREQ/cpuinfo_max_freq)
HW_MIN_KHZ=$(cat $CPUFREQ/cpuinfo_min_freq)
HW_MAX_MHZ=$((HW_MAX_KHZ / 1000))
HW_MIN_MHZ=$((HW_MIN_KHZ / 1000))

# Auto-detect frequency list (mirrors memfreq_bench read_freqs). Only
# invoked when the user didn't pass --freqs explicitly.
if [[ -z "$FREQ_LIST" ]]; then
    DETECTED=$(detect_freqs)
    if [[ -n "$DETECTED" ]]; then
        FREQ_LIST="$DETECTED"
        log_info "Auto-detected frequencies from /sys/cpufreq (matches memfreq_bench)"
    else
        log_error "Could not auto-detect frequency list."
        log_error "Pass --freqs '800 1500 3000' explicitly to override."
        exit 1
    fi
fi

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
echo -e "target_MHz\tactual_MHz\tbw_mem_MBps\tlat_mem_rd_ns\tsize_used_MB" > $BW_OUT
echo -e "target_MHz\tactual_MHz\tlat_mem_rd_ns\tsize_used_MB"               > $LAT_OUT

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
    # Two-tier extraction: exact match for ARRAY_MB, fallback to largest <= target.
    # lmbench's default size list varies by version — 1024m may not always
    # appear even when we asked for it. Capture the actual size used too.
    BW_LINE=$("${TASKSET_PREFIX[@]}" bw_mem -W $WARMUP -N $REPS ${ARRAY_MB}m rd 2>/dev/null | \
              awk -v target="$ARRAY_MB" '
                  $1 ~ /^[0-9]/ {
                      v = $1 + 0
                      if (v == target+0 && !exact) { exact = 1; print $1, $2; exit }
                      if (v <= target+0 && v > best)  { best = v; best_sz = $1; best_bw = $2 }
                  }
                  END { if (!exact && best_bw != "") print best_sz, best_bw }
              ')
    BW_SIZE=$(echo "$BW_LINE" | awk '{print $1}')
    BW_VAL=$(echo "$BW_LINE"  | awk '{print $2}')
    if [[ -n "$BW_VAL" && "$BW_SIZE" != "$ARRAY_MB" ]]; then
        log_warn "bw_mem: requested ${ARRAY_MB} MB, lmbench only had ${BW_SIZE} MB; using that"
    fi
    if [[ -z "$BW_VAL" ]]; then
        log_warn "bw_mem: no usable row in output (ARRAY_MB=${ARRAY_MB})"
    fi

    # ---- lat_mem_rd: random-access read latency ----
    # Output: rows of "<size_MB> <ns>". Same extraction.
    LAT_LINE=$("${TASKSET_PREFIX[@]}" lat_mem_rd -N $REPS ${ARRAY_MB}m 2>/dev/null | \
               awk -v target="$ARRAY_MB" '
                   $1 ~ /^[0-9]/ {
                       v = $1 + 0
                       if (v == target+0 && !exact) { exact = 1; print $1, $2; exit }
                       if (v <= target+0 && v > best)  { best = v; best_sz = $1; best_lat = $2 }
                   }
                   END { if (!exact && best_lat != "") print best_sz, best_lat }
               ')
    LAT_SIZE=$(echo "$LAT_LINE" | awk '{print $1}')
    LAT_VAL=$(echo "$LAT_LINE"  | awk '{print $2}')
    if [[ -n "$LAT_VAL" && "$LAT_SIZE" != "$ARRAY_MB" ]]; then
        log_warn "lat_mem_rd: requested ${ARRAY_MB} MB, lmbench only had ${LAT_SIZE} MB; using that"
    fi
    if [[ -z "$LAT_VAL" ]]; then
        log_warn "lat_mem_rd: no usable row in output (ARRAY_MB=${ARRAY_MB})"
    fi

    echo -e "$freq\t$actual_mhz\t${BW_VAL:-?}\t${LAT_VAL:-?}\t${BW_SIZE:-${LAT_SIZE:-?}}" >> $BW_OUT
    echo -e "$freq\t$actual_mhz\t${LAT_VAL:-?}\t${LAT_SIZE:-?}"                              >> $LAT_OUT

    log_ok "  bw_mem=${BW_VAL:-?} MB/s (${BW_SIZE:-?}MB),  lat_mem_rd=${LAT_VAL:-?} ns (${LAT_SIZE:-?}MB)"
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
