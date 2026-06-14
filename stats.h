/*
 * stats.h — Sweet-spot, plateau, and bootstrap confidence-interval helpers.
 *
 * All routines here are pure functions of their inputs (no global state).
 * They were extracted from memfreq_bench.c to keep that file's main()
 * focused on orchestration.
 *
 * Public API:
 *   - cmp_double:           qsort comparator for doubles
 *   - percentile:           linear-interpolation percentile (numpy default)
 *   - find_sweet_spot:      lowest freq (kHz) with throughput >= thr * max
 *   - bootstrap_sweet_spot_ci: 95% CI on sweet-spot freq via bootstrap
 *   - detect_plateau:       piecewise-linear plateau + slope ratio
 *
 * Constants MAX_FREQS and MAX_SAMPLES are also defined here because
 * detect_plateau and bootstrap_sweet_spot_ci need fixed-size local arrays.
 * memfreq_bench.c also uses MAX_FREQS for its per-workload mops[] arrays,
 * so it picks up the constant via #include "stats.h".
 */
#ifndef MEMFREQ_STATS_H
#define MEMFREQ_STATS_H

#define MAX_FREQS 256        /* max frequency points in any array */
#define MAX_SAMPLES 16       /* max samples per freq (bootstrap resampling) */

/*
 * qsort comparator for doubles in ascending order. Used by both main()
 * (for median finding across nsamples) and bootstrap_sweet_spot_ci().
 */
int cmp_double(const void *a, const void *b);

/*
 * Linear-interpolation percentile, matching numpy's default (method='linear').
 * `sorted` must be in ascending order. `p` in [0, 1].
 * For n <= 0, returns 0.0; for n == 1, returns sorted[0].
 */
double percentile(const double *sorted, int n, double p);

/*
 * Find the lowest frequency (kHz) whose throughput is >= threshold * max.
 * Returns 0 if no such frequency exists (no plateau, e.g. compute-bound).
 * `mops` and `freqs_khz` are parallel arrays of length n, ascending freq.
 * `*out_index` (if non-NULL) receives the index of the returned freq.
 */
int find_sweet_spot(const double *mops, const int *freqs_khz,
                    int n, int *out_index, double threshold);

/*
 * Bootstrap 95% confidence interval on the sweet-spot frequency.
 *
 * Resamples `nsamples` throughput values per frequency point with
 * replacement, takes the median of each resample, re-runs find_sweet_spot()
 * on the resampled-median curve, and repeats B times. *out_low_khz and
 * *out_high_khz receive the 2.5th and 97.5th percentiles of the B
 * sweet-spot values (or 0 if every iteration returned 0).
 *
 * Layout of raw_block: n_freqs contiguous chunks of nsamples doubles,
 * i.e. raw_block[f * nsamples + s] for the s-th sample at freq f.
 * The caller picks the workload's slice from the global raw[] buffer.
 *
 * Uses an internal LCG (bs_lcg) seeded with 42 for reproducibility.
 */
void bootstrap_sweet_spot_ci(
    const double *raw_block, int n_freqs, int nsamples,
    const int *freqs_khz, double threshold, int B,
    int *out_low_khz, int *out_high_khz);

/*
 * Naive O(N^2) piecewise-linear plateau detection.
 *
 * Finds the breakpoint k that minimizes total SSE of a two-segment fit.
 * Outputs:
 *   *out_breakpoint_mhz: the freq (MHz) at the breakpoint, or 0 if none
 *   *out_slope_ratio:    |slope_left| / max(|slope_right|, 1e-9)
 *   *out_sweet_spot_mhz: the 95% sweet spot for context (0 if no plateau)
 * Returns 0 if a plateau is detected (slope_ratio > 2.0), -1 otherwise.
 */
int detect_plateau(const double *mops, const int *freqs_khz, int n,
                   double threshold,
                   int *out_breakpoint_mhz,
                   double *out_slope_ratio,
                   int *out_sweet_spot_mhz);

#endif /* MEMFREQ_STATS_H */
