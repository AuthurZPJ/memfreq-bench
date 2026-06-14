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
#include <assert.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <ctype.h>
#ifdef __linux__
#include <linux/mempolicy.h>
#endif

#include "stats.h"  /* sweet-spot, plateau, bootstrap helpers + MAX_FREQS/MAX_SAMPLES */
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define CL        64          /* cache line size (bytes)               */
#define MAX_CPUS  256         /* max CPUs for multi-core mode          */
#define MAX_NODES 16          /* max NUMA nodes                        */
#define MAX_USER_THRESHOLDS 16 /* max entries in -L threshold list     */
#define MAX_SIZE_MB 65536     /* max working set: 64 GB                */

/* Convert ops/sec to MB/s for stride test (each op = 8 bytes) */
#define OPS_TO_MBS(ops) ((ops) * 8.0 / 1048576.0)

/* Cache line flush: platform-specific */
#if defined(__x86_64__) || defined(__i386__)
static inline void flush_cacheline(const void *addr)
{
	__asm__ volatile("clflush (%0)" : : "r"(addr) : "memory");
}
#elif defined(__aarch64__)
static inline void flush_cacheline(const void *addr)
{
	__asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
}
#else
static inline void flush_cacheline(const void *addr)
{
	(void)addr;  /* no-op on unsupported platforms */
}
#endif

/* Feature test: platform has cache-flush instruction? */
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
#define HAVE_CACHE_FLUSH 1
#else
#define HAVE_CACHE_FLUSH 0
#endif

#define dprintf(...) fprintf(stderr, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Topology detection for multi-core mode                              */
/* ------------------------------------------------------------------ */

/* forward declarations */
static int sysfs_read(const char *path, char *buf, size_t sz);
static int sysfs_write(const char *path, const char *val);
static int select_primary_threads(int *out_cpus, int max_cpus);

struct numa_node {
	int cpus[MAX_CPUS];
	int ncpus;
};

struct freq_domain {
	int cpus[MAX_CPUS];
	int ncpus;
	int leader;       /* first CPU, used for frequency control */
};

static int detect_numa_nodes(struct numa_node *nodes)
{
	DIR *d = opendir("/sys/devices/system/node");
	if (!d)
		return -1;

	int nnodes = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && nnodes < MAX_NODES) {
		if (strncmp(ent->d_name, "node", 4) != 0 ||
		    !isdigit(ent->d_name[4]))
			continue;
		int node_id = atoi(ent->d_name + 4);
		if (node_id >= MAX_NODES)
			continue;

		char path[512], buf[4096];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/node/%s/cpulist", ent->d_name);
		if (sysfs_read(path, buf, sizeof(buf)) < 0)
			continue;

		nodes[node_id].ncpus = 0;
		/* parse cpulist: "0-23,48-71" or "0,1,2,3" */
		char *tok = strtok(buf, ",");
		while (tok && nodes[node_id].ncpus < MAX_CPUS) {
			int start, end;
			if (sscanf(tok, "%d-%d", &start, &end) == 2) {
				for (int c = start; c <= end && nodes[node_id].ncpus < MAX_CPUS; c++)
					nodes[node_id].cpus[nodes[node_id].ncpus++] = c;
			} else if (sscanf(tok, "%d", &start) == 1) {
				nodes[node_id].cpus[nodes[node_id].ncpus++] = start;
			}
			tok = strtok(NULL, ",");
		}
		if (nodes[node_id].ncpus > 0)
			nnodes++;
	}
	closedir(d);
		/* pack nodes contiguously for sparse node numbering */
	int write_idx = 0;
	for (int i = 0; i < MAX_NODES; i++) {
		if (nodes[i].ncpus > 0) {
			if (i != write_idx)
				nodes[write_idx] = nodes[i];
			write_idx++;
		}
	}
	return write_idx;
}

static int detect_freq_domains(struct freq_domain *domains)
{
	DIR *d = opendir("/sys/devices/system/cpu");
	if (!d)
		return -1;

	int ndomains = 0;
	int seen[MAX_CPUS] = {0};

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "cpu", 3) != 0 ||
		    !isdigit(ent->d_name[3]))
			continue;
		int cpu_id = atoi(ent->d_name + 3);
		if (cpu_id >= MAX_CPUS || seen[cpu_id])
			continue;

		char path[256], buf[4096];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/related_cpus",
			 cpu_id);
		if (sysfs_read(path, buf, sizeof(buf)) < 0)
			continue;

		domains[ndomains].ncpus = 0;
		domains[ndomains].leader = cpu_id;
		char *tok = strtok(buf, " ");
		while (tok && domains[ndomains].ncpus < MAX_CPUS) {
			int c = atoi(tok);
			if (c < MAX_CPUS) {
				domains[ndomains].cpus[domains[ndomains].ncpus++] = c;
				seen[c] = 1;
			}
			tok = strtok(NULL, " ");
		}
		if (domains[ndomains].ncpus > 0)
			ndomains++;
	}
	closedir(d);
	return ndomains;
}

/*
 * Select N CPUs for multi-core testing.
 * Strategy: distribute evenly across NUMA nodes, one per freq domain.
 * Avoid SMT siblings when possible.
 */
static int select_cpus(int ncpus, int *out_cpus,
		       struct numa_node *nodes, int nnodes,
		       struct freq_domain *domains, int ndomains)
{
	if (nnodes <= 0 || ndomains <= 0)
		return -1;

	/* get primary threads (one per physical core, no SMT siblings) */
	int primary[MAX_CPUS];
	int nprimary = select_primary_threads(primary, MAX_CPUS);

	int allowed[MAX_CPUS] = {0};
	if (nprimary > 0) {
		for (int i = 0; i < nprimary; i++)
			allowed[primary[i]] = 1;
	}

	/* round-robin across NUMA nodes, preferring primary threads */
	int per_node = ncpus / nnodes;
	int remainder = ncpus % nnodes;
	int selected = 0;

	for (int n = 0; n < nnodes && selected < ncpus; n++) {
		int target = per_node + (n < remainder ? 1 : 0);
		int node_sel = 0;

		for (int d = 0; d < ndomains && node_sel < target; d++) {
			for (int c = 0; c < domains[d].ncpus && node_sel < target; c++) {
				int cpu = domains[d].cpus[c];
				/* prefer primary threads (avoid SMT siblings) */
				if (nprimary > 0 && !allowed[cpu])
					continue;
				int belongs = 0;
				for (int k = 0; k < nodes[n].ncpus; k++) {
					if (nodes[n].cpus[k] == cpu) {
						belongs = 1;
						break;
					}
				}
				if (belongs) {
					out_cpus[selected++] = cpu;
					node_sel++;
					break;
				}
			}
		}
	}

	return selected;
}

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

/* cmp_double / cmp_int moved to stats.c (cmp_double is exposed via     */
/* stats.h; cmp_int is a private helper inside stats.c).                */

/* ------------------------------------------------------------------ */
/* CPU pinning                                                         */
/* ------------------------------------------------------------------ */

static int pin_to_cpu(int cpu)
{
#ifdef __linux__
	cpu_set_t mask;
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return sched_setaffinity(0, sizeof(mask), &mask);
#else
	(void)cpu;
	dprintf("WARN: CPU pinning is Linux-only; ignored on this platform\n");
	return -1;
#endif
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
	int max_limit;

	/* read hardware max to safely widen the range first */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
		 cpu);
	max_limit = read_freq_khz(path);
	if (max_limit <= 0)
		return -1;

	/* clamp to hardware max so min/max writes don't fail on CPPC overshoot */
	if (khz > max_limit)
		khz = max_limit;

	/* three-step write: widen ceiling | set floor | tighten ceiling.
	 * This order is safe for both increasing and decreasing freq. */
	snprintf(val, sizeof(val), "%d", max_limit);
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
		 cpu);
	if (sysfs_write(path, val) < 0)
		return -1;

	snprintf(val, sizeof(val), "%d", khz);
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
		 cpu);
	if (sysfs_write(path, val) < 0)
		return -1;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
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
/* ---- recovery state for freq/boost restore on abnormal exit ---- */
static int rec_cpu = -1;            /* CPU whose freq was pinned; -1 = no cleanup needed */
static int rec_min = -1;            /* saved scaling_min_freq */
static int rec_max = -1;            /* saved scaling_max_freq */
static int rec_boost = 0;           /* 1 = turbo was successfully disabled */
static volatile sig_atomic_t rec_signal = 0; /* signal caught during critical section */

/*
 * Save original freq range, set recovery state, disable turbo boost.
 * Call freq_cleanup() (defined below) to restore on exit.
 */
static void freq_lock(int cpu)
{
	save_freq_range(cpu, &rec_min, &rec_max);
	rec_cpu = cpu;
	if (boost_disable() < 0)
		dprintf("WARN: could not disable turbo boost\n");
	else
		rec_boost = 1;
}

/* Recovery / signal-safe exit                                          */
/* ------------------------------------------------------------------ */
static void freq_cleanup(void)
{
	if (rec_cpu < 0)
		return;
	if (rec_boost)
		boost_enable();
	if (rec_max > 0 || rec_min > 0)
		restore_freq_range(rec_cpu, rec_min, rec_max);
	rec_cpu = -1;
}
static void signal_handler(int sig)
{
	rec_signal = sig;
}
static void install_recovery_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	atexit(freq_cleanup);
}
static void checked_signal(void)
{
	if (!rec_signal)
		return;
	int sig = rec_signal;
	rec_signal = 0;
	signal(sig, SIG_DFL);
	freq_cleanup();
	raise(sig);
}
/* ------------------------------------------------------------------ */
/* Random seed (uses /dev/urandom to avoid 1-second granularity)       */
/* ------------------------------------------------------------------ */
static void seed_rand(unsigned int mixin)
{
	unsigned int rseed;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (read(fd, &rseed, sizeof(rseed)) == sizeof(rseed)) {
			srand(rseed ^ mixin);
			close(fd);
			return;
		}
		close(fd);
	}
	srand((unsigned)(time(NULL) ^ mixin));
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

