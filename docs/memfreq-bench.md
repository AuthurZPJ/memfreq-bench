# memfreq_bench: 找到 Memory-Bound 工作负载的频率甜点

> Memory-bound 期间 CPU 不需要那么高频率，但过多降频又影响性能。
> 这个工具帮你在两者之间找到最优平衡点。

---

## 目录

- [核心原理](#核心原理)
- [五种工作负载](#五种工作负载三种默认两种可选)
- [完整测试流程](#完整测试流程以-96-核-arm-服务器为例)
- [使用方法](#使用方法)
  - [环境准备](#环境准备)
  - [运行](#运行)
  - [参数](#参数)
  - [可视化](#可视化)
- [解读输出](#解读输出)
  - [数据行](#数据行)
  - [甜点判定](#甜点判定)
  - [统计块](#统计块)
  - [JSON 输出](#json-输出)
  - [关键指标速查](#关键指标速查)
- [多核并行模式](#多核并行模式-n)
- [噪音分析](#噪音分析)
- [设计原理](#设计原理)
- [高核数服务器补充说明](#高核数服务器补充说明)
- [限制和注意事项](#限制和注意事项)
- [与其他工具对比](#与其他工具对比)
- [文件清单](#文件清单)

---

## 核心原理

CPU 频率决定了计算单元每秒能推进多少个"状态步"。但 CPU 的吞吐不一定随频率线性增长——**取决于瓶颈在哪里**：

```
                     吞吐
                      ↑
                      │         ╱   ← compute-bound：线性
                      │        ╱        （频率翻倍，吞吐翻倍）
                      │       ╱
                      │     ╱
        DRAM 上限  →  │     ╱──────────────────  ← memory-bound：平台期
                      │   ╱                        （DRAM 限制，再升频也无用）
                      │ ╱   ← 低频段：CPU 跟不上，
                      │╱      连 DRAM 都没榨满
                      └─────┴────────────────→ 频率
                            ↑
                         甜点区 (= 平台起点)
                  ─ 能保 95% DRAM 吞吐的最低频率
                  ─ 降到此处仍不丢性能，只省电
                  ─ 高于此频率只多耗电，吞吐不增
```

**为什么 memory-bound 会出现平台期？**

```
一条指令的执行时间 = 计算时间 + 等待时间（DRAM 访问）

compute-bound:
  计算时间 ≫ 等待时间
  → 降频 → 计算变慢 → 吞吐线性下降

memory-bound:
  等待时间 ≫ 计算时间
  → 降频 → 计算变慢 → 但等待时间不变（DRAM 速度跟 CPU 频率无关）
  → 吞吐几乎不变
```

这就是 DVFS 的节能空间：**如果工作负载是 memory-bound，把频率降到最低频附近，性能损失 <5%，但功耗可能降低 50%+**（因为功耗 ∝ V² × f，而 V 通常也随 f 降低）。

---

## 五种工作负载（三种默认，两种可选）

工具运行五种基准测试，覆盖不同的"内存依赖度"。默认启用 stride、chase、compute 三种；random（`-R`）和 flush（`-f`）需显式开启：

### 1. Stride（顺序遍历）

```c
uint64_t sum = 0;
for (size_t i = 0; i < count; i += stride)
    sum += arr[i];
```

| 参数 | 行为 |
|------|------|
| `stride=1` | 连续访问 u64，HW prefetcher 能预测 → 带宽测试（中等 mem-bound） |
| `stride=8` | 每 8 个 u64 = 64B = 一个 cache line → 每次访问一个新 cache line（默认，重度 mem-bound） |
| `stride=64` | 每 512B → 远超 prefetcher 预测范围 → 极端 mem-bound |

**微架构分析：**
- `sum += arr[i]` 有数据依赖（sum 依赖自身），限制了流水线深度
- 但编译器/CPU 可以对多个 sum 做 partial sum（如果循环展开），所以不是纯串行
- 主要瓶颈：**L3 miss → DRAM 带宽**（每次 cache line 加载是独立的，CPU 可以并行发射多个 outstanding request）

### 2. Chase（指针追踪）

```c
for (size_t i = 0; i < nnodes; i++)
    p = p->next;    // 必须等上一次 load 完成才能知道下一次的地址
```

**微架构分析：**
- 每个节点恰好 64 字节 = 一个 cache line
- 链表顺序是 Fisher-Yates 随机打乱的 → **HW prefetcher 完全无法预测**
- `p = p->next` 是严格的**串行依赖**：必须先 load `p` 所在的 cache line，才能知道下一个 `p->next` 的地址
- **每次 dereference = 一次完整的 L3 miss → DRAM round-trip**
- 测量的是 **DRAM 延迟**（不是带宽）

```
时序（DRAM 延迟约 100ns）：

  t=0ns     load p[0]->next → miss → 等 DRAM
  t=100ns   load p[X]->next → miss → 等 DRAM    (X 是随机值)
  t=200ns   load p[Y]->next → miss → 等 DRAM
  ...

  100 次 dereference = 100 × 100ns = 10 µs
  CPU 频率从 3GHz 降到 1GHz → 每次 dereference 仍等 ~100ns
  → 吞吐几乎不变
```

这是**最纯粹的 memory-bound** 测试。

### 3. Random（随机置换遍历）

```c
size_t *idx = build_random_index(count);  // Fisher-Yates shuffle
for (size_t i = 0; i < count; i++)
    sum += arr[idx[i]];
```

**微架构分析：**
- 访问模式完全随机 → HW prefetcher 无法预测
- 但与 chase 不同：**没有串行依赖**（每次访问的索引是预先确定的）
- CPU 可以并行发射多个 outstanding load → 测试**随机访问带宽**而非延迟
- 用 `-R` 启用

### 4. Compute（纯计算，对照组）

```c
double x = 1.00001;
for (size_t i = 0; i < 1000000; i++)
    x = x * 1.0000001 + 0.0000001;
```

**微架构分析：**
- `x = x * A + B` 有严格的数据依赖 → 无法 SIMD 化
- 零内存访问 → 不涉及 cache / DRAM
- 吞吐**完全由 CPU 频率决定** → 频率减半，吞吐减半

**用途：sanity check**。如果 compute 在不同频率下的吞吐比不等于频率比，说明频率没有成功锁定（turbo 还在跑、或者 governor 覆盖了你的设置）。详见 [compute_% 作为 sanity check](#compute_-作为-sanity-check)。

### 5. Stride + Flush（强制 L3 miss）

```c
for (size_t i = 0; i < count; i += stride) {
    sum += arr[i];
    flush_cacheline(&arr[i]);  // x86: clflush, ARM: dc cvac
}
```

**微架构分析：**
- 每次访问后立即驱逐 cache line → 下次必须从 DRAM 重新加载
- 消除 L3 hit 的可能性 → 测试**纯 DRAM 带宽**
- 用 `-f` 启用

---

## 完整测试流程（以 96 核 ARM 服务器为例）

```bash
# ── 1. 拓扑侦察 ──
lscpu | grep -E "L[123]|Thread|Core|Socket|NUMA"
numactl -H
cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq

# ── 2. 基本测试（L3 自动检测，数组自动 2× L3）──
sudo ./memfreq_bench -c 0 -A -t 3 -n 5

# ── 3. 多核带宽饱和测试（每 NUMA node 各 1 核）──
sudo ./memfreq_bench -N 2 -A -t 3 -n 5

# ── 4. 半核带宽饱和测试（48 物理核）──
sudo ./memfreq_bench -N 48 -A -t 3 -n 5

# ── 5. NUMA 绑定测试（强制内存到 Node 0）──
sudo ./memfreq_bench -c 0 -A -B 0 -t 3 -n 5

# ── 6. 可选：random permutation 测试 ──
sudo ./memfreq_bench -c 0 -A -R -t 3 -n 5

# ── 7. 可选：cache flush 测试（强制 L3 miss）──
sudo ./memfreq_bench -c 0 -A -f -t 3 -n 5

# ── 8. 可选：不同 stride 对比 ──
sudo ./memfreq_bench -c 0 -A -s 1     # prefetcher-friendly
sudo ./memfreq_bench -c 0 -A -s 64    # 极端 mem-bound

# ── 9. 可选：L3-resident 测试（数据驻留 L3）──
sudo ./memfreq_bench -c 0 -A -2 -t 3 -n 5
```

---

## 使用方法

### 环境准备

#### 1. 确认硬件拓扑

```bash
# L3 大小 — 决定数组该多大
lscpu | grep "L3 cache"
# 或详细拓扑
lscpu | grep -E "L[123]|Thread|Core|Socket|NUMA"
```

**关键：数组大小必须 > L3 cache，否则测试的是 L3 而非 DRAM。** 用 `-A` 可自动检测并设为 2× L3。

| L3 大小 | `-m` 参数 | 说明 |
|---------|-----------|------|
| ≤ 32 MB | 128（默认） | 标准服务器 |
| 32-128 MB | 256-512 | 高端服务器 |
| 128-300 MB | 512-1024 | 高核数 ARM（如 96 核 / 273 MB L3） |

**验证方法**：如果低频的 stride_% 异常高（接近 100%），可能数组没超出 L3——加大 `-m` 重测。

#### 2. 处理 SMT

SMT 兄弟线程共享同一物理核的 L1/L2 cache 和执行单元。cpufreq 通常按物理核调频，SMT 兄弟的负载不影响频率设置，所以**SMT 隔离不是必须的**。但如果追求低噪音测量：

```bash
# 查看 SMT 映射
cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list

# 隔离 SMT 兄弟（关闭每个物理核的第二个 SMT 线程）
echo off > /sys/devices/system/cpu/smt/control

# 测试完成后恢复
echo on > /sys/devices/system/cpu/smt/control
```

SMT 对测量精度的影响详见 [噪音分析](#噪音分析) 中的 SMT 噪音段。

#### 3. 了解 NUMA 拓扑

高核数服务器通常有多个 NUMA node，本地和远端 DRAM 延迟差异显著：

```bash
numactl -H
```

```
本地 NUMA node DRAM：~80-120 ns
远端 NUMA node DRAM：~150-250 ns（经过 interconnect）

→ 对 DVFS 甜点分析：本地 NUMA 测试就够了
→ 远端 DRAM 延迟更高 → 甜点只会更低（更多时间等 interconnect）
```

跨 NUMA 测试方法见下方 [运行](#运行) 中的 numactl 示例。

### 运行

```bash
# 基本测试（确认数组 > L3 后）
sudo ./memfreq_bench -m 512

# 指定 CPU（选物理核，非 SMT 线程）
sudo ./memfreq_bench -c 0 -m 512

# 自动检测 L3 大小，数组设为 2× L3
sudo ./memfreq_bench -c 0 -A

# 更大的 stride（更极端 memory-bound）
sudo ./memfreq_bench -c 0 -m 512 -s 16

# 更长的测试时间（减少系统噪音）
sudo ./memfreq_bench -c 0 -m 512 -t 5 -n 5

# 跨 NUMA 对比（把内存分配到远端 node）
sudo numactl --membind=1 ./memfreq_bench -c 0 -m 512

# L3-resident 测试（额外测量数据驻留 L3 时的甜点）
sudo ./memfreq_bench -c 0 -A -2 -t 3 -n 5

# 跳过 chase 测试
sudo ./memfreq_bench -C

# 配合功耗测量
# Intel:
sudo turbostat --Summary --show PkgWatt,CorWatt,MHz,Busy% -i 1 &
./memfreq_bench -m 512

# ARM:
watch -n 1 'cat /sys/class/hwmon/hwmon*/power1_input'
```

将功耗数据与频率数据对齐，就能画出 **性能-功耗 Pareto 曲线**，找到真正的能效最优点。

### 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-c CPU` | 0 | 绑定到指定 CPU 核心（逻辑 CPU 编号） |
| `-N NCPU` | — | 多核并行模式，自动分布到 NUMA 节点（见 [多核并行模式](#多核并行模式-n)） |
| `-m SIZE_MB` | 128 | 数组大小（MB），**必须 > L3 cache** |
| `-A` | — | 自动检测 L3 大小，将数组设为 2× L3 |
| `-s STRIDE` | 8 | 步长（uint64 单位），8 = 64B = 一个 cache line |
| `-t SECS` | 2 | 每个频率点的测试时间 |
| `-n N` | 3 | 每个频率点采样次数，取中位数 |
| `-S STEP_KHZ` | 25000 | CPPC 范围模式下的频率步长（25 MHz） |
| `-C` | — | 跳过 pointer chase 测试 |
| `-R` | — | 启用 random permutation 测试 |
| `-f` | — | 启用 cache flush（clflush/dc cvac） |
| `-2` | — | 额外运行 L3-resident sweep（数组大小 = 2× L2，数据完全驻留在 L3 中） |
| `-B NODE` | -1 | 将数组绑定到指定 NUMA 节点 |
| `-F` | — | 强制运行（跳过系统空闲检查） |
| `-T FRAC` | 0.95 | sweet-spot 阈值,(0, 1] 范围内 |
| `-L LIST` | — | 多阈值扫描,逗号分隔,例如 `0.8,0.9,0.95,0.99`,最多 16 个 |
| `-r` | — | 输出每样本原始数据(用于自定义分析) |
| `-P` | — | 抑制 plateau 检测输出(默认开启) |

### 可视化

```bash
# 自动运行 + ASCII 图表
sudo python3 memfreq_sweep.py

# 传递参数给 C 程序
sudo python3 memfreq_sweep.py -c 0 -m 512

# 保存 JSON 用于后续分析
sudo python3 memfreq_sweep.py --json

# 解析已有输出
python3 memfreq_sweep.py --file results.txt

# 跨 run 对比（多次 sweep 的甜点稳定性）
python3 memfreq_sweep.py --compare run1.json run2.json run3.json
```

`--compare` 自动检测每个文件是 JSON 还是 TSV,输出每个 workload 的 mean / std / min / max / range across runs。JSON 来自 `memfreq_sweep.py --json`(单次 sweep 后自动产生)。

---

## 解读输出

### 典型输出

```
# memfreq_bench results
# cpu=0 ncpus=192 array=512MB stride=8 duration=3s samples=5
# NUMA: 2 nodes allowed   L3 cache: 273 MB   Temp: 42.3°C
#
# target_MHz  actual_MHz  stride_Mops  stride_MBs  stride_%  chase_Mops  chase_%  compute_Mops  compute_%
2600          2598        156.6        1193.2      100.0     12.8        100.0    325.8         100.0
2575          2573        156.5        1192.4       99.9     12.8        100.0    322.1          98.9
2550          2548        156.3        1190.9       99.8     12.8        100.0    319.0          97.9
...
2000          1998        152.3        1160.2       97.2     12.5         98.1    250.3          76.8
#
# === Sweet spot (lowest freq ≥ 95% of max throughput) ===
# stride  sweet spot: 2000 MHz  (77% of max 2600 MHz)
# chase   sweet spot: 2000 MHz  (77% of max 2600 MHz)
# compute sweet spot: — (scales linearly, always needs max freq)
# stride_l3 sweet spot: 1800 MHz  (69% of max 2600 MHz)
```

注意输出包含：
- **NUMA 节点数、L3 大小、温度**：自动检测并在 header 中显示
- **stride_l3 sweet spot**：L3-resident sweep 的甜点（仅当使用 `-2` 时输出，数组大小 = 2× L2）

### 数据行

| 列 | 类型 | 计算 |
|----|------|------|
| `target_MHz` | int | 写到 `scaling_min/max_freq` 的目标频率 / 1000 |
| `actual_MHz` | int | 频率切换 + 100ms 稳定后**单次**读 `cpuinfo_cur_freq`（瞬时值），失败回退 `cpuinfo_avg_freq` |
| `stride_Mops` | %.1f | stride workload 的 ops/sec 取 `nsamples` 次中位数 / 1e6 |
| `stride_MBs` | %.1f | `stride_tput × 8 / 2^20`（`OPS_TO_MBS` 宏）—— 8 = `uint64_t` 元素字节数 |
| `stride_%` | %.1f | `stride_tput / s_max × 100` |
| `chase_Mops` | %.1f | chase workload 的 ops/sec 中位数 / 1e6 |
| `chase_%` | %.1f | `chase_tput / c_max × 100` |
| `compute_Mops` | %.1f | compute workload 的 ops/sec 中位数 / 1e6 |
| `compute_%` | %.1f | `compute_tput / p_max × 100` |

**分母 `s_max` / `c_max` / `p_max` 的定义**：取**最高有效频率点**（`ref = highest valid freq`）的对应 workload 吞吐，**不是扫到的全局最大值**。因此最高频点行的 `*_%` 定义上 = 100.0。

**隐含的工程细节**：

- **TAB 分隔**：实际是 TSV（本节用空格展示是为了可读性）
- **`actual_MHz ≠ target_MHz` 是诊断信号**：差超过 ±2% 警惕 CPPC 拒步 / turbo 漏过
- **中位数 vs 平均值**：每个 freq 跑 `nsamples` 次取**中位数**，对单次中断异常不敏感

#### 功耗列（仅当检测到功耗传感器时追加）

两种模式物理意义不同：

| 列 | 类型 | 计算 |
|----|------|------|
| `idle_W` | %.2f | 频率切换 + 1.1s 稳定后、测试**前** `read_power()` 采样。仅 HWMON |
| `load_W` | %.2f | 所有 workload 跑**完**后 `read_power()` 采样。仅 HWMON |
| `delta_W` | %.2f | `load_W - idle_W`（SoC 增量功耗）。仅 HWMON |
| `energy_J` | %.3f | RAPL: 累积 µJ 差 / 1e6；HWMON: `(load - idle) µW × elapsed_sec / 1e6` |

**RAPL**（Intel/AMD x86）：硬件积分的累积 µJ，直接差分 = 本次测试消耗的能量。只输出 `energy_J`。
**HWMON**（ARM 等）：瞬时 µW，`energy_J` 去掉 SoC baseline，只算测试本身的贡献。

`energy_J` 覆盖整个测试窗口——所有 workload（stride / chase / random / compute）共用一行。传感器未检测到时这 1-4 列不输出。

#### compute_% 作为 sanity check

compute 测试的 `%` 值应该近似等于频率比：

```
freq=1500 MHz, max=3000 MHz
compute_% 预期 ≈ 1500/3000 × 100 = 50%

如果 compute_% = 95% → 频率没锁住！turbo 仍在跑
如果 compute_% = 30% → 该频率点 set_freq 可能失败了
```

### 甜点判定

程序自动计算：**最低频率 ≥ 最大吞吐的 95%** = 甜点。

```
甜点在 27% 频率 → 功耗节省 = (1 - 0.27) × (V_low/V_high)² ≈ 50-70%
甜点在 80% 频率 → 功耗节省有限，工作负载对频率敏感
```

95% 阈值可通过 `-T FRAC` 调整（默认 0.95）。想看甜点对阈值的敏感度,加 `-L 0.8,0.9,0.95,0.99` 触发 `# --- sensitivity ---` 块。

### 统计块

数据行之后,会按顺序追加 5 个 `#` 前缀的块（默认开启,不影响原 TSV 数据行,旧 parser 不受影响）。

> **注意：** `-f` 启用的 `bench_stride_flush` 工作负载（强制 L3 miss）**不会**出现在统计块里——它的数据只出现在原始数据行。

#### 1. `# --- per-freq stats (workload) ---`

每个频率点的 min / max / median / IQR。看 IQR 列判断测量噪声：

```
# --- per-freq stats (stride) ---
# freq_MHz  min_MOps  max_MOps  median_MOps  iqr_MOps
# 800       140.1     142.5     141.3        1.2
```

- IQR 全程 < 1% → 机器安静,数据可信
- 低频点 IQR 突然变大 → 该频点测量置信度低,谨慎使用

#### 2. `# --- sweet-spot CI ---`（仅 `-r` 时出现）

每次 sweep 的甜点是单点估计;这个块给出"如果我重跑这个实验,甜点会落在哪"的区间。

```
# --- sweet-spot CI ---
# workload  sweet_MHz  low_MHz  high_MHz  method
# stride    2000       1500     2050      bootstrap_1000
# chase     2000       1800     2050      bootstrap_1000
# random    1800       1700     2000      bootstrap_1000
```

- `sweet_MHz` = 现有 95% 甜点(标题值)
- `low_MHz` / `high_MHz` = bootstrap 2.5/97.5 百分位(B = 1000)
- `method` = `bootstrap_1000`（确定性 LCG 种子 `state=42`,结果可复现）

**Bootstrap 法**：`B = 1000` 次重采样,每次对每个 freq 抽取 `nsamples` 个样本(放回)取中位数,重跑 `find_sweet_spot()`。取 1000 个甜点值的 2.5 和 97.5 百分位。

**解读:**
- `low == sweet == high` → 数据噪声极小,单点估计很可靠
- 区间窄(< 200 MHz)→ 决策鲁棒,可放心使用
- 区间宽(> 500 MHz)→ 测量噪声大;考虑增加 `-n 5` 或更长 `-t`
- `— / — / —` + `no_plateau` → 该 workload 无平台

#### 3. `# --- sensitivity (workload) ---`（仅 `-L` 时出现）

不同阈值下的甜点。判断决策鲁棒性：

```
# --- sensitivity (stride) ---
# threshold  sweet_spot_MHz
# 0.80       1800
# 0.95       2000
# 0.99       2400
```

- 0.80 和 0.99 给的甜点差 < 100 MHz → 决策鲁棒
- 差 > 500 MHz → 工作负载对阈值敏感,挑高的阈值更安全

#### 4. `# --- plateau ---`

分段线性检测,给出 `plateau_breakpoint` 和 `slope ratio`：

```
# --- plateau ---
# stride   plateau_breakpoint: 2050 MHz  (slope ratio 18.3x, 95% sweet spot 2000 MHz)
# chase    plateau_breakpoint: 2100 MHz  (slope ratio 22.1x, 95% sweet spot 2000 MHz)
```

- `slope ratio > 2.0` → 有明显平台期,mem-bound 信号强
- `—`  → 无平台,吞吐随频率持续上升
- 95% sweet spot 同时给出作为对照;两个独立方法一致 = 强证据
- 每个 workload 行下面会再跟一行 `power:`(仅当检测到 plateau 且有功耗传感器时):

```
# stride   power: 45W at sweet spot (savings: 36% vs 71W at 2600 MHz)
```

这条直接回答 DVFS 节能 thesis：甜点频率低 + 甜点处功率远小于 max_freq 功率 = 实际节能空间。

#### 5. `# --- raw_samples (workload) ---`（仅 `-r` 时出现）

每个 freq × sample 的原始吞吐。用于自定义分析（bootstrap CI、分布检验）。

```
# --- raw_samples (stride) ---
# freq_MHz  sample_idx  mops
# 800       1           140.5
# 800       2           141.2
```

行数 = `n_freqs × nsamples`。

### JSON 输出

`memfreq_sweep.py --json` 导出的 JSON 包含上面 5 个块的全部数据:

```json
{
  "meta": {...},
  "sweet_spot_mhz": {"stride": 2000, "chase": 2000},
  "per_freq_stats": {
    "stride": [{"freq_mhz": 800, "min": 140.1, "max": 142.5, "median": 141.3, "iqr": 1.2}, ...],
    "chase":  [...],
    "random": [...]
  },
  "sensitivity": {
    "stride": [{"threshold": 0.80, "sweet_spot_mhz": 1800}, {"threshold": 0.95, "sweet_spot_mhz": 2000}, ...],
    "chase":  [...]
  },
  "plateau": [
    {"workload": "stride",  "breakpoint_mhz": 2050, "slope_ratio": 18.3, "sweet_spot_mhz": 2000, "has_plateau": true},
    {"workload": "chase",   "breakpoint_mhz": 2100, "slope_ratio": 22.1, "sweet_spot_mhz": 2000, "has_plateau": true}
  ],
  "raw_samples": {
    "stride":  [{"freq_mhz": 800, "sample_idx": 1, "mops": 140.5}, ...],
    "chase":   [...],
    "random":  [...]
  },
  "sweet_spot_ci": [
    {"workload": "stride",  "sweet_mhz": 2000, "low_mhz": 1500, "high_mhz": 2050, "method": "bootstrap_1000"},
    {"workload": "chase",   "sweet_mhz": 2000, "low_mhz": 1800, "high_mhz": 2050, "method": "bootstrap_1000"},
    {"workload": "random",  "sweet_mhz": 1800, "low_mhz": 1700, "high_mhz": 2000, "method": "bootstrap_1000"}
  ],
  "data": [{"freq_mhz": 800, "stride_mops": 141.3, ...}, ...]
}
```

`null` / `0` 表示无数据(对应 TSV 里的 `—`)。`compare_runs()` 只看 `sweet_spot_mhz`,新加的字段是给下游自定义分析用的。

### 关键指标速查

按输出顺序排列——每行都回答一个独立问题：

| 看什么 | 含义 | 出现位置 |
|--------|------|----------|
| `*_%` 低频 ≥95% | 该 workload mem-bound | 数据行 |
| `actual_MHz ≈ target_MHz` | 频率锁定成功 | 数据行 |
| `compute_% ≈ freq_ratio` | sanity check 独立验证 | 数据行 |
| `iqr_MOps < 1% of median` | 测量噪声低 | `# --- per-freq stats ---` |
| `CI width < 200 MHz` | 甜点位置鲁棒 | `# --- sweet-spot CI ---`（`-r`）|
| `0.80 vs 0.99 sweet 差 < 100 MHz` | 阈值选择不敏感 | `# --- sensitivity ---`（`-L`）|
| `slope ratio > 2.0` | 平台期强 = 强 mem-bound | `# --- plateau ---` |
| `power: savings: >30%` | **直接答 DVFS 节能量** | `# --- plateau ---` 的 `power:` 行 |

**反向信号**：`stride_%` / `chase_%` 随频率线性增长，说明该 workload 有 compute 成分（不是纯 mem-bound），降频需谨慎——把 `95%` 阈值降到 `99%` 验证（用 `-L 0.95,0.99` 看 sensitivity）。

---

## 多核并行模式（`-N`）

### 为什么需要多核测试？

单核测试测的是 **latency-bound** 甜点（DRAM 延迟主导），多核测试测的是 **bandwidth-bound** 甜点（内存控制器带宽饱和）。两者代表不同的真实场景：

| 模式 | 瓶颈 | 甜点位置 | 代表场景 |
|------|------|----------|----------|
| 单核 (`-c 0`) | DRAM 延迟 (~100ns) | 很低频 | DB 查询、编译器、shell |
| 多核 (`-N 4`) | MC 带宽饱和 | 中频 | HPC、AI 训练、视频编码 |

### CPU 自动分配

`-N NCPU` 自动选择 NCPU 个核心，分配策略：

1. **检测 NUMA 拓扑**（从 `/sys/devices/system/node/`）
2. **检测频率域**（从 `cpufreq/related_cpus`）
3. **SMT 规避**：读取 `thread_siblings_list`，只选每个物理核的第一个逻辑核
4. **均匀分布**：round-robin 跨 NUMA 节点分配
5. **每频率域一个**：避免同一频率域内重复测试

```bash
# 2 NUMA node, 每个 node 选 1 核（共 2 核）
sudo ./memfreq_bench -N 2 -m 512

# 每个 NUMA node 选 2 核（共 4 核）
sudo ./memfreq_bench -N 4 -m 512

# 半核（48 物理核 = 每 node 24 核）
sudo ./memfreq_bench -N 48 -m 512
```

### 多核执行架构

```
父进程（coordinator）
  ├── set_freq(f)
  ├── fork → child 0 (CPU 0, Node 0) → bench → write shared mem
  ├── fork → child 1 (CPU 48, Node 1) → bench → write shared mem
  ├── waitpid(all)
  ├── set_freq(f-1)
  ├── fork → child 0 → bench → write shared mem
  ├── fork → child 1 → bench → write shared mem
  └── ...
```

- 共享内存：`mmap(MAP_SHARED|MAP_ANONYMOUS)` — 零拷贝结果收集
- 父进程控制频率，子进程只跑 benchmark
- 结果按频率点聚合（中位数）

### 多核输出

```
# memfreq_bench multi-core results
# ncpus=4 array=512MB stride=8
# CPUs: 0 48 1 49
#
# target_MHz  actual_MHz  stride_Mops  stride_MBs  stride_%  chase_Mops  chase_%  compute_Mops  compute_%
2600          2598        580.2        4422.1      100.0     48.1        100.0    1205.8        100.0
...
```

注意 cpufreq 对同 cluster 内的所有核是联动的，所以同 cluster 的多核测试有意义；跨 cluster 的核可能频率独立。

---

## 噪音分析

### Chase 的纯度

chase 内循环编译后（arm64, `-O2`）：

```asm
.Lloop:
    ldr   x0, [x0]        ; p = p->next  (LOAD, ~300 cycles 等 DRAM)
    add   x1, x1, #1      ; i++          (1 cycle, 可并行)
    cmp   x1, x2          ; i < nnodes   (1 cycle, 可并行)
    b.ne  .Lloop          ; branch       (1 cycle, 分支预测器命中)
```

| 成分 | 占比 | 说明 |
|------|------|------|
| DRAM load 延迟 | **~98%** | `p = p->next` 的串行依赖链 |
| 循环控制 (add/cmp/branch) | **~1-2%** | 被乱序执行隐藏在 load 延迟后面 |
| 外循环 `now()` 调用 | **<0.001%** | 每 10ms 才调一次（~15ns/次） |

**chase 是 practically pure latency bound。**

### TLB 噪音与 Hugepage

```
4KB pages:
  512 MB / 4KB = 131,072 个页
  L1 dTLB 通常 48-64 条目

chase 随机访问 → 几乎每次 dereference 都 L1 TLB miss
  → 触发多级页表 walk（额外 20-40ns per access）
  → 测到的"延迟"= DRAM 延迟(~100ns) + page walk(~40ns) ≈ 140ns

  DRAM 延迟被高估 ~40%
```

这个噪音在各频率点**一致**（不影响相对比较），但如果你关心绝对延迟值，需要用 hugepage 消除：

```bash
# 分配 2MB hugepage（512 MB 需要 256 个 hugepage）
echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

程序目前用 `posix_memalign` 分配内存（4KB page）。要使用 hugepage，需要修改为 `mmap` + `MAP_HUGETLB`，或使用 `madvise` + `MADV_HUGEPAGE`。

### SMT 噪音

SMT 兄弟线程共享同一物理核的 L1/L2 cache、TLB 和执行单元。chase 是 98% 时间等 DRAM → 执行单元几乎空闲 → 端口竞争影响小。但 L1/L2 eviction 会让部分访问命中 L2 而非 miss 到 DRAM → 降低 mem-bound 纯度。**对甜点分析而言，SMT 兄弟 idle 即可，不需要 offline。**

### 系统级噪音

| 噪音源 | 影响 | 缓解 |
|--------|------|------|
| 中断（定时器、网卡等） | 抢占测试线程 | `isolcpus=` + `irqaffinity=` 隔离 CPU |
| SMT 兄弟负载 | 共享 L1/L2/执行单元 | offlining SMT sibling（见上方） |
| 后台进程 | 调度竞争 | 测试前 `systemctl isolate multi-user.target` |
| DRAM refresh 冲突 | tRFC 阻塞（~260ns / 7.8µs） | 固有噪音，中位数采样过滤 |
| 温度节流 | 高频持续后降频 | `-t 3` 控制单次测试时长 |

### 中位数采样

每个频率点跑 `nsamples` 次，取中位数：

```
3 次采样:  [97.1, 98.5, 94.2]  → 排序后 [94.2, 97.1, 98.5] → 取 97.1
5 次采样:  [97.1, 98.5, 94.2, 96.8, 97.9] → 排序 → 取 97.1

中位数比平均值更鲁棒——一次中断导致的异常低值被丢弃而非拉低均值
```

---

## 设计原理

### 为什么需要三种测试？

单一测试无法区分工作负载的性质：

```
场景 A：stride 97% at 800MHz，compute 25% at 800MHz
  → 工作负载是 memory-bound → DVFS 可以大胆降频

场景 B：stride 60% at 800MHz，compute 25% at 800MHz
  → 工作负载是 mixed → DVFS 需要更保守

场景 C：stride 25% at 800MHz，compute 25% at 800MHz
  → 工作负载是 compute-bound → 降频 = 降性能，DVFS 无节能空间
```

compute 测试作为**基线**——它告诉你"如果纯粹是计算密集，各频率的吞吐上限是多少"。stride/chase 的百分比与 compute 百分比的差异，就是 memory-bound 的程度。

### 为什么数组要大于 L3？

如果工作集能放进 L3，所有访问都是 L3 hit → 不访问 DRAM → 测试变成 cache 性能测试而非 DRAM 性能测试。

```
L3 典型大小：4-64 MB
默认数组：128 MB → 确保超出大多数 L3

例外情况：
  高核数 ARM（273 MB L3）：必须用 -m 512 或更大
  Apple M1 Ultra（128 MB L3）：用 -m 256
  嵌入式（0 L3 / 几 MB LLC）：默认 128 MB 绰绰有余
```

### 为什么 chase 用随机链表而非随机数组？

随机数组（`arr[rand() % N]`）的问题是：
- 现代 CPU 的 L1 dTLB 通常有 64-128 条目 → 如果数组 < 128 × 4KB = 512KB，全部 TLB hit
- 随机访问模式可能被 memory controller 的请求队列部分重叠（多个 outstanding miss）

随机链表的优势：
- **严格的串行依赖**（`p = p->next`）→ 下一个地址必须等当前 load 完成
- CPU 无法发射多个并行 DRAM 请求
- 测量的就是**单次 DRAM round-trip 延迟**

### 频率锁定机制

```c
// 三步写入 — 关键顺序！
// 1. 放宽 max 到硬件上限 → range 变成 [old_min, hw_max]
// 2. 写 min=khz → range 变成 [khz, hw_max]（always valid since khz ≤ hw_max）
// 3. 收紧 max=khz → range 变成 [khz, khz]（locked）
// 如果先写 min=X（X > 当前 max），内核会拒绝：min > max
sysfs_write("scaling_max_freq", hw_max);   // widen ceiling
sysfs_write("scaling_min_freq", khz);      // set floor
sysfs_write("scaling_max_freq", khz);      // tighten ceiling
```

程序退出时恢复原始 min/max 范围（通过 `freq_cleanup()`，包括 signal handler 和 `atexit`）。

### Turbo/Boost 处理

测试前自动关闭 turbo boost，测试后恢复。

| 平台 | sysfs 路径 |
|------|------------|
| Intel (intel_pstate) | `/sys/devices/system/cpu/intel_pstate/no_turbo` |
| 其他 (acpi-cpufreq 等) | `/sys/devices/system/cpu/cpufreq/boost` |

---

## 高核数服务器补充说明

本节补充高核数（48+ 物理核）服务器的特殊注意事项，详细内容见各交叉引用。

### NUMA 拓扑

多 NUMA node 下，本地和远端 DRAM 延迟差异 1.5-2×。本地测试（默认）测的是最佳情况；用 `numactl --membind=<远端node>` 可测最差情况。详见 [环境准备](#3-了解-numa-拓扑)。

### 大 L3 的陷阱

273 MB L3 意味着 stride 128 MB 数组可能部分命中 L3。chase 128 MB 链表约一半访问命中 L3。**解决方案**：数组大小 ≥ 2× L3 → 对于 273 MB L3 用 `-m 512`，更保险用 `-m 1024`。**验证**：如果 chase 的 Mops 在高频率点异常高（>20 Mops），说明大量 L3 hit → 加大数组。

### cpufreq 联动

同 cluster 内的核可能频率联动（如 ARM 的 cpufreq policy 按 cluster 分组），设置 cpu0 的频率可能同时影响 cpu0-3。多核测试时注意这一点。

---

## 限制和注意事项

1. **需要 root**：写 cpufreq sysfs 需要 root 权限
2. **需要 cpufreq 驱动**：程序依次尝试三种频率枚举方式——`scaling_available_frequencies`（离散列表）、acpi_cppc 高低非线性性能值、`cpuinfo_min/max_freq`（范围模式）。三者都不可用时才报错退出
3. **功耗仅记录，不参与甜点判定**：甜点本身是纯性能角度的判定（95% 最大吞吐）。功耗数据在 plateau 块的 `power:` 行输出
4. **stride 的 sum 有数据依赖**：`sum += arr[i]` 限制了流水线深度，但编译器可能展开为 partial sum，精确吞吐数值因编译器而异
5. **统计输出默认开启**：per-freq min/max/IQR 和 plateau 检测默认输出到结果末尾（`#`-prefixed 块,不影响数据行）。想看 sweep 的甜点稳定性,配合 `memfreq_sweep.py --compare` 比较多次 JSON 结果

---

## 与其他工具对比

| 工具 | 测量目标 | 与 memfreq_bench 的区别 |
|------|----------|------------------------|
| `mbw` | 内存带宽 | 不扫频率 |
| `lmbench` `lat_mem_rd` | 各级 cache 延迟 | 固定频率，不测试吞吐-频率关系 |
| `stream` | 内存带宽 | 不扫频率 |
| `perf mem` | cache miss 统计 | 不控频率，不做 sweet spot 分析 |
| `turbostat` | 实际功耗/频率 | 不生成工作负载 |

**memfreq_bench 的独特价值**：它不只是测内存性能或 CPU 性能——它测的是**"内存性能如何随 CPU 频率变化"**，这直接回答 DVFS governor 的核心问题：降频会损失多少性能？

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `memfreq_bench.c` | C 基准测试（需 root + cpufreq） |
| `stats.c` / `stats.h` | 统计辅助函数（甜点、平台期、bootstrap CI） |
| `memfreq_sweep.py` | Python 运行器 + ASCII 可视化 + JSON 导出 |
| `Makefile` | 构建 `memfreq_bench` 和 `test_stats` |
| `run_all_tests.sh` | 一键测试套件（7 个预定义场景，默认模式 ~2.5-5 小时，`--quick` 模式 ~20-45 分钟） |
| `run_full_sweep.sh` | 全量扫描（stride × 核数 × NUMA × 缓存层次 × L3-resident，默认 ~30 小时，`--quick` ~3 小时，跑 Suites 1/2/3/5/7/8） |
| `tests/test_stats.c` | stats.c 的 C 单元测试 |
| `tests/test_stats_output.sh` | Shell 测试框架（77 个断言） |
| `docs/memfreq-bench.md` | 本文档 |
| `docs/memfreq-sweep.md` | Python 可视化工具使用说明 |
