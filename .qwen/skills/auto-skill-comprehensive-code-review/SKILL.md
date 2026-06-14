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

## Phase 5: User-corrected verification — Present findings for independent review

**This phase is essential when the user has domain expertise.** After producing the severity-graded report, present it to the user and ask them to independently verify each finding. Users often catch errors that both agents and the verification phase miss.

### What to expect from user corrections:

1. **"不成立" (not valid) markings with specific reasoning** — The user may mark findings as incorrect and provide technical justification. Common patterns:
   - **Wrong target file/function**: Agent confused shell script code with C code, or checked the wrong file (e.g., Makefile vs docs compile command)
   - **Theoretical risk vs practical reality**: Agent flagged a division-by-zero that can't occur because an earlier guard prevents it
   - **Correct behavior misidentified as bug**: Agent didn't understand the domain semantics (e.g., strtok with multi-char delimiter set working as intended for the actual input format)

2. **"部分确认" (partially confirmed) markings** — The user accepts the core finding but narrows the impact scope.

### How to handle user corrections:

1. **For each "不成立" marking**: Re-read the actual code at the exact location the user referenced. Do NOT assume the user is right or wrong — verify independently.
2. **Produce a correction table** showing: original finding → user's assessment → your re-verification → final status.
3. **Accept corrections gracefully**: If the user is right, mark the finding as "不成立" in the final report. If you find the user is wrong after re-verification, explain why with code evidence.

### Common agent errors that users catch:

- **File confusion**: Agent verifies finding against the wrong file (e.g., C `gen_freq_range` vs shell script loop)
- **Checking wrong location**: Agent says "Makefile is correct" when the finding was about docs/error-message
- **Over-caution**: Agent flags theoretical edge cases that are guarded elsewhere in the code
- **Misunderstanding domain APIs**: Agent doesn't know that `strtok(buf, ",-")` splits on BOTH characters as a set

### Final status table format:

```markdown
| # | Severity | Final Status | Notes |
|---|----------|-------------|-------|
| C1 | Critical | ✅ Confirmed | Parser column count mismatch |
| M2 | Medium | ❌ Not valid | c_max division-by-zero cannot occur |
| M6 | Medium | ✅ Confirmed | User checked wrong file (Makefile, not docs) |
```

## Phase 6: Execute fixes — Prioritized defect remediation

After findings are confirmed, execute fixes in a disciplined order.

### Fix ordering strategy:

1. **Simple surgical fixes first** (1-3 line changes): flag removal, guard addition, order swap. These build confidence and are easy to review.
2. **Medium-complexity fixes next** (regex updates, initialization changes): require understanding but not rewriting.
3. **Parser/logic rewrites last** (the hardest changes): require fixture updates and may cascade to test assertions.

### Procedure for each fix:

```
1. Edit the code
2. Compile (for C) or syntax-check (for Python)
3. Run full test suite
4. If tests fail, fix the test/fixture before proceeding
```

### Commit strategy:

- **One commit for all related fixes** when they share a root cause (e.g., "parser and fixture both wrong")
- **Separate commits for unrelated fix categories** (e.g., "C code fixes" vs "shell script fixes") when they touch different files
- **Commit message format**: Lead with the severity counts, then list each fix with a one-line description

### Parser rewrite pattern (for C1-type bugs):

When a Python parser uses hardcoded column counts to match C output:

1. **Replace fixed-count matching with header-based mapping**: Parse the column header line (`# target_MHz\tactual_MHz\t...`) to build a `{field_name: column_index}` dictionary, then use it for all data rows.
2. **Replace tab-split with whitespace-split** for space-padded fields: Use `body.split()` instead of `body.split("\t")` when C uses `printf("%-8s")` formatting.
3. **Regenerate test fixtures** to match the actual C output format. The fixture must be a byte-exact copy of what the C tool produces, not a hand-crafted approximation.
4. **Update test assertions** that reference fixture-specific values if the fixture format changed.

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

### User-corrected verification patterns
13. **Users catch file-confusion errors that agents miss** — An agent may verify a finding against the wrong file (e.g., check Makefile when the finding was about docs/error-message, or check C code when the finding was about a shell script). Always include the *exact file and line number* in each finding so the user can verify efficiently.
14. **Theoretical vs practical edge cases** — Agents tend to flag "what if X is zero?" without checking whether an earlier code path guarantees X > 0. Users with domain knowledge dismiss these instantly. Before flagging, grep for guards that prevent the condition.
15. **Parser rewrites must include fixture regeneration** — Fixing the parser alone is insufficient if the test fixture was hand-crafted to match the old parser's wrong assumptions. The fixture must be regenerated from actual C output, and any test assertions that reference fixture-specific values must be updated. This is a 3-step process: fix parser → regenerate fixture → update assertions.