/* ------------------------------------------------------------------ */
/* The "sink" pattern: store the per-iteration sum/x to a volatile     */
/* global to prevent the compiler from eliding the entire timing loop. */
/*                                                                     */
/* `static volatile double sink;` — every write is observable, so the  */
/* compiler can't constant-fold or DCE the loop. This is the right     */
/* pattern; the broken alternative `*(volatile T*)&local` (cast a      */
/* non-volatile local to volatile and read through the pointer) is the */
/* one that has known compiler bugs (gcc -O2 can still eliminate the    */
/* load because the "volatile" qualifier is on the read, not the        */
/* underlying object).                                                  */
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
	/* ops/sec = inner_loop_iterations × outer_iterations / time */
	size_t inner_iters = (count + stride - 1) / stride;
	return (double)iterations * (double)inner_iters / elapsed;
}

/* Flush-enabled stride: clflush/dc cvac after each read */
static double __attribute__((noinline))
bench_stride_flush(const uint64_t *arr, size_t count, size_t stride,
		   double secs)
{
	size_t iterations = 0;
	double start = now();

	while (now() - start < secs) {
		uint64_t sum = 0;
		for (size_t i = 0; i < count; i += stride) {
			sum += arr[i];
			flush_cacheline(&arr[i]);
		}
		sink = (double)sum;
		iterations++;
	}
	double elapsed = now() - start;
	size_t inner_iters = (count + stride - 1) / stride;
	return (double)iterations * (double)inner_iters / elapsed;
}

/* ------------------------------------------------------------------ */
/* Workload: random permutation traversal (Fisher-Yates shuffle)       */
/*                                                                     */
/* Creates an index array, shuffles it with Fisher-Yates, then walks   */
/* the data array using random indices.  This completely defeats the   */
/* HW prefetcher — every access is unpredictable.                      */
/*                                                                     */
/* Unlike pointer chasing (serial dependency), this allows the CPU to  */
/* issue multiple outstanding loads → tests random access bandwidth    */
/* rather than latency.                                                */
/* ------------------------------------------------------------------ */

static size_t *build_random_index(size_t count)
{
	size_t *idx = malloc(count * sizeof(size_t));
	if (!idx)
		return NULL;

	/* sequential init */
	for (size_t i = 0; i < count; i++)
		idx[i] = i;

	/* Fisher-Yates shuffle */
	for (size_t i = count - 1; i > 0; i--) {
		size_t j = (size_t)rand() % (i + 1);
		size_t tmp = idx[i];
		idx[i] = idx[j];
		idx[j] = tmp;
	}

	return idx;
}

static double __attribute__((noinline))
bench_random(const uint64_t *arr, const size_t *idx, size_t count,
	     double secs)
{
	size_t iterations = 0;
	double start = now();

	while (now() - start < secs) {
		uint64_t sum = 0;
		for (size_t i = 0; i < count; i++)
			sum += arr[idx[i]];
		sink = (double)sum;
		iterations++;
	}
	double elapsed = now() - start;
	return (double)iterations * count / elapsed;
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

	/* Returns NULL on failure (caller checks). The posix_memalign return
	 * value is the only failure indicator; on success nodes is set, on
	 * failure it is left unchanged but we return NULL without touching it. */
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
bench_chase(struct pnode *start, size_t nnodes, double secs)
{
	struct pnode *p = start;
	size_t iterations = 0;
	double t0 = now();

	while (now() - t0 < secs) {
		for (size_t i = 0; i < nnodes; i++)
			p = p->next;
		iterations++;
	}
	double elapsed = now() - t0;
	return (double)iterations * (double)nnodes / elapsed;
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
	double random_tput;     /* random permutation test, 0=N/A      */
	double compute_tput;
	double stride_cpu_time; /* CPU seconds for stride test          */
	double energy_uj;       /* loaded energy (µJ), 0=N/A           */
	double idle_power_uw;   /* idle power (µW), 0=N/A              */
	double load_power_uw;   /* loaded power (µW), 0=N/A            */
	double stride_min,  stride_max,  stride_iqr;
	double chase_min,   chase_max,   chase_iqr;
	double random_min,  random_max,  random_iqr;
	double compute_min, compute_max, compute_iqr;
};

/* ------------------------------------------------------------------ */
/* L3 cache detection                                                  */
/*                                                                     */
/* Scan /sys/devices/system/cpu/cpu0/cache/indexN/ for the highest     */
/* level unified (or data) cache.  Returns size in bytes, or -1.       */
/* ------------------------------------------------------------------ */

/*
 * Parse CPU list like "0-23" or "0,1,2,3" or "0-11,24-35"
 * Returns count of CPUs in the list.
 */
static int count_cpus_in_list(const char *cpulist)
{
	int count = 0;
	char buf[512];
	strncpy(buf, cpulist, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *tok = strtok(buf, ",");
	while (tok) {
		int start, end;
		if (sscanf(tok, "%d-%d", &start, &end) == 2) {
			count += end - start + 1;
		} else if (sscanf(tok, "%d", &start) == 1) {
			count++;
		}
		tok = strtok(NULL, ",");
	}
	return count;
}

static long detect_l3_size(void)
{
	long max_total = -1;
	char path[512], buf[512], shared_list[512];

	for (int idx = 0; idx < 16; idx++) {
		/* read cache level */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cache/index%d/level",
			 idx);
		if (sysfs_read(path, buf, sizeof(buf)) < 0)
			break;
		int level = atoi(buf);

		if (level < 3)
			continue;

		/* read cache type */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cache/index%d/type",
			 idx);
		if (sysfs_read(path, buf, sizeof(buf)) < 0)
			continue;

		/* only consider Unified or Data caches */
		if (strncmp(buf, "Unified", 7) != 0 &&
		    strncmp(buf, "Data", 4) != 0)
			continue;

		/* read size (per-core slice) */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cache/index%d/size",
			 idx);
		if (sysfs_read(path, buf, sizeof(buf)) < 0)
			continue;

		long slice_size = atol(buf);
		/* handle K/M suffix */
		size_t len = strlen(buf);
		if (len > 0 && (buf[len - 1] == 'K' || buf[len - 1] == 'k'))
			slice_size *= 1024;
		else if (len > 0 && (buf[len - 1] == 'M' || buf[len - 1] == 'm'))
			slice_size *= 1024 * 1024;

		/* read shared_cpu_list to find how many cores share this L3 */
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cache/index%d/shared_cpu_list",
			 idx);
		if (sysfs_read(path, shared_list, sizeof(shared_list)) < 0) {
			/* fallback: assume this is the total size */
			if (slice_size > max_total)
				max_total = slice_size;
			continue;
		}

		int num_sharing = count_cpus_in_list(shared_list);
		if (num_sharing <= 0)
			num_sharing = 1;

		/* total L3 = per-core slice × number of cores sharing it */
		/* For shared caches (L3+), sysfs size is already the total */
		long total_size;
		if (level >= 3)
			total_size = slice_size;
		else
			total_size = slice_size * num_sharing;

		if (total_size > max_total)
			max_total = total_size;
	}

	return max_total;
}

/* ------------------------------------------------------------------ */
/* NUMA detection via /proc/self/status                                */
/*                                                                     */
/* Parse Mems_allowed field from /proc/self/status.  This respects     */
/* cgroup cpuset restrictions automatically.                           */
/* ------------------------------------------------------------------ */

static int detect_numa_from_proc(int *nodes, int max_nodes)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f)
		return -1;

	char line[512];
	int nnodes = 0;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "Mems_allowed:", 13) != 0)
			continue;

		/* parse hex bitmask: "Mems_allowed:\t00000000,00000003\n" */
		char *p = line + 13;
		while (*p == '\t' || *p == ' ')
			p++;

		/* count set bits in hex digits (from right to left) */
		int bit = 0;
		for (int i = strlen(p) - 1; i >= 0; i--) {
			char c = p[i];
			if (c == '\n' || c == '\r')
				continue;
			if (c == ',')
				continue;
			int nibble = 0;
			if (c >= '0' && c <= '9') nibble = c - '0';
			else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
			else break;  /* end of hex */

			for (int b = 0; b < 4 && bit + b < max_nodes; b++) {
				if (nibble & (1 << b)) {
					nodes[nnodes++] = bit + b;
					if (nnodes >= max_nodes)
						break;
				}
			}
			bit += 4;
		}
		break;
	}

	fclose(f);
	return nnodes;
}

/* ------------------------------------------------------------------ */
/* SMT sibling avoidance                                               */
/*                                                                     */
/* Read thread_siblings_list from topology sysfs.  Returns a set of    */
/* "primary" threads (one per physical core).                          */
/* ------------------------------------------------------------------ */

