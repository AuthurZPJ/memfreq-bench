/*
 * memfreq_bench.c — Memory-bound frequency sweet-spot finder
 *
 * Creates controlled memory access patterns and measures throughput
 * at each available CPU frequency.  The goal: find the lowest frequency
 * that does not significantly hurt memory-bound throughput — the "sweet
 * spot" where reducing frequency saves energy without losing performance.
 *
 * Tests:
 *   stride    — Sequential array traversal with configurable stride.
 *               stride=1 is prefetcher-friendly (moderate mem-bound).
 *               stride≥8 skips cache lines (heavily mem-bound).
 *   chase     — Pointer chasing: random linked-list over 64-byte nodes.
 *               Serial data dependency = pure DRAM latency bound.
 *   compute   — FP arithmetic with no memory access (control: pure
 *               compute-bound, throughput ∝ frequency).
 *
 * Usage:
 *   gcc -O2 -o memfreq_bench memfreq_bench.c -lm
 *   sudo ./memfreq_bench              # default: CPU 0, 128 MB, stride 8
 *   sudo ./memfreq_bench -c 2 -s 16  # CPU 2, stride 16 (more mem-bound)
 *   sudo ./memfreq_bench -m 256       # 256 MB array
 *   sudo ./memfreq_bench -t 5         # 5 s per test (less noise)
 *
 * Output: tab-separated table, easily parsed by memfreq_sweep.py.
 *
 * Requires root to write to:
 *   /sys/devices/system/cpu/cpuN/cpufreq/scaling_*
 *   /sys/devices/system/cpu/intel_pstate/no_turbo  (or cpufreq/boost)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <math.h>

#define CL        64          /* cache line size (bytes)               */
#define MAX_FREQS 256

#define dprintf(...) fprintf(stderr, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ */
/* Sort helper (for median)                                            */
/* ------------------------------------------------------------------ */

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

/* ------------------------------------------------------------------ */
/* CPU pinning                                                         */
/* ------------------------------------------------------------------ */

static int pin_to_cpu(int cpu)
{
	cpu_set_t mask;
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return sched_setaffinity(0, sizeof(mask), &mask);
}

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                       */
/* ------------------------------------------------------------------ */

