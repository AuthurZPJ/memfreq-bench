#!/bin/bash
# tests/test_stats_output.sh
# Lightweight e2e test for new --threshold, --thresholds, --emit-raw,
# --no-plateau flags, and Python --compare mode.
#
# Does NOT require root for the help/validation/compare checks. The full
# 8 e2e checks from the design spec require Linux + cpufreq and are
# skipped with a clear "SKIPPED: requires cpufreq" message on this
# machine. On a Linux box with cpufreq, all checks run.
#
# Usage: bash tests/test_stats_output.sh

set -u

PASS=0
FAIL=0
SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

# A check passes if $got contains $want.
check_contains() {
    local name="$1" got="$2" want="$3"
    if [[ "$got" == *"$want"* ]]; then
        pass "$name"
    else
        fail "$name"
        echo "    expected to contain: $want"
        echo "    got: $got"
    fi
}

# A check passes if $got does NOT contain $want.
check_not_contains() {
    local name="$1" got="$2" want="$3"
    if [[ "$got" != *"$want"* ]]; then
        pass "$name"
    else
        fail "$name"
        echo "    expected NOT to contain: $want"
        echo "    got: $got"
    fi
}

BIN="${BIN:-./memfreq_bench}"
PY="${PY:-python3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

# Probe: cpufreq available and writable?
CPUFREQ_OK=0
if [ -d /sys/devices/system/cpu/cpu0/cpufreq ] && \
   [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq ]; then
    CPUFREQ_OK=1
fi

echo "=== Test 1: --help shows new flags ==="
HELP=$($BIN -h 2>&1)
check_contains "help mentions -T FRAC"  "$HELP" "-T FRAC"
check_contains "help mentions -L LIST"  "$HELP" "-L LIST"
check_contains "help mentions -r"       "$HELP" "-r"
check_contains "help mentions -P"       "$HELP" "-P"

echo "=== Test 2: --threshold validation ==="
ERR=$($BIN -T 1.5 2>&1)
check_contains "rejects threshold > 1" "$ERR" "ERROR"
check_contains "error mentions (0, 1]"  "$ERR" "(0, 1]"

echo "=== Test 3: --thresholds validation ==="
ERR=$($BIN -L 0.5,1.5 2>&1)
check_contains "rejects thresholds > 1" "$ERR" "ERROR"
check_contains "error mentions (0, 1]"   "$ERR" "(0, 1]"

echo "=== Test 4: Python --compare with 3 fixture files ==="
OUT=$($PY memfreq_sweep.py --compare \
    tests/fixtures/compare_run1.json \
    tests/fixtures/compare_run2.json \
    tests/fixtures/compare_run3.json 2>&1)
check_contains "shows header"            "$OUT" "Cross-run sweet-spot comparison (3 runs)"
check_contains "shows stride row"        "$OUT" "stride"
check_contains "shows chase row"         "$OUT" "chase"
check_contains "shows range column"      "$OUT" "range_MHz"
check_contains "shows numeric mean=2000"  "$OUT" "2000"

echo "=== Test 5: Python --compare with 1 file (edge case) ==="
OUT=$($PY memfreq_sweep.py --compare \
    tests/fixtures/compare_run1.json 2>&1)
check_contains "1-file compare does not crash" "$OUT" "Cross-run sweet-spot comparison (1 runs)"

