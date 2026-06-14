---
name: comprehensive-code-review
description: Multi-phase, multi-agent approach for deep code review of C/Python/Shell projects with independent verification of critical findings
source: auto-skill
extracted_at: '2026-06-14T04:19:14.872Z'
---

# Comprehensive Code Review Procedure

A structured, verification-heavy approach for auditing multi-language repositories (C, Python, Shell, docs). Designed to catch bugs that single-pass reading misses, especially in large C files (~2000+ lines).

## Phase 1: Scaffold — Read small, well-defined modules first

1. Read build system (Makefile, Dockerfile) and small utility modules (stats.h, stats.c) **in parallel** via multiple `read_file` calls.
2. Read `.gitignore` for build artifact awareness.
3. These files are small enough to review inline and establish baseline expectations (coding style, comment quality, boundary handling).

**Why**: Small modules reveal the project's quality bar. If they're clean, the review can focus on the large files where bugs concentrate. If they're sloppy, that signals systemic issues.

## Phase 2: Dispatch — Parallel agent review of large files

Split large files across sub-agents, each tasked with a deep multi-dimensional review:

- **Agent A**: Main C program (~2600 lines) — read in chunks, review for bugs, security, code quality, architecture, concurrency, platform compat, performance
- **Agent B**: Python + test code — all Python files and test fixtures
- **Agent C**: Shell scripts + documentation

Each agent must categorize findings by severity (Critical/High/Medium/Low) and provide: exact location, problem description, impact, fix suggestion.

**Critical**: Each agent MUST be instructed to **only research, not modify code**. This prevents premature fixes that obscure other findings.

## Phase 3: Independent verification — Never trust agent reports blindly

**This is the most important step.** Agent output can be garbled, hallucinated, or subtly wrong. Verify every Critical and High finding yourself:

1. Use `grep_search` to locate the exact function/variable in the source.
2. Use `read_file` with targeted line ranges to read the actual code.
3. Cross-check against project documentation (AGENTS.md, CLAUDE.md) — the project's own docs may explain seemingly-buggy behavior as intentional.

### Verification pattern for claimed bugs:

```
# For a claim about function X:
1. grep_search("X") → get exact line numbers
2. read_file(file, offset=line-20, limit=60) → read the actual code
3. Compare agent's description against the real code
4. Check AGENTS.md for intentional design notes about X
```

### Common verification pitfalls:
- **"Bug" that's actually documented as load-bearing**: e.g., AGENTS.md says "write-max-before-min order is load-bearing" — but the code's own comments reveal it's actually wrong for descending scans. Both sources must be read.
- **Agent garbled output**: If an agent's response is corrupted/garbled, **don't use it**. Re-read those files directly.
- **Severity inflation**: Agents sometimes label Medium issues as Critical. Re-verify the actual impact.

## Phase 4: Compile — Merge and deduplicate findings

1. Merge findings from all agents + your own verification.
2. Deduplicate: multiple agents may find the same issue.
3. Re-rank severity based on verified impact, not agent claims.
4. Group by module for clarity.

## Output format

Produce a severity-graded report:
- **Critical**: Must fix immediately — core functionality broken, data corruption, security hole
- **High**: Severe impact on correctness or reliability in common scenarios
- **Medium**: Impact in specific/unusual scenarios or robustness gaps
- **Low**: Code quality, style, minor efficiency

Each finding: **Location → Problem → Impact → Fix suggestion**.

## Key lessons from this project's reviews

### Code correctness
1. **Comments can document the correct approach while code implements the wrong one** — `set_freq()` comments describe "min-first for descending" but code does "always max-first". Read both.
2. **sysfs semantics vary by architecture** — x86 `cache/index3/size` reports total L3, not per-core slice. ARM may differ. Don't assume one interpretation.
3. **Memory leaks in cleanup sections** — grep for `free()` calls and cross-check against all `malloc/calloc` calls. Missing frees are easy to spot this way.
4. **Shell script copy-paste bugs** — when two scripts share 60% code, one may have a fix the other lacks (e.g., topology fallback). Compare them.
5. **Signal handling matters for sysfs-modifying tools** — any program that writes to `/sys/` needs SIGINT/SIGTERM handlers to restore original state.

### Testing & fixtures
6. **Test fixtures that don't match real output mask critical bugs** — if fixtures were hand-crafted to match a parser's *expected* column layout rather than captured from real tool output, all parser tests may give false passes. Always verify fixtures against actual `printf` format strings in the C source (column count, delimiter type, padding style).
7. **Tab vs space-padded output** — C `printf("%-8s")` uses space-padding, but Python `split("\t")` expects tabs. The C output and the parser's delimiter must be checked independently, not assumed.

### Wrapper / wrapped-tool interface
8. **Output format drift between C tool and Python parser** — when the C tool adds columns (e.g., `actual_MHz`, `stride_MBs`, power columns), a parser using fixed `len(parts) == N` checks will silently drop all real data rows. Verify column count and index mapping against the C `printf` statements, not against README documentation.
9. **Short-flag collisions** — a Python wrapper using `argparse` with `-c` for `--compare` and `-f` for `--file` will intercept flags the user intended for the C binary (`-c CPU`, `-f flush`). Use `parse_known_args()` carefully and check every short flag against the wrapped tool's getopt string.

### Shell script robustness
10. **`set -u` + conditional color variables** — if color vars (`RED`, `BLUE`, `NC`) are only assigned inside `if [[ -t 1 ]]`, they're undefined when stdout is redirected. With `set -u`, the first `log_*` call crashes. Always initialize to empty strings before the `if`.
11. **Script exit code ignores test failures** — scripts that track `TESTS_FAIL` but never use it as an exit code defeat CI. Always end with `[[ $TESTS_FAIL -gt 0 ]] && exit 1`.

### Multi-process state tracking
12. **Per-core vs per-freq ready flags** — if `shm->ready[c]` is a single boolean per core (not per frequency point), and it's cleared at the start of each freq loop, then a fork failure on the *last* frequency point wipes all previously-successful data for that core. Use per-frequency-per-core ready flags, or aggregate immediately after each freq loop.
