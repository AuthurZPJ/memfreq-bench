---
name: doc-code-consistency-audit
description: Systematically cross-reference all project documentation against actual code to find stale or incorrect claims
source: auto-skill
extracted_at: '2026-06-14T12:22:34.248Z'
---

# Documentation-Code Consistency Audit

A structured procedure for finding claims in project documentation that no longer match the actual code. Documentation drift is pervasive — code evolves through refactoring and bug fixes while docs lag behind. This audit catches the gap.

Distinct from "is the doc well-written?" reviews. This pattern answers one question: **does every factual claim in the docs match the current code?**

## When to use

- After a significant refactoring pass (especially output format changes, API changes, renamed functions)
- Before a release or milestone
- When a new contributor asks "is this doc still accurate?"
- As part of a broader code review (complements bug-finding reviews)

## Phase 1: Inventory — Read ALL documentation files

Read every documentation file in the project. For a typical project:

1. `README.md` — user-facing quick start
2. `docs/*.md` — design docs, guides, references
3. `AGENTS.md` / `CLAUDE.md` / `CONTRIBUTING.md` — developer-facing context
4. Inline docstrings in Python/Shell scripts that describe usage

**Read all files completely** — don't skim. The bugs hide in details like column counts in example output, loop bounds in code snippets, and line-count claims in architecture tables.

## Phase 2: Cross-reference — Verify every factual claim

For each document, systematically verify claims against the code. Organize by claim type:

### 2.1 Output format claims

**What to check**: Example output shown in docs vs actual `printf` format strings in code.

```
# For each output example in docs:
1. Find the corresponding printf/fprint in the code
2. Count the columns in the printf format string
3. Count the columns in the doc example
4. Verify column names match the header line
5. Check delimiter (tab vs space) and padding style
```

**Common drift patterns**:
- Code adds columns (e.g., `actual_MHz`, `stride_MBs`) but doc example still shows the old column set
- Doc shows space-aligned columns but code uses tab-separated
- Doc shows N columns but code outputs N+2 due to a power-mode conditional

### 2.2 Code snippet claims

**What to check**: Code examples in docs vs actual implementation.

```
# For each code snippet in docs:
1. Find the corresponding function in the code
2. Compare loop bounds, variable names, step counts
3. Check if the snippet shows 2 steps but code does 3 (or vice versa)
4. Verify magic numbers match (e.g., "100K iterations" vs actual `nnodes`)
```

**Common drift patterns**:
- Doc shows a 2-step algorithm but code was refactored to 3 steps
- Doc hardcodes a constant ("100K inner loop") but code uses a variable (`nnodes`)
- Doc shows pseudocode function names that were renamed

### 2.3 Numeric/metadata claims

**What to check**: Line counts, file sizes, assertion counts, function counts.

```bash
# Verify each numeric claim:
wc -l source_file.c           # vs doc's "~2600 lines"
grep -c "PASS:" test_output   # vs doc's "76 assertions"
```

**Common drift patterns**:
- Line counts stale after refactoring (code grew or shrank)
- Assertion counts change when tests are added/removed
- File size estimates (`~85 KB`) drift as code grows

### 2.4 Error message claims

**What to check**: Error messages quoted in docs vs actual `printf`/`dprintf` strings in code.

```
# For each quoted error message in docs:
1. grep for the error text in the code
2. Verify exact wording matches
3. Check if the error condition still exists
```

**Common drift patterns**:
- Doc quotes old error text that was reworded
- Doc describes an error that no longer occurs (code was fixed)
- Doc quotes a system error (`sched_setaffinity: No such file`) but code now prints a custom message

### 2.5 Architecture/structure claims

**What to check**: File listings, module descriptions, function-to-responsibility mappings.

```
# For each architecture table in docs:
1. Verify every listed file still exists
2. Check if unlisted files should be added (new files from refactoring)
3. Verify function names and descriptions match current code
4. Check "default on/off" claims for optional features
```

### 2.6 Limitation/caveat claims

**What to check**: Stated limitations vs actual capabilities.

```
# For each "limitation" or "caveat" in docs:
1. Check if the limitation was addressed by recent code changes
2. E.g., doc says "doesn't measure power" but code now reads RAPL sensors
3. E.g., doc says "requires X" but code now has a fallback
```

**Common drift patterns**:
- Limitation was fixed in code but doc still lists it
- Capability was added but doc doesn't mention it
- Caveat was partially addressed — doc needs nuanced update

## Phase 3: Two-pass review

**Pass 1** (quick scan): Read docs top-to-bottom, flag obvious mismatches. This catches the easy ones (wrong compile commands, missing files in listings).

**Pass 2** (deep cross-reference): For each doc section, grep the code for every specific claim. This catches the subtle ones (loop bounds, step counts, column names).

The two-pass approach is essential because Pass 1 creates a mental model of the docs, and Pass 2 verifies that model against reality. Many drift issues are invisible in either pass alone.

## Phase 4: Fix and verify

### Fix ordering:

1. **Compile command errors first** — these cause immediate user failure
2. **Output format examples next** — these cause confusion and wrong parsing
3. **Code snippet corrections** — these cause misunderstanding of internals
4. **Numeric/metadata updates** — these are cosmetic but erode trust
5. **Limitation/caveat rewording** — these affect user expectations

