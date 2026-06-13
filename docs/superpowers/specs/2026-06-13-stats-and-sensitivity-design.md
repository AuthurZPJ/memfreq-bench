# Design: Statistical Rigor for memfreq_bench

**Date:** 2026-06-13
**Status:** Draft, awaiting review
**Scope:** Single feature ("statistical rigor"), 2 files modified (`memfreq_bench.c`, `memfreq_sweep.py`).

## Goal

Make the sweet-spot decision defensible by:
1. Letting the user pick the threshold (currently hidden as `THRESHOLD = 0.95`)
2. Showing sensitivity of the sweet spot to threshold choice
3. Reporting per-point variance (min/max/IQR) so the user sees measurement noise
4. Detecting the plateau statistically (piecewise linear fit) instead of a single threshold heuristic
5. Supporting cross-run reproducibility comparison

Per user direction: **everything implementable in C goes in C; everything else is zero-dependency Python stdlib.**

## CLI changes (`memfreq_bench.c`)

| Flag | Type | Default | Behavior |
|------|------|---------|----------|
| `--threshold F` | float in (0, 1) | 0.95 | Single threshold for sweet-spot detection. Replaces the `THRESHOLD` constant. Applies to stride, chase, random, and stride+flush workloads. |
| `--thresholds L` | comma-separated floats in (0, 1) | (empty) | Multi-threshold sweep. For each value, run sweet-spot detection and emit one row in the `# --- sensitivity ---` block. Independent of `--threshold`. |
| `--emit-raw` | flag | off | Emit per-sample throughput in a `# --- raw_samples ---` block. Enables future power-user analysis (custom stats, distribution checks) but is **not required** for the Python `--compare` mode, which uses the headline sweet-spot value. |
| `--no-plateau` | flag | off | Suppress plateau detection (on by default). Plateau is cheap to compute and informational; this is a power-user opt-out. |

All flags are additive. Default behavior (no flags) matches the current output exactly.

## New output sections

All new content is appended to the existing TSV after the data rows, in `#`-prefixed blocks. Existing parsers that read only non-`#` lines are unaffected.

### Block 1: `# --- per-freq stats (stride|chase|compute) ---`

One block per workload that was run. Always emitted (controlled only by whether the workload itself ran, not by `--no-plateau`).

```
# --- per-freq stats (stride) ---
# freq_MHz  min_Mops  max_Mops  median_Mops  iqr_Mops
# 800        140.1     142.5     141.3        1.2
# 1200       155.0     157.2     156.0        1.5
# ...
```

- `min_Mops`, `max_Mops`: min and max of `nsamples` runs at this freq
- `median_Mops`: existing value (currently the only per-freq stat emitted)
- `iqr_Mops`: Q3 − Q1 over `nsamples` runs (3 samples → only 1 unique IQR value; 5 samples → meaningful)

### Block 2: `# --- sensitivity (stride|chase|compute) ---`

Only emitted when `--thresholds` is given. One block per workload.

```
# --- sensitivity (stride) ---
# threshold  sweet_spot_MHz
# 0.80       1800
# 0.85       1900
# 0.90       1950
# 0.95       2000
# 0.99       2400
```

For workloads with no plateau (compute), all thresholds return `—` (em-dash, consistent with existing output style for "no sweet spot").

### Block 3: `# --- plateau ---`

Always emitted (one line per workload).

```
# --- plateau ---
# stride   plateau_breakpoint: 2050 MHz  (slope ratio 18.3x, 95% sweet spot 2000 MHz)
# chase    plateau_breakpoint: 2100 MHz  (slope ratio 22.1x, 95% sweet spot 2000 MHz)
# compute  plateau_breakpoint: —  (no plateau; throughput keeps rising with frequency)
```

