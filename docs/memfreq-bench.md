# memfreq_bench: 找到 Memory-Bound 工作负载的频率甜点

> Memory-bound 期间 CPU 不需要那么高频率，但过多降频又影响性能。
> 这个工具帮你在两者之间找到最优平衡点。

---

## 核心原理

CPU 频率决定了计算单元每秒能推进多少个"状态步"。但 CPU 的吞吐不一定随频率线性增长——**取决于瓶颈在哪里**：

```
                     吞吐
                      ↑
         compute-bound│        ╱  ← 线性：频率翻倍，吞吐翻倍
                      │      ╱
                      │    ╱
                      │  ╱
                      │╱
         memory-bound │──────────────  ← 平台期：频率再高也没用，在等 DRAM
                      │
                      └──────────────────→ 频率
                          ↑
                       甜点区
                  (降频不丢性能，但省电)
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

## 三种工作负载

工具运行三种基准测试，覆盖不同的"内存依赖度"：

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
for (size_t i = 0; i < 100000; i++)
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

### 3. Compute（纯计算，对照组）

```c
double x = 1.00001;
for (size_t i = 0; i < 1000000; i++)
    x = x * 1.0000001 + 0.0000001;
```

**微架构分析：**
- `x = x * A + B` 有严格的数据依赖 → 无法 SIMD 化
- 零内存访问 → 不涉及 cache / DRAM
- 吞吐**完全由 CPU 频率决定** → 频率减半，吞吐减半

**用途：sanity check**。如果 compute 在不同频率下的吞吐比不等于频率比，说明频率没有成功锁定（turbo 还在跑、或者 governor 覆盖了你的设置）。

---

## 使用方法

### 编译

```bash
gcc -O2 -o memfreq_bench memfreq_bench.c -lm
```

### 运行前：环境准备

#### 第一步：确认硬件拓扑

```bash
# L3 大小 — 决定数组该多大
lscpu | grep "L3 cache"
# 或详细拓扑
lscpu | grep -E "L[123]|Thread|Core|Socket|NUMA"
```

**关键：数组大小必须 > L3 cache，否则测试的是 L3 而非 DRAM。**

| L3 大小 | `-m` 参数 | 说明 |
|---------|-----------|------|
| ≤ 32 MB | 128（默认） | 标准服务器 |
| 32-128 MB | 256-512 | 高端服务器 |
| 128-300 MB | 512-1024 | 高核数 ARM（如 96 核 / 273 MB L3） |

#### 第二步：处理 SMT

SMT 兄弟线程共享同一物理核的 L1/L2 cache 和执行单元。如果 SMT 兄弟在运行其他任务，会引入不可控噪音：

```bash
# 查看 SMT 映射（以 96 物理核 × SMT 2 = 192 逻辑 CPU 为例）
cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
# 输出: 0,96  → cpu 0 和 cpu 96 共享同一物理核

# 隔离 SMT 兄弟（关闭每个物理核的第二个 SMT 线程）
for s in $(ls /sys/devices/system/cpu/cpu*/topology/thread_siblings_list \
           | xargs -I{} sh -c 'cut -d, -f2 {}' | sort -u); do
    echo 0 > /sys/devices/system/cpu/cpu$s/online
done

# 测试完成后恢复
echo on > /sys/devices/system/cpu/smt/control
# 或逐个恢复：echo 1 > /sys/devices/system/cpu/cpu$s/online
```

SMT 隔离不是必须的——cpufreq 通常按物理核调频，SMT 兄弟的负载不影响频率设置。但如果追求低噪音测量，隔离是推荐的。

#### 第三步：了解 NUMA 拓扑

高核数服务器通常有多个 NUMA node，本地和远端 DRAM 延迟差异显著：

```bash
numactl -H
# 典型输出（4 NUMA node）：
# available: 4 nodes (0-3)
# node 0 cpus: 0 1 2 ... 23 96 97 98 ... 119
# node 0 size: 128 GB
# node distances:
#    0  1  2  3
# 0: 10 20 30 30
# 1: 20 10 30 30
# ...
```

```
本地 NUMA node DRAM：~80-120 ns
远端 NUMA node DRAM：~150-250 ns（经过 interconnect）

