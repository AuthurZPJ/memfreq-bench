# run_full_sweep.sh 完整指南

## 目录
- [概述](#概述)
- [脚本执行流程追踪](#脚本执行流程追踪)
- [七个测试套件详解](#七个测试套件详解)
- [输出报告解读](#输出报告解读)
- [数据分析与 DVFS 调优](#数据分析与-dvfs-调优)
- [常见问题](#常见问题)

---

## 概述

`run_full_sweep.sh` 是一个**深度内存性能扫描工具**，通过 57 个测试全面探测 CPU 频率与内存性能的复杂关系。

**核心目标**：找出不同工作负载模式下的"甜点频率"（sweet spot）—— 在保持 95% 以上性能的前提下，可以降到的最低频率。

**预计耗时**（以 25 个频率点为例）：
- 默认模式（`-t 5 -n 5`）：~30 小时（57 个测试，每个频率点 ~76 秒）
- `--quick` 模式（`-t 2 -n 5`）：~3 小时（**跑 Suites 1/2/3/5/7**，约 23-37 个测试，每个频率点 ~33 秒）

**与其他脚本的关系**：
- `run_all_tests.sh` — 7 个预定义套件,默认模式（`-t 3 -n 3`）~2.5-5 小时,`--quick` 模式（`-t 1 -n 1`）~20-45 分钟,适合快速健康检查或 PR 前的回归测试
- `run_full_sweep.sh`（本脚本）— 57 个测试的穷举矩阵,适合深入分析、新硬件评估或发表数据
- 当 `run_all_tests.sh` 全部通过后再用本脚本做深入扫描

**输出文件**：
- `FULL_REPORT.txt` — 分析表格 + DVFS 建议（含 95% bootstrap CI、平台期起点、阈值敏感性扫描）
- `data.csv` — 测试摘要（可导入 Excel，含 CI、平台期、每阈值甜点）
- `raw_data.csv` — 所有频率点的原始数据
- `*.txt` — 每个测试的详细输出（含 raw_samples、sensitivity、plateau 块）

**统计增强（默认开启）**：
- `-r`：保留每样本原始吞吐 → 计算 95% bootstrap 置信区间
- `-L 0.80,0.90,0.95,0.99`：多阈值甜点扫描 → 评估"放宽性能标准能省多少电"
- 阈值集合可通过 `SENS_THRESHOLDS` 环境变量自定义

---

## 脚本执行流程追踪

### 阶段 1：初始化与拓扑检测（< 1 秒）

```bash
1. 权限检查
   └─ 验证 EUID=0（root），否则退出

2. cpufreq 可用性检查
   └─ 检查 /sys/devices/system/cpu/cpu0/cpufreq/ 目录
   └─ 验证可写权限（需要 root 才能修改频率）

3. memfreq_bench 二进制检查
   └─ 确认 ./memfreq_bench 存在且可执行

4. 拓扑检测（detect_topology 函数）
   ├─ 读取 lscpu：Core(s) per socket × Sockets = 物理核数
   ├─ 读取 nproc：逻辑线程数
   ├─ 计算 SMT 倍数：NUM_SMT = 逻辑核 / 物理核
   ├─ 读取 numactl -H：NUMA 节点数
   ├─ 读取 cpuinfo_min_freq / cpuinfo_max_freq：频率范围
   └─ 输出示例：
      Topology: 96P/192T, SMT=2, NUMA=2, 2000-2600 MHz
```

**关键参数**：
- `NUM_PHYSICAL = 96`（你的机器）
- `NUM_LOGICAL = 192`（SMT 开启）
- `NUM_SMT = 2`
- `NUM_NUMA = 2`
- `FMIN_MHZ = 2000`
- `FMAX_MHZ = 2600`

### 阶段 2：创建输出目录（< 1 秒）

```bash
5. 创建输出目录
   └─ OUTPUT_DIR="full_sweep_$(date +%Y%m%d_%H%M%S)"
   └─ 示例：full_sweep_20250115_143052/
```

### 阶段 3：运行 57 个测试（~30 小时）

脚本按顺序执行 7 个测试套件。每个测试调用 `memfreq_bench`，该程序会：

```
单个测试的内部流程（以 s8 为例）：
  
  1. 解析参数：-c 0 -A -s 8 -t 5 -n 5 -r -L 0.80,0.90,0.95,0.99
     └─ -c 0: 绑定到 CPU 0
     └─ -A: 自动检测 L3 大小，设置数组 = 2×L3 = 546MB
     └─ -s 8: stride = 8（每次跳 64 字节 = 1 个 cache line）
     └─ -t 5: 每个频率点测试 5 秒
     └─ -n 5: 每个频率点采样 5 次取中位数
     └─ -r:  保留每样本原始吞吐（用于 bootstrap 95% CI）
     └─ -L:  多阈值扫描（0.80/0.90/0.95/0.99 各自找一次甜点）
        阈值集合通过环境变量 SENS_THRESHOLDS 自定义
  
  2. 频率扫描（从高到低）
     └─ 读取 acpi_cppc 或 scaling_available_frequencies
     └─ 生成频率列表：[2600, 2575, 2550, ..., 2025, 2000] MHz
        （假设 25 个频率点，步长 25 MHz）
     
     对每个频率点 fi（从 2600 到 2000）：
       a. set_freq(cpu, fi)
          └─ 写入 scaling_max_freq 和 scaling_min_freq = fi
       
       b. usleep(100ms)
          └─ 等待频率稳定
       
       c. verify_freq(cpu)
          └─ 读取 cpuinfo_cur_freq，验证实际频率
       
       d. usleep(1.1s)
          └─ 等待 power*_average 窗口稳定
       
       e. 测量空闲功耗
          └─ P_idle = read_power()  // hwmon1/power1_average
       
       f. 运行 stride 基准测试
          └─ 5 次 × 5 秒 = 25 秒
          └─ 每次：sum += arr[0], arr[8], arr[16], ...
             （546MB / 8B = 68M 次访问）
          └─ 记录吞吐：Mops/sec
       
       g. 运行 chase 基准测试
          └─ 5 次 × 5 秒 = 25 秒
          └─ 每次：p = p->next（随机链表遍历）
          └─ 记录吞吐：Mops/sec
       
       h. 运行 compute 基准测试
          └─ 5 次 × 5 秒 = 25 秒
          └─ 每次：x = x * 1.0000001 + 0.0000001
          └─ 记录吞吐：Mops/sec
       
       i. 测量负载功耗
          └─ P_load = read_power()
          └─ energy = (P_load - P_idle) × elapsed_time
  
  3. 恢复原始频率
     └─ restore_freq_range()：恢复 governor 的原始 min/max
  
  4. 生成输出
     └─ 表头：# target_MHz  actual_MHz  stride_Mops  stride_MBs  stride_%  chase_Mops  chase_%  compute_Mops  compute_%  idle_W  load_W  delta_W  energy_J
     └─ 数据行（每行一个频率点）：
        2600  2598  156.6  1193.2  100.0  12.8  100.0  325.8  100.0  120.3  122.5  2.2  55.0
        2575  2573  156.5  1192.4   99.9  12.8  100.0  322.1   98.9  120.1  122.2  2.1  52.5
        ...
        2000  1998  152.3  1160.2   97.2  12.5   98.1  250.3   76.8  118.5  120.4  1.9  47.5
     
     └─ 甜点计算：
        # stride sweet spot: 2200 MHz (85% of max 2600 MHz)
        # chase  sweet spot: 2000 MHz (77% of max 2600 MHz)
        # compute sweet spot: — (scales linearly, always needs max freq)
```

**时间估算**：

每个频率点的耗时（`-t 5 -n 5`）：
- 频率切换 + 稳定等待：0.1s
- 功耗窗口稳定（仅 hwmon）：1.1s
- stride 基准：5 次 × 5 秒 = 25s
- chase 基准：5 次 × 5 秒 = 25s
- compute 基准：5 次 × 5 秒 = 25s
- 合计：~76 秒/频率点

以 25 个频率点为例：25 × 76 ≈ 1900 秒 ≈ 32 分钟/测试。
57 个测试 × 32 分钟 ≈ 30 小时，加上进程启动和数据写入开销。

> **提示**：如果 30 小时太长，可以用 `--quick`（`-t 2 -n 5`，跑 Suites 1/2/3/5/7）将时间缩短到 ~3 小时，或只运行部分套件（`--suite 1,3`）。

### 阶段 4：生成报告（< 10 秒）

```bash
6. 调用 generate_full_report()
   ├─ 解析所有 *.txt 文件
   ├─ 提取每个测试的：
   │   ├─ max_MBs（最高频率的吞吐）
   │   ├─ stride_sweet_MHz（stride 甜点频率）
   │   └─ chase_sweet_MHz（chase 甜点频率）
   ├─ 生成 FULL_REPORT.txt（5 个分析表格 + DVFS 建议）
   ├─ 生成 data.csv（测试摘要）
   └─ 生成 raw_data.csv（所有频率点数据）
```

### 阶段 5：完成提示

```bash
7. 输出完成信息
   └─ 总耗时、测试数量、成功/失败统计
   └─ 提示查看 FULL_REPORT.txt 和 CSV 文件
```

---

## 七个测试套件详解

### Suite A: Stride Grid（7 个测试）

**目的**：测试不同 stride 值如何影响频率敏感度。

**测试列表**：
- `s1`：stride=1（8 字节步长，硬件预取友好）
- `s2`：stride=2（16 字节）
- `s4`：stride=4（32 字节）
- `s8`：stride=8（64 字节 = 1 cache line，**现实场景**）
- `s16`：stride=16（128 字节）
- `s32`：stride=32（256 字节）
- `s64`：stride=64（512 字节，极端内存密集）

**物理机制**：
```
stride=1:  arr[0], arr[1], arr[2], arr[3], ...
           └─ 硬件预取器能预测 → 高带宽 → 对频率敏感

stride=8:  arr[0], arr[8], arr[16], arr[24], ...
           └─ 每次跳一个 cache line → 预取器仍可工作 → 中等敏感

stride=64: arr[0], arr[64], arr[128], arr[192], ...
           └─ 跳跃太大，预取器失效 → 纯 DRAM 延迟 → 对频率不敏感
```

**预期结果**：
- `s1` 甜点最高（如 2500 MHz）—— 预取器让带宽成为瓶颈
- `s8` 甜点中等（如 2200 MHz）—— 现实工作负载
- `s64` 甜点最低（如 2000 MHz）—— 纯延迟主导

**分析方法**：
```
甜点差异 = s1_sweet - s64_sweet
         = 2500 - 2000 = 500 MHz

这个差异量化了硬件预取器对频率敏感度的贡献：
- 差异大（>300 MHz）→ 预取器效果显著，现实负载可降频
- 差异小（<100 MHz）→ 预取器效果有限，降频空间小
```

---

### Suite B: Random + Flush Modes（3 个测试）

**目的**：测试两种"反预取"技术的组合效果。

**测试列表**：
- `random`：`-R`（Fisher-Yates 随机排列索引数组）
- `flush`：`-f`（每次访问后执行 `clflush` 驱逐 cache line）
- `randflush`：`-R -f`（同时使用两种技术）

**物理机制**：
```
random 模式：
  idx[] = Fisher-Yates shuffle of [0..N-1]
  for i in 0..N-1:
      sum += arr[idx[i]]
      └─ 访问顺序完全随机 → 预取器无法预测
      └─ 但 cache line 可能仍在 L3 中

flush 模式：
  for i in 0..N-1 step 8:
      sum += arr[i]
      clflush(&arr[i])  // 驱逐 cache line
      └─ 强制每次访问都是 L3 miss
      └─ 但访问模式仍是顺序的

randflush 模式：
  idx[] = Fisher-Yates shuffle
  for i in 0..N-1:
      sum += arr[idx[i]]
      clflush(&arr[idx[i]])
      └─ 随机访问 + 强制驱逐 → 最严苛的内存延迟测试
```

**预期结果**：
- `random` 甜点略低于 `s8`（随机性削弱预取）
- `flush` 甜点显著低于 `s8`（强制 L3 miss）
- `randflush` 甜点最低（双重打击）

**分析方法**：
```
flush_penalty = s8_sweet - flush_sweet
             = 2200 - 2000 = 200 MHz

这个惩罚量化了 L3 cache 对频率敏感度的贡献：
- 惩罚大 → L3 命中率高，降频会显著降低性能
- 惩罚小 → L3 命中率低，工作负载本身就是 DRAM 密集
```

---

### Suite C: Multi-Core Sweep（11 个测试）

**目的**：测试核心数量如何影响内存带宽饱和与甜点频率。

**测试列表**：
- `mc1`, `mc2`, `mc4`, `mc8`, `mc12`, `mc16`, `mc24`, `mc32`, `mc48`, `mc64`, `mc96`

**物理机制**：
```
mc1:  1 个核心访问内存
      └─ 内存控制器（MC）空闲 → 延迟主导 → 低甜点

mc8:  8 个核心并行访问
      └─ MC 开始饱和 → 带宽竞争 → 中等甜点

mc48: 48 个核心（半个系统）
      └─ MC 接近饱和 → 带宽主导 → 高甜点

mc96: 96 个核心（全系统）
      └─ MC 完全饱和 → 纯带宽瓶颈 → 最高甜点
```

**预期结果**（示例）：
```
Cores   Max MB/s   MB/s/core   Stride SP   Chase SP
1       1193       1193.0      2200        2000
2       2200       1100.0      2300        2100
4       4100       1025.0      2400        2200
8       7200        900.0      2500        2300
16     11000        687.5      2550        2400
32     14500        453.1      2600        2500
48     15200        316.7      2600        2600
96     15500        161.5      2600        2600
```

**分析方法**：

1. **带宽饱和曲线**：
   ```
   画 Max MB/s vs Cores 曲线：
   
   16000 ┤                                    ╭──── mc96
   14000 ┤                                ╭───╯ mc48
   12000 ┤                            ╭───╯ mc32
   10000 ┤                        ╭───╯ mc16
    8000 ┤                    ╭───╯ mc8
    6000 ┤                ╭───╯ mc4
    4000 ┤            ╭───╯
    2000 ┤        ╭───╯ mc2
       0 ┼────╮───╯ mc1
          1   2   4   8  16  32  48  64  96
                    Cores
   
   拐点（knee point）≈ 32 核
   → 超过 32 核后带宽增长缓慢 → MC 饱和
   ```

2. **MB/s/core 下降曲线**：
   ```
   1200 ┤╮ mc1
   1000 ┤╰╮ mc2
    800 ┤ ╰╮ mc4
    600 ┤  ╰────╮ mc8
    400 ┤       ╰────╮ mc16
    200 ┤            ╰──────────╮ mc32, mc48, mc96
      0 ┼──────────────────────────
          1   2   4   8  16  32  48  96
                    Cores
   
   MB/s/core 在 32 核后趋于平坦 → 每个核心分到的带宽不再下降
   → 32 核是"最优并发度"
   ```

3. **甜点频率 vs 核心数**：
   ```
   2600 ┤                                    ╭──── mc48-96
   2500 ┤                            ╭───────╯ mc16-32
   2400 ┤                    ╭───────╯ mc8
   2300 ┤                ╭───╯ mc4
   2200 ┤            ╭───╯ mc2
   2100 ┤        ╭───╯
   2000 ┤╮───────╯ mc1
      0 ┼──────────────────────────────────
          1   2   4   8  16  32  48  64  96
                    Cores
   
   甜点随核心数单调递增 → 核心越多，带宽压力越大，需要更高频率
   ```

**DVFS 调优启示**：
- 单核任务（数据库查询、编译）：可降到 2200 MHz
- 4-8 核任务（Web 服务器、构建服务器）：降到 2400-2500 MHz
- 16+ 核任务（HPC、AI 训练）：保持 2550-2600 MHz

---

### Suite D: Multi-Core × Access Modes（14 个测试）

**目的**：测试多核 + 不同访问模式的组合效果。

**测试矩阵**：
```
        stride=1   stride=64   random   flush
mc2     mc2_s1     mc2_s64     mc2_R    mc2_f
mc4     mc4_s1     mc4_s64     mc4_R    mc4_f
mc8     mc8_s1     mc8_s64     mc8_R    mc8_f
mc16    mc16_s1    mc16_s64    —        —
```

**物理机制**：
```
mc4_s1（4 核 + stride=1）：
  └─ 预取器有效 + 多核带宽竞争 → 高甜点

mc4_s64（4 核 + stride=64）：
  └─ 预取器失效 + 多核带宽竞争 → 中等甜点

mc4_R（4 核 + random）：
  └─ 随机访问 + 多核 → 中等甜点

mc4_f（4 核 + flush）：
  └─ 强制 L3 miss + 多核 → 低甜点（纯延迟主导）
```

**分析方法**：

对比 `mc4_s1` vs `mc4_s64`：
```
mc4_s1:  stride_sweet = 2500 MHz
mc4_s64: stride_sweet = 2300 MHz
差异 = 200 MHz

→ 即使在多核场景下，预取器仍能贡献 200 MHz 的甜点提升
→ 现实多核工作负载可安全降频 200 MHz
```

对比 `mc4_s8` vs `mc4_f`：
```
mc4_s8:  stride_sweet = 2400 MHz（来自 Suite C）
mc4_f:   stride_sweet = 2100 MHz
差异 = 300 MHz

→ 即使在多核场景下，L3 cache 仍能贡献 300 MHz 的甜点提升
→ 多核 + 高 L3 命中率的工作负载应谨慎降频
```

---

### Suite E: Full NUMA Matrix（~10 个测试）

**目的**：测试 NUMA 本地性对内存延迟和带宽的影响。

**测试列表**（2 NUMA 节点）：
```
单核 NUMA 测试（4 个）：
  n0c_m0mem_local   — CPU node 0, Memory node 0（本地）
  n0c_m1mem_remote  — CPU node 0, Memory node 1（远程）
  n1c_m0mem_remote  — CPU node 1, Memory node 0（远程）
  n1c_m1mem_local   — CPU node 1, Memory node 1（本地）

多核 NUMA 测试（6 个）：
  mc2_B0  — 2 核，内存绑定到 node 0
  mc2_B1  — 2 核，内存绑定到 node 1
  mc4_B0  — 4 核，内存绑定到 node 0
  mc4_B1  — 4 核，内存绑定到 node 1
  mc8_B0  — 8 核，内存绑定到 node 0
  mc8_B1  — 8 核，内存绑定到 node 1
```

**物理机制**：
```
本地访问（n0c_m0mem_local）：
  CPU 0 ──[QPI/UPI]──> MC 0 ──[DDR]──> DRAM 0
  └─ 延迟：~80-100 ns（典型值）
  └─ 带宽：CPU 0 独享 MC 0 的全部带宽

远程访问（n0c_m1mem_remote）：
  CPU 0 ──[QPI/UPI]──> MC 1 ──[DDR]──> DRAM 1
           ↑
           额外 50-150 ns 互联延迟
  └─ 延迟：~130-250 ns
  └─ 带宽：受 QPI/UPI 互联带宽限制
```

**预期结果**：
```
n0c_m0mem_local:  Max MB/s = 1193, chase = 12.8 Mops, sweet = 2200 MHz
n0c_m1mem_remote: Max MB/s =  950, chase =  8.5 Mops, sweet = 2300 MHz

远程 vs 本地：
  吞吐下降 = (1193 - 950) / 1193 = 20%
  延迟上升 = (12.8 - 8.5) / 8.5 = 51%
  甜点上升 = 2300 - 2200 = 100 MHz
```

**分析方法**：

1. **远程惩罚量化**：
   ```
   吞吐惩罚 = (local_MBs - remote_MBs) / local_MBs × 100%
            = (1193 - 950) / 1193 = 20%
   
   延迟惩罚 = (local_chase - remote_chase) / remote_chase × 100%
            = (12.8 - 8.5) / 8.5 = 51%
   
   → 远程访问导致 20% 吞吐损失 + 51% 延迟增加
   → NUMA 感知的内存分配很重要
   ```

2. **远程甜点的含义**：
   ```
   如果 remote_sweet > local_sweet（如 2300 > 2200）：
     → 远程访问受互联带宽限制 → 需要更高频率来补偿
     → 远程内存访问的工作负载应谨慎降频
   
   如果 remote_sweet < local_sweet（罕见）：
     → 远程访问受延迟主导 → 频率不敏感
     → 可以安全降频
   ```

3. **多核 NUMA 测试**：
   ```
   mc4_B0（4 核绑定 node 0）vs mc4_B1（4 核绑定 node 1）：
   
   如果 mc4_B0 和 mc4_B1 性能相近：
     → 两个 NUMA 节点对称 → 内存分配策略不敏感
   
   如果差异大：
     → NUMA 节点不对称 → 需要 NUMA 感知的调度器
   ```

---

### Suite F: Stress-NG Style Comparison（7 个测试）

**目的**：在半核和全核规模下，对比所有访问模式。

**测试列表**：
```
半核（48 核）：
  half_s1, half_s8, half_s64, half_R, half_f

全核（96 核）：
  full_s8, full_R
```

**物理机制**：
```
半核测试反映"典型高负载"场景：
  └─ 48 核 = 50% 系统利用率 → 生产环境常见负载
  └─ 比较不同访问模式在半载下的表现

全核测试反映"极端负载"场景：
  └─ 96 核 = 100% 系统利用率 → HPC/AI 训练场景
  └─ 验证 MC 是否完全饱和
```

**预期结果**：
```
half_s8:  Max MB/s = 15200, sweet = 2600 MHz
full_s8:  Max MB/s = 15500, sweet = 2600 MHz

差异 = 300 MB/s (2%)

→ 从半核到全核，带宽仅增长 2% → MC 在 48 核时已饱和
→ 超过 48 核的工作负载无需更高频率
```

**分析方法**：

```
如果 full_s8 ≈ half_s8（差异 < 5%）：
  → MC 在半核时已饱和 → 超过半核的任务无需更高频率
  → DVFS governor 可在 cores > 48 时保持 max_freq

如果 full_s8 >> half_s8（差异 > 15%）：
  → MC 未完全饱和 → 全核任务仍能受益于更高频率
  → DVFS governor 应根据 cores_active 动态调整
```

---

### Suite G: Cache Hierarchy Sweep（~5 个测试）

**目的**：测试不同数组大小对甜点频率的影响，识别 L2/L3 cache 边界处的行为变化。

**测试列表**（动态生成，取决于检测到的缓存大小）：
```
½×L2  — 数组 = L2 cache 的一半（L1/L2-bound 基线）
2×L2  — 数组 = L2 cache 的两倍（L2/L3 过渡区）
½×L3  — 数组 = L3 cache 的一半（L3-bound）
2×L3  — 数组 = L3 cache 的两倍（DRAM-bound，当前默认）
4×L3  — 数组 = L3 cache 的四倍（deep DRAM-bound，验证收敛）
```

**物理机制**：
```
延迟层次结构：
  L1: ~1ns     (跟随核频)  → 无甜点（compute-bound）
  L2: ~4-5ns   (跟随核频)  → 无甜点（近似 compute-bound）
  L3: ~15-25ns (uncore freq)→ 有甜点（uncore 甜点）
  DRAM: ~100ns (独立控制器) → 有甜点（DRAM 甜点）

数组大小的影响：
  ½×L2:  工作集完全在 L2 内 → 纯 compute-bound → 无甜点
  2×L2:  部分 L3 命中 → 开始出现 uncore 甜点
  ½×L3:  主要在 L3 内 → L3-bound → 甜点较低
  2×L3:  L3 miss 为主 → DRAM-bound → 甜点较高
  4×L3:  深度 DRAM-bound → 甜点应稳定
```

**预期结果**：
```
cache_half_L2_1MB:   stride_sweet = 3300 MHz（无甜点，compute-bound）
cache_double_L2_2MB: stride_sweet = 2800 MHz（过渡区）
cache_half_L3_16MB:  stride_sweet = 2400 MHz（L3-bound）
cache_double_L3_32MB:stride_sweet = 2200 MHz（DRAM-bound）
cache_quad_L3_64MB:  stride_sweet = 2200 MHz（收敛验证）
```

**分析方法**：

1. **甜点跳变点识别**：
   ```
   绘制数组大小 vs 甜点频率曲线：
   
   3300 ┤╮ ½×L2
   2800 ┤ ╰╮ 2×L2
   2400 ┤   ╰╮ ½×L3
   2200 ┤     ╰──────╮ 2×L3, 4×L3
      0 ┼───────────────
        1MB  2MB 16MB 32MB 64MB
              Array Size
   
   跳变点 1: 2MB → 16MB（L3 边界）
   → L3 开始主导内存访问
   
   跳变点 2: 16MB → 32MB（L3 miss 主导）
   → DRAM 延迟成为主要瓶颈
   ```

2. **收敛验证**：
   ```
   如果 2×L3 和 4×L3 的甜点差异 < 100 MHz：
     → DRAM-bound 甜点已收敛 → 2×L3 足够大
     → 无需更大数组
   
   如果差异 > 300 MHz：
     → 可能还有更深的 cache 层次（如 HBM）
     → 需要更大数组或进一步分析
   ```

3. **工作负载映射**：
   ```
   数据库（小工作集，~100MB）：
     → 接近 ½×L3 场景 → 甜点 ~2400 MHz
     → 可安全降频到 2400 MHz
   
   视频编码（中等工作集，~2GB）：
     → 接近 2×L3 场景 → 甜点 ~2200 MHz
     → 可降频到 2200 MHz
   
   大规模模拟（大工作集，~10GB）：
     → 接近 4×L3 场景 → 甜点 ~2200 MHz
     → 降频空间有限
   ```

**DVFS 调优启示**：
- 小工作集应用（数据库、Web 服务器）：可降频到 L3-bound 甜点（~2400 MHz）
- 大工作集应用（HPC、AI）：需保持 DRAM-bound 甜点（~2200 MHz）
- 混合工作负载：根据工作集大小动态调整频率

---

## 输出报告解读

### FULL_REPORT.txt 结构

报告包含 5 个分析表格 + DVFS 建议：

```
================================================================
  memfreq_bench Full Sweep Analysis
================================================================

Timestamp : 2025-01-15T14:30:52+08:00
Hostname  : arm-server-01
Kernel    : 5.15.0-generic
Topology  : 96P/192T, SMT=2, NUMA=2
Frequency : 2000 – 2600 MHz
Duration  : 5s × 5 samples

Results: 57 tests (57 ok, 0 failed)
```

**解读**：
- 57 个测试全部成功 → 数据可靠
- 频率范围 2000-2600 MHz → 600 MHz 调节空间
- 2 NUMA 节点 → 需要关注本地性

---

### TABLE 1: Stride Grid

```
================================================================
  TABLE 1: Stride Grid — How stride affects sweet spot
================================================================

Test      Max MB/s   Stride SP   Stride 95% CI   Chase SP   Chase 95% CI   Plateau BP
----      --------   ---------   -------------   ---------   -------------   ----------
s1          1850.6        2500     2400-2500         2100     2050-2100         2700
s2          1650.3        2400     2350-2400         2050     2000-2050         2650
s4          1400.8        2300     2250-2300         2000     1950-2000         2600
s8          1193.2        2200     2150-2250         2000     1950-2000         2550
s16         1050.5        2150     2100-2200         2000     1950-2000         2500
s32          980.2        2100     2050-2150         2000     1950-2000         2450
s64          920.1        2000     2000-2000         2000     1950-2000         2400

Analysis:
  stride=1:  HW prefetcher effective → higher bandwidth → freq-sensitive
  stride=8:  1 cache line/access → realistic memory-bound
  stride=64: Extreme miss → pure DRAM latency → freq-insensitive
  Gap between stride=1 and stride=64 sweet spots shows
  prefetcher's contribution to frequency sensitivity.
  CI = 95% bootstrap range on the 95% sweet spot (wider = noisier)
  BP  = plateau breakpoint MHz (elbow where DRAM starts binding)
```

> **新增列**：
> - `Stride 95% CI` / `Chase 95% CI`：1000 次 bootstrap 重采样得到的 95% 置信区间。范围越宽说明测量噪声越大，需要更多样本。
> - `Plateau BP`：检测到的平台期起点（knee）。低于此频率，吞吐开始明显下降。

**解读步骤**：

1. **甜点差异**：
   ```
   prefetcher_contribution = s1_sweet - s64_sweet
                           = 2500 - 2000 = 500 MHz
   ```
   → 硬件预取器贡献了 500 MHz 的甜点提升 → 现实工作负载（类似 stride=8）可安全降频

2. **置信区间解读**：
   ```
   s8 stride:  SP=2200, CI=2150-2250
   → 真实甜点有 95% 概率在 2150-2250 之间
   → 即使最坏情况(2250)，仍比无脑 boost(3300)省 30% 电压
   ```
   s64 的 CI 退化为单点（2000-2000），说明测量非常稳定，因为纯 DRAM 延迟主导。

3. **平台期起点**：
   ```
   s1 BP=2700, s64 BP=2400
   → 高频段 2700-3300 已经进入平台期
   → 真的需要 3300 MHz 跑 s1 吗？不需要，2700 就够了
   ```

4. **带宽下降曲线**：
   ```
   从 s1 到 s64，带宽从 1850 降到 920 MB/s（-50%）
   → 预取器失效后性能损失显著
   → 顺序访问的工作负载应优化为小 stride
   ```

5. **chase 甜点恒定**：
   ```
   chase_sweet 始终 = 2000 MHz
   → 指针追踪完全不受预取器影响 → 纯延迟主导
   → 链式数据结构的工作负载可大幅降频
   ```

---

### TABLE 2: Multi-Core Sweep

```
================================================================
  TABLE 2: Multi-Core — How core count affects bandwidth
================================================================

Cores     Max MB/s   MB/s/core   Stride SP   Stride 95% CI   Chase SP   Chase 95% CI   Plateau BP
-----     --------   ---------   ---------   -------------   ---------   -------------   ----------
1 cores      1193      1193.0        2200     2150-2250         2000     1950-2000         2550
2 cores      2200      1100.0        2300     2250-2350         2100     2050-2150         2600
4 cores      4100      1025.0        2400     2350-2450         2200     2150-2250         2700
8 cores      7200       900.0        2500     2450-2550         2300     2250-2350         2800
12 cores     9500       791.7        2550     2500-2600         2400     2350-2450         2850
16 cores    11000       687.5        2550     2500-2600         2400     2350-2450         2900
24 cores    13000       541.7        2600     2550-2650         2500     2450-2550         2950
32 cores    14500       453.1        2600     2550-2650         2500     2450-2550         3000
48 cores    15200       316.7        2600     2550-2650         2600     2550-2650         3000
64 cores    15400       240.6        2600     2550-2650         2600     2550-2650         3000
96 cores    15500       161.5        2600     2550-2650         2600     2550-2650         3000

Analysis:
  MB/s should increase linearly until MC bandwidth saturates.
  MB/s/core should DECREASE as cores compete for bandwidth.
  Sweet spot should INCREASE with core count (more BW pressure).
  The knee point = optimal core count for this memory subsystem.
  CI widens as cores compete (more variance under load).
```

**解读步骤**：

1. **找拐点（knee point）**：
   ```
   带宽增长速率：
     1→2 核：+1007 MB/s (+84%)
     2→4 核：+1900 MB/s (+86%)  ← 超线性？可能是缓存效应
     4→8 核：+3100 MB/s (+76%)
     8→12 核：+2300 MB/s (+32%) ← 增速放缓
     12→16 核：+1500 MB/s (+16%)
     16→24 核：+2000 MB/s (+18%)
     24→32 核：+1500 MB/s (+12%) ← 接近饱和
     32→48 核：+700 MB/s (+5%)   ← 饱和
     48→96 核：+300 MB/s (+2%)   ← 完全饱和
   
   拐点 ≈ 32 核
   ```

2. **MB/s/core 分析**：
   ```
   1 核：1193 MB/s/core → 独享全部带宽
   96 核：161 MB/s/core → 共享带宽，每个核心仅得 13.5%
   
   效率下降 = (1193 - 161) / 1193 = 86%
   → 96 核时每个核心的内存效率仅为单核的 13.5%
   ```

3. **甜点 vs 核心数**：
   ```
   1 核：2200 MHz（延迟主导）
   8 核：2500 MHz（带宽竞争开始）
   32 核：2600 MHz（带宽饱和）
   96 核：2600 MHz（完全饱和，甜点不再上升）
   
   → 超过 32 核后甜点恒定 → MC 已饱和
   → DVFS governor 在 cores > 32 时可保持 max_freq
   ```

---

### TABLE 3: NUMA Matrix

```
================================================================
  TABLE 3: NUMA Matrix — CPU node vs Memory node
================================================================

  CPU node 0 → Mem node 0 (LOCAL):  1193 MB/s, stride=2200 [CI 2150-2250], chase=2000 [CI 1950-2000], BP=2550
  CPU node 0 → Mem node 1 (REMOTE):  950 MB/s, stride=2300 [CI 2250-2350], chase=2100 [CI 2050-2150], BP=2650
  CPU node 1 → Mem node 0 (REMOTE):  948 MB/s, stride=2300 [CI 2250-2350], chase=2100 [CI 2050-2150], BP=2650
  CPU node 1 → Mem node 1 (LOCAL):  1190 MB/s, stride=2200 [CI 2150-2250], chase=2000 [CI 1950-2000], BP=2550

Analysis:
  Local should have lower latency (higher chase Mops).
  Remote adds interconnect latency (~50-150ns extra).
  If remote sweet spot is LOWER → latency dominates (less freq-sensitive).
  If remote sweet spot is HIGHER → BW dominates (interconnect bottleneck).
```

**解读步骤**：

1. **对称性检查**：
   ```
   node 0 local ≈ node 1 local（1193 vs 1190）
   node 0 remote ≈ node 1 remote（950 vs 948）
   → 两个 NUMA 节点对称 → 内存分配策略不敏感
   ```

2. **远程惩罚**：
   ```
   吞吐惩罚 = (1193 - 950) / 1193 = 20%
   延迟惩罚 = (2000 - 2100) / 2000 = -5%（chase 下降 5%）
   甜点上升 = 2300 - 2200 = 100 MHz
   
   → 远程访问导致 20% 吞吐损失 + 5% 延迟增加 + 100 MHz 甜点上升
   → NUMA 感知的内存分配可避免 20% 性能损失
   ```

3. **远程甜点更高的含义**：
   ```
   remote_sweet (2300) > local_sweet (2200)
   → 远程访问受互联带宽限制 → 需要更高频率来补偿
   → 远程内存访问的工作负载应谨慎降频
   ```

---

### TABLE 4: Access Mode × Scale

```
================================================================
  TABLE 4: Access Mode × Scale Combinations
================================================================

Test          Max MB/s   Stride SP   Stride 95% CI   Chase SP   Chase 95% CI   Plateau BP
----          --------   ---------   -------------   ---------   -------------   ----------
mc2_s1           2350        2400     2350-2450         2150     2100-2200         2700
mc2_s64          2100        2250     2200-2300         2100     2050-2150         2600
mc2_R            2050        2300     2250-2350         2100     2050-2150         2650
mc2_f            1950        2150     2100-2200         2050     2000-2100         2550
mc4_s1           4500        2500     2450-2550         2250     2200-2300         2800
mc4_s64          4000        2350     2300-2400         2200     2150-2250         2700
mc4_R            3900        2400     2350-2450         2200     2150-2250         2750
mc4_f            3700        2250     2200-2300         2150     2100-2200         2650
...
half_s1         15000        2600     2550-2650         2600     2550-2650         3000
half_s8         15200        2600     2550-2650         2600     2550-2650         3000
half_s64        14800        2600     2550-2650         2600     2550-2650         3000
half_R          14900        2600     2550-2650         2600     2550-2650         3000
half_f          14700        2600     2550-2650         2600     2550-2650         3000
full_s8         15500        2600     2550-2650         2600     2550-2650         3000
full_R          15400        2600     2550-2650         2600     2550-2650         3000
```

**解读步骤**：

1. **小规模（2-4 核）的模式差异**：
   ```
   mc2_s1 vs mc2_s64：甜点差异 = 2400 - 2250 = 150 MHz
   mc4_s1 vs mc4_s64：甜点差异 = 2500 - 2350 = 150 MHz
   
   → 即使在多核下，预取器仍贡献 ~150 MHz
   → 多核 + 顺序访问的工作负载可安全降频 150 MHz
   ```

2. **大规模（半核/全核）的模式无关性**：
   ```
   half_s1 vs half_s64：甜点差异 = 0 MHz
   full_s8 vs full_R：甜点差异 = 0 MHz
   
   → 在 MC 饱和后，访问模式不再影响甜点
   → 超过 48 核的工作负载无需区分访问模式
   ```

---

### TABLE 5: Threshold Sensitivity

> 这是 `run_full_sweep.sh` 借助 `-L` (multi-threshold sweep) 自动汇总的甜点扫描表。
> 默认扫描 4 个阈值（0.80/0.90/0.95/0.99）评估"如果放宽性能标准，能省多少电"。
> 要点：越靠左（低阈值）→ 降频越多 → 越省电，但离峰值吞吐越远。

```
================================================================
  TABLE 5: Threshold Sensitivity — sweet spot at each threshold
  (How aggressive can the 95% threshold be pushed? Lower = more savings.)
================================================================

  --- stride workload ---
  Test          @0.80     @0.90     @0.95     @0.99
  ----          -------   -------   -------   -------
  s1             2000      2400      2500      2500
  s2             1900      2300      2400      2400
  s4             1800      2200      2300      2300
  s8             1700      2100      2200      2200
  s16            1600      2050      2150      2150
  s32            1500      2000      2100      2100
  s64            1400      2000      2000      2000
  mc1            1700      2100      2200      2200
  mc4            2000      2300      2400      2400
  mc8            2200      2400      2500      2500
  ...

  --- chase workload ---
  Test          @0.80     @0.90     @0.95     @0.99
  ----          -------   -------   -------   -------
  s1             1600      1900      2000      2000
  s8             1600      1900      2000      2000
  ...
```

**解读步骤**：

1. **阈值与省电的权衡**：
   ```
   0.80 → 0.99 区间内，stride 甜点从 1400 涨到 2000 MHz (+43%)
   → 如果业务能容忍 80% 峰值吞吐，可省约 (1 - 1400/2000)² ≈ 51% 电压（P ∝ V²f）
   → 这是大幅降频的核心数据支撑
   ```

2. **不同访问模式的"阈值韧性"**：
   ```
   stride (s8): @0.80=1700, @0.99=2200 → 差 500 MHz，对阈值敏感
   chase:        @0.80=1600, @0.99=2000 → 差 400 MHz，对阈值较韧
   → pointer-chase 几乎无平台期，阈值上推收益递减
   → 顺序访问在 0.80 阈值就掉到甜点以下
   ```

3. **多核阈值的非线性**：
   ```
   mc1:  @0.80=1700  @0.99=2200
   mc4:  @0.80=2000  @0.99=2400  ← 涨 300
   mc8:  @0.80=2200  @0.99=2500  ← 涨 300
   → 每翻倍核心数，甜点抬升 300 MHz，0.80/0.99 同步移动
   → 高核心数时降低阈值无额外省电（曲线被 MC 饱和压平）
   ```

4. **"—"行的含义**：
   ```
   chase 在 @0.99 处大量 "—"
   → chase 几乎总是线性，没有平台期可压
   → 这恰好印证 chase 是纯 DRAM 延迟主导的"最美" memory-bound 工作负载
   ```

> **调参建议**：
> - 延迟敏感（DB、RPC）：用 0.99 阈值 → 选 `@0.99` 列
> - 吞吐优先（HPC、视频编码）：用 0.95 阈值 → 选 `@0.95` 列
> - 极致省电（IoT、移动设备）：用 0.80 阈值 → 选 `@0.80` 列
>
> 阈值集合可通过环境变量 `SENS_THRESHOLDS` 自定义：
> ```bash
> # 只关心 0.90 和 0.99 两个保守档位
> sudo SENS_THRESHOLDS=0.90,0.99 ./run_full_sweep.sh --yes
> ```

---

### DVFS Governor Recommendations

```
================================================================
  DVFS GOVERNOR RECOMMENDATIONS
================================================================

Based on test results:

Latency-bound workloads (single task, DB queries, compilers):
  → Safe frequency floor: 2200 MHz
  → Below this, DRAM latency dominates and freq barely matters

Moderate multi-task (4 cores, web server, build server):
  → Safe frequency floor: 2400 MHz
  → Memory controller under moderate pressure

Heavy multi-task (48 cores, HPC, AI training):
  → Safe frequency floor: 2600 MHz
  → Memory controller near saturation

Governor strategy:
  - If compute_ratio > 0.7: use full frequency (compute-bound)
  - If mem_ratio > 0.7 and cores_active ≤ 4: drop to 2200 MHz
  - If mem_ratio > 0.7 and cores_active > 4: drop to 2400 MHz
  - If both high: interpolate between compute and memory sweet spots
```

**解读**：

1. **三档策略**：
   ```
   轻载（≤4 核）：2200 MHz → 节能 (2600-2200)/2600 = 15%
   中载（5-32 核）：2400 MHz → 节能 (2600-2400)/2600 = 8%
   重载（>32 核）：2600 MHz → 不节能（MC 饱和）
   ```

2. **插值策略**：
   ```
   如果 compute_ratio = 0.5, mem_ratio = 0.5：
     compute_sweet = 2600 MHz（compute 始终需要高频）
     memory_sweet = 2400 MHz（假设 4 核）
     
     target_freq = 0.5 × 2600 + 0.5 × 2400 = 2500 MHz
   ```

3. **能耗估算**：
   ```
   假设功耗 ∝ freq^2（简化模型）：
   
   轻载节能 = (2600^2 - 2200^2) / 2600^2 = 28%
   中载节能 = (2600^2 - 2400^2) / 2600^2 = 15%
   
   → 轻载时降频可节能 28%，中载节能 15%
   ```

---

## 数据分析与 DVFS 调优

### 使用 CSV 数据

1. **导入 Excel/LibreOffice**：
   ```bash
   # data.csv 包含测试摘要
   libreoffice --calc data.csv
   
   # raw_data.csv 包含所有频率点数据
   libreoffice --calc raw_data.csv
   ```

2. **绘制图表**：
   ```
   推荐图表：
   
   a) 甜点频率 vs 核心数（来自 data.csv）
      X 轴：测试名称（mc1, mc2, mc4, ...）
      Y 轴：stride_sweet_MHz
      
   b) 带宽 vs 核心数（来自 data.csv）
      X 轴：核心数（对数刻度）
      Y 轴：max_MBs
      → 找拐点
      
   c) 频率-性能曲线（来自 raw_data.csv）
      X 轴：target_MHz
      Y 轴：stride_MBs
      → 每个测试一条曲线，叠加显示
   ```

3. **高级分析**：
   ```bash
   # 计算每个测试的能效（MB/J）
   awk -F, 'NR>1 {
     test=$1; mbs=$3; energy=$NF
     if (energy > 0) {
       eff = mbs / energy
       printf "%s,%.2f\n", test, eff
     }
   }' raw_data.csv > efficiency.csv
   ```

### DVFS Governor 实现建议

基于测试结果，governor 可这样实现：

```c
// 伪代码
int select_freq(int cores_active, double compute_ratio, double mem_ratio) {
    // 从 FULL_REPORT.txt 提取的甜点表
    int sweet_spot_table[] = {
        // cores -> sweet spot (MHz)
        [1]  = 2200,
        [2]  = 2300,
        [4]  = 2400,
        [8]  = 2500,
        [16] = 2550,
        [32] = 2600,
        [48] = 2600,
        [96] = 2600,
    };
    
    // 查找最近的核心数
    int nearest_cores = find_nearest(cores_active, sweet_spot_table);
    int memory_sweet = sweet_spot_table[nearest_cores];
    
    // 计算混合甜点
    int compute_sweet = 2600;  // compute 始终需要高频
    int target = (compute_ratio * compute_sweet + 
                  mem_ratio * memory_sweet) / 
                 (compute_ratio + mem_ratio);
    
    // 限制范围
    return clamp(target, 2000, 2600);
}
```

---

## 常见问题

### Q1: 为什么测试需要这么长时间？

A: 每个测试在所有频率点上运行 3 个工作负载，每个工作负载做 5 次 × 5 秒采样。以 25 个频率点为例：
- 每个频率点：~76 秒（3 × 5 × 5s + 开销）
- 每个测试：25 × 76 ≈ 32 分钟
- 57 个测试：~30 小时

缩短时间的方法：
- `--quick`：`-t 2 -n 5`，跑 Suites 1/2/3/5/7，缩短到 ~3 小时
- `--suite 1,3`：只运行部分套件
- `--duration 3 --samples 3`：自定义参数

### Q2: `--quick` 模式和默认模式有什么区别？

A: `--quick` 跑 **Suites 1/2/3/5/7**（stride grid、random/flush、多核 sweep、NUMA 矩阵、缓存层次），跳过 4/6（多核组合、stress-NG 对比）。其中 Suite 1 缩减为 4 个代表性 stride（1, 8, 32, 64），并将每个频率点的测试参数调整：
- 默认：`-t 5 -n 5`（每个 workload 在每个频率点采样 5 次，每次 5 秒 = 25s/工作负载/频率点）
- Quick：`-t 2 -n 5`（5 次 × 2 秒 = 10s/工作负载/频率点）

默认跑全部 57 测试约 30 小时，Quick 跑约 23-37 测试约 3 小时。Quick 模式保留了 5 次采样的中位数降噪能力，适合快速验证或时间有限的场景。正式分析建议使用默认参数。

### Q3: 如何判断数据是否可靠？

A: 检查 `FULL_REPORT.txt` 顶部的统计：
```
Results: 57 tests (57 ok, 0 failed)
```
- 全部成功 → 数据可靠
- 有失败 → 检查失败的测试，可能是 cpufreq 权限或硬件问题

### Q4: 为什么某些测试的甜点 = 2600 MHz（最大频率）？

A: 表示该工作负载在整个频率范围内性能持续上升，没有"平台期"。常见于：
- 计算密集型工作负载（compute_ratio 高）
- 多核带宽饱和场景（MC 瓶颈）

### Q5: 如何优化测试时间？

A: 调整参数：
```bash
# 减少采样次数（精度略降）
sudo ./run_full_sweep.sh --samples 3

# 减少测试时长（噪声略增）
sudo ./run_full_sweep.sh --duration 3

# 只运行特定套件（套件编号 1-7）
sudo ./run_full_sweep.sh --suite 1,3    # 只测 stride 和多核

# 组合使用
sudo ./run_full_sweep.sh --samples 3 --duration 3 --suite 1,3
```

### Q6: 如何对比不同机器的结果？

A: 使用 `data.csv`：
```bash
# 机器 A
sudo ./run_full_sweep.sh --output results_A

# 机器 B
sudo ./run_full_sweep.sh --output results_B

# 对比
diff results_A/data.csv results_B/data.csv
```

### Q7: 测试过程中可以中断吗？

A: 可以，但已运行的测试数据会保留在输出目录中。重新运行时，脚本会创建新的输出目录，不会覆盖旧数据。

### Q8: 如何将结果集成到自动化流程？

A: 使用 `--yes` 跳过交互提示：
```bash
sudo ./run_full_sweep.sh --yes --output automated_results

# 解析结果
python3 analyze.py automated_results/data.csv
```

---

## 附录：测试命名规则

```
s{stride}              — 单核 + stride 值（如 s8 = stride=8）
random                 — 单核 + Fisher-Yates 随机
flush                  — 单核 + cache flush
randflush              — 单核 + random + flush
mc{N}                  — 多核 N 个（如 mc4 = 4 核）
mc{N}_s{stride}        — 多核 + stride（如 mc4_s64）
mc{N}_R                — 多核 + random（如 mc4_R）
mc{N}_f                — 多核 + flush（如 mc4_f）
n{X}c_m{Y}mem_{L}      — NUMA 测试（如 n0c_m1mem_remote）
mc{N}_B{M}             — 多核 + 内存绑定到 NUMA node M
half_{mode}            — 半核（48 核）+ 模式
full_{mode}            — 全核（96 核）+ 模式
```
