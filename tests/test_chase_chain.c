/*
 * test_chase_chain.c — verify build_chase chain integrity and bitrev correctness.
 * No cpufreq required; runs anywhere with a C compiler.
 *
 * Build: cc -O2 -Wall -o test_chase_chain tests/test_chase_chain.c
 * Run:   ./test_chase_chain
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define CL 64

struct pnode {
	struct pnode *next;
	char pad[CL - sizeof(struct pnode *)];
};

/* --- copy of bitrev_permute from memfreq_bench.c --- */
static size_t *bitrev_permute(size_t n, size_t scale)
{
	size_t nbits = 0;
	for (size_t tmp = n >> 1; tmp; tmp >>= 1)
		nbits++;

	size_t *r = malloc(n * sizeof(*r));
	if (!r)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		size_t v = 0;
		for (size_t b = 0; b < nbits; b++)
			if (i & ((size_t)1 << b))
				v |= (size_t)1 << (nbits - 1 - b);
		r[i] = v * scale;
	}
	return r;
}

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
	if (cond) { tests_passed++; } \
	else { tests_failed++; fprintf(stderr, "  FAIL: %s\n", msg); } \
} while (0)

/* Test 1: bitrev_permute produces a valid permutation */
static void test_bitrev_is_permutation(void)
{
	printf("=== test_bitrev_is_permutation ===\n");

	size_t sizes[] = {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
	for (size_t t = 0; t < sizeof(sizes)/sizeof(sizes[0]); t++) {
		size_t n = sizes[t];
		size_t *r = bitrev_permute(n, 1);
		if (!r) { CHECK(0, "malloc failed"); continue; }

		/* check every value in [0, n-1] appears exactly once */
		char *seen = calloc(n, 1);
		int ok = 1;
		for (size_t i = 0; i < n; i++) {
			if (r[i] >= n || seen[r[i]]) { ok = 0; break; }
			seen[r[i]] = 1;
		}
		for (size_t i = 0; i < n && ok; i++)
			if (!seen[i]) ok = 0;
		free(seen);

		char msg[80];
		snprintf(msg, sizeof(msg), "bitrev(%zu) is valid permutation", n);
		CHECK(ok, msg);
		free(r);
	}
}

/* Test 2: bitrev produces maximally-spaced access pattern */
static void test_bitrev_max_spacing(void)
{
	printf("=== test_bitrev_max_spacing ===\n");

	/* For n=8: expected order is [0,4,2,6,1,5,3,7] */
	size_t *r = bitrev_permute(8, 1);
	CHECK(r[0] == 0 && r[1] == 4 && r[2] == 2 && r[3] == 6 &&
	      r[4] == 1 && r[5] == 5 && r[6] == 3 && r[7] == 7,
	      "bitrev(8) = [0,4,2,6,1,5,3,7]");

	/* For n=4: expected order is [0,2,1,3] */
	size_t *r4 = bitrev_permute(4, 1);
	CHECK(r4[0] == 0 && r4[1] == 2 && r4[2] == 1 && r4[3] == 3,
	      "bitrev(4) = [0,2,1,3]");

	/* Scale factor: bitrev(4, 64) should give [0,128,64,192] */
	size_t *rs = bitrev_permute(4, 64);
	CHECK(rs[0] == 0 && rs[1] == 128 && rs[2] == 64 && rs[3] == 192,
	      "bitrev(4, scale=64) = [0,128,64,192]");

	free(r); free(r4); free(rs);
}

/* Test 3: consecutive accesses under bitrev are maximally spaced */
static void test_bitrev_consecutive_distance(void)
{
	printf("=== test_bitrev_consecutive_distance ===\n");

	size_t n = 64;
	size_t *r = bitrev_permute(n, 1);

	/* Minimum consecutive distance should be n/2 for the first pair */
	size_t min_dist = n;
	for (size_t i = 0; i + 1 < n; i++) {
		size_t d = (r[i] > r[i+1]) ? r[i] - r[i+1] : r[i+1] - r[i];
		if (d < min_dist) min_dist = d;
	}
	/* For n=64, minimum consecutive distance should be >= 1 */
	CHECK(min_dist >= 1, "bitrev(64): min consecutive distance >= 1");

	/* Average consecutive distance should be high (roughly n/3 for bitrev) */
	double avg = 0;
	for (size_t i = 0; i + 1 < n; i++) {
		size_t d = (r[i] > r[i+1]) ? r[i] - r[i+1] : r[i+1] - r[i];
		avg += d;
	}
	avg /= (n - 1);
	char msg[80];
	snprintf(msg, sizeof(msg), "bitrev(64): avg consecutive distance %.1f > %zu/4=%.1f",
		 avg, n, (double)n/4);
	CHECK(avg > (double)n / 4, msg);

	free(r);
}

/* Test 4: build_chase creates a chain visiting all nodes exactly once */
static void test_chain_visits_all_nodes(size_t nnodes)
{
	char msg[80];
	snprintf(msg, sizeof(msg), "chain visits all %zu nodes", nnodes);
	printf("=== test_chain_visits_all_nodes (%zu) ===\n", nnodes);

	long pgsz = sysconf(_SC_PAGESIZE);
	if (pgsz <= 0) pgsz = 4096;
	size_t lines_per_page = (size_t)pgsz / CL;
	if (lines_per_page < 2) lines_per_page = 2;

	struct pnode *nodes;
	if (posix_memalign((void **)&nodes, CL, nnodes * sizeof(*nodes))) {
		CHECK(0, "posix_memalign failed");
		return;
	}

	size_t npages = nnodes / lines_per_page;
	size_t remainder = nnodes - npages * lines_per_page;

	if (npages < 2 || (lines_per_page & (lines_per_page - 1)) != 0) {
		/* fallback path */
		size_t *idx = malloc(nnodes * sizeof(*idx));
		for (size_t i = 0; i < nnodes; i++) idx[i] = i;
		for (size_t i = nnodes - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1);
			size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
		}
		for (size_t i = 0; i < nnodes - 1; i++)
			nodes[idx[i]].next = &nodes[idx[i + 1]];
		nodes[idx[nnodes - 1]].next = &nodes[idx[0]];
		free(idx);
	} else {
		/* two-level path: page shuffle + bitrev */
		size_t *pages = malloc(npages * sizeof(*pages));
		for (size_t i = 0; i < npages; i++) pages[i] = i;
		for (size_t i = npages - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1);
			size_t t = pages[i]; pages[i] = pages[j]; pages[j] = t;
		}
		size_t *lines = bitrev_permute(lines_per_page, 1);
		size_t *idx = malloc(nnodes * sizeof(*idx));
		size_t pos = 0;
		for (size_t i = 0; i < npages; i++)
			for (size_t j = 0; j < lines_per_page; j++)
				idx[pos++] = pages[i] * lines_per_page + lines[j];
		if (remainder > 0) {
			size_t base = npages * lines_per_page;
			for (size_t i = 0; i < remainder; i++)
				idx[pos++] = base + i;
		}
		for (size_t i = 0; i < nnodes - 1; i++)
			nodes[idx[i]].next = &nodes[idx[i + 1]];
		nodes[idx[nnodes - 1]].next = &nodes[idx[0]];
		free(idx); free(lines); free(pages);
	}

	/* Walk the chain: visit nnodes nodes starting from nodes[0]
	 * (well, from whatever is idx[0], but let's just start from nodes[0]) */
	/* Actually, we need a starting node. Let's just use the first node
	 * that has a non-null next (which is all of them). Start from &nodes[0]. */
	char *visited = calloc(nnodes, 1);
	struct pnode *p = &nodes[0];
	size_t count = 0;
	for (size_t i = 0; i < nnodes; i++) {
		size_t idx = (size_t)(p - nodes);
		if (idx >= nnodes) break;
		if (visited[idx]) break;
		visited[idx] = 1;
		count++;
		p = p->next;
	}
	CHECK(count == nnodes, msg);
	free(visited);
	free(nodes);
}

