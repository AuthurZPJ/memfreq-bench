# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`memfreq_bench` finds the CPU frequency "sweet spot" for memory-bound workloads — the lowest frequency at which a workload still achieves ≥95% of its peak DRAM throughput. The point is to characterize the energy-saving headroom available from DVFS for memory-bound code (P ∝ V²·f, so if throughput doesn't drop with f, power can).

The tool runs three workloads at every available CPU frequency and prints a TSV table that the Python wrapper parses and visualizes.

## Build & run

```bash
# Build (via Makefile or directly)
make                              # → memfreq_bench binary
# or: gcc -O2 -o memfreq_bench memfreq_bench.c stats.c -lm

# Build + run the C unit tests for the stats helpers (no root needed):
make test_stats && ./test_stats

# Requires root for cpufreq sysfs writes
sudo ./memfreq_bench -m 512       # array must exceed L3 cache

# One-click wrappers
sudo ./run_all_tests.sh           # 7 predefined suites, ~15–40 min
sudo ./run_full_sweep.sh          # exhaustive grid, ~3–4 h

# Visualization
sudo python3 memfreq_sweep.py     # run + ASCII bar charts
python3 memfreq_sweep.py --file results.txt   # re-parse saved output
python3 memfreq_sweep.py --json                # also write memfreq_results.json
```

There is one lightweight test suite (`bash tests/test_stats_output.sh`,
77 assertions, runs anywhere POSIX) plus the e2e `run_*.sh` scripts.

## Repository layout

| File | Role |
|------|------|
| `memfreq_bench.c` | Main C benchmark (~90 KB, ~2930 lines). Workloads, sysfs, topology, fork-based multicore coordinator, output formatting. |
| `stats.c` | Statistical helpers (~213 lines): `find_sweet_spot`, `bootstrap_sweet_spot_ci`, `detect_plateau`, `percentile`, `cmp_double`. |
| `stats.h` | Public API for stats.c plus `MAX_FREQS` / `MAX_SAMPLES` constants. |
| `memfreq_sweep.py` | Python runner + TSV parser + ASCII chart visualizer. |
| `Makefile` | Build for `memfreq_bench` and `test_stats`. |
| `tests/test_stats.c` | C unit tests for the stats helpers (no Linux/cpufreq required). |
| `tests/test_stats_output.sh` | Shell test harness: 77 assertions over Python parser, JSON output, compare mode, plus runs `test_stats`. |
| `tests/fixtures/` | TSV/JSON fixtures used by the shell harness. |
| `run_all_tests.sh` | One-click suite runner (single-core, multi-core, strides, random, flush, NUMA, cache hierarchy). |
| `run_full_sweep.sh` | Exhaustive multi-hour sweep across stride × core-count × NUMA × cache hierarchy matrix (57 tests). |
| `docs/memfreq-bench.md` | Full design rationale, microarchitectural analysis, noise model, tuning guide. **The single best reference for "why" questions.** |
| `README.md` | Quick-start only. |

## Architecture of `memfreq_bench.c`

The main translation unit, organized by `/* --- */` section banners (line
numbers drift with edits — use the banners, not line refs, to find things):

1. **Platform flush primitive** (`flush_cacheline`): `clflush` on x86,
   `dc cvac` on aarch64, no-op elsewhere. Used only by the `-f` (cache-flush)
   workload.

2. **Topology detection** (`detect_numa_nodes`, `detect_freq_domains`,
   `select_cpus`, `select_primary_threads`): reads
   `/sys/devices/system/node/*/cpulist`, `cpufreq/related_cpus`; distributes
   CPUs across NUMA nodes one-per-freq-domain, preferring non-SMT primary
   threads.

3. **Frequency enumeration** (`read_freqs`): handles two regimes —
   `scaling_available_frequencies` (discrete) and CPPC
   `scaling_min_freq`/`scaling_max_freq` range mode (generates a stepped grid
   via `gen_freq_range`). The `step_khz` CLI flag controls CPPC step size.