static int select_primary_threads(int *out_cpus, int max_cpus)
{
	DIR *d = opendir("/sys/devices/system/cpu");
	if (!d)
		return -1;

	int seen[MAX_CPUS] = {0};
	int nprimary = 0;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && nprimary < max_cpus) {
		if (strncmp(ent->d_name, "cpu", 3) != 0 ||
		    !isdigit(ent->d_name[3]))
			continue;
		int cpu_id = atoi(ent->d_name + 3);
		if (cpu_id >= MAX_CPUS || seen[cpu_id])
			continue;

		/* read thread_siblings_list */
		char path[256], buf[256];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/"
			 "thread_siblings_list", cpu_id);
		if (sysfs_read(path, buf, sizeof(buf)) < 0) {
			/* no SMT info, assume standalone */
			out_cpus[nprimary++] = cpu_id;
			seen[cpu_id] = 1;
			continue;
		}

		/* parse first CPU from list (e.g. "0,48" → take 0) */
		int first = atoi(buf);
		if (first >= MAX_CPUS)
			continue;

		if (!seen[first]) {
			out_cpus[nprimary++] = first;
			/* mark all siblings as seen */
			char *tok = strtok(buf, ",-");
			while (tok) {
				int c = atoi(tok);
				if (c < MAX_CPUS)
					seen[c] = 1;
				tok = strtok(NULL, ",-");
			}
		} else {
			seen[cpu_id] = 1;
		}
	}

	closedir(d);
	return nprimary;
}

/* ------------------------------------------------------------------ */
/* Thermal monitoring                                                  */
/*                                                                     */
/* Read CPU temperature from thermal zones.  Returns max temp in      */
/* millidegrees Celsius, or -1 if unavailable.                         */
/* ------------------------------------------------------------------ */

static long read_max_temp(void)
{
	long max_temp = -1;
	DIR *d = opendir("/sys/class/thermal");
	if (!d)
		return -1;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "thermal_zone", 12) != 0)
			continue;

		char path[512], buf[64];
		snprintf(path, sizeof(path),
			 "/sys/class/thermal/%s/temp", ent->d_name);
		if (sysfs_read(path, buf, sizeof(buf)) == 0) {
			long temp = atol(buf);
			if (temp > max_temp)
				max_temp = temp;
		}
	}

	closedir(d);
	return max_temp;
}

/* ------------------------------------------------------------------ */
/* mbind wrapper                                                       */
/*                                                                     */
/* Bind memory to a specific NUMA node.  Uses raw syscall to avoid    */
/* libnuma dependency.                                                 */
/* ------------------------------------------------------------------ */

static int bind_memory(void *addr, size_t len, int node)
{
	if (node < 0)
		return 0;  /* no binding requested */

#ifdef __linux__
	unsigned long nodemask = 1UL << node;
	long ret = syscall(SYS_mbind, addr, len, MPOL_BIND,
			   &nodemask, sizeof(nodemask) * 8, 0);
	return (ret == 0) ? 0 : -1;
#else
	(void)addr; (void)len;
	dprintf("WARN: NUMA binding (-B) is Linux-only; ignored on this platform\n");
	return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* getrusage CPU time                                                  */
/*                                                                     */
/* Returns CPU time (user + system) in seconds since program start.    */
/* ------------------------------------------------------------------ */

static double cpu_time_sec(void)
{
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) < 0)
		return -1.0;
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	       ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

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

/*
 * Idle gate: refuse to run if system is too busy.
 * Returns online CPU count on success, -1 if benchmark should abort.
 */
static int idle_gate_check(int force_run)
{
	int ncpus_online = 0;
	if (!force_run && check_system_idle(&ncpus_online) < 0)
		return -1;
	if (ncpus_online == 0)
		ncpus_online = sysconf(_SC_NPROCESSORS_ONLN);
	return ncpus_online;
}

/* ------------------------------------------------------------------ */
/* Power / energy measurement                                          */
/*                                                                     */
/* Tries multiple sources in order of preference:                       */
/*   1. hwmon power*_average (µW, time-averaged) — ARM servers         */
/*      Supports configurable averaging interval via:                   */
/*        power*_average_interval (ms)                                  */
/*      Set to 1000ms (1s) before benchmarking for stable              */
/*      readings.                                                       */
/*   2. hwmon power*_input (µW, instantaneous) — generic fallback       */
/*   3. Intel RAPL energy_uj (µJ, cumulative) — x86 only               */
/*                                                                     */
/* For hwmon (average power in µW):                                    */
/*   energy = ((P_before + P_after) / 2) × elapsed_time                */
/*                                                                     */
/* For RAPL (cumulative energy in µJ):                                 */
/*   energy = counter_after - counter_before                           */
/*                                                                     */
/* On ARM without hwmon power sensors, this returns -1 (not available).*/
/* ------------------------------------------------------------------ */

#define MAX_POWER_PATHS 8
static char power_paths[MAX_POWER_PATHS][256];
static int  npower_paths = 0;
static int  power_is_uj  = 0;  /* 1=RAPL (cumulative µJ), 0=hwmon (µW) */
static int  power_is_avg = 0;  /* 1=power*_average, 0=power*_input     */

/*
 * Try to set power*_average_interval to target_ms for each hwmon device.
 * This controls the time window over which power*_average is computed.
 * Unit is milliseconds. 1000ms (1s) gives a stable, low-noise reading
 * that covers a full benchmark inner-loop iteration.
 */
static void configure_avg_interval(int target_ms)
{
	char val[32];
	snprintf(val, sizeof(val), "%d", target_ms);

	for (int h = 0; h < 16; h++) {
		for (int p = 1; p <= 4; p++) {
			char path[256];
			snprintf(path, sizeof(path),
				 "/sys/class/hwmon/hwmon%d/"
				 "power%d_average_interval", h, p);
			/* silently ignore if file doesn't exist */
			sysfs_write(path, val);
		}
	}
}

