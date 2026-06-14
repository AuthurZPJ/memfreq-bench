---
name: readiness-readability-review
description: Two-phase approach to evaluate a benchmarking/measurement tool's functional readiness and code readability, combining methodology verification with dimension-based readability scoring
source: auto-skill
extracted_at: '2026-06-14T05:39:52.496Z'
---

# Readiness & Readability Review

A structured evaluation pattern for benchmarking and measurement tools that goes beyond bug-finding to answer two questions:
1. **Is this tool ready?** — Does it correctly measure what it claims to measure?
2. **Is the code readable?** — Where are the readability strengths and debts?

Distinct from bug-hunting code reviews (which find correctness issues). This pattern validates *fitness for purpose* and *maintainability*.

## Phase 1: Functional Readiness Evaluation

### Step 1.1 — Build & Test Baseline

Before any code analysis, verify the project is in a buildable, testable state:

```bash
make clean && make          # zero warnings with -Wall -Wextra?
make test && ./test         # unit tests pass?
bash tests/test_harness.sh  # integration tests pass?
```

**What to report**: compiler warnings count, test pass/fail/skip counts. A project that doesn't build cleanly or has failing tests is not "ready" regardless of methodology quality.

### Step 1.2 — Methodology Correctness

For benchmarking/measurement tools, the central question is: *does the tool correctly answer its stated question?*

Evaluate these dimensions:

| Dimension | What to check |
|-----------|---------------|
| **Workload design** | Are the workloads representative of what they claim to measure? (e.g., bandwidth vs latency vs compute) |
| **Control variables** | Is there a control workload that validates the measurement setup? (e.g., compute-bound test as sanity check for frequency locking) |
| **Measurement integrity** | Does the tool verify its own assumptions? (e.g., checking that frequency actually changed, detecting system noise) |
| **Error handling** | Are failure modes detected and reported? (e.g., signal handlers for cleanup, sysfs write validation) |
| **Statistical rigor** | Are results reported with confidence intervals, not just point estimates? |

**Key pattern**: Look for self-validating controls. A well-designed measurement tool includes a workload whose behavior is *predictable* (e.g., compute throughput ∝ frequency) so the user can verify the measurement setup worked. If this control is missing, flag it.

### Step 1.3 — Honest Limitations Table

Every measurement tool has inherent limitations. Produce a table:

| Limitation | Impact | Mitigation present? |
|------------|--------|---------------------|
| Platform constraint (e.g., Linux-only) | Cannot run on macOS/Windows | Documented? |
| Measurement fidelity (e.g., sysfs vs MSR) | May not reflect true hardware state | Sanity checks? |
| Result interpretation (e.g., single point vs interval) | May overstate confidence | Bootstrap CI? |

A tool is "ready" if its limitations are (a) documented and (b) detectable at runtime.

## Phase 2: Readability Audit

### Step 2.1 — Targeted Deep Reading

Read the codebase in a strategic order:

1. **Small, well-defined modules first** (headers, utility files) — establish quality baseline
2. **Core algorithm functions** — evaluate clarity of the actual logic
3. **Orchestration / main()** — evaluate complexity management
4. **Output formatting** — evaluate combinatorial complexity
5. **Cross-language parsers** — evaluate schema consistency between producer and consumer

Use `grep_search` to find function definitions and `read_file` with offset/limit to read them in chunks.

### Step 2.2 — Dimension-Based Scoring

Score each dimension on a 5-star scale:

| Dimension | What to evaluate | Red flags |
|-----------|------------------|-----------|
| **File-level organization** | Module boundaries, separation of concerns | Single monolithic file for everything |
| **Function-level readability** | Naming consistency, clear signatures, bounded scope | Functions >100 lines, unclear naming |
| **Comment quality** | Why-comments over what-comments, domain explanations | Missing "why" for non-obvious decisions |
| **Main/orchestration complexity** | Delegated to helper functions vs inline | >300 lines, combinatorial printf/formatting |
| **Code duplication** | Shared logic extracted vs copy-paste | Same loop pattern in multiple places |
| **Cross-language consistency** | Producer/consumer schema agreement | Parser uses regex to match fragile output format |

### Step 2.3 — Produce a Summary Table

```markdown
| Dimension | Rating | Notes |
|-----------|--------|-------|
| File organization | ★★★★★ | Clean module split |
| Function readability | ★★★★☆ | Consistent naming |
| Comment quality | ★★★★☆ | Good "why" coverage |
| main() complexity | ★★☆☆☆ | 900-line god function |
| Code duplication | ★★☆☆☆ | Output formatting × 6 branches |
| Cross-language | ★★★☆☆ | Regex parsing is fragile |
```

### Step 2.4 — Actionable Recommendation

End with the single highest-leverage readability improvement. For large C programs, this is typically:

- Extracting output formatting from `main()` into per-block print functions
- Unifying duplicated orchestration loops (e.g., single-core vs multi-core)
- Replacing magic numbers with named constants

