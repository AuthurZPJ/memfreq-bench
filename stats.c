/*
 * stats.c — Implementation of sweet-spot, plateau, and bootstrap helpers.
 * See stats.h for the public API and contracts.
 */
#include "stats.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>

int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

double percentile(const double *sorted, int n, double p)
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

int find_sweet_spot(const double *mops, const int *freqs_khz,
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
			if (out_index)
				*out_index = i;
			return freqs_khz[i];
		}
	}
	return 0;
}

/*
 * Tiny deterministic LCG (Numerical Recipes constants). Used by the
 * bootstrap so the CI is reproducible without touching the global rand()
 * state (which the bench uses for Fisher-Yates random-array init).
 */
static unsigned int bs_lcg(unsigned int *state)
{
	*state = 1664525u * (*state) + 1013904223u;
	return *state;
}

/*
 * Sweep bounds for the bootstrap resample buffer. The C caller picks
 * the actual B at the call site (currently 1000) and passes nsamples
 * from the CLI; both must fit in these bounds.
 */
#define BS_MAX_B       1000
#define BS_MAX_SAMPLES MAX_SAMPLES

void bootstrap_sweet_spot_ci(
    const double *raw_block, int n_freqs, int nsamples,
    const int *freqs_khz, double threshold, int B,
    int *out_low_khz, int *out_high_khz)
{
	/* Fail loud on bad inputs rather than silently truncate or overrun. */
	assert(B >= 2 && B <= BS_MAX_B);
	assert(nsamples >= 1 && nsamples <= BS_MAX_SAMPLES);
	assert(n_freqs >= 1 && n_freqs <= MAX_FREQS);

	double sweets[BS_MAX_B];          /* B <= 1000, fixed-size */
	double resample[BS_MAX_SAMPLES];  /* per-iteration resample buffer      */
	double mops_b[MAX_FREQS];
	unsigned int state = 42u;  /* deterministic seed */

	for (int b = 0; b < B; b++) {
		for (int f = 0; f < n_freqs; f++) {
			for (int s = 0; s < nsamples; s++) {
				unsigned int r = bs_lcg(&state);
				int idx = (int)(r % (unsigned int)nsamples);
				resample[s] = raw_block[f * nsamples + idx];
			}
			qsort(resample, nsamples, sizeof(double), cmp_double);
			mops_b[f] = resample[nsamples / 2];
		}
		int dummy_idx = -1;
		int khz = find_sweet_spot(mops_b, freqs_khz, n_freqs,
		                          &dummy_idx, threshold);
		sweets[b] = (double)khz;
	}
	qsort(sweets, B, sizeof(double), cmp_double);
	/* Use percentile() (linear-interp, numpy default) so the 2.5/97.5
	 * percentile is computed correctly for any B, not only multiples
	 * of 1000. The previous B*25/1000 formula was an integer-multiply
	 * that only happened to give 25/975 at B=1000. */
	*out_low_khz  = (int)percentile(sweets, B, 0.025);
	*out_high_khz = (int)percentile(sweets, B, 0.975);
}

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

int detect_plateau(const double *mops, const int *freqs_khz, int n,
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

	/* A high slope_ratio means "throughput rises then plateaus" but
	 * tells us nothing if there's no usable sweet spot at this
	 * threshold (e.g. extreme noise, or a flat compute-bound curve
	 * where nothing hits the threshold). Reporting "plateau detected"
	 * with sweet_MHz = 0 is misleading — return -1 so the caller
	 * can show "no plateau" instead. */
	return (slope_ratio > 2.0 && sweet_khz > 0) ? 0 : -1;
}