static void detect_power_sensors(void)
{
	npower_paths = 0;
	power_is_uj = 0;

	/* Priority 1: hwmon power*_average (time-averaged, preferred) */
	for (int h = 0; h < 16 && npower_paths < MAX_POWER_PATHS; h++) {
		char base[128];
		snprintf(base, sizeof(base),
			 "/sys/class/hwmon/hwmon%d", h);

		char name[192];
		snprintf(name, sizeof(name), "%s/name", base);
		char namebuf[64];
		if (sysfs_read(name, namebuf, sizeof(namebuf)) < 0)
			continue;

		for (int p = 4; p >= 1 && npower_paths < MAX_POWER_PATHS; p--) {
			char path[256];
			snprintf(path, sizeof(path),
				 "%s/power%d_average", base, p);
			char buf[64];
			if (sysfs_read(path, buf, sizeof(buf)) == 0) {
				long long val = strtoll(buf, NULL, 10);
				if (val > 0) {
					snprintf(power_paths[npower_paths],
						 sizeof(power_paths[0]),
						 "%s", path);
					npower_paths++;
					break;
				}
			}
		}
	}
	if (npower_paths > 0) {
		/* configure averaging interval to 1s (1000ms) */
		configure_avg_interval(1000);
		power_is_avg = 1;
		return;
	}

	/* Priority 2: hwmon power*_input (instantaneous) */
	for (int h = 0; h < 16 && npower_paths < MAX_POWER_PATHS; h++) {
		char base[128];
		snprintf(base, sizeof(base),
			 "/sys/class/hwmon/hwmon%d", h);

		char name[192];
		snprintf(name, sizeof(name), "%s/name", base);
		char namebuf[64];
		if (sysfs_read(name, namebuf, sizeof(namebuf)) < 0)
			continue;

		for (int p = 4; p >= 1 && npower_paths < MAX_POWER_PATHS; p--) {
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

	/* Priority 3: Intel RAPL (x86 only) */
	const char *rapl_paths[] = {
		"/sys/class/powercap/intel-rapl:0/energy_uj",
		"/sys/class/powercap/intel-rapl:0:0/energy_uj",
		"/sys/class/powercap/amd-rapl:0/energy_uj",
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
/* Multi-core orchestration (stress-ng style)                          */
/* ------------------------------------------------------------------ */

/*
 * Run benchmark on multiple cores in parallel.
 * Strategy:
 *   1. Detect NUMA and frequency domain topology
 *   2. Select N CPUs (evenly distributed across NUMA nodes)
 *   3. Fork N children, each pinned to one CPU
 *   4. Each child runs single-core sweep, writes to temp file
 *   5. Parent collects and aggregates results
 *
 * For now, simplified implementation: just run sequentially on each CPU.
 * TODO: true parallel fork-based execution.
 */
/*
 * Shared memory structure for multi-core results.
 * Each child writes its per-frequency results here.
 */
struct mc_shared {
	int ncpus;
	int nfreqs;
	int freqs[MAX_FREQS];              /* frequency list (kHz)       */
	struct {
		double stride[MAX_FREQS];  /* per-freq stride tput      */
		double chase[MAX_FREQS];   /* per-freq chase tput       */
		double compute[MAX_FREQS]; /* per-freq compute tput     */
		int actual_khz[MAX_FREQS]; /* actual freq observed      */
	} per_core[MAX_CPUS];
	volatile int ready[MAX_CPUS];      /* 1 = child done (sync flag)        */
};

/* Forward declarations for output helpers used by both run_multicore()
 * and main().  Definitions follow run_multicore(). */
enum power_mode { PWR_NONE, PWR_RAPL, PWR_HWMON };
static void print_column_header(int do_chase, enum power_mode pm);
static void print_data_rows(const struct result *results, int nfreqs,
                            double s_max, double c_max, double p_max,
                            int do_chase, enum power_mode pm);

/*
 * Run benchmark on multiple cores in parallel using fork().
 *
 * Architecture (from stress-ng patterns):
 *   - Parent: controls frequency, forks/reaps children
 *   - Children: pin to CPU, allocate array, run benchmark, write to shared mem
 *   - Shared memory: mmap(MAP_SHARED|MAP_ANONYMOUS)
 *   - Aggregation: median across cores per frequency point
 */
static int run_multicore(int ncpu, int size_mb, int stride, int test_secs,
			 int nsamples, int do_chase, int step_khz,
			 int force_run, double threshold)
{
	/* ---- detect topology ---- */
	struct numa_node nodes[MAX_NODES];
	int nnodes = detect_numa_nodes(nodes);
	if (nnodes <= 0) {
		dprintf("ERROR: cannot detect NUMA topology\n");
		return 1;
	}

	struct freq_domain domains[MAX_CPUS];
	int ndomains = detect_freq_domains(domains);
	if (ndomains <= 0) {
		dprintf("ERROR: cannot detect frequency domains\n");
		return 1;
	}

	/* ---- select CPUs ---- */
	int selected_cpus[MAX_CPUS];
	int nselected = select_cpus(ncpu, selected_cpus, nodes, nnodes,
				    domains, ndomains);
	if (nselected < ncpu) {
		dprintf("WARN: requested %d CPUs, but only found %d suitable\n",
			ncpu, nselected);
		ncpu = nselected;
	}
	if (ncpu <= 0) {
		dprintf("ERROR: no CPUs selected\n");
		return 1;
	}

	/* ---- system idle check ---- */
	int ncpus_online = idle_gate_check(force_run);
	if (ncpus_online < 0)
		return 1;

	/* ---- read frequencies (use first selected CPU) ---- */
	int freqs[MAX_FREQS];
	int freq_is_range = 0;
	int nfreqs = read_freqs(selected_cpus[0], freqs, MAX_FREQS,
				step_khz, &freq_is_range);
	if (nfreqs < 2) {
		dprintf("ERROR: need ≥2 frequencies on cpu%d\n",
			selected_cpus[0]);
		return 1;
	}

	/* ---- allocate shared memory ---- */
	size_t shm_size = sizeof(struct mc_shared);
	struct mc_shared *shm = mmap(NULL, shm_size,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm == MAP_FAILED) {
		perror("mmap shared");
		return 1;
	}

	shm->ncpus = ncpu;
	shm->nfreqs = nfreqs;
	memcpy(shm->freqs, freqs, nfreqs * sizeof(int));
	memset((void *)shm->ready, 0, sizeof(shm->ready));

	/* ---- lock frequency + disable turbo ---- */
	freq_lock(selected_cpus[0]);

	/* ---- banner ---- */
	dprintf("=== memfreq_bench (multi-core) ===\n");
	dprintf("Mode      : %d cores in parallel\n", ncpu);
	dprintf("NUMA      : %d nodes\n", nnodes);
	dprintf("Freq dom  : %d domains\n", ndomains);
	dprintf("Freq pts  : %d (%d – %d MHz)\n",
		nfreqs, freqs[0] / 1000, freqs[nfreqs - 1] / 1000);
	dprintf("CPUs      : ");
	for (int i = 0; i < ncpu; i++)
		dprintf("%d ", selected_cpus[i]);
	dprintf("\n\n");

	/* ---- sweep: high → low ---- */
	for (int fi = nfreqs - 1; fi >= 0; fi--) {
		int target_khz = freqs[fi];

		/* parent sets frequency */
		if (set_freq(selected_cpus[0], target_khz) < 0) {
			dprintf("WARN: cannot set freq %d kHz, skipping\n",
				target_khz);
			continue;
		}
		/* set frequency on all freq domain leaders, not just the first */
		for (int di = 0; di < ndomains; di++) {
			if (domains[di].leader != selected_cpus[0])
				set_freq(domains[di].leader, target_khz);
		}
		usleep(100000);

		dprintf("  %4d MHz: forking %d children ...\n",
			target_khz / 1000, ncpu);

		/* clear ready flags */
		for (int c = 0; c < ncpu; c++)
			shm->ready[c] = 0;

		/* fork children */
		pid_t pids[MAX_CPUS];
		for (int c = 0; c < ncpu; c++) {
			pid_t pid = fork();
			if (pid < 0) {
				perror("fork");
				pids[c] = -1;
				continue;
			}
			if (pid == 0) {
				/* ---- child process ---- */
				int my_cpu = selected_cpus[c];
				pin_to_cpu(my_cpu);

				/* allocate per-core array */
				size_t array_bytes =
					(size_t)size_mb * 1024UL * 1024UL;
				size_t count = array_bytes / sizeof(uint64_t);

				uint64_t *arr = NULL;
				if (posix_memalign((void **)&arr, CL,
						   array_bytes)) {
					_exit(1);
				}
				for (size_t i = 0; i < count; i++)
					arr[i] = i * 2654435761ULL;

				struct pnode *chase_nodes = NULL;
				size_t nn = 0;
				if (do_chase) {
					seed_rand((unsigned int)my_cpu);
					nn = array_bytes / CL;
					chase_nodes = build_chase(nn);
				}

				/* verify actual frequency */
				int actual = verify_freq(my_cpu, target_khz);
				shm->per_core[c].actual_khz[fi] =
					actual > 0 ? actual : target_khz;

				/* run benchmarks */
				double *buf_mc = malloc(nsamples * sizeof(double));
				if (!buf_mc) { dprintf("WARN: mc buf alloc failed\n"); _exit(1); }
				int ns = nsamples;

				/* stride */
				for (int s = 0; s < ns; s++)
					buf_mc[s] = bench_stride(arr, count,
							       stride,
							       test_secs);
				qsort(buf_mc, ns, sizeof(double), cmp_double);
				shm->per_core[c].stride[fi] = buf_mc[ns / 2];

				/* chase */
				if (do_chase && chase_nodes) {
					for (int s = 0; s < ns; s++)
						buf_mc[s] = bench_chase(
							chase_nodes, nn,
							test_secs);
					qsort(buf_mc, ns, sizeof(double),
					      cmp_double);
					shm->per_core[c].chase[fi] =
						buf_mc[ns / 2];
				}

				/* compute */
				for (int s = 0; s < ns; s++)
					buf_mc[s] = bench_compute(test_secs);
				qsort(buf_mc, ns, sizeof(double), cmp_double);
				shm->per_core[c].compute[fi] = buf_mc[ns / 2];

				free(arr);
				free(chase_nodes);
				free(buf_mc);

				shm->ready[c] = 1;
				_exit(0);
			}
			pids[c] = pid;
		}

		/* ---- parent: wait for all children ---- */
		for (int c = 0; c < ncpu; c++) {
			if (pids[c] > 0) {
				int status;
				while (waitpid(pids[c], &status, 0) < 0 && errno == EINTR)
					;
			}
		}
		checked_signal();
	}

	/* ---- restore ---- */
	freq_cleanup();

	/* ---- aggregate results (median across cores per freq) ---- */
	printf("# %s\n", "memfreq_bench multi-core results");
	printf("# ncpus=%d array=%dMB stride=%d\n", ncpu, size_mb, stride);
	printf("# CPUs: ");
	for (int i = 0; i < ncpu; i++)
		printf("%d ", selected_cpus[i]);
	printf("\n#\n");

	/* find reference (highest freq) */
	double *stride_buf = calloc(ncpu, sizeof(double));
	double *chase_buf = calloc(ncpu, sizeof(double));
	double *compute_buf = calloc(ncpu, sizeof(double));

	/* collect per-freq median across cores into struct result[]
	 * so we can reuse print_column_header() and print_data_rows(). */
	struct result mc_results[MAX_FREQS];
	memset(mc_results, 0, sizeof(mc_results));

	for (int fi = 0; fi < nfreqs; fi++) {
		int nc = 0;
		for (int c = 0; c < ncpu; c++) {
			if (shm->ready[c] && shm->per_core[c].stride[fi] > 0) {
				stride_buf[nc] = shm->per_core[c].stride[fi];
				if (do_chase)
					chase_buf[nc] =
						shm->per_core[c].chase[fi];
				compute_buf[nc] =
					shm->per_core[c].compute[fi];
				nc++;
			}
		}
		if (nc == 0) {
			mc_results[fi].valid = 0;
			continue;
		}

		qsort(stride_buf, nc, sizeof(double), cmp_double);
		if (do_chase) {
			qsort(chase_buf, nc, sizeof(double), cmp_double);
			mc_results[fi].chase_tput = chase_buf[nc / 2];
		}
		qsort(compute_buf, nc, sizeof(double), cmp_double);
		mc_results[fi].valid = 1;
		mc_results[fi].freq_khz = freqs[fi];
		mc_results[fi].actual_khz = shm->per_core[0].actual_khz[fi];
		mc_results[fi].stride_tput = stride_buf[nc / 2];
		mc_results[fi].compute_tput = compute_buf[nc / 2];
	}

	/* reference = highest freq */
	int mc_ref = -1;
	for (int fi = nfreqs - 1; fi >= 0; fi--) {
		if (mc_results[fi].valid) { mc_ref = fi; break; }
	}
	if (mc_ref < 0) {
		dprintf("ERROR: no frequency point succeeded in multi-core mode\n");
		free(stride_buf); free(chase_buf); free(compute_buf);
		munmap(shm, shm_size);
		return 1;
	}
	double s_max = mc_results[mc_ref].stride_tput;
	double c_max = do_chase ? mc_results[mc_ref].chase_tput : 0;
	double p_max = mc_results[mc_ref].compute_tput;

	/* build ascending-freq arrays for find_sweet_spot() */
	double stride_mops[MAX_FREQS], chase_mops[MAX_FREQS], compute_mops[MAX_FREQS];
	int    freqs_khz[MAX_FREQS];
	int    n_valid = 0;
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!mc_results[fi].valid) continue;
		stride_mops[n_valid]  = mc_results[fi].stride_tput;
		chase_mops[n_valid]   = mc_results[fi].chase_tput;
		compute_mops[n_valid] = mc_results[fi].compute_tput;
		freqs_khz[n_valid]    = mc_results[fi].freq_khz;
		n_valid++;
	}

	int stride_sweet_idx = -1, chase_sweet_idx = -1;
	int stride_sweet = find_sweet_spot(stride_mops, freqs_khz, n_valid,
	                                   &stride_sweet_idx, threshold);
	int chase_sweet  = do_chase
		? find_sweet_spot(chase_mops, freqs_khz, n_valid,
		                  &chase_sweet_idx, threshold)
		: 0;

	/* column header + data rows (reuse single-core helpers, PWR_NONE) */
	print_column_header(do_chase, PWR_NONE);
	print_data_rows(mc_results, nfreqs, s_max, c_max, p_max,
	                do_chase, PWR_NONE);

	/* summary */
	printf("#\n");
	printf("# === Sweet spot (lowest freq ≥ %.0f%% of max throughput) ===\n",
	       threshold * 100);

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

	free(stride_buf);
	free(chase_buf);
	free(compute_buf);
	munmap(shm, shm_size);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Output formatting helpers                                           */
/*                                                                     */
/* Each function prints one section of the TSV output.  The caller     */
/* passes the data it needs; no function touches global state beyond   */
/* the power-path globals already read by detect_power_sensors().      */
/* ------------------------------------------------------------------ */

static enum power_mode get_power_mode(void)
{
	if (npower_paths > 0 && !power_is_uj)
		return PWR_HWMON;
	if (npower_paths > 0)
		return PWR_RAPL;
	return PWR_NONE;
}

/*
 * Column header row.  The column set depends on power mode and whether
 * the chase workload is enabled — 6 branches in the original code,
 * collapsed to one function here.
 */
static void print_column_header(int do_chase, enum power_mode pm)
{
	if (pm == PWR_HWMON) {
		if (do_chase) {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "chase_Mops",   "chase_%",
			     "compute_Mops", "compute_%",
			     "idle_W", "load_W", "delta_W", "energy_J");
		} else {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "compute_Mops", "compute_%",
			     "idle_W", "load_W", "delta_W", "energy_J");
		}
	} else if (pm == PWR_RAPL) {
		if (do_chase) {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "chase_Mops",   "chase_%",
			     "compute_Mops", "compute_%",
			     "energy_J");
		} else {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "compute_Mops", "compute_%",
			     "energy_J");
		}
	} else {
		if (do_chase) {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "chase_Mops",   "chase_%",
			     "compute_Mops", "compute_%");
		} else {
			printf("# %s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     "target_MHz", "actual_MHz",
			     "stride_Mops", "stride_MBs", "stride_%",
			     "compute_Mops", "compute_%");
		}
	}
}