static int sysfs_read(const char *path, char *buf, size_t sz)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)sz, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int sysfs_write(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int n = fputs(val, f);
	fclose(f);
	return n < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Frequency management                                                */
/* ------------------------------------------------------------------ */

static int read_freq_khz(const char *path)
{
	char buf[64];

	if (sysfs_read(path, buf, sizeof(buf)) < 0)
		return -1;
	return atoi(buf);
}

/*
 * Generate ascending frequency list from fmin to fmax with given step.
 * Always includes fmax as the last entry.  Returns count, or 0 on error.
 */
static int gen_freq_range(int *freqs, int max, int fmin, int fmax, int step_khz)
{
	if (fmin <= 0 || fmax <= 0 || fmin >= fmax)
		return 0;
	if (step_khz <= 0)
		step_khz = 25000;   /* 25 MHz default step */

	int count = 0;
	for (int f = fmin; f <= fmax && count < max; f += step_khz)
		freqs[count++] = f;

	/* ensure max is included */
	if (count > 0 && freqs[count - 1] != fmax && count < max)
		freqs[count++] = fmax;

	return count;
}

/*
 * Read available frequencies.  Three detection paths, tried in order:
 *
 * Path 1 (discrete): scaling_available_frequencies
 *   Traditional cpufreq drivers expose a whitespace-separated list.
 *
 * Path 2 (acpi_cppc): highest_perf / lowest_nonlinear_perf
 *   ARM servers with CPPC expose performance levels via:
 *     /sys/devices/system/cpu/cpuN/acpi_cppc/highest_perf
 *     /sys/devices/system/cpu/cpuN/acpi_cppc/lowest_nonlinear_perf
 *   highest_perf → cpuinfo_max_freq (ceiling)
 *   lowest_nonlinear_perf → floor of the "efficient" range
 *     (below this perf level, power savings diminish — non-linear region)
 *   We map perf → kHz linearly: khz = (perf / highest_perf) × max_khz
 *
 * Path 3 (cpuinfo range): cpuinfo_min_freq → cpuinfo_max_freq
 *   Fallback when acpi_cppc is absent.  Uses the full range exposed by
 *   the driver (which may include the non-linear inefficient region below
 *   lowest_nonlinear_perf).
 */
static int read_freqs(int cpu, int *freqs, int max, int step_khz, int *is_range)
{
	char path[256], buf[4096];

	*is_range = 0;

	/* Path 1: try discrete list */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/"
		 "scaling_available_frequencies", cpu);

	if (sysfs_read(path, buf, sizeof(buf)) == 0) {
		int count = 0;
		char *tok = strtok(buf, " \n");
		while (tok && count < max) {
			freqs[count++] = atoi(tok);
			tok = strtok(NULL, " \n");
		}

		/* sort ascending */
		for (int i = 0; i < count - 1; i++)
			for (int j = i + 1; j < count; j++)
				if (freqs[i] > freqs[j]) {
					int t = freqs[i];
					freqs[i] = freqs[j];
					freqs[j] = t;
				}
		if (count >= 2)
			return count;
	}

	/* Path 2: acpi_cppc perf levels */
	int highest_perf, lowest_nl_perf, max_khz;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/acpi_cppc/highest_perf", cpu);
	highest_perf = read_freq_khz(path);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/acpi_cppc/"
		 "lowest_nonlinear_perf", cpu);
	lowest_nl_perf = read_freq_khz(path);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
	max_khz = read_freq_khz(path);

	if (highest_perf > 0 && lowest_nl_perf > 0 && max_khz > 0 &&
	    lowest_nl_perf < highest_perf) {
		int min_khz = (int)((long long)lowest_nl_perf *
				    max_khz / highest_perf);
		int count = gen_freq_range(freqs, max, min_khz, max_khz,
					   step_khz);
		if (count >= 2) {
			*is_range = 1;
			return count;
		}
	}

	/* Path 3: cpuinfo_min/max range (legacy CPPC fallback) */
	int fmin, fmax;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq",
		 cpu);
	fmin = read_freq_khz(path);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
		 cpu);
	fmax = read_freq_khz(path);

	int count = gen_freq_range(freqs, max, fmin, fmax, step_khz);
	if (count >= 2) {
		*is_range = 1;
		return count;
	}

	return 0;
}

/*
 * Save the original min/max so we can restore them exactly at the end.
 * The governor may have different min and max (e.g. min=800MHz, max=3800MHz).
 * Our set_freq pins both to the same value, which would corrupt the range.
 */
static void save_freq_range(int cpu, int *out_min, int *out_max)
{
	char path[256];

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
		 cpu);
	*out_min = read_freq_khz(path);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
		 cpu);
	*out_max = read_freq_khz(path);
}

static void restore_freq_range(int cpu, int orig_min, int orig_max)
{
	char path[256], val[32];

	if (orig_max > 0) {
		snprintf(val, sizeof(val), "%d", orig_max);
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/"
			 "scaling_max_freq", cpu);
		sysfs_write(path, val);
	}
	if (orig_min > 0) {
		snprintf(val, sizeof(val), "%d", orig_min);
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/"
			 "scaling_min_freq", cpu);
		sysfs_write(path, val);
	}
}

/*
 * Pin frequency to a single value.
 *
 * Order matters: kernel rejects writes where min > max.
 *   - To go UP:   write max first (raise ceiling), then min (raise floor)
 *   - To go DOWN: write min first (lower floor), then max (lower ceiling)
 *
 * Simpler approach: always write max first, then min.
 *   max=X → widens or keeps range → always succeeds
 *   min=X → narrows to [X, X]     → always succeeds (X ≤ X)
 */
static int set_freq(int cpu, int khz)
{
	char path[256], val[32];

	snprintf(val, sizeof(val), "%d", khz);

	/* max first (safe: widens range) */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
		 cpu);
	if (sysfs_write(path, val) < 0)
		return -1;

	/* then min (safe: now max == khz, so min ≤ max always holds) */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
		 cpu);
	if (sysfs_write(path, val) < 0)
		return -1;

	return 0;
}