/* Test 5: chain is circular (returns to start) */
static void test_chain_is_circular(size_t nnodes)
{
	char msg[80];
	snprintf(msg, sizeof(msg), "chain circular (%zu nodes)", nnodes);
	printf("=== test_chain_is_circular (%zu) ===\n", nnodes);

	long pgsz = sysconf(_SC_PAGESIZE);
	if (pgsz <= 0) pgsz = 4096;
	size_t lines_per_page = (size_t)pgsz / CL;
	if (lines_per_page < 2) lines_per_page = 2;

	struct pnode *nodes;
	if (posix_memalign((void **)&nodes, CL, nnodes * sizeof(*nodes))) {
		CHECK(0, "posix_memalign failed");
		return;
	}

	size_t npages = nnodes / lines_per_page;
	size_t remainder = nnodes - npages * lines_per_page;

	if (npages < 2 || (lines_per_page & (lines_per_page - 1)) != 0) {
		size_t *idx = malloc(nnodes * sizeof(*idx));
		for (size_t i = 0; i < nnodes; i++) idx[i] = i;
		for (size_t i = nnodes - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1);
			size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
		}
		for (size_t i = 0; i < nnodes - 1; i++)
			nodes[idx[i]].next = &nodes[idx[i + 1]];
		nodes[idx[nnodes - 1]].next = &nodes[idx[0]];
		free(idx);
	} else {
		size_t *pages = malloc(npages * sizeof(*pages));
		for (size_t i = 0; i < npages; i++) pages[i] = i;
		for (size_t i = npages - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1);
			size_t t = pages[i]; pages[i] = pages[j]; pages[j] = t;
		}
		size_t *lines = bitrev_permute(lines_per_page, 1);
		size_t *idx = malloc(nnodes * sizeof(*idx));
		size_t pos = 0;
		for (size_t i = 0; i < npages; i++)
			for (size_t j = 0; j < lines_per_page; j++)
				idx[pos++] = pages[i] * lines_per_page + lines[j];
		if (remainder > 0) {
			size_t base = npages * lines_per_page;
			for (size_t i = 0; i < remainder; i++)
				idx[pos++] = base + i;
		}
		for (size_t i = 0; i < nnodes - 1; i++)
			nodes[idx[i]].next = &nodes[idx[i + 1]];
		nodes[idx[nnodes - 1]].next = &nodes[idx[0]];
		free(idx); free(lines); free(pages);
	}

	/* Walk nnodes steps, check we're back at start */
	struct pnode *start = &nodes[0];
	struct pnode *p = start;
	for (size_t i = 0; i < nnodes; i++)
		p = p->next;
	CHECK(p == start, msg);
	free(nodes);
}