→ 对 DVFS 甜点分析：本地 NUMA 测试就够了
→ 远端 DRAM 延迟更高 → 甜点只会更低（更多时间等 interconnect）
```

### 运行

```bash
# 基本测试（确认数组 > L3 后）
sudo ./memfreq_bench -m 512

# 指定 CPU（选物理核，非 SMT 线程）
sudo ./memfreq_bench -c 0 -m 512

# 更大的 stride（更极端 memory-bound）
sudo ./memfreq_bench -c 0 -m 512 -s 16

# 更长的测试时间（减少系统噪音）
sudo ./memfreq_bench -c 0 -m 512 -t 5 -n 5

# 跨 NUMA 对比（把内存分配到远端 node）
sudo numactl --membind=1 ./memfreq_bench -c 0 -m 512

# 跳过 chase 测试
sudo ./memfreq_bench -C
```

### 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-c CPU` | 0 | 绑定到指定 CPU 核心（逻辑 CPU 编号） |
| `-m SIZE_MB` | 128 | 数组大小（MB），**必须 > L3 cache** |
| `-s STRIDE` | 8 | 步长（uint64 单位），8 = 64B = 一个 cache line |
| `-t SECS` | 2 | 每个频率点的测试时间 |
| `-n N` | 3 | 每个频率点采样次数，取中位数 |
| `-C` | — | 跳过 pointer chase 测试 |

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
```

---

## 解读输出

### 典型输出

```
# memfreq_bench results
# cpu=0 array=512MB stride=8 duration=3s samples=5
#
# freq_MHz  stride_Mops  stride_%  chase_Mops  chase_%  compute_Mops  compute_%
800    152.3    97.2    12.5    98.1    82.1    25.3
1200   154.1    98.4    12.6    98.8    123.4   37.9
1800   155.8    99.5    12.7    99.6    184.2   56.6
2400   156.5    99.9    12.8    100.0   246.1   75.6
3000   156.6    100.0   12.8    100.0   325.8   100.0
#
# === Sweet spot (lowest freq ≥ 95% of max throughput) ===
# stride  sweet spot: 800 MHz  (27% of max 3000 MHz)
# chase   sweet spot: 800 MHz  (27% of max 3000 MHz)
# compute sweet spot: — (scales linearly, always needs max freq)
```

### 关键指标

| 看什么 | 含义 |
|--------|------|
| stride_% 在低频时仍 ≥95% | 顺序内存访问是 mem-bound，降频几乎无损 |
| chase_% 在低频时仍 ≥95% | 随机内存访问是 mem-bound，降频几乎无损 |
| compute_% ≈ freq 比 | sanity check 通过，频率锁定正确 |
| stride/chase_% 随频率线性增长 | 该工作负载有 compute 成分，降频需谨慎 |

### 甜点判定

程序自动计算：**最低频率 ≥ 最大吞吐的 95%** = 甜点。

```
甜点在 27% 频率 → 功耗节省 = (1 - 0.27) × (V_low/V_high)² ≈ 50-70%
甜点在 80% 频率 → 功耗节省有限，工作负载对频率敏感
```

95% 阈值可通过修改代码中的 `THRESHOLD` 常量调整。

### compute_% 作为 sanity check

compute 测试的 `%` 值应该近似等于频率比：

```
freq=1500 MHz, max=3000 MHz
compute_% 预期 ≈ 1500/3000 × 100 = 50%