static int boost_disable(void)
{
	if (sysfs_write("/sys/devices/system/cpu/intel_pstate/no_turbo",
			"1") == 0)
		return 0;
	return sysfs_write("/sys/devices/system/cpu/cpufreq/boost", "0");
}

static int boost_enable(void)
{
	if (sysfs_write("/sys/devices/system/cpu/intel_pstate/no_turbo",
			"0") == 0)
		return 0;
	return sysfs_write("/sys/devices/system/cpu/cpufreq/boost", "1");
}

/* ------------------------------------------------------------------ */
/* Workload: stride traversal                                          */
/*                                                                     */
/* Walks array[0], array[stride], array[2*stride], ...                 */
/* stride=1  → cache-friendly, HW prefetcher effective                 */
/* stride=8  → every 8th u64 = 64 B apart → one cache line per access  */
/* stride=64 → 512 B apart → far beyond prefetcher reach               */
/*                                                                     */
/* All access are to unique cache lines → every access is an L3 miss   */
/* once the working set exceeds L3.  The CPU stalls on DRAM.           */
/* ------------------------------------------------------------------ */

static volatile double sink;

static double __attribute__((noinline))
bench_stride(const uint64_t *arr, size_t count, size_t stride, double secs)
{
	size_t iterations = 0;
	double start = now();

	while (now() - start < secs) {
		uint64_t sum = 0;
		for (size_t i = 0; i < count; i += stride)
			sum += arr[i];
		sink = (double)sum;
		iterations++;
	}
	double elapsed = now() - start;
	/* "accesses per second" = total cache line loads / time */
	return (double)iterations * (count / stride) / elapsed;
}

/* ------------------------------------------------------------------ */
/* Workload: pointer chasing                                           */
/*                                                                     */
/* Random permutation of nodes linked by ->next pointers.  Each node   */
/* occupies exactly one cache line (64 B).  The access pattern is      */
/* random → HW prefetcher cannot help → each dereference = L3 miss.    */
/*                                                                     */
/* Data dependency chain:                                              */
/*   p = p->next  (next iteration CANNOT start until this loads)       */
/* → measures raw DRAM round-trip latency per access.                  */
/* ------------------------------------------------------------------ */

struct pnode {
	struct pnode *next;
	char pad[CL - sizeof(struct pnode *)];
};

_Static_assert(sizeof(struct pnode) == CL,
	       "pnode must be exactly one cache line");

static struct pnode *build_chase(size_t nnodes)
{
	struct pnode *nodes;

	if (posix_memalign((void **)&nodes, CL, nnodes * sizeof(*nodes)))
		return NULL;

	/* Fisher-Yates index shuffle */
	size_t *idx = malloc(nnodes * sizeof(*idx));
	if (!idx) { free(nodes); return NULL; }
	for (size_t i = 0; i < nnodes; i++)
		idx[i] = i;
	for (size_t i = nnodes - 1; i > 0; i--) {
		size_t j = (size_t)rand() % (i + 1);
		size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
	}

	/* Build circular linked list in shuffled order */
	for (size_t i = 0; i < nnodes - 1; i++)
		nodes[idx[i]].next = &nodes[idx[i + 1]];
	nodes[idx[nnodes - 1]].next = &nodes[idx[0]];

	free(idx);
	return nodes;
}

static double __attribute__((noinline))
bench_chase(struct pnode *start, double secs)
{
	struct pnode *p = start;
	size_t iterations = 0;
	double t0 = now();

	while (now() - t0 < secs) {
		for (size_t i = 0; i < 100000; i++)
			p = p->next;
		iterations++;
	}
	double elapsed = now() - t0;
	return (double)iterations * 100000.0 / elapsed;
}

/* ------------------------------------------------------------------ */
/* Workload: pure compute                                              */
/*                                                                     */
/* FP multiply-add chain with data dependency but ZERO memory access.  */
/* Throughput is purely proportional to CPU frequency.                 */
/* Used as a control: this test should show linear freq scaling.       */
/* ------------------------------------------------------------------ */

