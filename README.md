# memfreq_bench

Find the CPU frequency sweet spot for memory-bound workloads — the lowest frequency that preserves ≥95% of maximum DRAM throughput.

## Why

When a workload is memory-bound, CPU frequency reduction barely affects performance but significantly reduces power consumption (P ∝ V²·f). This tool measures exactly where that boundary lies.

## Quick Start

```bash
gcc -O2 -o memfreq_bench memfreq_bench.c -lm
sudo ./memfreq_bench -m 512          # array must exceed L3 cache
sudo python3 memfreq_sweep.py        # run + visualize
```

## Three Workloads

| Test | Access Pattern | Bottleneck |
|------|---------------|------------|
| **stride** | Sequential array traversal (configurable stride) | DRAM bandwidth |
| **chase** | Random linked-list pointer chasing (64B nodes) | DRAM latency |
| **compute** | FP multiply-add chain (zero memory access) | CPU frequency (control) |

## Output

```
freq_MHz  stride_Mops  stride_%  chase_Mops  chase_%  compute_Mops  compute_%
800       152.3        97.2      12.5        98.1     82.1          25.3
1200      154.1        98.4      12.6        98.8     123.4         37.9
...
3000      156.6        100.0     12.8        100.0    325.8         100.0

# stride  sweet spot: 800 MHz  (27% of max 3000 MHz)
# chase   sweet spot: 800 MHz  (27% of max 3000 MHz)
```

## Documentation

See [docs/memfreq-bench.md](docs/memfreq-bench.md) for full design rationale, noise analysis, and high-core-count tuning guide.

## Requirements

- Linux with cpufreq driver configured
- Root access (for sysfs frequency writes)
- GCC, Python 3

## License

GPL-2.0