如果 compute_% = 95% → 频率没锁住！turbo 仍在跑
如果 compute_% = 30% → 该频率点 set_freq 可能失败了
```

---

## 噪音分析

### Chase 的纯度

chase 内循环编译后（arm64, `-O2`）：

```asm
.Lloop:
    ldr   x0, [x0]        ; p = p->next  (LOAD, ~300 cycles 等 DRAM)
    add   x1, x1, #1      ; i++          (1 cycle, 可并行)
    cmp   x1, x2          ; i < 100000   (1 cycle, 可并行)
    b.ne  .Lloop          ; branch       (1 cycle, 分支预测器命中)
```

乱序执行窗口分析：

```
每次循环迭代的时序：

  load 发出 ──────── 等 ~300 cycles ──────── load 完成
  │                                          │
  ├── add  (1 cycle) ──┐                     │
  ├── cmp  (1 cycle) ──┤ 已被 retire         │
  └── b.ne (1 cycle) ──┘                     │
                                              ↓
                                      发射下一条 load

  实际每迭代延迟 = max(load_latency, add+cmp+branch)
                = max(300 cycles, 3 cycles)
                = 300 cycles
```

| 成分 | 占比 | 说明 |
|------|------|------|
| DRAM load 延迟 | **~98%** | `p = p->next` 的串行依赖链 |
| 循环控制 (add/cmp/branch) | **~1-2%** | 被乱序执行隐藏在 load 延迟后面 |
| 外循环 `now()` 调用 | **<0.001%** | 每 10ms 才调一次（~15ns/次） |

**chase 是 practically pure latency bound。** 1-2% 的 compute 开销在所有频率点一致，不影响频率-吞吐的相对比较。

### TLB 噪音

```
4KB pages:
  512 MB / 4KB = 131,072 个页
  L1 dTLB 通常 48-64 条目

chase 随机访问 → 几乎每次 dereference 都 L1 TLB miss
  → 触发多级页表 walk（额外 20-40ns per access）
  → 测到的"延迟"= DRAM 延迟(~100ns) + page walk(~40ns) ≈ 140ns

  DRAM 延迟被高估 ~40%
```

这个噪音在各频率点**一致**（不影响相对比较），但如果你关心绝对延迟值，需要用 hugepage 消除（见下方"进阶优化"）。

### 系统级噪音

| 噪音源 | 影响 | 缓解 |
|--------|------|------|
| 中断（定时器、网卡等） | 抢占测试线程 | `isolcpus=` + `irqaffinity=` 隔离 CPU |
| SMT 兄弟负载 | 共享 L1/L2/执行单元 | offlining SMT sibling |
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
  → 工作负载是 memory-bound
  → DVFS 可以大胆降频

场景 B：stride 60% at 800MHz，compute 25% at 800MHz
  → 工作负载是 mixed（有计算也有访存）
  → DVFS 需要更保守

场景 C：stride 25% at 800MHz，compute 25% at 800MHz
  → 工作负载是 compute-bound
  → 降频 = 降性能，DVFS 无节能空间
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

**验证方法**：如果低频的 stride_% 异常高（接近 100%），可能数组没超出 L3——加大 `-m` 重测。

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
// 写 max 先于 min — 关键顺序！
// 如果先写 min=X（X > 当前 max），内核会拒绝：min > max
// 先写 max=X → range 变成 [old_min, X]（widened）
// 再写 min=X → range 变成 [X, X]（always valid since X ≤ X）
sysfs_write("scaling_max_freq", khz);
sysfs_write("scaling_min_freq", khz);
```

程序退出时恢复原始 min/max 范围（分别保存、分别恢复），不影响 governor 的后续行为。

### Turbo/Boost 处理

测试前自动关闭 turbo boost（防止频率飙到标称值之上），测试后恢复。

| 平台 | sysfs 路径 |
|------|------------|
| Intel (intel_pstate) | `/sys/devices/system/cpu/intel_pstate/no_turbo` |
| 其他 (acpi-cpufreq 等) | `/sys/devices/system/cpu/cpufreq/boost` |

---