static double __attribute__((noinline))
bench_compute(double secs)
{
	double x = 1.00001;
	size_t iterations = 0;
	double t0 = now();

	while (now() - t0 < secs) {
		for (size_t i = 0; i < 1000000; i++)
			x = x * 1.0000001 + 0.0000001;
		iterations++;
	}
	double elapsed = now() - t0;
	sink = x;
	return (double)iterations * 1000000.0 / elapsed;
}

/* ------------------------------------------------------------------ */
/* Result row                                                          */
/* ------------------------------------------------------------------ */

struct result {
	int    freq_khz;
	int    valid;           /* 0 if set_freq failed for this point  */
	int    actual_khz;      /* verified actual freq after set       */
	double stride_tput;
	double chase_tput;
	double compute_tput;
	double energy_uj;       /* RAPL energy for this point (µJ), 0=N/A */
};

/* ------------------------------------------------------------------ */
/* System idle gate (from sbc-bench)                                   */
/*                                                                     */
/* Refuse to benchmark if the system is obviously busy.  Background     */
/* load contaminates measurements with cache pollution, CPU contention, */
/* and frequency governor interference.                                 */
/* ------------------------------------------------------------------ */

static double read_loadavg1(void)
{
	FILE *f = fopen("/proc/loadavg", "r");
	if (!f)
		return -1.0;
	double avg;
	if (fscanf(f, "%lf", &avg) != 1) {
		fclose(f);
		return -1.0;
	}
	fclose(f);
	return avg;
}

/*
 * Check if the system is idle enough for benchmarking.
 * Returns 0 if OK, -1 if too busy.
 *
 * Heuristic: loadavg(1min) should be < online_cpus.
 * On a 96-core server, loadavg=5 is fine (5/96 = 5% utilization).
 * On a 4-core board, loadavg=5 means the system is saturated.
 */
