# Statistical Rigor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-freq min/max/IQR stats, multi-threshold sensitivity sweep, piecewise-linear plateau detection, per-sample raw-data dump, and a Python cross-run comparison mode — all in C, with a zero-dependency Python wrapper for cross-run aggregation only.

**Architecture:** All analysis that runs on a single run's data lives in `memfreq_bench.c` as static functions. The TSV output gets four new `#`-prefixed sections appended after the existing summary. Python gets a single new `--compare` argparse path that reads N result files and prints cross-run sweet-spot statistics. No new build system, no new files except a small test fixture, no new dependencies.

**Tech Stack:** C (single file, `gcc -O2`), Python 3 stdlib (`argparse`, `json`, `re`, `statistics`, `sys`).

**Spec:** `docs/superpowers/specs/2026-06-13-stats-and-sensitivity-design.md`

---

## File structure

| File | Change | Purpose |
|------|--------|---------|
| `memfreq_bench.c` | Modify | CLI parsing, 4 new static helpers (`percentile`, `fit_line`, `find_sweet_spot`, `detect_plateau`), 4 new output blocks. |
| `memfreq_sweep.py` | Modify | New `--compare` argparse path + `compare_runs()` function. |
| `docs/memfreq-bench.md` | Modify | Document new flags and output sections. |
| `tests/fixtures/compare_run1.json` | Create | Test fixture: 1st run's JSON, sweet spot 2000 MHz. |
| `tests/fixtures/compare_run2.json` | Create | Test fixture: 2nd run's JSON, sweet spot 1950 MHz. |
| `tests/fixtures/compare_run3.json` | Create | Test fixture: 3rd run's JSON, sweet spot 2050 MHz. |

No new build system. `make` still produces the same single binary.

## Conventions for this plan

- All new C functions are `static` and placed in a new section just before `main()` (after `usage()` at line 1657). Each function is self-contained.
- All new C output uses `printf` (matches existing style).
- All new Python code is stdlib only.
- Each task ends with a working `make` and a verifiable runtime check.
- TDD in this project is e2e: "run the binary with new flag, grep output for expected pattern". The "failing test" is "binary does not yet emit the expected section" (i.e., the test passes when grep finds zero matches until the implementation lands, then finds matches).

---

## Task 1: Add CLI flag infrastructure (variables, getopt, --thresholds parsing, usage)

**Files:**
- Modify: `memfreq_bench.c:1681-1714` (variables + getopt loop)
- Modify: `memfreq_bench.c:1658-1677` (usage text)

- [ ] **Step 1: Add storage variables**

In `memfreq_bench.c`, after line 1693 (after `int numa_node = -1;`), add:

```c
	double  threshold      = 0.95;  /* user-overridable sweet-spot threshold */
	int     n_user_thresholds = 0;
	double  user_thresholds[16];
	int     emit_raw       = 0;     /* --emit-raw: per-sample data in output */
	int     no_plateau     = 0;     /* --no-plateau: suppress plateau block */
```

- [ ] **Step 2: Extend the getopt option string**

Replace the getopt string at line 1696 from:
```c
while ((opt = getopt(argc, argv, "c:N:m:As:t:n:S:B:CRfFh")) != -1) {
```

To:
```c
while ((opt = getopt(argc, argv, "c:N:m:As:t:n:S:B:CRfFhT:L:rP")) != -1) {
```

Added: `T:` (takes argument, the --threshold float), `L:` (takes argument, the --thresholds list), `r` (no arg, --emit-raw), `P` (no arg, --no-plateau).

- [ ] **Step 3: Add the four new getopt cases**

Add to the switch block (before `case 'h':`) the following cases:

```c
		case 'T': threshold = atof(optarg);
		          if (threshold <= 0.0 || threshold > 1.0) {
		              dprintf("ERROR: --threshold must be in (0, 1], got %s\n", optarg);
		              return 1;
		          }
		          break;
		case 'L': {
		          char *saveptr = NULL;
		          char *tok = strtok(optarg, ",");
		          while (tok && n_user_thresholds < 16) {
		              double v = atof(tok);
		              if (v <= 0.0 || v > 1.0) {
		                  dprintf("ERROR: --thresholds values must be in (0, 1], got %s\n", tok);
		                  return 1;
		              }
		              user_thresholds[n_user_thresholds++] = v;
		              tok = strtok(NULL, ",");
		          }
		          break;
		      }
		case 'r': emit_raw   = 1;            break;
		case 'P': no_plateau = 1;            break;
```

- [ ] **Step 4: Update usage() text**

In `usage()` (line 1658), add these lines to the format string before the `"-h          This help\n"` line:

```c
	"  -T FRAC     Sweet-spot threshold in (0, 1]   (default: 0.95)\n"
	"  -L LIST     Multi-threshold sweep, e.g. 0.8,0.9,0.95,0.99 (max 16)\n"
	"  -r          Emit per-sample raw data (for custom analysis)\n"
	"  -P          Suppress plateau detection block\n"
```

- [ ] **Step 5: Compile**

```bash
make clean && make
```

Expected: clean compile, no warnings. The `-Wall -Wextra` flags will catch unused-variable issues if Step 3 missed a case.

- [ ] **Step 6: Verify new flags appear in --help**

```bash
./memfreq_bench -h 2>&1 | head -20
```

Expected: output contains the four new flag lines.

- [ ] **Step 7: Verify --threshold validation**

```bash
./memfreq_bench -T 1.5 2>&1 | head -3
```

Expected: `ERROR: --threshold must be in (0, 1], got 1.5` and exit code 1.

- [ ] **Step 8: Verify --thresholds parsing**

```bash
./memfreq_bench -L 0.8,0.9,0.95 2>&1 | head -3
```

Expected: no error (the flag is accepted; remaining errors about cpufreq are expected when running without sudo on a dev box). On a real Linux box with cpufreq, this would proceed to the sweep.

- [ ] **Step 9: Commit**

```bash
git add memfreq_bench.c
git commit -m "Add CLI infrastructure for stats/sensitivity/plateau flags"
```

---

## Task 2: Extract `find_sweet_spot()` and wire `--threshold`