## Phase 3: Incremental Refactoring Execution

When Phase 2 identifies readability debt, execute the refactoring in **two independent commits** — each safe to merge alone.

### Commit 1: Extract output formatting from `main()` into static helpers

**Problem pattern**: `main()` is a 900-line god function because output formatting (printf combinatorics for power modes × workloads × optional columns) is inline.

**Procedure**:

1. **Read the entire output section** of `main()` to identify discrete blocks (column headers, data rows, per-freq stats, CI, sensitivity, plateau, raw samples, sanity checks).
2. **Insert all static helper functions** before `main()` in a single edit. Each function should:
   - Take all data it needs via parameters (no hidden global dependencies beyond pre-existing module globals)
   - Have a clear doc comment explaining what it prints
   - Match the original output byte-for-byte
3. **Add forward declarations** if helpers reference types defined later in the file.
4. **Compile immediately** after insertion — expect `-Wunused-function` warnings only (helpers exist but aren't called yet). This verifies syntax.
5. **Replace inline blocks one at a time** in `main()` with calls to the new helpers. After each replacement, compile to catch integration errors.
6. **Final compile + full test run** to verify zero regression.

**Anti-pattern: `offsetof` accessor trick** — Don't try to pass struct field offsets as parameters to generic print functions. Use function-pointer accessors instead:

```c
typedef void (*stat_accessors)(const struct result *,
                               double *mn, double *mx, double *md, double *iq);
static void stride_stats(const struct result *r,
                         double *mn, double *mx, double *md, double *iq)
{ *mn = r->stride_min; *mx = r->stride_max;
  *md = r->stride_tput; *iq = r->stride_iqr; }
```

### Commit 2: Eliminate cross-function duplication

After Commit 1, scan for remaining duplication — typically between `main()` (single-core) and other entry points (e.g., `run_multicore()`).

**Procedure**:

1. **Use an explore agent** to do a systematic scan for duplicate patterns across all entry points. Report exact line ranges.
2. **Categorize findings** by severity: Major (functional duplication), Moderate (verbatim copy), Minor (intentional structural similarity).
3. **Fix Major duplications first** — these often reveal bugs (e.g., one entry point hardcoding a threshold that the other respects from CLI).
4. **Share helpers across entry points** — add forward declarations if needed, pass `threshold` as parameter instead of hardcoding.
5. **Extract shared setup/teardown** — e.g., `freq_lock(cpu)` replaces the inline save-range + set-recovery-state + disable-boost pattern.
6. **Merge identical array initializations** — e.g., four `enabled[] = {1, do_chase ? 1 : 0, do_random ? 1 : 0, 1}` arrays become one `wl_enabled[]` declared before the conditional blocks.
7. **Compile + full test run** after each logical change.

**What NOT to deduplicate**:
- Functions with `__attribute__((noinline))` — the separation is load-bearing for compiler behavior
- Error paths that differ by context (`_exit()` in fork child vs `return` in main)
- Per-workload benchmark loops where the bench function signature differs

### Verification checklist for each commit

```
[ ] make clean && make         → 0 warnings, 0 errors
[ ] make test_stats && ./test_stats  → ALL CHECKS PASSED
[ ] bash tests/test_harness.sh  → N passed, 0 failed (skip = expected)
[ ] git diff --stat             → only expected files changed
```

## Key Lessons

1. **"Ready" ≠ "bug-free"** — A tool can have zero test failures but still not correctly measure what it claims. Methodology review catches this.
2. **Self-validating controls are the hallmark of good measurement tools** — If the tool can detect its own failures (e.g., compute_% as frequency-lock sanity check), it's production-grade.
3. **Readability debt concentrates in output formatting** — In C programs that produce structured output, the `printf` combinatorics (power modes × workloads × optional columns) often balloon `main()` to unmanageable size. This is the most common readability bottleneck.
4. **Cross-language fragility** — When C produces output and Python parses it, the schema lives only in the C code's `printf` format strings and the Python code's regexes. Any change to one side can silently break the other.
5. **Read both code AND project docs before judging** — A seemingly bad pattern may be documented as intentional (e.g., "write-max-before-min order is load-bearing").
6. **Two-commit refactoring is safer than one big commit** — Commit 1 (extract) is pure code motion; Commit 2 (deduplicate) is behavioral. Each is independently reviewable and revertable.
7. **Forward declarations solve ordering problems** — When a helper needs to be called from a function defined earlier in the file, add a forward declaration block rather than reshuffling the file.
8. **Deduplication often reveals bugs** — When two entry points duplicate logic, one typically has a divergence that's actually a bug (e.g., hardcoded threshold vs. CLI-overridable). The act of deduplicating forces you to pick one behavior, surfacing the bug.
9. **`-Wunused-function` is your friend during insertion** — When you add helpers before replacing their inline counterparts, the compiler tells you exactly which ones are still unused, confirming your replacement plan.