static int check_system_idle(int *online_cpus)
{
	*online_cpus = 0;

	/* count online CPUs */
	for (int i = 0; i < 1024; i++) {
		char path[128], buf[8];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/online", i);
		if (sysfs_read(path, buf, sizeof(buf)) == 0 && buf[0] == '1')
			(*online_cpus)++;
	}
	/* fallback: assume nproc */
	if (*online_cpus == 0)
		*online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	double loadavg = read_loadavg1();
	if (loadavg < 0)
		return 0;  /* can't read, don't block */

	if (loadavg > *online_cpus) {
		dprintf("ERROR: system too busy for reliable benchmarking\n");
		dprintf("       loadavg(1min) = %.2f, online CPUs = %d\n",
			loadavg, *online_cpus);
		dprintf("       Use -F to force run anyway\n");
		return -1;
	}

	if (loadavg > *online_cpus * 0.5) {
		dprintf("WARN: system moderately loaded (loadavg=%.2f, "
			"cpus=%d) — results may have noise\n",
			loadavg, *online_cpus);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Power / energy measurement                                          */
/*                                                                     */
/* Tries multiple sources in order of preference:                       */
/*   1. hwmon power*_input (µW instantaneous) — some ARM servers       */
/*   2. Intel RAPL energy_uj (µJ cumulative) — x86 only               */
/*                                                                     */
/* For hwmon (instantaneous power in µW), we sample before and after   */
/* and compute energy = avg_power × elapsed_time.                       */
/*                                                                     */
/* On ARM without hwmon power sensors, this returns -1 (not available).*/
/* ------------------------------------------------------------------ */

#define MAX_POWER_PATHS 8
static char power_paths[MAX_POWER_PATHS][256];
static int  npower_paths = 0;
static int  power_is_uj  = 0;  /* 1=RAPL (cumulative µJ), 0=hwmon (µW) */

static void detect_power_sensors(void)
{
	npower_paths = 0;
	power_is_uj = 0;

	/* Try hwmon power sensors first */
	for (int h = 0; h < 16 && npower_paths < MAX_POWER_PATHS; h++) {
		char base[128];
		snprintf(base, sizeof(base),
			 "/sys/class/hwmon/hwmon%d", h);

		/* check if this hwmon device exists */
		char name[192];
		snprintf(name, sizeof(name), "%s/name", base);
		char namebuf[64];
		if (sysfs_read(name, namebuf, sizeof(namebuf)) < 0)
			continue;

		/* look for power*_input files */
		for (int p = 1; p <= 4 && npower_paths < MAX_POWER_PATHS; p++) {
			char path[256];
			snprintf(path, sizeof(path),
				 "%s/power%d_input", base, p);
			char buf[64];
			if (sysfs_read(path, buf, sizeof(buf)) == 0) {
				long long val = strtoll(buf, NULL, 10);
				if (val > 0) {
					snprintf(power_paths[npower_paths],
						 sizeof(power_paths[0]),
						 "%s", path);
					npower_paths++;
				}
			}
		}
	}
	if (npower_paths > 0)
		return;

	/* Fallback: Intel RAPL (x86 only) */
	const char *rapl_paths[] = {
		"/sys/class/powercap/intel-rapl:0/energy_uj",
		"/sys/class/powercap/intel-rapl:0:0/energy_uj",
		NULL
	};
	for (int i = 0; rapl_paths[i] && npower_paths < MAX_POWER_PATHS; i++) {
		char buf[64];
		if (sysfs_read(rapl_paths[i], buf, sizeof(buf)) == 0) {
			long long val = strtoll(buf, NULL, 10);
			if (val > 0) {
				snprintf(power_paths[npower_paths],
					 sizeof(power_paths[0]),
					 "%s", rapl_paths[i]);
				npower_paths++;
				power_is_uj = 1;
			}
		}
	}
}

/*
 * Read total power/energy from all detected sensors.
 * Returns: cumulative µJ (RAPL) or instantaneous µW (hwmon), or -1 if N/A.
 */
static long long read_power(void)
{
	if (npower_paths == 0)
		return -1;

	long long total = 0;
	for (int i = 0; i < npower_paths; i++) {
		char buf[64];
		if (sysfs_read(power_paths[i], buf, sizeof(buf)) == 0) {
			long long val = strtoll(buf, NULL, 10);
			if (val > 0)
				total += val;
		}
	}
	return total > 0 ? total : -1;
}

/* ------------------------------------------------------------------ */
/* Frequency verification                                              */
/*                                                                     */
/* After setting a target frequency, read the actual running frequency.*/
/* Uses cpuinfo_cur_freq (instantaneous) or cpuinfo_avg_freq (average  */
/* over recent interval) — NOT scaling_cur_freq which only reflects    */
/* the governor's request, not what hardware actually delivers.        */
/*                                                                     */
/* On ARM CPPC systems, frequency fluctuates naturally as firmware     */
/* autonomously adjusts within the requested range.  We just record    */
/* the actual value for the output table — no warnings.                */
/* ------------------------------------------------------------------ */

static int verify_freq(int cpu, int target_khz)
{
	char path[256], buf[64];

	/* try cpuinfo_cur_freq first (instantaneous actual frequency) */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq",
		 cpu);
	if (sysfs_read(path, buf, sizeof(buf)) == 0) {
		int actual = atoi(buf);
		if (actual > 0)
			return actual;
	}

	/* fallback: cpuinfo_avg_freq (average over recent interval) */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_avg_freq",
		 cpu);
	if (sysfs_read(path, buf, sizeof(buf)) == 0) {
		int actual = atoi(buf);
		if (actual > 0)
			return actual;
	}

	(void)target_khz;  /* no warning — fluctuation is normal on CPPC */
	return -1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	dprintf(
"Usage: %s [options]\n"
"  -c CPU      Pin to this CPU            (default: 0)\n"
"  -m SIZE_MB  Array size in MB           (default: 128)\n"
"  -s STRIDE   Stride in uint64 units     (default: 8 = 64B = 1 cache line)\n"
"  -t SECS     Seconds per test point     (default: 2)\n"
"  -n N        Samples per point (median) (default: 3)\n"
"  -S STEP_KHZ Frequency step in kHz      (default: 25000, CPPC range mode only)\n"
"  -C          Skip pointer chase test\n"
"  -F          Force run even if system is busy (skip idle check)\n"
"  -h          This help\n", prog);
}