`slope ratio` = segment1_slope / segment2_slope. Larger = more pronounced plateau. A workload is "plateau-like" when `ratio > 2.0` (below-breakpoint throughput rises at least 2× faster than above-breakpoint). Below threshold: emit `—` and the "no plateau" note. The 2.0 cutoff is a deliberate, conservative default — a flatter plateau (ratio = 3) is unambiguously memory-bound, a borderline case (ratio = 1.5) is ambiguous, and a non-plateau (ratio = 1) is compute-bound.

### Block 4: `# --- raw_samples (stride|chase|compute) ---`

Only emitted when `--emit-raw` is given. One block per workload.

```
# --- raw_samples (stride) ---
# freq_MHz  sample_idx  mops
# 800       1           140.5
# 800       2           141.2
# 800       3           140.8
# 1200      1           155.0
# ...
```

Row count: `n_freqs × nsamples`. For 20 freqs × 5 samples = 100 rows per workload.

## Algorithm details

### `min_max_iqr` (per freq point)

In the existing per-freq inner sample loop, after the `nsamples` runs complete, the median is already computed. Extend with:

```c
double mn =  INFINITY, mx = -INFINITY;
double q1 = 0, q3 = 0;  /* require nsamples >= 3 for IQR */
for (int s = 0; s < nsamples; s++) {
    if (samples[s] < mn) mn = samples[s];
    if (samples[s] > mx) mx = samples[s];
}
if (nsamples >= 3) {
    /* Q1, Q3 via linear interpolation, matches numpy default */
    q1 = percentile(samples, nsamples, 0.25);
    q3 = percentile(samples, nsamples, 0.75);
}
double iqr = q3 - q1;
```

`percentile` uses the same linear-interpolation method as numpy's default. `samples[]` is already sorted (median needs it sorted). ~25 lines.

### Multi-threshold sweet-spot detection

Refactor the existing sweet-spot detection into:

```c
/* Returns the lowest freq with throughput >= threshold * max_throughput.
 * Returns -1 if no such freq (no plateau). */
static int find_sweet_spot(const double *mops, const int *freqs,
                           int n, double threshold);
```

Then `--thresholds=A,B,C` becomes a loop over the parsed values, calling `find_sweet_spot` once per threshold. ~30 lines.

### Plateau detection (naive O(N²) piecewise linear)

For each candidate breakpoint index `k` (1 ≤ k < n-1):
1. Fit line `y = a1*x + b1` to points (freqs[0..k], mops[0..k]) via least squares
2. Fit line `y = a2*x + b2` to points (freqs[k+1..n-1], mops[k+1..n-1])
3. Compute total SSE = sum of squared residuals over all n points
4. Track the k that minimizes SSE

After the loop, also compute the corresponding 95% sweet spot for context. If `slope_ratio < 2.0`, emit `—` (no meaningful plateau).

Complexity: N candidates × (2 × O(N) line fits + O(N) residual sum) = O(N²) per workload. For N=20 freqs, ~8000 double-precision ops per workload. < 1 ms total. ~60 lines C.

Least-squares helpers (~10 lines each, two of them):
```c
static void fit_line(const double *x, const double *y, int n,
                     double *out_slope, double *out_intercept);
```

### `--emit-raw` raw sample emission

The per-sample `samples[]` array is already populated. After median is computed, if `emit_raw` is set, print `freq, sample_idx, mops` for each sample. ~10 lines.

## Python wrapper changes (`memfreq_sweep.py`)

### New mode: `--compare FILE [FILE ...]`

Usage:
```bash
python3 memfreq_sweep.py --compare run1.json run2.json run3.json
```

For each workload (stride, chase), report cross-run statistics on the sweet-spot value:

```
=== Cross-run sweet-spot comparison (3 runs) ===
workload   mean_MHz   std_MHz   min_MHz   max_MHz   range_MHz
stride     2000       35        1950      2050      100
chase      2000       0         2000      2000      0
compute    —          —         —         —         —
```

Std uses `statistics.stdev` (sample std, n-1 denominator). Works for n ≥ 2; for n=1, std column is `—`.

### Autodetect file format