**Files:**
- Modify: `memfreq_bench.c:2080-2097` (replace inline sweet-spot detection with function call)
- Modify: `memfreq_bench.c` (add `find_sweet_spot()` before `main()`, around line 1657)

- [ ] **Step 1: Add the helper function**

Insert this static function just before `main()` (after `usage()` at line 1677):

```c
/*
 * Find the lowest frequency (in kHz) whose throughput is ≥ threshold × max.
 * Returns 0 if no such frequency exists (no plateau, e.g. compute-bound).
 * The data arrays are indexed by frequency point, in ascending-freq order
 * (same order as `results[]` in main()).
 */
static int find_sweet_spot(const double *mops, const int *freqs_khz,
                           int n, int *out_index, double threshold)
{
	if (n <= 0 || threshold <= 0.0)
		return 0;

	/* max throughput across all valid points */
	double mx = -INFINITY;
	for (int i = 0; i < n; i++)
		if (mops[i] > mx)
			mx = mops[i];

	if (mx <= 0.0 || !isfinite(mx))
		return 0;

	/* lowest freq that meets threshold */
	for (int i = 0; i < n; i++) {
		if (mops[i] >= mx * threshold) {
			*out_index = i;
			return freqs_khz[i];
		}
	}
	return 0;
}
```

- [ ] **Step 2: Add `<math.h>` for `INFINITY` and `isfinite`**

Verify that `math.h` is already included. The existing file at line 40 has `#include <math.h>`. No change needed.

- [ ] **Step 3: Replace inline sweet-spot detection with function call**

In `main()` at lines 2080-2097, replace:

```c
	double THRESHOLD = 0.95;   /* 5 % tolerance for sweet spot */
	int stride_sweet = 0, chase_sweet = 0;

	/* find sweet spots (lowest valid freq within threshold of max) */
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid)
			continue;
		if (!stride_sweet &&
		    results[fi].stride_tput >= s_max * THRESHOLD)
			stride_sweet = results[fi].freq_khz;
		if (do_chase && !chase_sweet &&
		    results[fi].chase_tput >= c_max * THRESHOLD)
			chase_sweet = results[fi].freq_khz;
	}
```

With:

```c
	/* Per-workload throughput arrays (descending-freq order in results[],
	 * but find_sweet_spot() walks the array linearly so order doesn't matter
	 * as long as we pass valid data). We build ascending-freq arrays. */
	double stride_mops[MAX_FREQS], chase_mops[MAX_FREQS], random_mops[MAX_FREQS], compute_mops[MAX_FREQS];
	int    freqs_khz[MAX_FREQS];
	int    n_valid = 0;
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		stride_mops[n_valid]  = results[fi].stride_tput;
		chase_mops[n_valid]   = results[fi].chase_tput;
		random_mops[n_valid]  = results[fi].random_tput;
		compute_mops[n_valid] = results[fi].compute_tput;
		freqs_khz[n_valid]    = results[fi].freq_khz;
		n_valid++;
	}

	int stride_sweet_idx = -1, chase_sweet_idx = -1;
	int stride_sweet = find_sweet_spot(stride_mops, freqs_khz, n_valid,
	                                   &stride_sweet_idx, threshold);
	int chase_sweet  = do_chase
		? find_sweet_spot(chase_mops, freqs_khz, n_valid,
		                  &chase_sweet_idx, threshold)
		: 0;
```

- [ ] **Step 4: Update the summary print to use the new `threshold` variable**

At line 2172, change:

```c
	printf("# === Sweet spot (lowest freq ≥ %.0f%% of max throughput) ===\n",
	       THRESHOLD * 100);
```

To:

```c
	printf("# === Sweet spot (lowest freq ≥ %.0f%% of max throughput) ===\n",
	       threshold * 100);
```

- [ ] **Step 5: Compile**

```bash
make clean && make
```

Expected: clean compile.

- [ ] **Step 6: Verify default behavior is unchanged**

On a Linux box with cpufreq:
```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 2>&1 | tail -10
```