/*
 * Data rows: one TSV line per valid frequency point.
 * Power columns are appended when a sensor is available.
 */
static void print_data_rows(const struct result *results, int nfreqs,
                            double s_max, double c_max, double p_max,
                            int do_chase, enum power_mode pm)
{
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid)
			continue;
		int target_mhz = results[fi].freq_khz / 1000;
		int actual_mhz = results[fi].actual_khz / 1000;
		double s_pct = results[fi].stride_tput / s_max * 100.0;
		double p_pct = results[fi].compute_tput / p_max * 100.0;

		if (pm == PWR_HWMON) {
			double idle_w = results[fi].idle_power_uw / 1e6;
			double load_w = results[fi].load_power_uw / 1e6;
			double delta_w = load_w - idle_w;
			double e_j = results[fi].energy_uj / 1e6;
			double s_mbs = OPS_TO_MBS(results[fi].stride_tput);
			if (do_chase) {
				double c_pct =
					results[fi].chase_tput / c_max * 100.0;
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.2f\t%.2f\t%.2f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].chase_tput / 1e6, c_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       idle_w, load_w, delta_w, e_j);
			} else {
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.2f\t%.2f\t%.2f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       idle_w, load_w, delta_w, e_j);
			}
		} else if (pm == PWR_RAPL) {
			double e_j = results[fi].energy_uj / 1e6;
			double s_mbs = OPS_TO_MBS(results[fi].stride_tput);
			if (do_chase) {
				double c_pct =
					results[fi].chase_tput / c_max * 100.0;
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].chase_tput / 1e6, c_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       e_j);
			} else {
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].compute_tput / 1e6, p_pct,
				       e_j);
			}
		} else {
			double s_mbs = OPS_TO_MBS(results[fi].stride_tput);
			if (do_chase) {
				double c_pct =
					results[fi].chase_tput / c_max * 100.0;
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].chase_tput / 1e6, c_pct,
				       results[fi].compute_tput / 1e6, p_pct);
			} else {
				printf("%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n",
				       target_mhz, actual_mhz,
				       results[fi].stride_tput / 1e6, s_mbs, s_pct,
				       results[fi].compute_tput / 1e6, p_pct);
			}
		}
	}
}

/*
 * Per-freq stats blocks: min/max/median/IQR for each workload at each
 * frequency point.  Header label uses "Mops" for stride/chase and
 * "MOps" for random/compute — matches the original C source casing.
 *
 * Accessor functions extract the four stats from a result row so we
 * can iterate workloads without offsetof gymnastics.
 */
typedef void (*stat_accessors)(const struct result *,
                               double *mn, double *mx, double *md, double *iq);

static void stride_stats(const struct result *r,
                         double *mn, double *mx, double *md, double *iq)
{ *mn = r->stride_min; *mx = r->stride_max;
  *md = r->stride_tput; *iq = r->stride_iqr; }

static void chase_stats(const struct result *r,
                        double *mn, double *mx, double *md, double *iq)
{ *mn = r->chase_min; *mx = r->chase_max;
  *md = r->chase_tput; *iq = r->chase_iqr; }

static void random_stats(const struct result *r,
                         double *mn, double *mx, double *md, double *iq)
{ *mn = r->random_min; *mx = r->random_max;
  *md = r->random_tput; *iq = r->random_iqr; }

static void compute_stats(const struct result *r,
                          double *mn, double *mx, double *md, double *iq)
{ *mn = r->compute_min; *mx = r->compute_max;
  *md = r->compute_tput; *iq = r->compute_iqr; }

static void print_one_per_freq_stats(const char *workload, const char *unit,
                                     const struct result *results, int nfreqs,
                                     stat_accessors get)
{
	printf("#\n# --- per-freq stats (%s) ---\n", workload);
	printf("# freq_MHz  min_%s  max_%s  median_%s  iqr_%s\n",
	       unit, unit, unit, unit);
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		double mn, mx, md, iq;
		get(&results[fi], &mn, &mx, &md, &iq);
		printf("# %d\t%.1f\t%.1f\t%.1f\t%.2f\n",
		       results[fi].freq_khz / 1000,
		       mn / 1e6, mx / 1e6, md / 1e6, iq / 1e6);
	}
}

static void print_per_freq_stats(const struct result *results, int nfreqs,
                                 int do_chase, int do_random)
{
	print_one_per_freq_stats("stride", "Mops", results, nfreqs, stride_stats);
	if (do_chase)
		print_one_per_freq_stats("chase", "Mops", results, nfreqs, chase_stats);
	if (do_random)
		print_one_per_freq_stats("random", "MOps", results, nfreqs, random_stats);
	print_one_per_freq_stats("compute", "MOps", results, nfreqs, compute_stats);
}

/*
 * Sweet-spot CI block: bootstrap-resamples per-freq raw samples and
 * reports 2.5th/97.5th percentile of the resulting sweet-spot values.
 *
 * Raw layout: raw[(fi*4 + w)*nsamples + s], w ∈ {0=stride, 1=chase,
 * 2=random, 3=compute}.  compact_to_results[] maps the compacted fi
 * (0..n_valid-1) to the results[] index.
 */
static void print_sweet_spot_ci(const double *raw, int nsamples,
                                const double *mops[4],
                                const int *freqs_khz, int n_valid,
                                double threshold,
                                int headline_khz[4], int enabled[4],
                                const int *compact_to_results)
{
	const char *labels[] = {"stride", "chase", "random", "compute"};

	printf("#\n# --- sweet-spot CI ---\n");
	printf("# workload  sweet_MHz  low_MHz  high_MHz  method\n");
	for (int w = 0; w < 4; w++) {
		if (!enabled[w]) continue;
		int sweet = headline_khz[w];
		if (w >= 2) {
			int idx = -1;
			sweet = find_sweet_spot(mops[w], freqs_khz,
			                        n_valid, &idx, threshold);
		}
		if (sweet <= 0) {
			printf("# %-8s  \xe2\x80\x94         \xe2\x80\x94       \xe2\x80\x94         no_plateau\n",
			       labels[w]);
			continue;
		}
		double *slice = malloc((size_t)n_valid * nsamples
		                       * sizeof(double));
		if (!slice) {
			printf("# %-8s  %-9d  \xe2\x80\x94       \xe2\x80\x94         bootstrap_1000 (oom)\n",
			       labels[w], sweet / 1000);
			continue;
		}
		for (int fi = 0; fi < n_valid; fi++) {
			int rfi = compact_to_results[fi];
			memcpy(slice + fi * nsamples,
			       raw + ((size_t)rfi * 4 + w) * nsamples,
			       nsamples * sizeof(double));
		}
		int lo_khz = 0, hi_khz = 0;
		bootstrap_sweet_spot_ci(slice, n_valid, nsamples,
		                        freqs_khz, threshold, 1000,
		                        &lo_khz, &hi_khz);
		free(slice);
		printf("# %-8s  %-9d  %-7d  %-9d  bootstrap_1000\n",
		       labels[w],
		       sweet / 1000,
		       lo_khz / 1000,
		       hi_khz / 1000);
	}
}

/*
 * Sensitivity blocks: re-runs sweet-spot detection at each user-supplied
 * threshold so the user can see how sensitive the decision is.
 */