4. **Frequency control** (`set_freq`): a **three-step write** is
   load-bearing — (1) widen max to hardware limit, (2) set min to target,
   (3) tighten max to target. Writing min first when it exceeds current max
   would be rejected by the kernel. The target is clamped to hardware max
   to prevent CPPC overshoot. `freq_lock()` handles setup, `freq_cleanup()`
   handles teardown (including signal-safe exit via `atexit`).

5. **Workloads** (all `__attribute__((noinline))` so the compiler keeps the
   loop intact):
   - `bench_stride` — `sum += arr[i]` with configurable stride (uint64
     units; default 8 = 64 B = one cache line).
   - `bench_stride_flush` — same but with `flush_cacheline` after each
     access (forces L3 miss). Enabled by `-f`.
   - `bench_random` — pre-built Fisher-Yates shuffled index; measures
     random-access bandwidth (parallel outstanding loads, no serial dep).
   - `bench_chase` — pointer-chasing a Fisher-Yates-shuffled singly-linked
     list of 64-byte `pnode`s. The serial dep chain `p = p->next` is the
     design — this is pure DRAM latency, not bandwidth. This is the
     "purest" memory-bound test.
   - `bench_compute` — `x = x * A + B` chain, zero memory access.
     **Sanity check**: `compute_%` should track the freq ratio; if it
     doesn't, the frequency lock didn't take (turbo leaked through, or
     `set_freq` silently failed).

6. **Single-core `main`**: CLI parsing → L3 auto-detect (`-A` sets array to
   2× L3) → idle gate → pin → enumerate freqs → outer loop over freq → for
   each, `nsamples` runs of each workload, take median → emit TSV + 5
   optional `# --- ... ---` stats blocks.

7. **Multi-core `run_multicore`**: fork-per-CPU model inspired by stress-ng.
   Parent sets frequency (freq domain leader), children
   `sched_setaffinity` + bench, write throughput to a `MAP_SHARED` anonymous
   region, parent aggregates and takes the per-freq median across children.
   Used for bandwidth-saturation measurement (MC bandwidth-bound sweet spot)
   as distinct from single-core latency-bound sweet spot.

8. **System-idle gate** (`check_system_idle`, from sbc-bench): refuses to
   run if loadavg / online-CPU churn / etc. looks noisy. `-F` to force.

9. **Power sensing** (`detect_power_sensors` / `read_power`): RAPL or hwmon,
   but only sampled — not part of the sweet-spot calculation, just logged
   alongside inside the plateau block.

10. **Statistical helpers** (`stats.c` + `stats.h`): the sweet-spot /
    plateau / bootstrap math. `MAX_FREQS` and `MAX_SAMPLES` from `stats.h`
    are also used by `memfreq_bench.c` for the per-workload `mops[]` arrays
    and the bootstrap slice buffer.

## CLI flags most worth knowing

| Flag | Why it matters |
|------|----------------|
| `-A` | Auto-size array to 2× L3. Default 128 MB is too small for ≥128 MB L3 systems (Apple M1 Ultra, high-core-count ARM). |
| `-m N` | Manual array size in MB. **Must exceed L3**, else the test is cache-bound, not DRAM-bound. |
| `-s N` | Stride in uint64 units. 8 = one cache line. Larger = more mem-bound. |
| `-t N -n N` | Per-freq duration (sec) and sample count. Median is taken; `t=3 n=5` is the typical low-noise setting. |
| `-S N` | kHz step in CPPC range mode. Default 25000 (25 MHz). |
| `-N N` | Multi-core mode: pick N CPUs, distribute across NUMA. Measures bandwidth saturation, not latency. |
| `-B N` | Bind array to NUMA node N. Use with `numactl` to create local/remote latency splits. |
| `-R -f` | Add random-permutation and clflush workloads. |
| `-F` | Skip the idle gate. |
| `-T FRAC` | Sweet-spot threshold (default 0.95). Must be in (0, 1]. |
| `-L LIST` | Multi-threshold sweep, comma-separated (e.g. `0.8,0.9,0.95,0.99`). Emits the per-workload `# --- sensitivity ---` block. |
| `-r` | Emit per-sample raw throughput in `# --- raw_samples (workload) ---` blocks. Also enables the bootstrap `# --- sweet-spot CI ---` block (which requires the raw data). |
| `-P` | Suppress the entire `# --- plateau ---` block (including the `power:` sub-row). |

