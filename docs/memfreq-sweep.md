# memfreq_sweep.py — 可视化工具使用说明

## 概述

`memfreq_sweep.py` 是 `memfreq_bench` 的配套可视化工具。四种模式：

| 模式 | 命令 | 用途 |
|------|------|------|
| 直接运行 | `python3 memfreq_sweep.py` | 自动调用 C 二进制并画图 |
| 解析已保存的结果 | `--file results.txt` | 重新查看历史测试 |
| 跨运行对比 | `--compare f1.json f2.json` | 比较多次运行的甜点稳定性 |
| 目录报告 | `--report output/dir/` | 批量扫描整个测试目录，生成 Markdown 报告 |

---

## 模式 1：直接运行

```bash
sudo python3 memfreq_sweep.py                       # 默认参数
sudo python3 memfreq_sweep.py -c 2 -s 16             # 传递参数给 C 二进制
sudo python3 memfreq_sweep.py --json                  # 同时输出 JSON
```

不接任何参数时，脚本自动查找同目录下的 `memfreq_bench` 二进制，运行后解析 stdout，输出 ASCII 柱状图。

额外参数（`-c 2 -s 16`）会被透传给 `memfreq_bench`。优先使用 `sudo`。

**输出示例：**

```
$ sudo python3 memfreq_sweep.py -c 0 -A
$ sudo ./memfreq_bench -c 0 -m 256 -s 8 -t 2 -n 3

============================================================
  Memory-Bound Frequency Sweet-Spot Analysis
============================================================
  CPU=0  array=256MB  stride=8  duration=2s

── Stride (sequential memory access) ────────────────────
   MHz     Mops      %  throughput
   800    152.3   97.2  ████████████████████████████████████░░
  1200    154.1   98.4  ████████████████████████████████████░░
  ...
  3000    156.6  100.0  ██████████████████████████████████████

── Sweet spot summary ─────────────────────────────────
  Stride  :  800 MHz  (27% of 3000 MHz)
  Chase   :  800 MHz  (27% of 3000 MHz)
  Compute :  always scales linearly — no sweet spot

  → STRIDE is heavily memory-bound.
  → CHASE is purely latency-bound (DRAM round-trip).
```

三张柱状图（Stride / Chase / Compute）+ 甜点摘要 + 分析建议。统计块（per-freq stats / CI / sensitivity / plateau / raw_samples）只包含 stride / chase / random，不含 compute（compute 仅用于频率锁定 sanity check）。如果用了 `-r`，还会输出 bootstrap CI 和 plateau 检测。

---

## 模式 2：解析已保存的结果

```bash
python3 memfreq_sweep.py --file results/s8.txt
python3 memfreq_sweep.py --file results/s8.txt --json
```

`memfreq_bench` 输出的 `#` 前缀 TSV 文件可以直接用于解析。无需 root，可离线使用。

`--json` 会额外输出 `memfreq_results.json`，包含所有统计块的完整结构化数据，适合给其他工具调用。

```bash
# JSON 结构概要
{
  "meta": {"cpu": "0", "array": "256MB", ...},
  "sweet_spot_mhz": {"stride": 800, "chase": 800},
  "per_freq_stats": {"stride": [...], "chase": [...], "random": [...]},
  "sensitivity": {"stride": [...], "chase": [...]},
  "plateau": [...],
  "sweet_spot_ci": [...],
  "data": [...]
}
```

---

## 模式 3：跨运行对比

```bash
python3 memfreq_sweep.py --compare run1.json run2.json run3.json
# 或混合 TSV 和 JSON：
python3 memfreq_sweep.py --compare baseline.txt experiment.txt
```

自动识别 JSON 和 TSV 格式，输出跨运行统计：

```
==============================================================================
  Cross-run sweet-spot comparison (3 runs)
==============================================================================
workload     mean_MHz    std_MHz    min_MHz    max_MHz  range_MHz
stride           1975       35.4       1950       2000         50
chase            1800       28.9       1750       1850        100
random             —          —          —          —          —
```

标准差和范围用于评估甜点稳定性。单次运行（1 个文件）输出 `—` 作为标准差。