static void print_sensitivity(int n_user_thresholds,
                              const double *user_thresholds,
                              const double *mops[4],
                              const int *freqs_khz, int n_valid,
                              int enabled[4])
{
	const char *labels[] = {"stride", "chase", "random", "compute"};

	for (int w = 0; w < 4; w++) {
		if (!enabled[w]) continue;
		printf("#\n# --- sensitivity (%s) ---\n", labels[w]);
		printf("# threshold  sweet_spot_MHz\n");
		for (int ti = 0; ti < n_user_thresholds; ti++) {
			int idx = -1;
			int khz = find_sweet_spot(mops[w], freqs_khz, n_valid,
			                          &idx, user_thresholds[ti]);
			if (khz > 0)
				printf("# %.2f\t%d\n",
				       user_thresholds[ti], khz / 1000);
			else
				printf("# %.2f\t\xe2\x80\x94\n", user_thresholds[ti]);
		}
	}
}

/*
 * Plateau block: piecewise-linear breakpoint + slope ratio per workload,
 * with power savings at the sweet spot when a sensor is available.
 */
static void print_plateau(const double *mops[4], const int *freqs_khz,
                          int n_valid, double threshold,
                          const struct result *results, int nfreqs,
                          int enabled[4],
                          const int *compact_to_results)
{
	const char *labels[] = {"stride", "chase", "random", "compute"};

	printf("#\n# --- plateau ---\n");
	for (int w = 0; w < 4; w++) {
		if (!enabled[w]) continue;
		int bp_mhz = 0, sweet_mhz = 0;
		double ratio = 0.0;
		int rc = detect_plateau(mops[w], freqs_khz, n_valid,
		                        threshold, &bp_mhz, &ratio, &sweet_mhz);
		if (rc == 0)
			printf("# %-8s plateau_breakpoint: %d MHz  (slope ratio %.1fx, 95%% sweet spot %d MHz)\n",
			       labels[w], bp_mhz, ratio, sweet_mhz);
		else
			printf("# %-8s plateau_breakpoint: \xe2\x80\x94  (no plateau; throughput keeps rising with frequency)\n",
			       labels[w]);

		/* Power at sweet spot */
		int    max_power_idx      = -1;
		double max_power_w        = 0.0;
		int    max_power_freq_khz = 0;
		for (int fi = 0; fi < nfreqs; fi++) {
			if (!results[fi].valid) continue;
			if (results[fi].load_power_uw <= 0) continue;
			max_power_idx      = fi;
			max_power_w        = results[fi].load_power_uw / 1e6;
			max_power_freq_khz = results[fi].freq_khz;
		}
		if (rc == 0 && max_power_idx >= 0) {
			int sweet_compact_idx = -1;
			(void)find_sweet_spot(mops[w], freqs_khz,
			                      n_valid, &sweet_compact_idx,
			                      threshold);
			if (sweet_compact_idx < 0 ||
			    sweet_compact_idx >= n_valid)
				continue;
			int sweet_results_idx = compact_to_results[sweet_compact_idx];
			if (sweet_results_idx < 0)
				continue;
			double sweet_power_w = results[sweet_results_idx].load_power_uw / 1e6;
			double savings = (max_power_w - sweet_power_w)
			                 / max_power_w * 100.0;
			printf("# %-8s power: %.0fW at sweet spot (savings: %.0f%% vs %.0fW at %d MHz)\n",
			       labels[w], sweet_power_w, savings,
			       max_power_w, max_power_freq_khz / 1000);
		} else if (rc != 0) {
			printf("# %-8s power: N/A (no plateau)\n",
			       labels[w]);
		} else {
			printf("# %-8s power: N/A (no sensor)\n",
			       labels[w]);
		}
	}
}

/*
 * Raw samples block: per-freq × per-sample throughput for each workload.
 */