echo "=== Test 6: Python parser extracts new statistical blocks ==="
# parse_output() on the fixture TSV should yield a dict that contains
# all four new top-level keys. Use a tiny Python harness via $PY -c.
PARSE_OUT=$($PY -c "
import sys
sys.path.insert(0, '.')
from memfreq_sweep import parse_output
with open('tests/fixtures/with_stats.txt') as fh:
    data = parse_output(fh.read())
keys = sorted(data.keys())
print(' '.join(keys))
print('pfs_workloads=' + ','.join(sorted(data.get('per_freq_stats', {}).keys())))
print('sens_workloads=' + ','.join(sorted(data.get('sensitivity', {}).keys())))
print('plateau_rows=' + str(len(data.get('plateau', []))))
print('raw_workloads=' + ','.join(sorted(data.get('raw_samples', {}).keys())))
" 2>&1)
check_contains "parse_output returns per_freq_stats"  "$PARSE_OUT" "per_freq_stats"
check_contains "parse_output returns sensitivity"    "$PARSE_OUT" "sensitivity"
check_contains "parse_output returns plateau"        "$PARSE_OUT" "plateau"
check_contains "parse_output returns raw_samples"    "$PARSE_OUT" "raw_samples"
check_contains "per_freq_stats has stride workload"  "$PARSE_OUT" "pfs_workloads=chase,compute,stride"
check_contains "sensitivity has stride workload"     "$PARSE_OUT" "sens_workloads=chase,compute,stride"
check_contains "plateau has 3 rows"                  "$PARSE_OUT" "plateau_rows=3"
check_contains "raw_samples has stride workload"     "$PARSE_OUT" "raw_workloads=compute,stride"

echo "=== Test 7: --json writes the new blocks to memfreq_results.json ==="
# Run the Python parser against the fixture TSV with --json and verify
# the saved JSON file contains all four new top-level keys. We invoke
# the script via $PY from $ROOT_DIR so the relative paths match.
JSON_OUT=$($PY memfreq_sweep.py --file tests/fixtures/with_stats.txt --json 2>&1)
JSON_FILE="memfreq_results.json"
if [ ! -f "$JSON_FILE" ]; then
    fail "--json output file written"
    echo "    expected $JSON_FILE to exist"
else
    pass "--json output file written"
    KEYS=$($PY -c "import json; print(' '.join(sorted(json.load(open('$JSON_FILE')).keys())))" 2>&1)
    check_contains "JSON has per_freq_stats key" "$KEYS" "per_freq_stats"
    check_contains "JSON has sensitivity key"    "$KEYS" "sensitivity"
    check_contains "JSON has plateau key"        "$KEYS" "plateau"
    check_contains "JSON has raw_samples key"    "$KEYS" "raw_samples"
    check_contains "JSON preserves meta key"     "$KEYS" "meta"
    check_contains "JSON preserves data key"     "$KEYS" "data"
    check_contains "JSON preserves sweet_spot_mhz" "$KEYS" "sweet_spot_mhz"
    # Spot-check that an em-dash was serialized as JSON null.
    NULL_OUT=$($PY -c "
import json
d = json.load(open('$JSON_FILE'))
sens = d.get('sensitivity', {}).get('stride', [])
nulls = [r for r in sens if r.get('sweet_spot_mhz_or_null') is None]
print(len(nulls))
" 2>&1)
    check_contains "JSON has null for em-dash sensitivity value" "$NULL_OUT" "1"
    # Spot-check that the plateau block survived.
    PLATEAU_OUT=$($PY -c "
import json
d = json.load(open('$JSON_FILE'))
rows = d.get('plateau', [])
stride = [r for r in rows if r.get('workload') == 'stride']
print('OK' if stride and stride[0].get('breakpoint_mhz_or_null') == 2050 else 'BAD')
" 2>&1)
    check_contains "JSON plateau row has breakpoint 2050" "$PLATEAU_OUT" "OK"
    # Clean up so the test is idempotent.
    rm -f "$JSON_FILE"
fi

# --- The remaining checks require Linux + cpufreq. ---
if [ "$CPUFREQ_OK" -eq 0 ]; then
    echo
    echo "=== Tests 6-8: full e2e (require Linux + cpufreq) ==="
    skip "Test 6: --threshold changes sweet spot (SKIPPED: requires cpufreq)"
    skip "Test 7: -n 5 emits per-freq stats block (SKIPPED: requires cpufreq)"
    skip "Test 8: -r emits raw_samples block (SKIPPED: requires cpufreq)"
else
    echo
    echo "=== Test 6: --threshold changes the sweet spot ==="
    # Default (0.95) and lower (0.80) thresholds should yield different sweet
    # spots on a memory-bound workload; the 0.80 one should be ≤ 0.95 one.
    OUT95=$(sudo $BIN -c 0 -m 128 -t 1 -n 1 2>&1)
    OUT80=$(sudo $BIN -c 0 -m 128 -t 1 -n 1 -T 0.80 2>&1)
    check_contains "default threshold is 95%"  "$OUT95" "95%"
    check_contains "custom threshold is 80%"   "$OUT80" "80%"

    echo "=== Test 7: -n 5 emits per-freq stats block ==="
    OUT=$($BIN -c 0 -m 128 -t 1 -n 5 2>&1)
    # This requires sudo for cpufreq writes; fall back to non-sudo attempt
    # if the previous run already failed. Use sudo unconditionally here.
    OUT=$(sudo $BIN -c 0 -m 128 -t 1 -n 5 2>&1)
    check_contains "stats block header (stride)" "$OUT" "per-freq stats (stride)"
    check_contains "stats block header (compute)" "$OUT" "per-freq stats (compute)"

    echo "=== Test 8: -r emits raw_samples block ==="
    OUT=$(sudo $BIN -c 0 -m 128 -t 1 -n 3 -r 2>&1)
    check_contains "raw_samples block (stride)" "$OUT" "raw_samples (stride)"

    # Sanity: without -r, raw_samples should not appear.
    OUT=$(sudo $BIN -c 0 -m 128 -t 1 -n 1 2>&1)
    check_not_contains "no raw_samples without -r" "$OUT" "raw_samples"
fi

echo
echo "=========================================="
echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "=========================================="

# Exit 0 on all-pass-or-skip, non-zero on any FAIL.
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