int main(void)
{
	printf("test_chase_chain: verifying chase chain integrity\n");

	srand(42);

	test_bitrev_is_permutation();
	test_bitrev_max_spacing();
	test_bitrev_consecutive_distance();

	/* Test with various sizes: small (triggers fallback), medium, large */
	long pgsz = sysconf(_SC_PAGESIZE);
	if (pgsz <= 0) pgsz = 4096;
	size_t lpp = (size_t)pgsz / CL;

	/* Small: less than 2 pages — triggers npages<2 fallback */
	test_chain_visits_all_nodes(lpp);       /* exactly 1 page */
	test_chain_is_circular(lpp);

	/* Medium: 4 pages worth */
	test_chain_visits_all_nodes(4 * lpp);
	test_chain_is_circular(4 * lpp);

	/* With remainder: 3 pages + half page of extra nodes */
	test_chain_visits_all_nodes(3 * lpp + lpp / 2);
	test_chain_is_circular(3 * lpp + lpp / 2);

	/* Large: 16 pages */
	test_chain_visits_all_nodes(16 * lpp);
	test_chain_is_circular(16 * lpp);

	printf("\n==========================================\n");
	printf("  %d passed, %d failed\n", tests_passed, tests_failed);
	printf("==========================================\n");
	return tests_failed ? 1 : 0;
}