## 高核数服务器调优指南

### SMT 的影响

SMT 兄弟线程共享同一物理核的 L1 cache、L2 cache、TLB 和执行单元。测试期间如果 SMT 兄弟在跑任务：

```
物理核内的资源争抢：
  ┌─────────────────────────────┐
  │  物理核                       │
  │  ┌───────────┐              │
  │  │ L1 cache  │ ← 两线程共享  │  测试线程的 cache line 可能被兄弟 evict
  │  └───────────┘              │
  │  ┌───────────┐              │
  │  │ L2 cache  │ ← 两线程共享  │  同上
  │  └───────────┘              │
  │  ┌────┐ ┌────┐              │
  │  │ T0 │ │ T1 │              │  执行单元共享（端口竞争）
  │  └────┘ └────┘              │
  └─────────────────────────────┘

chase 是 98% 时间等 DRAM → 执行单元几乎空闲 → 端口竞争影响小
但 L1/L2 eviction 会让部分访问命中 L2 而非 miss 到 DRAM → 降低 mem-bound 纯度
```

**建议**：对甜点分析而言，SMT 兄弟 idle 即可，不需要 offline。只有追求最低噪音测量时才 offline。

### NUMA 的影响

```
典型 96 核服务器拓扑（4 NUMA node）：

  ┌─────────────────┐  ┌─────────────────┐
  │  Node 0          │  │  Node 1          │
  │  24 物理核        │  │  24 物理核        │
  │  DDR channel 0   │  │  DDR channel 1   │
  │  ~100ns 本地延迟  │  │  ~100ns 本地延迟  │
  └────────┬─────────┘  └────────┬─────────┘
           │ interconnect        │
  ┌────────┴─────────┐  ┌────────┴─────────┐
  │  Node 2          │  │  Node 3          │
  │  24 物理核        │  │  24 物理核        │
  │  DDR channel 2   │  │  DDR channel 3   │
  │  ~100ns 本地延迟  │  │  ~100ns 本地延迟  │
  └─────────────────┘  └─────────────────┘

  跨 node 访问延迟：~150-250 ns
```

**本地 NUMA 测试**（默认）：`-c 0` + 内存由内核分配（通常本地）→ 测的是"最佳情况的 DRAM 甜点"。

**跨 NUMA 测试**（可选）：`numactl --membind=1 ./memfreq_bench -c 0` → 内存强制分配到 Node 1 而 CPU 在 Node 0 → 测的是"最差情况的 DRAM 甜点"（延迟更高，但瓶颈在 interconnect 带宽）。

### 大 L3 的陷阱

273 MB L3 意味着：
- stride 128 MB 数组可能部分或全部命中 L3（取决于 L3 slice 分配策略）
- chase 128 MB 链表：2M 个 64B 节点 × 随机访问 → L3 容量是节点数的 ~4.3M 个 cache line → 约一半的 chase 访问命中 L3

**解决方案**：数组大小 ≥ 2× L3 → 对于 273 MB L3 用 `-m 512`，更保险用 `-m 1024`。

**验证**：如果 chase 的 Mops 在高频率点异常高（比如 >20 Mops），说明大量 L3 hit → 加大数组。

---

## 进阶优化

### 用 Hugepage 消除 TLB 噪音

4KB page 下，512 MB 数组需要 131,072 个页表项。chase 的随机访问几乎每次都 TLB miss，引入 ~40ns 的 page walk 开销。

```bash
# 分配 2MB hugepage（512 MB 需要 256 个 hugepage）
echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 验证
cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# 输出: 256
```

程序目前用 `posix_memalign` 分配内存（4KB page）。要使用 hugepage，需要修改为 `mmap` + `MAP_HUGETLB`，或使用 `madvise` + `MADV_HUGEPAGE` 让内核做透明大页合并。

### 多核并行带宽饱和测试

当前工具只测单核。如果想知道"多核并行访存时带宽饱和在什么频率"：