int main(int argc, char **argv)
{
	int   cpu       = 0;
	int   size_mb   = 128;
	int   stride    = 8;
	int   test_secs = 2;
	int   nsamples  = 3;
	int   do_chase  = 1;
	int   step_khz  = 25000;   /* 25 MHz default step for CPPC range */
	int   force_run = 0;
	int   opt;

	while ((opt = getopt(argc, argv, "c:m:s:t:n:S:CFh")) != -1) {
		switch (opt) {
		case 'c': cpu       = atoi(optarg); break;
		case 'm': size_mb   = atoi(optarg); break;
		case 's': stride    = atoi(optarg); break;
		case 't': test_secs = atoi(optarg); break;
		case 'n': nsamples  = atoi(optarg); break;
		case 'S': step_khz  = atoi(optarg); break;
		case 'C': do_chase  = 0;            break;
		case 'F': force_run = 1;            break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	/* ---- validate ---- */
	if (stride < 1) {
		dprintf("ERROR: stride must be ≥ 1 (got %d)\n", stride);
		return 1;
	}

	/* ---- system idle gate (from sbc-bench) ---- */
	int ncpus_online = 0;
	if (!force_run && check_system_idle(&ncpus_online) < 0)
		return 1;
	if (ncpus_online == 0)
		ncpus_online = sysconf(_SC_NPROCESSORS_ONLN);

	/* ---- detect power sensors ---- */
	detect_power_sensors();

	/* ---- pin ---- */
	if (pin_to_cpu(cpu) < 0) {
		perror("sched_setaffinity");
		return 1;
	}

	/* ---- allocate ---- */
	size_t array_bytes = (size_t)size_mb * 1024 * 1024;
	size_t count       = array_bytes / sizeof(uint64_t);

	uint64_t *arr = NULL;
	if (posix_memalign((void **)&arr, CL, array_bytes)) {
		perror("posix_memalign");
		return 1;
	}
	for (size_t i = 0; i < count; i++)
		arr[i] = i * 2654435761ULL;          /* Knuth hash fill */

	struct pnode *chase_nodes = NULL;
	if (do_chase) {
		srand((unsigned)time(NULL));
		size_t nnodes = array_bytes / CL;
		chase_nodes = build_chase(nnodes);
		if (!chase_nodes) {
			dprintf("WARN: chase alloc failed, skipping\n");
			do_chase = 0;
		}
	}

	/* ---- read available frequencies ---- */
	int freqs[MAX_FREQS];
	int freq_is_range = 0;
	int nfreqs = read_freqs(cpu, freqs, MAX_FREQS, step_khz, &freq_is_range);
	if (nfreqs < 2) {
		dprintf("ERROR: need ≥2 frequencies, got %d.\n", nfreqs);
		dprintf("       Checked three paths:\n");
		dprintf("         1. scaling_available_frequencies (discrete list)\n");
		dprintf("         2. acpi_cppc highest/lowest_nonlinear_perf\n");
		dprintf("         3. cpuinfo_min/max_freq (range, step=%d kHz)\n",
			step_khz);
		dprintf("       Is cpufreq configured on cpu%d?\n", cpu);
		free(arr);
		free(chase_nodes);
		return 1;
	}

	/* ---- save original frequency range ---- */
	int orig_min, orig_max;
	save_freq_range(cpu, &orig_min, &orig_max);

	/* ---- banner ---- */
	dprintf("=== memfreq_bench ===\n");
	dprintf("CPU       : %d (of %d online)\n", cpu, ncpus_online);
	dprintf("Array     : %d MB (%zu cache lines)\n",
		size_mb, array_bytes / CL);
	dprintf("Stride    : %d (= %d B, %s)\n", stride, stride * 8,
		stride >= 8 ? "1 cache line / access" : "prefetcher-friendly");
	dprintf("Chase     : %s\n", do_chase ? "enabled" : "disabled");
	dprintf("Duration  : %d s × %d samples (median)\n",
		test_secs, nsamples);
	dprintf("Power     : %s\n", npower_paths > 0 ?
		(power_is_uj ? "RAPL (cumulative energy)" :
			       "hwmon (instantaneous power)") :
		"not available (energy measurement skipped)");
	if (freq_is_range) {
		dprintf("Freq mode : range, step %d kHz\n", step_khz);
		dprintf("Freq range: %d – %d MHz\n",
			freqs[0] / 1000, freqs[nfreqs - 1] / 1000);
	} else {
		dprintf("Freq mode : discrete list\n");
	}
	dprintf("Freq pts  : %d\n", nfreqs);
	dprintf("Sweep     : high → low (max freq first = reference baseline)\n");
	dprintf("\n");

	/* ---- disable turbo ---- */
	if (boost_disable() < 0)
		dprintf("WARN: could not disable turbo boost\n");

	/* ---- sweep ---- */
	struct result *results = calloc(nfreqs, sizeof(*results));
	double *buf = malloc(nsamples * sizeof(*buf));

	/*
	 * Sweep from highest to lowest frequency.
	 * First measurement at max freq establishes the reference baseline
	 * with warm caches.  Subsequent lower freq measurements compare
	 * against this baseline.
	 */
	int progress = 0;
	for (int fi = nfreqs - 1; fi >= 0; fi--) {
		results[fi].freq_khz = freqs[fi];
		results[fi].valid = 0;
		results[fi].actual_khz = 0;
		results[fi].energy_uj = 0;

		if (set_freq(cpu, freqs[fi]) < 0) {
			dprintf("\nWARN: cannot set cpu%d to %d kHz, skipping\n",
				cpu, freqs[fi]);
			continue;
		}
		/* let frequency settle */
		usleep(100000);

		/* verify actual frequency (from sbc-bench) */
		int actual = verify_freq(cpu, freqs[fi]);
		results[fi].actual_khz = actual > 0 ? actual : freqs[fi];

		progress++;
		dprintf("\r[%2d/%2d]  %4d MHz ...   ",
			progress, nfreqs, freqs[fi] / 1000);
		fflush(stderr);

		/* energy: sample power/energy before tests */
		long long power_before = npower_paths > 0 ? read_power() : -1;
		double t_energy_start = now();

		results[fi].valid = 1;

		/* stride */
		for (int s = 0; s < nsamples; s++)
			buf[s] = bench_stride(arr, count, stride, test_secs);
		qsort(buf, nsamples, sizeof(double), cmp_double);
		results[fi].stride_tput = buf[nsamples / 2];

		/* chase */
		if (do_chase) {
			for (int s = 0; s < nsamples; s++)
				buf[s] = bench_chase(chase_nodes, test_secs);
			qsort(buf, nsamples, sizeof(double), cmp_double);
			results[fi].chase_tput = buf[nsamples / 2];
		}

		/* compute */
		for (int s = 0; s < nsamples; s++)
			buf[s] = bench_compute(test_secs);
		qsort(buf, nsamples, sizeof(double), cmp_double);
		results[fi].compute_tput = buf[nsamples / 2];

		/* energy: compute total energy for this frequency point */
		if (npower_paths > 0 && power_before > 0) {
			double t_energy_end = now();
			long long power_after = read_power();
			if (power_is_uj) {
				/* RAPL: cumulative µJ, take difference */
				if (power_after > power_before)
					results[fi].energy_uj =
						(double)(power_after - power_before);
			} else if (power_after > 0) {
				/* hwmon: instantaneous µW → µJ = µW × s */
				double avg_uw =
					(power_before + power_after) / 2.0;
				double elapsed = t_energy_end - t_energy_start;
				results[fi].energy_uj = avg_uw * elapsed;
			}
		}
	}
	dprintf("\r                                  \r");
	fflush(stderr);

	/* ---- restore ---- */
	boost_enable();
	restore_freq_range(cpu, orig_min, orig_max);

	/* ---- find valid reference (highest successful freq) ---- */
	int ref = -1;
	for (int fi = nfreqs - 1; fi >= 0; fi--) {
		if (results[fi].valid) {
			ref = fi;
			break;
		}
	}
	if (ref < 0) {
		dprintf("ERROR: no frequency point succeeded\n");
		free(buf);
		free(results);
		free(arr);
		free(chase_nodes);
		return 1;
	}

	/* ---- output (tab-separated for easy parsing) ---- */

	/* header */
	printf("# %s\n", "memfreq_bench results");
	printf("# cpu=%d ncpus=%d array=%dMB stride=%d duration=%ds samples=%d",
	       cpu, ncpus_online, size_mb, stride, test_secs, nsamples);
	if (npower_paths > 0)
		printf(" power=%s", power_is_uj ? "rapl" : "hwmon");
	printf("\n#\n");

	/* column headers */
	if (npower_paths > 0) {
		if (do_chase) {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops",  "stride_%",
			     "chase_Mops",   "chase_%",
			     "compute_Mops", "compute_%",
			     "energy_J");
		} else {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops",  "stride_%",
			     "compute_Mops", "compute_%",
			     "energy_J");
		}
	} else {
		if (do_chase) {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops",  "stride_%",
			     "chase_Mops",   "chase_%",
			     "compute_Mops", "compute_%");
		} else {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops",  "stride_%",
			     "compute_Mops", "compute_%");
		}
	}

	/* reference = highest valid freq */
	double s_max = results[ref].stride_tput;
	double c_max = do_chase ? results[ref].chase_tput : 0;
	double p_max = results[ref].compute_tput;

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

	/* data rows */
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid)
			continue;
		int target_mhz = results[fi].freq_khz / 1000;
		int actual_mhz = results[fi].actual_khz / 1000;
		double s_pct = results[fi].stride_tput / s_max * 100.0;
		double p_pct = results[fi].compute_tput / p_max * 100.0;

		if (npower_paths > 0) {
			double e_j = results[fi].energy_uj / 1e6;
			if (do_chase) {
				double c_pct =
					results[fi].chase_tput / c_max * 100.0;
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_pct,
				       results[fi].chase_tput / 1e6, c_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       e_j);
			} else {
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       e_j);
			}
		} else {
			if (do_chase) {
				double c_pct =
					results[fi].chase_tput / c_max * 100.0;
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_pct,
				       results[fi].chase_tput / 1e6, c_pct,
				       results[fi].compute_tput / 1e6, p_pct);
			} else {
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_pct,
				       results[fi].compute_tput / 1e6, p_pct);
			}
		}
	}

	/* summary */
	printf("#\n");
	printf("# === Sweet spot (lowest freq ≥ %.0f%% of max throughput) ===\n",
	       THRESHOLD * 100);

	if (stride_sweet)
		printf("# stride  sweet spot: %d MHz  (%d%% of max %d MHz)\n",
		       stride_sweet / 1000,
		       stride_sweet * 100 / freqs[nfreqs - 1],
		       freqs[nfreqs - 1] / 1000);
	if (do_chase && chase_sweet)
		printf("# chase   sweet spot: %d MHz  (%d%% of max %d MHz)\n",
		       chase_sweet / 1000,
		       chase_sweet * 100 / freqs[nfreqs - 1],
		       freqs[nfreqs - 1] / 1000);
	printf("# compute sweet spot: — (scales linearly, always needs max freq)\n");

	/* Interpretation guide */
	printf("#\n");
	printf("# How to read:\n");
	printf("#   stride/chase %% stays high at low freq → workload is memory-bound\n");
	printf("#     → sweet spot is low, you can save energy by dropping freq\n");
	printf("#   stride/chase %% drops with freq        → workload has compute component\n");
	printf("#     → sweet spot is higher, be careful with aggressive DVFS\n");
	printf("#   compute %% ≈ freq ratio                → sanity check (pure compute)\n");

	free(buf);
	free(results);
	free(arr);
	free(chase_nodes);
	return 0;
}