Each file argument is tried as JSON first, then TSV. JSON detection: file starts with `{` after whitespace. JSON parser uses `json.load`. TSV parser: existing `parse_output` function.

### Stdlib only

Imports: `argparse`, `json`, `os`, `re`, `statistics`, `sys`. All stdlib. No new dependencies.

### Unchanged

- Existing `--file` mode (parse TSV → visualize) works as before
- Existing `--json` mode (TSV → JSON) works as before
- ASCII bar chart, sweet spot markers, summary text — all unchanged

## Backward compatibility

- Existing TSV data rows: **untouched** in format and order
- New content is appended after the data rows in `#`-prefixed sections
- Default CLI invocation produces the same output as before (no regression)
- Default `THRESHOLD=0.95` behavior preserved when `--threshold` not given
- Existing `memfreq_sweep.py` parser: still works (it ignores `#` lines and reads only data rows)
- New flags are all additive; no existing flag changes meaning

## Testing approach

End-to-end (no unit test framework, consistent with the project's style):

1. **Default behavior**: `sudo ./memfreq_bench` produces output identical to current (modulo a new `# --- plateau ---` block, which is additive). Verify diff against saved baseline.

2. **Threshold CLI**: `sudo ./memfreq_bench --threshold 0.90`. Verify sweet spot changes (or stays same) as expected for a memory-bound workload.

3. **Multi-threshold**: `sudo ./memfreq_bench --thresholds=0.80,0.90,0.95,0.99`. Verify `# --- sensitivity ---` block has 4 rows, sweet spot values are non-decreasing.

4. **Stats block**: `sudo ./memfreq_bench -n 5`. Verify `# --- per-freq stats ---` block has IQR values that are smaller than min/max range.

5. **Raw emission**: `sudo ./memfreq_bench --emit-raw -n 5`. Verify `# --- raw_samples ---` block has exactly `n_freqs × 5` rows.

6. **Plateau detection**: `sudo ./memfreq_bench`. For a memory-bound workload, expect `slope_ratio > 2`. For compute-bound, expect `—`.

7. **Python `--compare`**: `python3 memfreq_sweep.py --compare file1.json file2.json file3.json`. Verify mean/std/min/max columns populated. Edge case: 1 file → std is `—`.

8. **JSON roundtrip**: `python3 memfreq_sweep.py --file results.txt --json` then `python3 memfreq_sweep.py --compare out.json`. Verify cross-run comparison works on the JSON.

## Out of scope (explicit non-goals)

- No changes to the C inner benchmark loops (workload code is untouched)
- No changes to the multi-core fork-coordinator (`run_multicore`)
- No changes to the shell scripts (`run_all_tests.sh`, `run_full_sweep.sh`)
- No new build system; `make` still produces a single binary
- No CI / GitHub Actions
- No new external dependencies in either C or Python
- No paper / writeup; this is code-only

## Estimated diff size

- `memfreq_bench.c`: +~200 lines (mostly mechanical additions to existing structure)
- `memfreq_sweep.py`: +~70 lines (one new argparse path, one new function)
- `docs/memfreq-bench.md`: +~30 lines (document the new flags and output sections)
- No new files

## Resolved design decisions

1. `--thresholds` accepts floats (e.g., `0.8,0.9,0.95,0.99`) to be consistent with `--threshold`. Percentages would be a UX trap (forgetting the `/100`).

1. `--thresholds` accepts floats (e.g., `0.8,0.9,0.95,0.99`) to be consistent with `--threshold`. Percentages would be a UX trap (forgetting the `/100`).

2. Plateau is on by default (see CLI table); `--no-plateau` is the opt-out. Reasoning: it's cheap, informational, and only adds a few lines of output. Users who find it noisy can opt out.

3. Python `--compare` reports only the headline 95% sweet spot (mean/std/min/max). Power users who want per-threshold comparison across runs can use `--file` on each run individually, or we can add `--compare-sensitivity` later if demand exists.