```bash
# 用 GNU parallel 同时在多个核上跑 stride
for cpu in 0 1 2 3; do
    taskset -c $cpu ./memfreq_bench -c $cpu -m 512 -C &
done
wait
```

注意：cpufreq 对同 cluster 内的所有核是联动的，所以同 cluster 的多核测试有意义。跨 cluster 的核可能频率独立。

### 配合功耗测量

甜点只是性能角度。要量化实际节能：

```bash
# Intel: turbostat
sudo turbostat --Summary --show PkgWatt,CorWatt,MHz,Busy% -i 1 &
./memfreq_bench -m 512

# ARM: 读 RAPL 或 SoC-specific power sensor
watch -n 1 'cat /sys/class/hwmon/hwmon*/power1_input'
```

将功耗数据与频率数据对齐，就能画出 **性能-功耗 Pareto 曲线**，找到真正的能效最优点。

---

## 限制和注意事项

1. **需要 root**：写 cpufreq sysfs 需要 root 权限
2. **需要 cpufreq 驱动**：如果没有 `scaling_available_frequencies`，程序会报错退出
3. **只测单核**：每个频率点只测一个 core，不测多核并行访存的带宽饱和效应
4. **不测功耗**：程序不读 RAPL / 电能计——甜点是纯性能角度的判定。要测实际节能效果，配合 `turbostat` 或 RAPL
5. **stride 的 sum 有数据依赖**：`sum += arr[i]` 限制了流水线深度，但编译器可能展开为 partial sum，实际行为取决于 `-O2` 的优化策略。这不影响结论（仍然是 mem-bound 主导），但精确吞吐数值会因编译器而异
6. **chase 的 100K 内循环**：如果 DRAM 极快（如 HBM），100K 次 × 50ns = 5ms 就结束，外层 `now()` 的调用开销（~15ns/call）占比 ~0.3%，可忽略
7. **TLB 噪音**：4KB page 下 chase 的测量延迟包含 page walk 开销（~40ns/access），绝对延迟被高估 ~40%。不影响频率-吞吐的相对比较，需要绝对值请用 hugepage
8. **cpufreq 联动**：同 cluster 内的核可能频率联动（如 ARM 的 cpufreq policy 按 cluster 分组），设置 cpu0 的频率可能同时影响 cpu0-3

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

## 完整测试流程（以 96 核 ARM 服务器为例）

```bash
# ── 1. 拓扑侦察 ──
lscpu | grep -E "L[123]|Thread|Core|Socket|NUMA"
numactl -H
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies

# ── 2. 可选：隔离 SMT 兄弟 ──
for s in $(ls /sys/devices/system/cpu/cpu*/topology/thread_siblings_list \
           | xargs -I{} sh -c 'cut -d, -f2 {}' | sort -u); do
    echo 0 > /sys/devices/system/cpu/cpu$s/online
done

# ── 3. 基本测试（数组 > L3）──
sudo ./memfreq_bench -c 0 -m 512 -t 3 -n 5

# ── 4. 可视化 ──
sudo python3 memfreq_sweep.py -c 0 -m 512 --json

# ── 5. 可选：不同 stride 对比 ──
sudo ./memfreq_bench -c 0 -m 512 -s 1    # prefetcher-friendly
sudo ./memfreq_bench -c 0 -m 512 -s 64   # 极端 mem-bound

# ── 6. 可选：跨 NUMA 对比 ──
sudo numactl --membind=1 ./memfreq_bench -c 0 -m 512 -t 3 -n 5

# ── 7. 恢复 SMT ──
echo on > /sys/devices/system/cpu/smt/control
```

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `memfreq_bench.c` | C 基准测试（需 root + cpufreq） |
| `memfreq_sweep.py` | Python 运行器 + ASCII 可视化 + JSON 导出 |
| `docs/memfreq-bench.md` | 本文档 |