## Output format

TSV to stdout, `#`-prefixed metadata/header lines, then rows. The Python parser keys off:

- `# cpu=… array=… stride=… duration=… samples=…` — meta line
- `# stride  sweet spot: <MHz>` / `# chase   sweet spot: <MHz>` / `# compute sweet spot: —` — headline values

Data rows: `target_MHz<TAB>actual_MHz<TAB>stride_Mops<TAB>stride_MBs<TAB>stride_%<TAB>chase_Mops<TAB>chase_%<TAB>compute_Mops<TAB>compute_%` (chase columns absent with `-C`)

After the data rows, up to 5 optional `# --- ... ---` blocks (all default-on unless suppressed by their flag):

| Block | Default? | Flag | Contents |
|-------|----------|------|----------|
| `# --- per-freq stats (workload) ---` | on | — | per-freq min / max / median / IQR; one block per workload |
| `# --- sweet-spot CI ---` | off | `-r` | bootstrap 95% CI on the sweet-spot freq; method label `bootstrap_1000` |
| `# --- sensitivity (workload) ---` | off | `-L LIST` | sweet spot at each user-supplied threshold |
| `# --- plateau ---` | on | `-P` to suppress | piecewise-linear breakpoint + slope ratio + 95% sweet spot, with a `power:` sub-row per workload when a power sensor is available |
| `# --- raw_samples (workload) ---` | off | `-r` | per-freq × per-sample raw throughput |

Sweet-spot threshold is the literal constant `THRESHOLD` (= 0.95) in the C source; override at runtime with `-T`.

## Important constraints when modifying

- **Root required.** Every cpufreq sysfs write needs it. Test scripts assume sudo.
- **Linux + cpufreq driver.** macOS dev environment (this machine) can build the binary but cannot run the workloads — actual testing requires a Linux box with `/sys/devices/system/cpu/cpu*/cpufreq/`.
- **Frequency write order is load-bearing.** Write `scaling_max_freq` before `scaling_min_freq`, or kernel rejects. See `set_freq`.
- **`-m` must exceed L3** or the benchmark measures cache, not DRAM. Use `-A`.
- **chase is the latency-bound test, stride is moderate, compute is the control.** Don't conflate their sweet spots — they answer different questions.
- **`compute_%` is a sanity check on freq locking**, not a result. If it doesn't track the freq ratio, the measurement is invalid.
- The main C file is `memfreq_bench.c` (~90 KB, ~2930 lines); statistical
  helpers live in `stats.c` (~213 lines) + `stats.h` (~84 lines). All
  files are cleanly sectioned by `/* --- */` banners — find-by-section
  is faster than find-by-symbol, and line numbers drift with edits.

## Where to look when…

- **"Why does output look weird"** → `docs/memfreq-bench.md` "噪音分析" / "解读输出" sections.
- **"How do I add a new workload"** → model after `bench_compute` (`__attribute__((noinline))`, signature `double bench_X(double secs)`, return Mops, called from `main`'s per-freq inner loop and `run_multicore`'s per-child loop).
- **"How do I change the sweet-spot definition"** → the `THRESHOLD` constant + the per-metric sweet-spot scan near the print loop in `main`.
- **"How do I support a new arch for cache flush"** → extend the `#if defined(__x86_64__)` block at the top of the C file.