static void print_raw_samples(const double *raw, int nsamples, int nfreqs,
                              const struct result *results, int enabled[4])
{
	const char *labels[] = {"stride", "chase", "random", "compute"};

	for (int w = 0; w < 4; w++) {
		if (!enabled[w]) continue;
		printf("#\n# --- raw_samples (%s) ---\n", labels[w]);
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

/*
 * Freq-lock sanity check: compute workload is CPU-bound, so its
 * throughput must scale linearly with actual_khz if the frequency
 * lock took.  R² < 0.90 is the smoking gun that it didn't.
 */
static void print_freq_lock_sanity(const struct result *results, int nfreqs)
{
	int n_lock_pts = 0;
	for (int fi = 0; fi < nfreqs; fi++)
		if (results[fi].valid) n_lock_pts++;
	if (n_lock_pts < 4)
		return;

	double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		double x = results[fi].actual_khz / 1000.0;
		double y = results[fi].compute_tput;
		sum_x  += x;
		sum_y  += y;
		sum_xx += x * x;
		sum_xy += x * y;
	}
	double denom = n_lock_pts * sum_xx - sum_x * sum_x;
	if (denom <= 0)
		return;

	double slope     = (n_lock_pts * sum_xy - sum_x * sum_y) / denom;
	double intercept = (sum_y - slope * sum_x) / n_lock_pts;
	double mean_y    = sum_y / n_lock_pts;
	double ss_res = 0, ss_tot = 0;
	for (int fi = 0; fi < nfreqs; fi++) {
		if (!results[fi].valid) continue;
		double x = results[fi].actual_khz / 1000.0;
		double y = results[fi].compute_tput;
		double pred = slope * x + intercept;
		ss_res += (y - pred) * (y - pred);
		ss_tot += (y - mean_y) * (y - mean_y);
	}
	double r2 = ss_tot > 0 ? 1.0 - ss_res / ss_tot : 1.0;
	printf("#\n# --- freq-lock sanity ---\n");
	printf("# compute throughput vs actual frequency: "
	       "R^2 = %.3f, slope = %.3f Mops/MHz\n", r2, slope);
	if (r2 < 0.90) {
		printf("# WARN: compute throughput does NOT scale linearly "
		       "with frequency (R^2 = %.3f < 0.90)\n", r2);
		printf("#       The frequency lock did NOT take — turbo, "
		       "governor, or sysfs write may be bypassed.\n");
		printf("#       Sweet-spot numbers from this run are NOT "
		       "trustworthy. Re-run with:\n");
		printf("#         - governor=performance, no turbo, "
		       "verify /sys/devices/system/cpu/*/cpufreq/\n");
		printf("#         - sudo ./memfreq_bench -F  (skip idle gate)\n");
	} else {
		printf("# OK: compute scales linearly with frequency — "
		       "freq lock took, sweet-spot numbers are valid.\n");
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	dprintf(
"Usage: %s [options]\n"
"  -c CPU      Pin to this CPU            (default: 0)\n"
"  -N NCPU     Test NCPU cores in parallel (stress-ng style)\n"
"              Auto-distributes across NUMA nodes and freq domains\n"
"  -m SIZE_MB  Array size in MB           (default: 128)\n"
"  -A          Auto-size array to 2× L3   (ensures working set > L3)\n"
"  -s STRIDE   Stride in uint64 units     (default: 8 = 64B = 1 cache line)\n"
"  -t SECS     Seconds per test point     (default: 2)\n"
"  -n N        Samples per point (median) (default: 3)\n"
"  -S STEP_KHZ Frequency step in kHz      (default: 25000, CPPC range mode only)\n"
"  -C          Skip pointer chase test\n"
"  -R          Add random permutation test (Fisher-Yates, defeats prefetcher)\n"
"  -f          Flush cache line after each access (clflush/dc cvac)\n"
"  -F          Force run even if system is busy (skip idle check)\n"
"  -B NODE     Bind array memory to NUMA node (default: -1 = no binding)\n"
"  -T FRAC     Sweet-spot threshold in (0, 1]   (default: 0.95)\n"
"  -L LIST     Multi-threshold sweep, e.g. 0.8,0.9,0.95,0.99 (max 16)\n"
"  -r          Emit per-sample raw data (for custom analysis)\n"
"  -P          Suppress plateau detection block\n"
"  -y          Summary mode: data table + sweet-spot headlines only\n"
"              (skips per-freq stats, CI, sensitivity, plateau, raw samples)\n"
"  -h          This help\n", prog);
}

/*
 * Sweet-spot, plateau, and bootstrap helpers moved to stats.c.
 * See stats.h for the API. This keeps main() focused on orchestration.
 */

int main(int argc, char **argv)
{
	int   cpu       = 0;
	int   ncpu      = 0;  /* 0 = single-core mode, >0 = multi-core */
	int   size_mb   = 128;
	int   auto_size = 0;  /* -A: auto-size to 2× L3 */
	int   stride    = 8;
	int   test_secs = 2;
	int   nsamples  = 3;
	int   do_chase  = 1;
	int   do_random = 0;  /* -R: random permutation test */
	int   do_flush  = 0;  /* -f: flush cache line after each access */
	int   step_khz  = 25000;   /* 25 MHz default step for CPPC range */
	int   force_run = 0;
	int   numa_node = -1;  /* -1 = no binding */
	double  threshold      = 0.95;  /* user-overridable sweet-spot threshold */
	int     n_user_thresholds = 0;
	double  user_thresholds[MAX_USER_THRESHOLDS];
	int     emit_raw       = 0;     /* -r: per-sample data in output */
	int     no_plateau     = 0;     /* -P: suppress plateau block */
	int     summary        = 0;     /* -y: summary mode (skip optional blocks) */
	int   opt;

	/* ---- help: short-circuit before any privilege check ---- */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
	}

	while ((opt = getopt(argc, argv, "c:N:m:As:t:n:S:B:CRfFhT:L:rPy")) != -1) {
		switch (opt) {
		case 'c': cpu       = atoi(optarg); break;
		case 'N': ncpu      = atoi(optarg); break;
		case 'm': size_mb   = atoi(optarg); break;
		case 'A': auto_size = 1;            break;
		case 's': stride    = atoi(optarg); break;
		case 't': test_secs = atoi(optarg); break;
		case 'n': nsamples  = atoi(optarg); break;
		case 'S': step_khz  = atoi(optarg); break;
		case 'B': numa_node = atoi(optarg); break;
		case 'C': do_chase  = 0;            break;
		case 'R': do_random = 1;            break;
		case 'f': do_flush  = 1;            break;
		case 'F': force_run = 1;            break;
		case 'T': threshold = atof(optarg);
		          if (threshold <= 0.0 || threshold > 1.0) {
		              dprintf("ERROR: threshold must be in (0, 1], got %s\n", optarg);
		              return 1;
		          }
		          break;
		case 'L': {
		          char *tok = strtok(optarg, ",");
		          if (!tok || !*tok) {
		              dprintf("ERROR: -L requires a non-empty comma-separated list\n");
		              return 1;
		          }
		          while (tok && n_user_thresholds < MAX_USER_THRESHOLDS) {
		              double v = atof(tok);
		              if (v <= 0.0 || v > 1.0) {
		                  dprintf("ERROR: threshold value must be in (0, 1], got %s\n", tok);
		                  return 1;
		              }
		              user_thresholds[n_user_thresholds++] = v;
		              tok = strtok(NULL, ",");
		          }
		          if (tok && n_user_thresholds == MAX_USER_THRESHOLDS) {
		              dprintf("WARN: -L truncated to %d entries (more given)\n",
		                      MAX_USER_THRESHOLDS);
		          }
		          break;
		      }
		case 'r': emit_raw   = 1;            break;
		case 'P': no_plateau = 1;            break;
		case 'y': summary    = 1;            break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (do_flush && !HAVE_CACHE_FLUSH)
		dprintf("WARN: -f (cache flush) has no effect on this platform\n");

	/* ---- detect L3 cache (pre-root: doesn't need root) ---- */
	long l3_bytes = detect_l3_size();
	if (auto_size && l3_bytes > 0) {
		size_mb = (int)((l3_bytes * 2) / (1024 * 1024));
		if (size_mb < 16)
			size_mb = 16;  /* minimum 16 MB */
	}
	/* Bound-check size_mb. The cast to size_t is deliberately BEFORE the
	 * multiplication (1024UL * 1024UL keeps each factor explicitly unsigned
	 * long) so the result is never computed in 32-bit int — that would
	 * overflow at size_mb == 2048. */
	if (size_mb < 1 || size_mb > MAX_SIZE_MB) {
		dprintf("ERROR: -m %d out of range [1, %d] MB\n",
			size_mb, MAX_SIZE_MB);
		return 1;
	}

	/* ---- platform checks (must come before any sysfs write) ---- */
	if (geteuid() != 0) {
		dprintf("ERROR: this tool must be run as root (try: sudo %s ...)\n",
			argv[0]);
		dprintf("       It needs write access to /sys/devices/system/cpu/*/cpufreq/\n");
		return 1;
	}
#ifndef __linux__
	dprintf("ERROR: this tool is Linux-only (cpufreq sysfs required).\n");
# if defined(__APPLE__)
	dprintf("       On macOS use a Linux VM, a remote Linux box, or "
		"Docker with --privileged + /sys mounted.\n");
# else
	dprintf("       No cpufreq sysfs interface is available on this platform.\n");
# endif
	return 1;
#endif

	/* ---- cpufreq sysfs check (post-getopt: needs `cpu` value) ---- */
	char cpufreq_path[256];
	snprintf(cpufreq_path, sizeof(cpufreq_path),
	         "/sys/devices/system/cpu/cpu%d/cpufreq/", cpu);
	DIR *cpufreq_dir = opendir(cpufreq_path);
	if (!cpufreq_dir) {
		dprintf("ERROR: cpufreq sysfs path not found for cpu %d: %s\n",
			cpu, cpufreq_path);
		dprintf("       Is the cpufreq driver loaded? Check:\n");
		dprintf("         ls %s\n", cpufreq_path);
		dprintf("         cat /sys/devices/system/cpu/cpu%d/cpufreq/scaling_driver\n",
			cpu);
		return 1;
	}
	closedir(cpufreq_dir);
	install_recovery_handlers();

	if (l3_bytes > 0 && (size_t)size_mb * 1024UL * 1024UL < (size_t)l3_bytes) {
		dprintf("WARN: array size (%d MB) < L3 cache (%ld MB)\n",
			size_mb, l3_bytes / (1024 * 1024));
		dprintf("      Working set may fit in L3 → test may not be "
			"fully memory-bound\n");
		dprintf("      Use -A to auto-size to 2× L3, or -m %ld\n",
			(l3_bytes * 2) / (1024 * 1024));
	}

	/* ---- multi-core mode ---- */
	if (ncpu > 1) {
		return run_multicore(ncpu, size_mb, stride, test_secs,
				     nsamples, do_chase, step_khz, force_run,
				     threshold);
	}

	/* ---- validate ---- */
	if (stride < 1) {
		dprintf("ERROR: stride must be ≥ 1 (got %d)\n", stride);
		return 1;
	}

	/* ---- system idle gate (from sbc-bench) ---- */
	int ncpus_online = idle_gate_check(force_run);
	if (ncpus_online < 0)
		return 1;

	/* ---- detect power sensors ---- */
	detect_power_sensors();

	/* ---- pin ---- */
	if (pin_to_cpu(cpu) < 0) {
		perror("sched_setaffinity");
		return 1;
	}

	/* ---- allocate ---- */
	size_t array_bytes = (size_t)size_mb * 1024UL * 1024UL;
	size_t count       = array_bytes / sizeof(uint64_t);

	uint64_t *arr = NULL;
	if (posix_memalign((void **)&arr, CL, array_bytes)) {
		perror("posix_memalign");
		return 1;
	}
	for (size_t i = 0; i < count; i++)
		arr[i] = i * 2654435761ULL;          /* Knuth hash fill */

	/* ---- NUMA binding ---- */
	if (numa_node >= 0) {
		if (bind_memory(arr, array_bytes, numa_node) < 0) {
			dprintf("WARN: mbind to node %d failed\n", numa_node);
			dprintf("      Falling back to default allocation\n");
		} else {
			dprintf("Memory bound to NUMA node %d\n", numa_node);
		}
	}

	struct pnode *chase_nodes = NULL;
	size_t nnodes = 0;
	if (do_chase) {
		seed_rand(0);
		nnodes = array_bytes / CL;
		chase_nodes = build_chase(nnodes);
		if (!chase_nodes) {
			dprintf("WARN: chase alloc failed, skipping\n");
			do_chase = 0;
		}
	}

	size_t *random_idx = NULL;
	if (do_random) {
		random_idx = build_random_index(count);
		if (!random_idx) {
			dprintf("WARN: random index alloc failed, skipping\n");
			do_random = 0;
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
		free(random_idx);
		return 1;
	}

	/* ---- lock frequency + disable turbo ---- */
	freq_lock(cpu);

	/* ---- banner ---- */
	int proc_nodes[MAX_NODES];
	int nproc_nodes = detect_numa_from_proc(proc_nodes, MAX_NODES);

	long max_temp = read_max_temp();

	dprintf("=== memfreq_bench ===\n");
	dprintf("CPU       : %d (of %d online)\n", cpu, ncpus_online);
	if (l3_bytes > 0)
		dprintf("L3 cache  : %ld MB (total shared across all cores)\n",
			l3_bytes / (1024 * 1024));
	if (nproc_nodes > 0)
		dprintf("NUMA      : %d nodes allowed\n", nproc_nodes);
	if (max_temp > 0)
		dprintf("Temp      : %ld.%ld°C\n", max_temp / 1000,
			(max_temp % 1000) / 100);
	dprintf("Array     : %d MB (%zu cache lines)%s\n",
		size_mb, array_bytes / CL,
		auto_size ? " (auto-sized to 2× L3)" : "");
	if (numa_node >= 0)
		dprintf("NUMA bind : node %d\n", numa_node);
	dprintf("Stride    : %d (= %d B, %s)\n", stride, stride * 8,
		stride >= 8 ? "1 cache line / access" : "prefetcher-friendly");
	dprintf("Chase     : %s\n", do_chase ? "enabled" : "disabled");
	dprintf("Duration  : %d s × %d samples (median)\n",
		test_secs, nsamples);
	dprintf("Power     : %s\n", npower_paths > 0 ?
		(power_is_uj ? "RAPL (cumulative energy)" :
		 power_is_avg ? "hwmon power*_average (1s avg)" :
			       "hwmon power*_input (instantaneous)") :
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

	struct result *results = calloc(nfreqs, sizeof(*results));
	double *buf = malloc(nsamples * sizeof(*buf));

	/* Raw sample storage: only allocated when --emit-raw is set.
	 * Layout: [freq_idx][workload][sample_idx], where workload order is
	 * 0=stride, 1=chase, 2=random, 3=compute. */
	double *raw = NULL;
	if (emit_raw)
		raw = calloc((size_t)nfreqs * 4 * nsamples, sizeof(double));

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
		results[fi].idle_power_uw = 0;
		results[fi].load_power_uw = 0;
		results[fi].stride_min = 0; results[fi].stride_max = 0; results[fi].stride_iqr = 0;
		results[fi].chase_min  = 0; results[fi].chase_max  = 0; results[fi].chase_iqr  = 0;
		results[fi].compute_min= 0; results[fi].compute_max= 0; results[fi].compute_iqr= 0;

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

		/*
		 * For hwmon power*_average: wait for the averaging window
		 * (1s) to settle so we get a stable idle reading.
		 * The test core is pinned but idle — all benchmarks are
		 * single-core, so other cores are running their normal
		 * background tasks.
		 */
		if (npower_paths > 0 && !power_is_uj)
			usleep(1100000);  /* 1.1s > averaging interval */

		progress++;
		dprintf("\r[%2d/%2d]  %4d MHz ...   ",
			progress, nfreqs, freqs[fi] / 1000);
		fflush(stderr);

		/* Measure idle power (no benchmark running yet) */
		long long power_idle = npower_paths > 0 ? read_power() : -1;
		if (power_idle > 0)
			results[fi].idle_power_uw = (double)power_idle;

		/* Measure loaded power (sample before tests start) */
		long long power_before = power_idle;  /* reuse idle as start */
		double t_energy_start = now();

		results[fi].valid = 1;

		/* stride */
		double cpu_before = cpu_time_sec();
		for (int s = 0; s < nsamples; s++) {
			if (do_flush)
				buf[s] = bench_stride_flush(arr, count,
							     stride, test_secs);
			else
				buf[s] = bench_stride(arr, count, stride,
						      test_secs);
		}
		qsort(buf, nsamples, sizeof(double), cmp_double);
		if (raw) memcpy(raw + ((size_t)fi * 4 + 0) * nsamples, buf, nsamples * sizeof(double));
		results[fi].stride_tput = buf[nsamples / 2];
		results[fi].stride_min = buf[0];
		results[fi].stride_max = buf[nsamples - 1];
		results[fi].stride_iqr = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;
		double cpu_after = cpu_time_sec();
		results[fi].stride_cpu_time = cpu_after - cpu_before;

		/* chase */
		if (do_chase) {
			for (int s = 0; s < nsamples; s++)
				buf[s] = bench_chase(chase_nodes, nnodes, test_secs);
			qsort(buf, nsamples, sizeof(double), cmp_double);
			if (raw) memcpy(raw + ((size_t)fi * 4 + 1) * nsamples, buf, nsamples * sizeof(double));
			results[fi].chase_tput = buf[nsamples / 2];
			results[fi].chase_min  = buf[0];
			results[fi].chase_max  = buf[nsamples - 1];
			results[fi].chase_iqr  = nsamples >= 3
			    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
			    : 0.0;
		}

		/* random permutation */
		if (do_random) {
			for (int s = 0; s < nsamples; s++)
				buf[s] = bench_random(arr, random_idx, count,
						      test_secs);
			qsort(buf, nsamples, sizeof(double), cmp_double);
			if (raw) memcpy(raw + ((size_t)fi * 4 + 2) * nsamples, buf, nsamples * sizeof(double));
			results[fi].random_tput = buf[nsamples / 2];
			results[fi].random_min  = buf[0];
			results[fi].random_max  = buf[nsamples - 1];
			results[fi].random_iqr  = nsamples >= 3
			    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
			    : 0.0;
		}

		/* compute */
		for (int s = 0; s < nsamples; s++)
			buf[s] = bench_compute(test_secs);
		qsort(buf, nsamples, sizeof(double), cmp_double);
		if (raw) memcpy(raw + ((size_t)fi * 4 + 3) * nsamples, buf, nsamples * sizeof(double));
		results[fi].compute_tput = buf[nsamples / 2];
		results[fi].compute_min = buf[0];
		results[fi].compute_max = buf[nsamples - 1];
		results[fi].compute_iqr = nsamples >= 3
		    ? percentile(buf, nsamples, 0.75) - percentile(buf, nsamples, 0.25)
		    : 0.0;

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
				/*
				 * hwmon: µW average power.
				 * Record loaded power, compute delta energy:
				 *   delta_energy = (P_load - P_idle) × time
				 * This isolates the single test core's
				 * incremental power from the full-SoC baseline.
				 */
				results[fi].load_power_uw = (double)power_after;
				double delta_uw = power_after - power_idle;
				double elapsed = t_energy_end - t_energy_start;
				if (delta_uw > 0)
					results[fi].energy_uj =
						delta_uw * elapsed;
			}
		}
		checked_signal();
	}
	dprintf("\r                                  \r");
	fflush(stderr);

	/* ---- thermal check ---- */
	long temp_after = read_max_temp();
	if (temp_after > 85000)  /* > 85°C */
		dprintf("WARN: temperature reached %ld.%ld°C — possible "
			"thermal throttling!\n",
			temp_after / 1000, (temp_after % 1000) / 100);

	/* ---- restore ---- */
	freq_cleanup();

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
		printf(" power=%s", power_is_uj ? "rapl" :
		       power_is_avg ? "hwmon_avg" : "hwmon_inst");
	printf("\n#\n");

	/* column headers */
	enum power_mode pm = get_power_mode();
	print_column_header(do_chase, pm);

	/* reference = highest valid freq */
	double s_max = results[ref].stride_tput;
	double c_max = do_chase ? results[ref].chase_tput : 0;
	double p_max = results[ref].compute_tput;

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

	/* Map compact index (0..n_valid-1) → results[] index. The compact
	 * arrays freqs_khz[]/mops[] are built from results[].valid rows in
	 * the same iteration order, so a compacted freq must correspond to
	 * exactly one results[] entry. We assert this here so the bootstrap
	 * slice-gather (which needs to pull raw[f][w] from results[f]) can
	 * fail loud on the impossible case instead of silently zero-filling. */
	int compact_to_results[MAX_FREQS];
	for (int ci = 0; ci < n_valid; ci++) {
		compact_to_results[ci] = -1;
		for (int k = 0; k < nfreqs; k++) {
			if (results[k].valid && results[k].freq_khz == freqs_khz[ci]) {
				compact_to_results[ci] = k;
				break;
			}
		}
		assert(compact_to_results[ci] >= 0);
	}

	int stride_sweet_idx = -1, chase_sweet_idx = -1;
	int stride_sweet = find_sweet_spot(stride_mops, freqs_khz, n_valid,
	                                   &stride_sweet_idx, threshold);
	int chase_sweet  = do_chase
		? find_sweet_spot(chase_mops, freqs_khz, n_valid,
		                  &chase_sweet_idx, threshold)
		: 0;

	/* data rows */
	print_data_rows(results, nfreqs, s_max, c_max, p_max, do_chase, pm);

	/* summary */
	printf("#\n");
	printf("# === Sweet spot (lowest freq ≥ %.0f%% of max throughput) ===\n",
	       threshold * 100);

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

	/* ---- per-freq stats blocks ---- */
	if (!summary)
		print_per_freq_stats(results, nfreqs, do_chase, do_random);

	/* ---- sweet-spot CI block ----
	 * Quantifies run-to-run uncertainty on the 95% sweet spot by
	 * bootstrap-resampling the per-freq raw samples, re-running
	 * find_sweet_spot() on each resample, and reporting the 2.5th
	 * and 97.5th percentile of the B sweet-spot values.
	 *
	 * Requires -r / --emit-raw so raw per-sample throughputs are
	 * available. The previous IQR-based default (alpha * IQR on the
	 * median curve) was dropped because it conflated per-sample
	 * dispersion with sweet-spot dispersion — the two are different
	 * statistics, and a confidence interval on the sweet spot has
	 * to propagate resampled medians, not raw IQR. The bootstrap
	 * does this correctly. */
	/* workload-enabled flags: stride always, chase/random if enabled, compute always */
	int wl_enabled[] = {1, do_chase ? 1 : 0, do_random ? 1 : 0, 1};

	if (raw && !summary) {
		const double *ci_mops[] = {stride_mops, chase_mops, random_mops, compute_mops};
		int headline_khz[] = {stride_sweet, chase_sweet, 0, 0};
		print_sweet_spot_ci(raw, nsamples, ci_mops, freqs_khz, n_valid,
		                    threshold, headline_khz, wl_enabled,
		                    compact_to_results);
	}

	/* ---- sensitivity blocks (only if --thresholds / -L was given) ----
	 * Re-runs sweet-spot detection at each user-supplied threshold so the
	 * user can see how sensitive the decision is to threshold choice.
	 * Per workload: stride always; chase/random only if that workload ran.
	 * Workloads with no plateau (compute) emit em-dash for every threshold. */
	if (n_user_thresholds > 0 && !summary) {
		const double *sens_mops[] = {stride_mops, chase_mops, random_mops, compute_mops};
		print_sensitivity(n_user_thresholds, user_thresholds,
		                  sens_mops, freqs_khz, n_valid, wl_enabled);
	}

	/* ---- plateau block (one row per workload that ran) ----
	 * Gated on -P / --no-plateau. For each workload, find the
	 * piecewise-linear breakpoint that minimizes SSE and report
	 * the slope ratio. slope_ratio > 2.0 ⇒ real plateau (mem-bound);
	 * < 2.0 ⇒ throughput keeps rising with frequency (compute-bound). */
	if (!no_plateau && !summary) {
		const double *plat_mops[] = {stride_mops, chase_mops, random_mops, compute_mops};
		print_plateau(plat_mops, freqs_khz, n_valid, threshold,
		              results, nfreqs, wl_enabled, compact_to_results);
	}

	/* ---- raw samples (only if --emit-raw / -r) ---- */
	if (raw && !summary)
		print_raw_samples(raw, nsamples, nfreqs, results, wl_enabled);

	/* ---- freq-lock sanity check ---- */
	print_freq_lock_sanity(results, nfreqs);

	free(buf);
	free(raw);
	free(results);
	free(arr);
	free(chase_nodes);
	free(random_idx);
	return 0;
}