---

## 模式 4：目录报告（新增）

```bash
python3 memfreq_sweep.py --report output/results_20240101_143052/
```

扫描目录下所有 `.txt` 文件，按文件名模式分组（Stride Grid / 多核 / NUMA / 特殊模式），生成结构化 Markdown 报告：

```
output/results_20240101_143052/
├── s8.txt                  ← 单测文件
├── s16.txt
├── mc4.txt
├── mc48.txt
├── random.txt
├── REPORT.md               ← 自动生成的报告（--report 产物）
└── ...                     ← 其他测试结果
```

**报告内容：**
- **测试总览** — 每个 x.txt 的 Stride/Chase 甜点列表
- **分组表格** — 按测试类型分组（stride / 多核 / NUMA / 特殊 / 其他）
  - 每组显示：测试名、最大带宽、甜点频率、平台期起点

```bash
# 完整工作流：Shell 跑完之后 Python 出报告
sudo ./run_all_tests.sh
python3 memfreq_sweep.py --report output/results_20240101_143052/
cat output/results_20240101_143052/REPORT.md
```

---

## 工作原理

### 数据流

```
memfreq_bench (C)       memfreq_sweep.py (Python)
       │                       │
       │ TSV → stdout           │
       │  或 → results/*.txt    │
       ▼                       ▼
  ┌────────────────┐    ┌────────────────┐
  │ # cpu=0 ...    │    │ parse_output() │
  │ 800  152.3  97 │ →  │  → dict        │
  │ 1200 154.1  98 │    │                │
  │ # stride sweet │    │ visualize()    │
  │ # plateau...   │    │ compare_runs() │
  └────────────────┘    │ generate_report│
                         └────────────────┘
                            │
                     ┌──────┴──────┐
                     ▼             ▼
                ASCII 柱状图    REPORT.md
                (stdout)        (Markdown)
```

`memfreq_bench` 输出两种内容：
1. **TSV 数据行**（无 `#`）：频率点 × 吞吐量
2. **`#` 注释行**：元信息、甜点、统计块（per-freq stats / sensitivity / plateau / CI / raw samples）

Python 用正则提取 `# --- ... ---` 块边界，对每种块类型用独立解析器。

---

## 常见工作流

### 快速查看单次测试

```bash
make
sudo python3 memfreq_sweep.py -A -r -L 0.80,0.90,0.95,0.99 --json
```

### 全量扫描 + 报告中

```bash
sudo ./run_all_tests.sh          # 产出 output/results_*/
python3 memfreq_sweep.py --report output/results_*/
```

### 比较不同硬件的甜点

```bash
# 机器 A
sudo python3 memfreq_sweep.py --json
mv memfreq_results.json machine_a.json

# 机器 B
sudo python3 memfreq_sweep.py --json
mv memfreq_results.json machine_b.json

# 本地对比
python3 memfreq_sweep.py --compare machine_a.json machine_b.json
```

---

## 常见问题

**Q: 为什么 `--report` 输出的是 Markdown 而不是图片？**
没有引入 matplotlib 等依赖。Markdown 文件可以方便地在 GitHub、GitLab、VS Code 等环境中预览，也方便嵌入其他文档。

**Q: `--file` 和 `--compare` 支持混合格式吗？**
支持。`--compare` 自动检测每个文件的开头是 `{`（JSON）还是非 `{`（TSV），分别用 `json.load` 和 `parse_output` 解析。

**Q: 柱状图的百分比值代表什么？**
`stride_%` 和 `chase_%` 是相对于最高频率点吞吐量的百分比。100% = 该工作负载在最高频率下的吞吐量。百分比越高说明频率降低对性能影响越小——这正是判断 memory-bound 的依据。

**Q: 需要 sudo 吗？**
直接运行时需要（cpufreq sysfs 写需要 root）。`--file`、`--compare`、`--report` 解析已存在的文件时不需要。

---

## 参考

- `memfreq_bench -h` — 完整的 C 二进制参数列表
- `docs/memfreq-bench.md` — 工具设计原理和微架构分析
- `AGENTS.md` — 仓库结构和开发指南