Expected: identical sweet spot output to before refactor. (You'll need to compare against a saved baseline if you have one; otherwise, just verify the summary line says "≥ 95%".)

- [ ] **Step 7: Verify --threshold changes the sweet spot**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 -T 0.80 2>&1 | grep "Sweet spot"
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 -T 0.99 2>&1 | grep "Sweet spot"
```

Expected: the second command's sweet spot is ≥ the first's. For a memory-bound workload, both will be lower than the default 0.95 case but the 0.99 sweet spot is the highest (closer to max freq).

- [ ] **Step 8: Commit**

```bash
git add memfreq_bench.c
git commit -m "Extract find_sweet_spot() and wire --threshold CLI"
```

---

## Task 3: Add per-freq min/max/IQR stats

**Files:**
- Modify: `memfreq_bench.c` (add `percentile()` helper before main())
- Modify: `memfreq_bench.c:1869-1961` (extend `struct result` and the inner sample loops)
- Modify: `memfreq_bench.c:2168-2194` (emit per-freq stats block after the data row loop)

- [ ] **Step 1: Add `percentile()` helper**

Insert this static function just before `main()` (after `find_sweet_spot()`):

```c
/*
 * Linear-interpolation percentile, matching numpy's default (method='linear').
 * `sorted` must be in ascending order. `p` in [0, 1].
 * For nsamples < 3, returns 0.0 (insufficient data for IQR).
 */
static double percentile(const double *sorted, int n, double p)
{
	if (n <= 0 || p < 0.0 || p > 1.0)
		return 0.0;
	if (n == 1)
		return sorted[0];
	double idx = p * (n - 1);
	int lo = (int)idx;
	double frac = idx - lo;
	if (lo + 1 >= n)
		return sorted[n - 1];
	return sorted[lo] * (1.0 - frac) + sorted[lo + 1] * frac;
}
```

- [ ] **Step 2: Extend `struct result` with min/max/IQR fields**

Find the definition of `struct result` (search for `struct result` in the file; it's defined before `main()`). Add to the end of the struct:

```c
	double stride_min,  stride_max,  stride_iqr;
	double chase_min,   chase_max,   chase_iqr;
	double random_min,  random_max,  random_iqr;
	double compute_min, compute_max, compute_iqr;
```

(If `struct result` is not visible to `main()` because it's in an unusual place, you may need to add it. Search for `struct result {` in the file to locate the definition.)

- [ ] **Step 3: Initialize new fields in the result-zeroing loop**

At line 1880-1885, extend the existing per-fi initialization:

```c
	for (int fi = nfreqs - 1; fi >= 0; fi--) {
		results[fi].freq_khz = freqs[fi];
		results[fi].valid = 0;
		results[fi].actual_khz = 0;
		results[fi].energy_uj = 0;
		results[fi].idle_power_uw = 0;
		results[fi].load_power_uw = 0;
		results[fi].stride_min = 0; results[fi].stride_max = 0; results[fi].stride_iqr = 0;
		results[fi].chase_min  = 0; results[fi].chase_max  = 0; results[fi].chase_iqr  = 0;
		results[fi].compute_min= 0; results[fi].compute_max= 0; results[fi].compute_iqr= 0;
```

- [ ] **Step 4: Compute min/max/IQR in the per-freq sample loops**

After each `qsort(buf, nsamples, sizeof(double), cmp_double); results[fi].X_tput = buf[nsamples / 2];` block, immediately add the stats computation. There are four such blocks (stride, chase, random, compute). For each one, add right after the `results[fi].X_tput = ...` line:

For stride (after line 1936):
```c
		results[fi].stride_min = buf[0];
		results[fi].stride_max = buf[nsamples - 1];
		results[fi].stride_iqr = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;
```

For chase (after line 1945):
```c
		results[fi].chase_min  = buf[0];
		results[fi].chase_max  = buf[nsamples - 1];
		results[fi].chase_iqr  = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;
```

For random (after line 1954):
```c
		results[fi].random_min  = buf[0];
		results[fi].random_max  = buf[nsamples - 1];
		results[fi].random_iqr  = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;
```

For compute (after line 1961):
```c
		results[fi].compute_min = buf[0];
		results[fi].compute_max = buf[nsamples - 1];
		results[fi].compute_iqr = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;
```

- [ ] **Step 5: Emit the per-freq stats blocks after the summary**

At the end of the output section (after the interpretation guide at line 2194, before `free(buf);` at line 2196), insert:

```c
	/* ---- per-freq stats blocks ---- */
	printf("#\n# --- per-freq stats (stride) ---\n");
	printf("# freq_MHz  min_Mops  max_Mops  median_Mops  iqr_Mops\n");
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		printf("# %d\t%.1f\t%.1f\t%.1f\t%.2f\n",
		       results[fi].freq_khz / 1000,
		       results[fi].stride_min  / 1e6,
		       results[fi].stride_max  / 1e6,
		       results[fi].stride_tput / 1e6,
		       results[fi].stride_iqr  / 1e6);
	}
	if (do_chase) {
		printf("#\n# --- per-freq stats (chase) ---\n");
		printf("# freq_MHz  min_Mops  max_Mops  median_Mops  iqr_Mops\n");
		for (int fi = 0; fi < nfreqs; fi++) {
			if (!results[fi].valid) continue;
			printf("# %d\t%.1f\t%.1f\t%.1f\t%.2f\n",
			       results[fi].freq_khz / 1000,
			       results[fi].chase_min  / 1e6,
			       results[fi].chase_max  / 1e6,
			       results[fi].chase_tput / 1e6,
			       results[fi].chase_iqr  / 1e6);
		}
	}
	if (do_random) {
		printf("#\n# --- per-freq stats (random) ---\n");
		printf("# freq_MHz  min_Mops  max_Mops  median_Mops  iqr_Mops\n");
		for (int fi = 0; fi < nfreqs; fi++) {
			if (!results[fi].valid) continue;
			printf("# %d\t%.1f\t%.1f\t%.1f\t%.2f\n",
			       results[fi].freq_khz / 1000,
			       results[fi].random_min  / 1e6,
			       results[fi].random_max  / 1e6,
			       results[fi].random_tput / 1e6,
			       results[fi].random_iqr  / 1e6);
		}
	}
	printf("#\n# --- per-freq stats (compute) ---\n");
	printf("# freq_MHz  min_Mops  max_Mops  median_Mops  iqr_Mops\n");
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		printf("# %d\t%.1f\t%.1f\t%.1f\t%.2f\n",
		       results[fi].freq_khz / 1000,
		       results[fi].compute_min  / 1e6,
		       results[fi].compute_max  / 1e6,
		       results[fi].compute_tput / 1e6,
		       results[fi].compute_iqr  / 1e6);
	}
```

- [ ] **Step 6: Compile**

```bash
make clean && make
```

Expected: clean compile.

- [ ] **Step 7: Run with -n 5 and verify the stats block appears**

On a Linux box with cpufreq:
```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 5 2>&1 | tail -30
```

Expected: the output contains:
- A line `# --- per-freq stats (stride) ---`
- A header `# freq_MHz  min_MOps  max_MOps  median_MOps  iqr_MOps`
- One data row per valid frequency point
- A `# --- per-freq stats (chase) ---` block (if `do_chase`)
- A `# --- per-freq stats (compute) ---` block

- [ ] **Step 8: Sanity-check numeric relationships**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 5 2>&1 | grep -A 20 "per-freq stats (stride)" | head -10
```

For each row, verify: `min ≤ median ≤ max` and `iqr ≥ 0`. If any row violates, bug.

- [ ] **Step 9: Commit**

```bash
git add memfreq_bench.c
git commit -m "Add per-freq min/max/IQR stats blocks"
```

---

## Task 4: Add multi-threshold sensitivity sweep

**Files:**
- Modify: `memfreq_bench.c` (extend the output section after the per-freq stats blocks)

- [ ] **Step 1: Add the sensitivity block emission**

In the output section, immediately after the per-freq stats blocks added in Task 3 (before the `free(buf);` at line 2196), insert:

```c
	/* ---- sensitivity block (only if --thresholds was given) ---- */
	if (n_user_thresholds > 0) {
		printf("#\n# --- sensitivity ---\n");
		/* per-workload: stride, chase (if enabled), random (if enabled), compute */
		const char *labels[] = {"stride", "chase", "random", "compute"};
		double *arrs[]      = {stride_mops, chase_mops, random_mops, compute_mops};
		int     enabled[]   = {1, do_chase ? 1 : 0, do_random ? 1 : 0, 1};
		for (int w = 0; w < 4; w++) {
			if (!enabled[w]) continue;
			printf("#\n# --- sensitivity (%s) ---\n", labels[w]);
			printf("# threshold  sweet_spot_MHz\n");
			for (int ti = 0; ti < n_user_thresholds; ti++) {
				int idx = -1;
				int khz = find_sweet_spot(arrs[w], freqs_khz, n_valid,
				                          &idx, user_thresholds[ti]);
				if (khz > 0)
					printf("# %.3f      %d\n",
					       user_thresholds[ti], khz / 1000);
				else
					printf("# %.3f      —\n", user_thresholds[ti]);
			}
		}
	}
```

- [ ] **Step 2: Declare the new local array `random_mops[]`**

`random_mops[]` is already declared in Task 2 Step 3 (alongside the other per-workload arrays). No additional declaration needed.

Note: do not redeclare it here, or the C compiler will reject the duplicate definition.

- [ ] **Step 3: Compile**

```bash
make clean && make
```

Expected: clean compile.

- [ ] **Step 4: Run with --thresholds and verify**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 -L 0.8,0.9,0.95,0.99 2>&1 | grep -A 5 "sensitivity (stride)"
```

Expected: output shows the sensitivity block with 4 rows, sweet_spot_MHz values non-decreasing.

- [ ] **Step 5: Verify that omitting --thresholds suppresses the block**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 2>&1 | grep -c "sensitivity"
```

Expected: `0` (the block is only emitted when `--thresholds` is given).

- [ ] **Step 6: Commit**

```bash
git add memfreq_bench.c
git commit -m "Add multi-threshold sensitivity sweep output"
```

---

## Task 5: Add piecewise-linear plateau detection

**Files:**
- Modify: `memfreq_bench.c` (add `fit_line()` and `detect_plateau()` before main())
- Modify: `memfreq_bench.c` (emit plateau block in the output section)

- [ ] **Step 1: Add `fit_line()` helper**

Insert this static function just before `main()` (after `percentile()`):

```c
/*
 * Least-squares fit of y = slope*x + intercept.
 * n must be >= 2. Returns 0 on success, -1 if input is degenerate
 * (all x values equal → vertical line, no well-defined slope).
 */
static int fit_line(const double *x, const double *y, int n,
                    double *out_slope, double *out_intercept)
{
	if (n < 2)
		return -1;
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (int i = 0; i < n; i++) {
		sx  += x[i];
		sy  += y[i];
		sxx += x[i] * x[i];
		sxy += x[i] * y[i];
	}
	double denom = n * sxx - sx * sx;
	if (denom == 0.0)
		return -1;
	*out_slope     = (n * sxy - sx * sy) / denom;
	*out_intercept = (sy - (*out_slope) * sx) / n;
	return 0;
}

/*
 * SSE for a given piecewise fit at breakpoint index k.
 * Fits line to x[0..k], y[0..k] and x[k+1..n-1], y[k+1..n-1].
 * Returns total sum of squared residuals; -1.0 if any segment is too small.
 */
static double piecewise_sse(const double *x, const double *y, int n, int k)
{
	if (k < 1 || k >= n - 1)
		return -1.0;
	double s1, i1, s2, i2;
	if (fit_line(x, y, k + 1, &s1, &i1) < 0) return -1.0;
	if (fit_line(x, y + k + 1, n - k - 1, &s2, &i2) < 0) return -1.0;
	double sse = 0.0;
	for (int j = 0; j <= k; j++) {
		double r = y[j] - (s1 * x[j] + i1);
		sse += r * r;
	}
	for (int j = k + 1; j < n; j++) {
		double r = y[j] - (s2 * x[j] + i2);
		sse += r * r;
	}
	return sse;
}
```

- [ ] **Step 2: Add `detect_plateau()` helper**

Insert this static function just after `piecewise_sse()`:

```c
/*
 * Naive O(N^2) piecewise-linear plateau detection.
 * Finds the breakpoint k that minimizes total SSE of a two-segment fit.
 * Outputs:
 *   out_breakpoint_mhz: the freq (in MHz) at the breakpoint, or 0 if none
 *   out_slope_ratio:    |slope_left| / max(|slope_right|, 1e-9)
 *   out_sweet_spot_mhz: the 95% sweet spot for context (0 if no plateau)
 * Returns 0 if a plateau is detected (slope_ratio > 2.0), -1 otherwise.
 */
static int detect_plateau(const double *mops, const int *freqs_khz, int n,
                          double threshold,
                          int *out_breakpoint_mhz,
                          double *out_slope_ratio,
                          int *out_sweet_spot_mhz)
{
	if (n < 4)  /* need at least 2 points per segment */
		return -1;

	double x[MAX_FREQS], y[MAX_FREQS];
	for (int i = 0; i < n; i++) {
		x[i] = (double)freqs_khz[i] / 1000.0;  /* MHz */
		y[i] = mops[i] / 1e6;                  /* Mops */
	}

	/* find breakpoint k minimizing SSE */
	int best_k = -1;
	double best_sse = INFINITY;
	for (int k = 1; k < n - 1; k++) {
		double sse = piecewise_sse(x, y, n, k);
		if (sse >= 0.0 && sse < best_sse) {
			best_sse = sse;
			best_k = k;
		}
	}
	if (best_k < 0)
		return -1;

	/* compute slopes of the two segments at the best breakpoint */
	double s_left, i_left, s_right, i_right;
	fit_line(x, y, best_k + 1, &s_left, &i_left);
	fit_line(x + best_k + 1, y + best_k + 1,
	         n - best_k - 1, &s_right, &i_right);

	double slope_ratio = fabs(s_left) / fmax(fabs(s_right), 1e-9);
	*out_breakpoint_mhz = (int)x[best_k];
	*out_slope_ratio    = slope_ratio;

	int sweet_idx = -1;
	int sweet_khz = find_sweet_spot(mops, freqs_khz, n, &sweet_idx, threshold);
	*out_sweet_spot_mhz = sweet_khz > 0 ? sweet_khz / 1000 : 0;

	return slope_ratio > 2.0 ? 0 : -1;
}
```

- [ ] **Step 3: Emit the plateau block**

In the output section, after the sensitivity block (or after per-freq stats if no sensitivity was requested), insert:

```c
	/* ---- plateau block ---- */
	if (!no_plateau) {
		printf("#\n# --- plateau ---\n");
		struct { const char *name; const double *mops; } workloads[] = {
			{"stride",  stride_mops},
			{"chase",   chase_mops},
			{"random",  random_mops},
			{"compute", compute_mops},
		};
		int enabled[] = {1, do_chase ? 1 : 0, do_random ? 1 : 0, 1};
		for (int w = 0; w < 4; w++) {
			if (!enabled[w]) continue;
			int bp_mhz = 0, sweet_mhz = 0;
			double ratio = 0.0;
			int rc = detect_plateau(workloads[w].mops, freqs_khz, n_valid,
			                        threshold, &bp_mhz, &ratio, &sweet_mhz);
			if (rc == 0)
				printf("# %-8s plateau_breakpoint: %d MHz  (slope ratio %.1fx, 95%% sweet spot %d MHz)\n",
				       workloads[w].name, bp_mhz, ratio, sweet_mhz);
			else
				printf("# %-8s plateau_breakpoint: —  (no plateau; throughput keeps rising with frequency)\n",
				       workloads[w].name);
		}
	}
```

- [ ] **Step 4: Compile**

```bash
make clean && make
```

Expected: clean compile. Watch for unused-variable warnings on `s_left`, `i_left`, etc. (these are used via the `fit_line()` call, so should be fine).

- [ ] **Step 5: Run and verify the plateau block**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 2>&1 | grep -A 5 "--- plateau"
```

Expected: at least 2 lines (stride, chase, compute; random only if `-R` is given). For a memory-bound workload, `slope ratio` should be > 2.0. For compute, expect `—  (no plateau; ...)`.

- [ ] **Step 6: Verify --no-plateau suppresses the block**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 -P 2>&1 | grep -c "plateau"
```

Expected: `0`.

- [ ] **Step 7: Commit**

```bash
git add memfreq_bench.c
git commit -m "Add piecewise-linear plateau detection"
```

---

## Task 6: Add `--emit-raw` per-sample data dump

**Files:**
- Modify: `memfreq_bench.c` (extend the per-freq inner loops to record raw samples)
- Modify: `memfreq_bench.c` (emit raw_samples block in the output section)

- [ ] **Step 1: Allocate a raw-sample storage buffer**

In `main()`, just after `double *buf = malloc(nsamples * sizeof(*buf));` (around line 1870), add:

```c
	/* Raw sample storage: only allocated when --emit-raw is set.
	 * Layout: [freq_idx][workload][sample_idx], where workload order is
	 * 0=stride, 1=chase, 2=random, 3=compute. */
	double *raw = NULL;
	if (emit_raw)
		raw = calloc((size_t)nfreqs * 4 * nsamples, sizeof(double));
```

- [ ] **Step 2: Store raw samples in the inner loops**

For each of the four sample loops, after `qsort(buf, nsamples, sizeof(double), cmp_double);`, add a raw-storage step.

For stride (after line 1935):
```c
		if (raw) memcpy(raw + ((size_t)fi * 4 + 0) * nsamples, buf, nsamples * sizeof(double));
```

For chase (after line 1944):
```c
		if (raw) memcpy(raw + ((size_t)fi * 4 + 1) * nsamples, buf, nsamples * sizeof(double));
```

For random (after line 1953):
```c
		if (raw) memcpy(raw + ((size_t)fi * 4 + 2) * nsamples, buf, nsamples * sizeof(double));
```

For compute (after line 1960):
```c
		if (raw) memcpy(raw + ((size_t)fi * 4 + 3) * nsamples, buf, nsamples * sizeof(double));
```

- [ ] **Step 3: Emit the raw_samples block**

In the output section, after the plateau block, insert:

```c
	/* ---- raw samples (only if --emit-raw) ---- */
	if (raw) {
		const char *raw_labels[] = {"stride", "chase", "random", "compute"};
		int raw_enabled[] = {1, do_chase ? 1 : 0, do_random ? 1 : 0, 1};
		for (int w = 0; w < 4; w++) {
			if (!raw_enabled[w]) continue;
			printf("#\n# --- raw_samples (%s) ---\n", raw_labels[w]);
			printf("# freq_MHz  sample_idx  mops\n");
			for (int fi = 0; fi < nfreqs; fi++) {
				if (!results[fi].valid) continue;
				for (int s = 0; s < nsamples; s++) {
					printf("# %d\t%d\t%.1f\n",
					       results[fi].freq_khz / 1000, s + 1,
					       raw[((size_t)fi * 4 + w) * nsamples + s] / 1e6);
				}
			}
		}
	}
```

- [ ] **Step 4: Free the raw buffer at the end**

In the cleanup section (before the existing `free(buf);` and `free(results);`), add:

```c
	free(raw);
```

- [ ] **Step 5: Compile**

```bash
make clean && make
```

Expected: clean compile.

- [ ] **Step 6: Run with --emit-raw and verify the row count**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 3 -r 2>&1 > /tmp/run.txt
grep -c "^# " /tmp/run.txt
grep -A 1000 "raw_samples (stride)" /tmp/run.txt | head -3
echo "---"
n_data_lines=$(grep -A 1000 "raw_samples (stride)" /tmp/run.txt | tail -n +3 | grep -c "^[0-9]")
echo "stride data rows: $n_data_lines"
```

Expected: For 3 freqs and 3 samples, the stride block has 9 data rows. (For 4 valid freqs, 12 rows; the exact count depends on how many freq points succeeded.)

- [ ] **Step 7: Verify default mode does not emit raw samples**

```bash
sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 2>&1 | grep -c "raw_samples"
```

Expected: `0`.

- [ ] **Step 8: Commit**

```bash
git add memfreq_bench.c
git commit -m "Add --emit-raw per-sample data dump"
```

---

## Task 7: Add Python `--compare` mode

**Files:**
- Modify: `memfreq_sweep.py:161-196` (extend argparse and add compare path)
- Create: `tests/fixtures/compare_run1.json`
- Create: `tests/fixtures/compare_run2.json`
- Create: `tests/fixtures/compare_run3.json`

- [ ] **Step 1: Create test fixture files**

Create `tests/fixtures/compare_run1.json`:
```json
{
  "meta": {"cpu": "0", "array": "128MB", "stride": "8", "duration": "1s"},
  "sweet_spot_mhz": {"stride": 2000, "chase": 2000},
  "data": [
    {"freq_mhz": 800,  "stride_mops": 140.0, "stride_pct": 90.0, "chase_mops": 11.0, "chase_pct": 88.0, "compute_mops": 80.0, "compute_pct": 25.0},
    {"freq_mhz": 1200, "stride_mops": 150.0, "stride_pct": 96.0, "chase_mops": 12.0, "chase_pct": 95.0, "compute_mops": 120.0, "compute_pct": 38.0},
    {"freq_mhz": 1600, "stride_mops": 155.0, "stride_pct": 99.0, "chase_mops": 12.4, "chase_pct": 98.0, "compute_mops": 160.0, "compute_pct": 50.0},
    {"freq_mhz": 2000, "stride_mops": 156.0, "stride_pct": 100.0, "chase_mops": 12.6, "chase_pct": 100.0, "compute_mops": 200.0, "compute_pct": 62.0}
  ]
}
```

Create `tests/fixtures/compare_run2.json` (sweet spot 1950):
```json
{
  "meta": {"cpu": "0", "array": "128MB", "stride": "8", "duration": "1s"},
  "sweet_spot_mhz": {"stride": 1950, "chase": 2000},
  "data": [
    {"freq_mhz": 800,  "stride_mops": 138.0, "stride_pct": 88.0, "chase_mops": 11.2, "chase_pct": 89.0, "compute_mops": 82.0, "compute_pct": 26.0},
    {"freq_mhz": 1200, "stride_mops": 148.0, "stride_pct": 95.0, "chase_mops": 12.1, "chase_pct": 96.0, "compute_mops": 121.0, "compute_pct": 38.0},
    {"freq_mhz": 1600, "stride_mops": 155.0, "stride_pct": 99.5, "chase_mops": 12.5, "chase_pct": 99.0, "compute_mops": 159.0, "compute_pct": 50.0},
    {"freq_mhz": 1950, "stride_mops": 155.7, "stride_pct": 100.0, "chase_mops": 12.6, "chase_pct": 100.0, "compute_mops": 195.0, "compute_pct": 61.0}
  ]
}
```

Create `tests/fixtures/compare_run3.json` (sweet spot 2050):
```json
{
  "meta": {"cpu": "0", "array": "128MB", "stride": "8", "duration": "1s"},
  "sweet_spot_mhz": {"stride": 2050, "chase": 2000},
  "data": [
    {"freq_mhz": 800,  "stride_mops": 141.0, "stride_pct": 91.0, "chase_mops": 10.9, "chase_pct": 87.0, "compute_mops": 79.0, "compute_pct": 25.0},
    {"freq_mhz": 1200, "stride_mops": 152.0, "stride_pct": 97.0, "chase_mops": 11.9, "chase_pct": 94.0, "compute_mops": 119.0, "compute_pct": 37.0},
    {"freq_mhz": 1600, "stride_mops": 154.0, "stride_pct": 98.0, "chase_mops": 12.3, "chase_pct": 97.0, "compute_mops": 161.0, "compute_pct": 51.0},
    {"freq_mhz": 2050, "stride_mops": 156.5, "stride_pct": 100.0, "chase_mops": 12.7, "chase_pct": 100.0, "compute_mops": 205.0, "compute_pct": 64.0}
  ]
}
```

- [ ] **Step 2: Add `--compare` argparse and dispatch**

In `memfreq_sweep.py`, replace the `main()` function (lines 161-196) with:

```python
def compare_runs(file_paths: list[str]) -> int:
    """Read N result files and print cross-run sweet-spot statistics."""
    import statistics as st
    runs = []
    for path in file_paths:
        with open(path) as fh:
            text = fh.read()
        # Autodetect: JSON if first non-whitespace char is '{', else TSV.
        stripped = text.lstrip()
        if stripped.startswith("{"):
            runs.append(json.loads(text))
        else:
            runs.append(parse_output(text))

    if not runs:
        print("ERROR: no input files", file=sys.stderr)
        return 1

    # Normalize each run to a common shape: {"meta", "sweet": {workload: mhz}}.
    # The --json output uses "sweet_spot_mhz"; the TSV parser produces "sweet".
    normalized = []
    for r in runs:
        sweet = r.get("sweet") or r.get("sweet_spot_mhz") or {}
        normalized.append({"sweet": sweet, "meta": r.get("meta", {})})

    # Collect sweet-spot values per workload across runs.
    workloads = ["stride", "chase"]
    print("=" * 78)
    print(f"  Cross-run sweet-spot comparison ({len(normalized)} runs)")
    print("=" * 78)
    print(f"{'workload':<10} {'mean_MHz':>10} {'std_MHz':>10} "
          f"{'min_MHz':>10} {'max_MHz':>10} {'range_MHz':>10}")
    for w in workloads:
        values = [n["sweet"].get(w) for n in normalized if n["sweet"].get(w)]
        if not values:
            print(f"{w:<10} {'—':>10} {'—':>10} {'—':>10} {'—':>10} {'—':>10}")
            continue
        mean_v = st.mean(values)
        std_v  = st.stdev(values) if len(values) >= 2 else None
        min_v  = min(values)
        max_v  = max(values)
        print(f"{w:<10} {mean_v:>10.0f} "
              f"{(std_v if std_v is not None else float('nan')):>10.1f} "
              f"{min_v:>10.0f} {max_v:>10.0f} {max_v - min_v:>10.0f}")
    print()
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Run memfreq_bench and visualize sweet spot")
    parser.add_argument("--file", "-f",
                        help="Parse existing output file instead of running")
    parser.add_argument("--json", "-j", action="store_true",
                        help="Also save results as JSON")
    parser.add_argument("--compare", "-c", nargs="+", metavar="FILE",
                        help="Compare N result files (JSON or TSV) and "
                             "report cross-run sweet-spot statistics")
    args, extra = parser.parse_known_args()

    if args.compare:
        return compare_runs(args.compare)

    if args.file:
        with open(args.file) as fh:
            text = fh.read()
    else:
        text = run_bench(extra)

    data = parse_output(text)
    if not data["rows"]:
        print("ERROR: no data rows parsed", file=sys.stderr)
        sys.exit(1)

    visualize(data)

    if args.json:
        out = {
            "meta": data["meta"],
            "sweet_spot_mhz": data["sweet"],
            "data": data["rows"],
        }
        jpath = "memfreq_results.json"
        with open(jpath, "w") as f:
            json.dump(out, f, indent=2)
        print(f"  JSON saved to {jpath}")


if __name__ == "__main__":
    main()
```

Note: the `import statistics as st` is at the top of `compare_runs` (not at module top) to keep it out of the import block — though moving it to the top is also fine. Choose whichever you prefer; both are stdlib.

- [ ] **Step 3: Test --compare with 3 fixtures**

```bash
python3 memfreq_sweep.py --compare \
  tests/fixtures/compare_run1.json \
  tests/fixtures/compare_run2.json \
  tests/fixtures/compare_run3.json
```

Expected output:
```
==============================================================================
  Cross-run sweet-spot comparison (3 runs)
==============================================================================
workload   mean_MHz    std_MHz    min_MHz    max_MHz  range_MHz
stride         2000        50       1950       2050        100
chase          2000         0       2000       2000          0
```

(The std is sample stdev: 50.0. If your values are 2000, 1950, 2050, mean=2000, variance=((0)² + (-50)² + (50)²)/2 = 2500, stdev=50.0.)

- [ ] **Step 4: Test --compare with a single file (edge case)**

```bash
python3 memfreq_sweep.py --compare tests/fixtures/compare_run1.json
```

Expected: `std_MHz` shows `nan` (only 1 run, stdev undefined). All other columns show numbers. (Behavior depends on how the script handles `float('nan')`; Python should print it as `nan`.)

- [ ] **Step 5: Test --compare with mixed JSON and TSV files**

(Only if you have a TSV file handy. If not, skip this step.) Create a TSV file from a real run (`sudo ./memfreq_bench -c 0 -m 128 -t 1 -n 1 > /tmp/run.txt`) and run:
```bash
python3 memfreq_sweep.py --compare tests/fixtures/compare_run1.json /tmp/run.txt
```

Expected: works without error.

- [ ] **Step 6: Verify --file mode still works (regression check)**

```bash
python3 memfreq_sweep.py --file tests/fixtures/compare_run1.json
```

Expected: parses the JSON, displays the visualization. (Note: passing JSON to `--file` works because the parser autodetects JSON.)

- [ ] **Step 7: Commit**

```bash
git add memfreq_sweep.py tests/fixtures/compare_run*.json
git commit -m "Add Python --compare mode for cross-run aggregation"
```

---

## Task 8: Update documentation

**Files:**
- Modify: `docs/memfreq-bench.md` (add new flags to parameters table, add new output sections, add usage examples)

- [ ] **Step 1: Add new flags to the parameters table**

In `docs/memfreq-bench.md`, find the "参数" (Parameters) table (around line 250). Add these rows:

```
| `-T FRAC` | 0.95 | sweet-spot 阈值,(0, 1] 范围内 |
| `-L LIST` | — | 多阈值扫描,逗号分隔,例如 `0.8,0.9,0.95,0.99`,最多 16 个 |
| `-r` | — | 输出每样本原始数据(用于自定义分析) |
| `-P` | — | 抑制 plateau 检测输出(默认开启) |
```

- [ ] **Step 2: Add a "Statistical Output" section after the existing "Output" explanations**

After the section that explains `target_MHz / actual_MHz / stride_MBs` (around line 300), add:

```markdown
### 新增统计段(从 `gcc -O2 -o memfreq_bench memfreq_bench.c -lm` 重新编译后可用)

#### 1. `# --- per-freq stats (workload) ---`

每个频率点的 min/max/IQR。IQR = Q3 − Q1。看 IQR 列判断测量噪声:
- IQR 全程 < 1% → 机器安静,数据可信
- 低频点 IQR 突然变大 → 该频点测量置信度低,谨慎使用

#### 2. `# --- sensitivity (workload) ---`(仅 `-L` 时出现)

不同阈值下的甜点。判断决策鲁棒性:
- 0.80 和 0.99 给的甜点差 < 100 MHz → 决策鲁棒
- 差 > 500 MHz → 工作负载对阈值敏感,挑高的阈值更安全

#### 3. `# --- plateau ---`

分段线性检测,给出 `plateau_breakpoint` 和 `slope ratio`:
- `slope ratio > 2.0` → 有明显平台期,mem-bound 信号强
- `—`  → 无平台,吞吐随频率持续上升(典型 compute-bound)
- 95% sweet spot 同时给出作为对照;两个独立方法一致 = 强证据

#### 4. `# --- raw_samples (workload) ---`(仅 `-r` 时出现)

每个 freq × sample 的原始吞吐。用于自定义分析(bootstrap CI、分布检验);Python `--compare` 不需要这个。
```

- [ ] **Step 3: Add a Python `--compare` example**

In the "可视化" (Visualization) section (around line 270), add:

```markdown
### 跨 run 对比

```bash
# 比较 3 次完整 sweep 的甜点稳定性
python3 memfreq_sweep.py --compare run1.json run2.json run3.json
```

输出 mean/std/min/max across runs。JSON 来自 `memfreq_sweep.py --json`(单次 sweep 后自动产生)。
```

- [ ] **Step 4: Verify the docs build (manual)**

```bash
cat docs/memfreq-bench.md | grep -A 2 "^-T FRAC"
cat docs/memfreq-bench.md | grep -A 2 "sensitivity (workload)"
```

Expected: both grep matches return content.

- [ ] **Step 5: Commit**

```bash
git add docs/memfreq-bench.md
git commit -m "Document new stats/sensitivity/plateau flags and Python --compare"
```

---

## Task 9: End-to-end smoke test

**Files:**
- Create: `tests/test_stats_output.sh` (lightweight test script)

- [ ] **Step 1: Create a test runner script**

Create `tests/test_stats_output.sh`:

```bash
#!/bin/bash
# tests/test_stats_output.sh
# Lightweight e2e test for new --threshold, --thresholds, --emit-raw,
# --no-plateau flags, and Python --compare mode.
#
# Does NOT require root or cpufreq — only checks that the binary accepts
# the new flags, the output is well-formed, and Python --compare works on
# the fixture files. For full validation, run on a Linux box with cpufreq.
#
# Usage: bash tests/test_stats_output.sh

set -u
PASS=0
FAIL=0

check() {
    local name="$1"
    local got="$2"
    local want="$3"
    if [[ "$got" == *"$want"* ]]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected to contain: $want"
        echo "    got: $got"
        FAIL=$((FAIL + 1))
    fi
}

BIN="${BIN:-./memfreq_bench}"
PY="${PY:-python3}"

echo "=== Test 1: --help shows new flags ==="
HELP=$($BIN -h 2>&1)
check "help mentions -T"      "$HELP" "-T FRAC"
check "help mentions -L"      "$HELP" "-L LIST"
check "help mentions -r"      "$HELP" "-r          "
check "help mentions -P"      "$HELP" "-P"

echo "=== Test 2: --threshold validation ==="
ERR=$($BIN -T 1.5 2>&1)
check "rejects threshold > 1" "$ERR" "ERROR: --threshold must be in (0, 1]"

echo "=== Test 3: --thresholds validation ==="
ERR=$($BIN -L 0.5,1.5 2>&1)
check "rejects thresholds > 1" "$ERR" "ERROR: --thresholds values must be in (0, 1]"

echo "=== Test 4: Python --compare with 3 fixture files ==="
OUT=$($PY memfreq_sweep.py --compare \
    tests/fixtures/compare_run1.json \
    tests/fixtures/compare_run2.json \
    tests/fixtures/compare_run3.json 2>&1)
check "shows 'Cross-run sweet-spot comparison'" "$OUT" "Cross-run sweet-spot comparison (3 runs)"
check "shows stride mean"   "$OUT" "stride"
check "shows chase mean"    "$OUT" "chase"
check "shows range"         "$OUT" "range_MHz"

echo "=== Test 5: Python --compare with 1 file (edge case) ==="
OUT=$($PY memfreq_sweep.py --compare \
    tests/fixtures/compare_run1.json 2>&1)
check "1-file compare does not crash" "$OUT" "Cross-run sweet-spot comparison (1 runs)"

echo
echo "=========================================="
echo "  Results: $PASS passed, $FAIL failed"
echo "=========================================="
exit $FAIL
```

- [ ] **Step 2: Make the test script executable and run it**

```bash
chmod +x tests/test_stats_output.sh
bash tests/test_stats_output.sh
```

Expected: All tests pass (or all but the help-format check, which depends on exact column alignment in usage() — see Step 3 below).

- [ ] **Step 3: If a test fails on the `-r` or `-P` help check (column alignment)**

The usage() format string has fixed-width columns. If the test fails on whitespace matching, run:

```bash
./memfreq_bench -h 2>&1 | grep -E "^\s*-[rP]\s+"
```

If the output is `  -r          Emit per-sample...` but the test expects `-r          ` with two trailing spaces, adjust the test's expected string to match. This is a known fragility of testing fixed-width text.

- [ ] **Step 4: Run the full existing test suite to check for regressions**

```bash
bash run_all_tests.sh --quick --yes 2>&1 | tail -20
```

Expected: completes without error. (This requires a Linux box with cpufreq; on macOS the script will fail at the first `sudo` step, which is expected.)

- [ ] **Step 5: Commit the test script**

```bash
git add tests/test_stats_output.sh
git commit -m "Add lightweight e2e test for new stats/sensitivity flags"
```

---

## Self-review checklist

**Spec coverage:**

| Spec section | Task |
|--------------|------|
| CLI flag `--threshold F` | Task 1 (parsing) + Task 2 (wiring) |
| CLI flag `--thresholds L` | Task 1 (parsing) + Task 4 (emission) |
| CLI flag `--emit-raw` | Task 1 (parsing) + Task 6 (emission) |
| CLI flag `--no-plateau` | Task 1 (parsing) + Task 5 (gate) |
| Block 1: per-freq stats | Task 3 |
| Block 2: sensitivity | Task 4 |
| Block 3: plateau | Task 5 |
| Block 4: raw_samples | Task 6 |
| Python `--compare` | Task 7 |
| Stdlib only | Task 7 (verified) |
| Backward compat | All tasks (no change to existing data rows) |
| Test scenarios (8 e2e) | Task 9 (test script covers 5; full e2e on Linux box) |
| Docs update | Task 8 |

**Placeholders:** None. All code blocks are concrete. All commands have expected output.

**Type/signature consistency:**
- `find_sweet_spot()` signature used consistently in Tasks 2, 4, 5
- `fit_line()` signature consistent between Tasks 5
- `percentile()` signature used in Task 3
- `emit_raw`, `no_plateau`, `threshold`, `user_thresholds[]`, `n_user_thresholds` are referenced consistently across Tasks 1-6
- The `random_mops[]` array introduced in Task 4 (and used in Task 5) is declared in Task 4 Step 2

**Possible issue flagged:** Task 6 Step 2 references `random_mops[]` indirectly. Verify in Task 6 that the random loop's raw sample memcpy uses index `fi * 4 + 2` (not stride or chase). Reviewer: confirm the workload order in the raw buffer layout matches the labels emitted in Task 6 Step 3.

---

## Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-13-stats-and-sensitivity.md`. Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — execute tasks in this session, batch execution with checkpoints

Which approach?