### Verification after each fix:

```bash
# For code-related fixes:
make clean && make && make test && ./test
bash tests/test_harness.sh

# For doc-only fixes:
git diff --stat    # verify only doc files changed
```

### Commit strategy:

- Group related doc fixes into one commit
- Commit message should list each corrected claim
- If doc fixes accompany code changes, include them in the same commit

## Output format

Produce a table of findings:

```markdown
| # | File | Location | Claim in doc | Actual in code | Severity |
|---|------|----------|--------------|----------------|----------|
| 1 | README | Line 42 | 7 columns in output | 9 columns (added actual_MHz, stride_MBs) | High |
| 2 | design.md | Line 713 | 2-step set_freq | 3-step (widen → floor → tighten) | Medium |
| 3 | AGENTS.md | Line 38 | ~2600 lines | ~2930 lines | Low |
```

Severity guide:
- **High**: Causes user failure (wrong compile command, wrong error message to look for)
- **Medium**: Causes misunderstanding (wrong algorithm steps, wrong output format)
- **Low**: Cosmetic drift (line counts, file sizes)

## Phase 5: Project-wide constant sweep

After fixing individual drift issues, check whether any **project-wide constants** changed and were incompletely propagated.

### When this applies:

- A test count changed (e.g., 52 → 57 tests after adding a new suite)
- A suite count changed (e.g., 6 → 7 suites)
- A duration estimate changed (e.g., "~10-30 min" → "~15-40 min")
- An API signature changed (e.g., function gained a parameter)

### Procedure:

1. **Grep every file type** for the old constant value:
   ```bash
   grep -rn "52 tests\|52 个测试\|52-test" *.md docs/ *.sh *.py
   ```
   Don't limit to `.md` — shell scripts, Python files, and config files all contain documentation-like claims (help text, comments, estimation constants).

2. **Check shell script hardcoded constants** — Scripts often have `est_tests=52` or similar constants used for progress display. When the actual count changes, these go stale. Prefer dynamic computation:
   ```bash
   # BAD: hardcoded constant that goes stale
   est_tests=52

   # GOOD: dynamic count from actual suite contents
   est_tests=0
   est_tests=$((est_tests + 7))   # Suite A
   est_tests=$((est_tests + 11))  # Suite C
   # ...
   ```

3. **Check help text in scripts** — `--help` output often describes test counts, suite descriptions, and duration estimates. These are documentation embedded in code.

4. **Check for sibling documentation files** — Projects often have both `AGENTS.md` (for one AI tool) and `CLAUDE.md` (for another) that contain overlapping content. When one is updated, the other must be synced. The sync pattern:
   ```bash
   cp AGENTS.md CLAUDE.md
   # Then change only the tool-specific header line
   sed -i '' 's/guidance to Codex/guidance to Claude Code/' CLAUDE.md
   ```

### Output-line propagation checklist

When the C binary adds or removes an output line (e.g., `# stride_l3 sweet spot: N MHz`), the downstream pipeline must be updated atomically. Use this checklist:

1. **C printf** — the source of truth
2. **Python parser** — `parse_output()` must recognize the new line pattern
3. **Shell report extractor** — `extract_data()` must parse the new field (often a new `|`-delimited column)
4. **Test fixtures** — add example data for the new line/block to `.txt` and `.json` fixtures
5. **Test assertions** — update expected counts (e.g., `plateau_rows=2` → `3`)
6. **Doc example outputs** — every `# ---` block example and sweet-spot summary in docs must show/hide the new line
7. **CSV headers** — add the new column name to the header row
8. **Report tables** — add the new table or column to the report generation code
9. **DVFS recommendations** — add new recommendation if the output drives one

The same checklist applies in reverse when *removing* an output artifact (e.g., removing `compute` from statistical blocks). Every downstream consumer must drop the corresponding parsing/extraction/display code.

## Key lessons

1. **First pass catches 20% of issues, second pass catches the other 80%** — Quick reading misses subtle numeric drift and code snippet mismatches. Always do both passes.
2. **Output format examples are the highest-risk drift** — Users copy-paste examples to build parsers. If the doc example has 7 columns but the tool outputs 9, every downstream parser breaks.
3. **Compile commands in docs are the easiest to get wrong** — After extracting a module (e.g., `stats.c`), every doc that shows the old compile command (`gcc file.c`) needs updating. Grep for the compile pattern across all docs.
4. **Limitation sections go stale the fastest** — Features get added, limitations get fixed, but nobody remembers to update the "limitations" section. Always re-read it after a feature-adding refactor.
5. **Code snippets in design docs are time capsules** — They capture the code at the time the doc was written. After any refactoring of the described function, re-verify every snippet.
6. **Error messages quoted in docs must be grep-verified** — Don't trust your memory of what the error says. Grep for the exact quoted text in the source.
7. **File listing tables need periodic refresh** — After adding `stats.c`, `stats.h`, test files, and shell scripts, the original 3-row table is incomplete. Always check the file listing against `ls`.
