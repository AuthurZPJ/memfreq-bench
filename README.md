# memfreq_bench

Find the CPU frequency sweet spot for memory-bound workloads — the lowest frequency that preserves ≥95% of maximum DRAM throughput.

## Why

When a workload is memory-bound, CPU frequency reduction barely affects performance but significantly reduces power consumption (P ∝ V²·f). This tool measures exactly where that boundary lies.

## Quick Start

```bash
make                                   # builds memfreq_bench + test_stats
sudo ./memfreq_bench -A                # auto-size array to 2× L3 (recommended)
sudo python3 memfreq_sweep.py          # run + visualize
```

The `make` build links `stats.c` automatically — if you prefer gcc directly,
the full command is `gcc -O2 -o memfreq_bench memfreq_bench.c stats.c -lm`.

## Three Workloads

| Test | Access Pattern | Bottleneck |
|------|---------------|------------|
| **stride** | Sequential array traversal (configurable stride) | DRAM bandwidth |
| **chase** | Random linked-list pointer chasing (64B nodes) | DRAM latency |
| **compute** | FP multiply-add chain (zero memory access) | CPU frequency (control) |

## Optional Flags Worth Knowing

```bash
sudo ./memfreq_bench -A -r -L 0.80,0.90,0.95,0.99
#   -A                          auto-size array to 2× L3 (use this!)
#   -N 4                        multi-core bandwidth-saturation test
#   -r                          keep per-sample data → enables bootstrap CI
#   -L 0.80,0.90,0.95,0.99      sweet spot at 4 thresholds
```

`-r` unlocks the new `# --- sweet-spot CI ---` and `# --- sensitivity ---`
blocks; `-L` adds multi-threshold scanning. Run `memfreq_bench -h` for the
full flag list.

## Output

```
target_MHz  actual_MHz  stride_Mops  stride_MBs  stride_%  chase_Mops  chase_%  compute_Mops  compute_%
800         800         152.3        1218.4      97.2      12.5        98.1     82.1          25.3
1200        1200        154.1        1232.8      98.4      12.6        98.8     123.4         37.9
...
3000        3000        156.6        1252.8      100.0     12.8        100.0    325.8         100.0

# stride  sweet spot: 800 MHz  (27% of max 3000 MHz)
# chase   sweet spot: 800 MHz  (27% of max 3000 MHz)
#
# --- sweet-spot CI ---                  ← only with -r
# workload  sweet_MHz  low_MHz  high_MHz  method
# stride    800        800      800       bootstrap_1000
# chase     800        800      800       bootstrap_1000
#
# --- sensitivity (stride) ---           ← only with -L
# threshold  sweet_spot_MHz
# 0.80       800
# 0.90       800
# 0.95       800
```

## Common Pitfalls

These are the things that bit me during development — read this before opening
an issue.

1. **"-m 128 is too small" on big-L3 systems.** Default 128 MB is fine for
   a typical laptop (8-32 MB L3) but tiny vs Apple M1 Ultra / Epyc / Xeon.
   If your L3 is ≥ 64 MB, **always use `-A`** (auto-size to 2× L3). The
   test is meaningless if the array fits in cache.

2. **Running on macOS / Windows.** The tool needs `/sys/devices/system/cpu/cpu*/cpufreq/`,
   which is Linux-only. On macOS you'll get a clear error message:
   "ERROR: this tool is Linux-only (cpufreq sysfs required)."
   The fix is a Linux VM, a remote Linux box, or Docker with `--privileged` + `/sys` mounted.

3. **"compute_% doesn't track the frequency ratio"** — the smoking gun
   that the frequency lock didn't take. If you ran with turbo enabled or
   a governor that ignored your sysfs write, the actual frequency leaked
   through and your sweet-spot numbers are noise. Re-run with
   `sudo ./memfreq_bench -F` (skip idle gate) and verify the compute_%
   column tracks e.g. f/3000 at each row. The tool will also print a
   "WARN: compute throughput does NOT scale linearly with frequency"
   at the end if it detects this automatically.

4. **"-r missing → no CI block in output"** — the `# --- sweet-spot CI ---`
   block only appears if you pass `-r` (it needs per-sample data to
   bootstrap). Same for raw_samples. If you ran without `-r` and wonder
   where the CI is, that's why.

5. **"My sweet spot is 3000 MHz (the max)"** — usually means either (a) the
   workload isn't actually memory-bound (try a larger stride with `-s 64`),
   or (b) your system has only a few discrete frequencies, so the "95% of
   peak" is at the highest one by definition. Check `# cpu=... stride=...`
   in the header and look at the data rows.

6. **The two wrapper scripts:**
   - `run_all_tests.sh` — 6 predefined suites, **~10-30 min**. Use for a
     quick health check.
   - `run_full_sweep.sh` — 52 tests across stride × core-count × NUMA,
     **~3-4 h**. Use for deep analysis. See `docs/run_full_sweep.md`.

## One-Click Wrappers

```bash
sudo ./run_all_tests.sh                  # 6 suites, 10-30 min
sudo ./run_all_tests.sh --suite 1,4      # just suites 1 and 4
sudo ./run_full_sweep.sh                 # 52 tests, 3-4 hours
sudo ./run_full_sweep.sh --quick         # 15 tests, ~30 min
sudo python3 memfreq_sweep.py            # run + ASCII bar charts
sudo python3 memfreq_sweep.py --file results.txt   # re-parse saved output
sudo python3 memfreq_sweep.py --json     # also write memfreq_results.json
```

## Documentation

- `docs/memfreq-bench.md` — full design rationale, microarchitecture, noise
  model, high-core-count tuning. The single best reference for "why".
- `docs/run_full_sweep.md` — complete guide to the 52-test deep sweep and
  how to read the FULL_REPORT.txt it generates.

## Requirements

- Linux with cpufreq driver configured
- Root access (for sysfs frequency writes)
- GCC, Python 3

## License

GPL-2.0
