# 五个工作负载的 cache 层级分析

## 目录

- [为什么需要这个分析](#为什么需要这个分析)
- [总览表](#总览表)
- [1. Stride](#1-stride顺序遍历)
- [2. Chase](#2-chase指针追踪)
- [3. Random](#3-random随机置换需-r)
- [4. Compute](#4-compute对照组默认)
- [5. Stride + Flush](#5-stride--flush显式驱逐需-f)
- [6. L3-Resident Sweep](#6-l3-resident-sweep-2)
- [横向对比](#横向对比两个轴统一分类)
- [频率 sweet spot 矩阵](#频率-sweet-spot-矩阵)
- [如何验证这些分析](#如何验证这些分析)
- [附录](#附录为什么这-5-个放在一起刚好)

---

> 本文用统一的"cache 穿透"视角拆解 `memfreq_bench` 的五个工作负载,解释**为什么它们在相同硬件、相同频率下会得出不同的 sweet spot**。
>
> 假定硬件:96 核 ARM server, L1d=64 KB/core, L2=1 MB/core, L3=273 MB(共享), `array=512 MB`。
> 单次 cache line = 64 B。DRAM 延迟约 100 ns,DRAM 峰值带宽约 25 GB/s(单 channel DDR4-3200)。

## 为什么需要这个分析

同一个 `-c 0 -A -t 3 -n 5` 跑出来的 5 个 workload(3 默认 + 2 可选)会**画出五条形状不同的曲线**。把它们的差异点抽出来,比单独看每个更容易理解:

| 区分维度 | 含义 | 直接影响 |
|---|---|---|
| 单次粒度 | 一次 load 读多少字节 | L1/L2 hit 判定 |
| 总工作集 | 一轮跑下来**总共**触发了多少不同 cache line | L3 是否装得下 |
| Prefetcher 角色 | 硬件预取器能否预测 | 延迟被隐藏的程度 |
| MLP (Memory-Level Parallelism) | CPU 一次能同时发多少个未完成的 load | 是测带宽还是测延迟 |
| 数据依赖 | 下一条指令是否等上一条 | 频率敏感度 |
| 测的是什么 | 带宽/延迟/频率基线 | sweet spot 在哪 |

下面每节都用这 6 个维度展开,最后用一张"频率 sweet spot 矩阵"收口。

---

## 总览表

| Workload | 单次粒度 | 工作集 | L1 hit | L3 hit | Prefetcher | MLP | 测什么 | 频率 sweet spot |
|---|---|---|---|---|---|---|---|---|
| **stride** | 1 cache line | `-m` MB | prefetched | 顺序 0% | ✅ 完美预测 | 多个 outstanding | **DRAM 带宽** | **很低**(DRAM 主导) |
| **chase** | 1 cache line | `-m` MB | miss | 部分(~55%,跨 pass) | ❌ 链式依赖 | **= 1** | **DRAM 延迟 + TLB page walk** | **取决于构造模式**(见 §2) |
| **random** | 1 cache line | `-m` MB | miss | 部分(~50%) | ❌ 索引乱序 | 多个 outstanding | **DRAM 随机带宽** | **中等**(MLP 用得上) |
| **compute** | 0(无内存) | 0 | n/a | n/a | n/a | n/a | **频率基线** | **无 sweet spot**(线性) |
| **flush** | 1 cache line | `-m` MB | prefetched (≈100%) | ≈0%(同 stride,顺序遍历超出 L3 容量) | ✅ 仍有效(stride=8) | 3-6(同 stride) | **clflush 指令开销 + stride** | 与 stride 接近(略高) |

> **核心对比**:stride 测"DRAM 在 prefetch 协同下能跑多快",chase 测"单次随机 deref 的端到端延迟(DRAM + TLB page walk)",random 测"DRAM 在乱序访问下能跑多快",compute 测"CPU 不靠内存能跑多快",flush 测"stride + clflush 指令开销"(**不是**裸 DRAM 带宽 —— 详见 §5)。

---

## 1. Stride(顺序遍历)

### 代码

```c
uint64_t sum = 0;
for (size_t i = 0; i < count; i += stride)
    sum += arr[i];
```

`-s 8`(默认)= 每次跳 8 个 `uint64` = 64 B = 1 个 cache line。

### 单次粒度 vs 工作集

- **粒度**:64 B(1 cache line)—— "最小有意义单位"就是 1 行,stride < 8 时多次访问落在同一行,prefetch 难量化;stride > 8 时预取器跟不上。
- **工作集**:`-m` MB 全部(512 MB)—— 8 M 个不同的 cache line。

### Cache 穿透分析

```text
时间 ──────────────────────────────────────────→
访问: [0] [1] [2] ... [1M-1] [1M] [1M+1] ... [8M-1]
       ↑                                       ↑
       line 0 进 L1,很快被踢到 L2,再踢到 L3,再踢到 DRAM

L1 (64 KB = 1024 行):  失守点 ≈ 访问 #1024
L2 (1 MB = 16K 行):    失守点 ≈ 访问 #16384
L3 (273 MB = 4.4M 行): 装得下数组? NO → 失守点 ≈ 访问 #4.4M
DRAM (512 MB):         持续喂数据
```

L3 失守后,后续所有访问的 demand load 都打 DRAM(顺序访问的"最近 4.4 M 行"覆盖不了 8 M 行的工作集)。

### Prefetcher 角色(关键)

L2 streamer 看到 `i += 8`(步长 64 B = 1 行)识别为线性流,提前 8-32 个 cache line 预拉进 L2(L1 有独立的 spatial prefetcher 负责从 L2 拉到 L1)。所以 CPU 视角的 load 指令多数是 **L1 hit(prefetched)**,不是真 L3 miss。

但 **streamer 的源头是 DRAM** —— 它不会无中生有。结果是:

- L1 hit rate 报表上看着高(预取有效)
- DRAM 流量满载(streamer 在 100% 利用率地喂)

### MLP

`sum += arr[i]` 有 sum 自依赖,但 stride 模式允许 OoO 引擎**同时发 N 个 load**(只要 N 个 `arr[i]` 互不相关)。实际上 L2 streamer 在背景里预拉,demand 流水线在前面消费,典型的"3-6 路 MLP"。

### 测什么 → 频率 sweet spot

- **测的是 DRAM 带宽**:DRAM 满载意味着 CPU 在带宽上"拉满"状态。
- **频率 sweet spot 应该很低**:DRAM 带宽跟 CPU 频率几乎解耦(DDR 时钟跟 core 时钟是不同 PLL),所以频率从 max 砍到 30% 附近,stride 吞吐都基本不变。
- **典型场景**:96 核 ARM 实测 sweet spot 在 27-30% max 频率(参见 `docs/memfreq-bench.md` 典型输出)。

---

## 2. Chase(指针追踪)

### 代码

```c
for (size_t i = 0; i < nnodes; i++)
    p = p->next;       // 严格串行依赖
```

`pnode` 结构体大小 = 64 B(见 `_Static_assert(sizeof(struct pnode) == CL)`)。默认使用**两级反预取构造**(page 级 Fisher-Yates 洗牌 + 页内 cache line 位反转,lmbench 技术);`--simple-chase` 回退到单层 Fisher-Yates。

### 单次粒度 vs 工作集

- **粒度**:64 B(1 cache line) —— 每个节点 = 1 行。
- **工作集**:`-m` MB / 64 B = `-m × 16 K` 个节点。512 MB → 8 M 个节点。

### Cache 穿透分析(关键区别:MLP = 1)

```text
时间 ───────────────────────────────────────→
load p[0]      load p[X]      load p[Y]
   ↓              ↓              ↓
 等 ~100 ns     等 ~100 ns     等 ~100 ns    (L3 miss 时;L3 hit 时 ~30 ns)
   ↓              ↓              ↓
 拿到 next      拿到 next      拿到 next
   ↓              ↓              ↓
 才能发下一个   才能发下一个   才能发下一个
```

**chase 与 stride 面对相同的 cache 容量约束**(L1 < L2 < L3),但 MLP 完全不同:

- **MLP = 1**:每次 deref 必须等前一次拿到 `next` 指针才能发起下一次 load
- CPU 在 DRAM 等待期间无法发射其他 load
- L1/L2 容量对 chase 来说不重要 —— 工作集(8M 节点)远超 L1(1K 行)/L2(16K 行),任何节点 revisit 时都已从 L1/L2 evict。但 **L3 hit 率非零**:chase 遍历随机置换链表,多 pass 下上一 pass 末尾的 4.4M 个节点仍在 L3 → 跨 pass L3 hit 率 ≈ 55%(与 random 相同的 LRU 数学)。单 pass 内 hit 率为 0(每个节点只访问一次)
- MLP=1 影响的是延迟隐藏能力,不是 cache 容量利用

### Prefetcher 角色(无效)

L2 streamer 试图识别"步长为 X 的线性流" —— 但 chase 链是 **Fisher-Yates 洗牌**过的,`p->next` 跳到随机位置,streamer 看到的步长是"高熵随机",无法预取。

chase 和 random 都能让 prefetcher 完全失效(随机访问模式,streamer 无法识别稳定步长)。

### MLP(再次强调)

`MLP = 1` 是 chase 最关键的属性,比"L3 hit 率"还关键。L3 miss + MLP > 1 是 DRAM 带宽测试,L3 miss + MLP = 1 是 DRAM 延迟测试(但实际 chase 的 L3 hit 率跨 pass 非零(~55%),且含 TLB page walk 开销,见上方 sweet spot 分析)。

### 测什么 → 频率 sweet spot

- **设计意图是测 DRAM 延迟**:每次 deref 等一次 DRAM round-trip,若 DRAM 延迟主导,频率降低应无影响(CPU 时钟和 DDR 时钟解耦)。
- **L3 hit 率非零(跨 pass)**:chase 遍历随机置换链表,与 random 工作负载共享相同的 LRU 统计。L3 容量 4.4M 行 < 8M 节点,但多 pass 遍历下上一 pass 最后 4.4M 个节点仍在 L3 → 跨 pass L3 hit 率 ≈ 4.4M/8M ≈ 55%。chase 测的不是纯 DRAM 延迟,而是 L3/DRAM 混合延迟。
- **TLB 行为取决于构造模式**:
  - `--simple-chase`(单层 Fisher-Yates):每次 deref 跳到随机页 → L1 dTLB miss 率接近 100% → page walk(~60-200 cycles)主导 → K≈91 cycles(实测,来自 [memory-hierarchy-chase.md](memory-hierarchy-chase.md))→ throughput ∝ f → **频率敏感**,sweet spot ~96%。
  - **默认两级构造**(page-shuffle + bit-rev):每 64 次 deref 才换页 → L1 dTLB miss 率仅 ~1.6%(1/64)→ page walk 贡献降至 ~1-3 cycles/deref → K 显著降低 → 频率敏感度下降 → sweet spot 可能低于 `--simple-chase` 模式。K≈91 数据是否来自默认模式尚不确定,需实测验证。
- **B≈0 的 OoO 重叠解释存疑**:MLP=1 下,下一条 deref 的地址依赖当前 load 的返回值,无法在 DRAM 等待期间发射下一条 load。B≈0 更可能表示 per-deref 时间被频率敏感的 active work(TLB/L1 查询)主导,而非 DRAM 延迟被"重叠隐藏"。
- **huge page 可恢复"纯延迟"特性**:若用 2MB huge page,L1 dTLB 覆盖达 128MB,K 降至 ~5 cycles,crossover 跌至 ~125 MHz,此时 chase 才真正频率不敏感、sweet spot 最低。但 `-H` flag 尚未实现(见该文第 10 章)。

---

## 3. Random(随机置换,需 `-R`)

### 代码

```c
size_t *idx = build_random_index(count);   // Fisher-Yates 洗牌,预先建好
for (size_t i = 0; i < count; i++)
    sum += arr[idx[i]];
```

### 单次粒度 vs 工作集

- **粒度**:每次 load 读 8 B(1 个 `uint64_t`),但 cache line = 64 B → **每个 cache line 被 8 次连续的 `idx[i]` 访问命中**(8 个 uint64 元素共享一个 cache line)。这增大了 L3 hit 窗口:8 次访问分散在 64M 元素的置换中,平均间隔 ~8M 次访问
- **工作集**:`-m` MB 数组 + 一张 `-m` MB 索引表(`random_idx`,每 `uint64_t` 元素对应一个 `size_t` 索引)。**总工作集 = 2× `-m` MB**(如 512 MB 数组 + 512 MB 索引 = 1024 MB)。索引表顺序访问且 > L3,会竞争 L3 容量并 evict 数组行,实际 L3 hit 率可能低于纯 `L3/array` 估算

### Cache 穿透分析(与 stride 的关键区别)

stride 的 L3 hit 率 ≈ 0(顺序访问一遍覆盖不了 8 M 行),但 random 的 L3 hit 率**显著非零**。

```text
对 array[i] 的一次随机访问:
  - P(在 L1d) ≈ L1/array ≈ 64 KB / 512 MB ≈ 0.012%   (忽略)
  - P(在 L2)  ≈ L2 /array ≈ 1 MB / 512 MB ≈ 0.2%     (忽略)
  - P(在 L3)  ≈ L3 /array ≈ 273 MB / 512 MB ≈ 53%    ← 关键!

→ random 的 demand load 约 50% 命中 L3,50% 打到 DRAM
```

L3 命中**不访问 DRAM**(数据从 L3 返回),直接减少约 53% 的 DRAM 流量;同时**对 latency 影响显著**(L3 hit ~30 ns vs DRAM ~100 ns)。

### Prefetcher 角色(无效,与 chase 类似)

`idx[]` 是 Fisher-Yates 洗过的,`idx[i+1]` 和 `idx[i]` 指向随机行。L2 streamer 看不到稳定步长,完全无法预取。

### MLP(关键区别:MLP > 1)

这是 random 和 chase 共享"L3 失守"却**测不同东西**的根本原因:

- `sum += arr[idx[i]]` 里 **`idx[i]` 在循环开始前就已确定**(索引表是预生成的)
- 每次循环可以独立地发起 `arr[idx[i]]` 的 load,不需要等上一次结果
- 加上 OoO 引擎,典型能发 8-16 个 outstanding load,MLP ≈ 8-16

```text
random:    [load idx[0]] [load idx[1]] [load idx[2]] ... [load idx[15]]
chase:     [load p[0]]  ─── 等 100ns ─── [load p[X]] ─── 等 100ns ─── [load p[Y]] ...
           ↑ MLP>1,DRAM 带宽满载            ↑ MLP=1,DRAM 带宽闲置
```

### 测什么 → 频率 sweet spot

- **测的是 DRAM 随机访问带宽**:和 stride 同类(都是带宽),但访问模式是随机的,排除了 prefetch 红利和顺序 locality。
- **频率 sweet spot 介于 stride 和 chase 之间**:53% 的 L3 hit 是片上访问(延迟取决于 uncore 频率,通常部分跟随 core 频率),加上 random 也有高 TLB miss 率,使 random 比 stride 更频率敏感;但 MLP > 1 能隐藏部分延迟,不如 chase(MLP=1 + K≈91 cycles)敏感。
- **典型场景**:random sweet spot 通常在 35-45% max freq,高于 stride(~27%),低于 chase(~96%)。

---

## 4. Compute(对照组,默认)

### 代码

```c
double x = 1.00001;
size_t iterations = 0;
double t0 = now();

while (now() - t0 < secs) {
    for (size_t i = 0; i < 1000000; i++)
        x = x * 1.0000001 + 0.0000001;
    iterations++;
}
return (double)iterations * 1000000.0 / elapsed;
```

### 单次粒度 vs 工作集

- **粒度**:**0 B**(完全不碰内存)
- **工作集**:**0**

### Cache 穿透分析(无)

compute 是这个仓库的"对照组" —— 它**根本不走 cache 路径**。`x` 留在寄存器(标量 -O2 不溢出),`1.0000001` 和 `0.0000001` 是立即数(在指令流里),`1.00001` 和 `t0` 都在栈帧(进入 L1d,但**只读一次**)。

L1d 命中率 100%(但只用了几个 cache line 反复 hit),L2/L3/DRAM **完全旁路**。

### Prefetcher 角色(无)

无可预取的数据。

### MLP(无)

`x = x * A + B` 是**严格标量串行依赖**(`latency-bound`),但瓶颈是**计算延迟,不是内存延迟**。1 GHz 下:

```text
每个迭代 = 1 个 FMA ≈ 4 cycles
1 M 迭代 = 4 M cycles = 4 ms @ 1 GHz
1 M 迭代 = 4 M cycles ≈ 1.33 ms @ 3 GHz
→ 频率翻 3 倍,迭代时间变 1/3,吞吐变 3 倍
```

**频率敏感度 = 100%**。这是"线性"行为的物理根源。

### 测什么 → 频率 sweet spot

- **测的是 CPU 计算单元的频率响应**(无 sweet spot)
- **用途是 sanity check**:如果 `compute_%` 不等于频率比,说明:
  - `compute_%` 异常高(> 频率比)→ 频率没锁住,turbo 漏过
  - `compute_%` 异常低(< 频率比)→ 该频点 `set_freq` 失败,实际频率低于 target
- **频率 sweet spot = 无**(compute 永远需要最大频率才能保持性能)

详见 [memfreq-bench.md](memfreq-bench.md#compute-作为频率锁定-sanity-check) 的 compute_% sanity check 段。

---

## 5. Stride + Flush(显式驱逐,需 `-f`)

### 代码

```c
for (size_t i = 0; i < count; i += stride) {
    sum += arr[i];
    flush_cacheline(&arr[i]);   // x86: clflush | ARM: dc civac
}
```

### 单次粒度 vs 工作集

- **粒度**:64 B(1 cache line)
- **工作集**:`-m` MB(同上)

### Cache 穿透分析(关键:flush 几乎不改变 hit rate)

`flush_cacheline` 把当前行从**所有 cache 层级**驱逐。但下一次 `load` 访问的是**下一行**(`N+stride`),不是刚刚被驱逐的那一行:

```text
t=0:    load line N     → streamer 在 t=-K 时已经预取进 L1,hit
t=1:    clflush line N  → 驱逐 line N(但 N 之后不再访问,驱逐"无副作用")
t=2:    load line N+8   → streamer 在更早就预取了这行,hit
t=3:    clflush line N+8
...
```

**在默认 stride=8、prefetch 距离 K=8-32 的典型场景下**,稳态 L1 hit rate **跟 plain stride 几乎一样(≈100% from prefetch)**。flush **不强制 L3 miss**。

flush 真正改的是**每次迭代的 cycle 预算**:

```text
每次迭代:
  ┌─ load arr[i]      → ~1 cycle  (L1 hit,prefetched)
  ├─ sum += arr[i]    → ~1 cycle  (register)
  └─ clflush &arr[i]  → ~60-120 cycles throughput (非 serializing:占用 cache port,不阻塞流水线,
                        但连续 clflush 的 issue 间隔受吞吐限制)
                        ──────────
                        ≈ 62-122 cycles / iteration  (vs stride 的 ~1-2 cycle)
```

也就是说,stride+flush 跑得**比 plain stride 慢约 30-60 倍** —— 不是因为 cache miss 变多,而是每个迭代受 clflush 吞吐限制。clflush 的精确吞吐因微架构而异(Intel Skylake ~83 cycles,AMD Zen3 ~120 cycles)。

> **常见误读**:把 flush 理解为"强制 L3 miss 后测裸 DRAM 带宽"。这是错的 —— flush 不改变 L3 miss rate(取决于 stride vs prefetch 距离),它改变的是**每条 load 指令的额外开销**(clflush 的 cache port 吞吐占用,~60-120 cycles)。

### Prefetcher 角色(被 flush 削弱但仍然有效)

- L2 streamer 看到 `i += 8`(步长 1 line)继续识别为线性流
- 提前 8-32 行预取,所以 **stride 8 下每次 load 仍是 L1 hit (prefetched)**
- 跟 plain stride 一样有 100% L1 hit from prefetch

只有当 stride > prefetch 距离时(K > stride 不成立),flush 才会真正强制 L1 miss —— 但那种情况下本来 prefetch 就跟不上,stride 已经接近 miss-everywhere 状态。

### MLP(同 stride)

flush 不引入新的依赖,MLP 仍是 3-6(同 stride)。

### 测什么 → 频率 sweet spot

- **测的是"stride + clflush 指令开销"**,**不是"裸 DRAM 带宽"**
- 真实测的量是 **clflush 指令吞吐量**(在 ~60-120 cycle / line 的吞吐限制下,每秒能完成多少次 evict)
- **频率 sweet spot 高于 stride**:clflush 开销(~60-120 cycles)是 CPU 频率敏感的指令吞吐,使 flush 比 stride 更频率敏感。sweet spot 显著高于 stride 的 ~27%,接近 compute 的行为
- **典型场景**(估算,需以 `run_full_sweep.sh` 实际输出为准):flush sweet spot 显著高于 stride,因 clflush 成本与频率耦合
- **真正的 micro-benchmark 用途**:用 flush 来**控制 cache 状态**做对比实验(比如比较"同样 cache-cold 起点下,两个算法谁快"),而不是测 DRAM 带宽

---

## 6. L3-Resident Sweep(`-2`)

`-2` 不是一个新 workload,而是**改变数组大小**来测不同 cache 层级下的 sweet spot。

### 原理

默认 sweep 的数组 = 2× L3(数据超出 L3,测的是 DRAM-bound sweet spot)。`-2` 额外跑一轮 sweep,数组 = 2× L2(数据完全驻留在 L3 中):

```
频率从高到低:
  ┌─ compute-bound（数据在任何缓存里）
  │
  ├─ L2 miss, L3 hit → L3-bandwidth 受限   ← -2 测这里
  │
  └─ L3 miss, DRAM   → DRAM-bandwidth 受限  ← 默认测这里
```

L2 miss 之后就已经是 mem-bound 了。但 L3 和 DRAM 的带宽/延迟特性不同,导致甜点频率也不同:

### 在 MLP × Prefetcher 框架中的位置

L3-resident sweep 仍然使用 stride workload,所以 MLP > 1 + prefetcher 友好 — 与 stride 在同一象限。区别在于**瓶颈层级不同**:

| | 默认 stride | `-2` stride_l3 |
|---|---|---|
| 数组大小 | 2× L3 | 2× L2 |
| 数据驻留 | DRAM | L3 |
| 瓶颈 | DRAM 带宽 | L3 带宽 |
| sweet spot | 较低(DRAM 带宽充裕) | 可能更高或更低(取决于 L3 带宽特性) |

### 解读

| 对比 | 含义 |
|------|------|
| `stride_l3 < stride` | L3 带宽充裕,L3-resident 工作负载可以更激进降频 |
| `stride_l3 ≈ stride` | L3 和 DRAM 甜点趋同,带宽瓶颈相似 |
| `stride_l3 > stride` | L3 带宽不如 DRAM（罕见），可能 L3 较小或争用严重 |

> **与 [run_full_sweep.sh Suite 7](run_full_sweep.md#suite-7-g-cache-hierarchy-sweep5-个测试) (Cache Hierarchy) 的关系**:Suite 7 的 2×L2 测试已经跑了类似大小的数组,但 `-2` 直接提取 `stride_l3 sweet spot`,不需要手动从 cache hierarchy 数据中推断。

---

## 横向对比:两个轴统一分类

把 5 个内存型工作负载放在 **MLP × prefetcher 友好度** 这两个轴的 2D 平面上:

```text
                 prefetcher 友好               prefetcher 无效
              ┌──────────────────────┬──────────────────────┐
  MLP > 1     │  stride              │  random              │
  (带宽测试)   │  (顺序流,DRAM 带宽)  │  (乱序流,DRAM 带宽)  │
              │  flush(同象限,       │                      │
              │   多 ~60-120 cycle   │                      │
              ├──────────────────────┼──────────────────────┤
  MLP = 1     │  (本仓库未覆盖)      │  chase               │
  (延迟测试)   │  假想:顺序链表       │  (DRAM 延迟 +         │
              │                      │   TLB page walk)     │
              └──────────────────────┴──────────────────────┘
```

**flush**:与 stride 同属 MLP>1 + prefetcher 友好象限,但每个迭代多 ~60-120 cycle clflush 吞吐成本(频率敏感),使 flush 的 sweet spot 显著高于 stride,接近 compute 的行为(详见 §5)。

**compute**:不在 2D 平面里(零内存访问),作为频率基线对照。

---

## 频率 sweet spot 矩阵

把 5 个工作负载 + 核心数组合展开,典型的频率 sweet spot(占 max 频率百分比)如下。

> **免责声明**:以下数字是**估算值**而非实测。准确数字以 `run_full_sweep.sh` 实际输出为准(`output/full_sweep_*/data.csv` 里有每 workload 的 `stride_sweet_MHz` 等字段)。具体数值因 CPU 微架构(L1/L2 关联度、prefetch 策略)、DRAM 时序、内存控制器调度策略而异,典型 ±5% 偏差。

| Workload × 核心数 | 1 核 | 4 核 | 96 核 |
|---|---|---|---|
| **stride** | ~27% | ~35% | ~50% |
| **chase** | ~96%(simple) / 待测(默认) | ~96%(simple) / 待测(默认) | ~96%(simple) / 待测(默认) |
| **random** | ~38% | ~45% | ~60% |
| **compute** | **无 sweet spot**(线性) | **无** | **无** |
| **flush** | ~70% | ~75% | ~80% |

趋势解读:

- **stride** sweet spot 随核心数**升高** —— 核多 → MC 带宽压力大 → 不能再随便降频
- **chase** sweet spot 几乎不随核心数变化 —— 串行依赖让 MC 永远不饱和;绝对值取决于构造模式(`--simple-chase`:~96%,因 TLB page walk 频率敏感;默认两级构造:TLB miss 率降至 ~1.6%,sweet spot 可能更低,需实测验证。详见 [memory-hierarchy-chase.md](memory-hierarchy-chase.md))
- **random** 介于 stride 和 chase 之间
- **compute** 不适用

这个矩阵是 `run_full_sweep.sh` 跑全套 8 个 suite 的设计依据 —— 它要测的就是这套"workload × core count"组合下的 sweet spot 漂移,给出不同 DVFS 场景下的合理频率。

---

## 如何验证这些分析

本文的 cache 行为分析可以通过 `perf stat` 验证。

**flag 约定**(来源: `memfreq_bench.c` 的 `main` 函数,搜索 `do_chase`/`do_random`/`do_flush` 声明):
- `do_chase = 1` 默认 ON(用 `-C` 关)
- `do_random = 0` 默认 OFF(用 `-R` 开)
- `do_flush = 0` 默认 OFF(用 `-f` 开)
- stride **无法被关闭**(它跟 compute 一起是默认必跑)

```bash
# stride 隔离: 关 chase / random / flush,只剩 stride + compute
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,instructions,cycles \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -C    # 预期 L1 hit 高(prefetch 有效),L3 miss 高(超出 L3)

# chase: 默认就开(不要加 -C!)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,instructions,cycles \
    ./memfreq_bench -c 0 -A -t 1 -n 1       # 跑 stride + chase + compute;perf 看到两者混合
                                              # (stride 隔离不了,需解读时按预期趋势判断)

# random: 关 chase + 开 random
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,instructions,cycles \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -C -R  # 跑 stride + random + compute;L3 miss ~50%

# flush: 关 chase + 开 flush(注意:flush 不能脱离 stride 单独跑,perf 看到的是 stride+flush 混合)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,instructions,cycles \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -C -f  # 预期 L1 hit 跟 stride 一样(≈100%),但 cycles / instructions 显著升高(clflush 开销)
```

> **局限**:stride 是必跑项,无法用现有 flag 组合完全关掉。因此 chase / random / flush 的 perf 数字都会跟 stride 混合。**解读 perf 数据时按"预期趋势"判断**(比如 chase 命令的 L1 miss rate 应该**远高于**纯 stride 命令),而不是期望绝对数字完全等于孤立值。

验证要点:

| Workload | 预期 L1 miss rate(相对趋势) | 预期 L3 miss rate(相对趋势) | 预期 IPC(相对趋势) |
|----------|:---:|:---:|:---:|
| stride | **基线**(prefetch 有效) | **基线**(超出 L3) | 基线 |
| chase | **远高于 stride**(随机跳转,无 prefetch) | **低于 random**(跨 pass ~55% L3 hit) | **远低于 stride**(大量 DRAM stall) |
| random | 高于 stride(无 prefetch) | 约 47%(1 - L3/array 比) | 中等(MLP 缓解) |
| flush | ≈ stride(同 prefetch 模式) | ≈ stride(同顺序遍历) | **低于 stride**(clflush 开销,估算 ~30-60x cycles/iter) |
| compute | 极低(寄存器操作) | 极低 | 高(纯计算) |

> **提示**:[run_full_sweep.sh Suite 7 (Cache Hierarchy)](run_full_sweep.md#suite-7-g-cache-hierarchy-sweep5-个测试) 通过跑不同数组大小(½×L2, 2×L2, ½×L3, 2×L3, 4×L3)直接观察吞吐在不同缓存层级的阶跃,可以验证本文"stride 在 L3 失守后打 DRAM"的分析。

---

## 附录:为什么这 5 个放在一起刚好

任何内存访问模式都可以用"MLP + prefetcher 友好度"两个轴分类。完整矩阵见上方[横向对比](#横向对比)。本仓库 5 个工作负载覆盖了 4 个象限中的 3.5 个(stride+flush 共享一个象限):

- **stride + flush**:MLP>1, prefetcher 友好 → DRAM 带宽
- **random**:MLP>1, prefetcher 无效 → DRAM 随机带宽
- **chase**:MLP=1, prefetcher 无效 → DRAM 延迟 + TLB page walk(4KB page 下频率敏感)
- **未覆盖**:MLP=1, prefetcher 友好 → 顺序链表(prefetcher 可预测但串行依赖)

`compute` 不在 2D 平面里(它根本没内存访问),它在这个矩阵上方,作为"频率基线"。

## Insights

> **"MLP × prefetcher"二维分类法**是分析 memory-bound 性能最实用的工具。带宽测试要求 MLP > 1(否则测不到带宽上限),延迟测试要 MLP = 1(否则隐藏了延迟);prefetcher 友好表示"实际负载可以借助硬件加速",不友好表示"最坏情况"。这两个轴的 4 个象限对应 4 种典型性能瓶颈场景,而本仓库 5 个 workload 几乎覆盖全。

> **"工作集 > cache ⇒ 必然 evict"是 cache 行为的铁律**。但 evict ≠ 0% hit rate —— 关键在于访问模式:
> - **stride 顺序** ⇒ L3 hit 率近 0(每次 wrap 后旧行已被顺序 evict,顺序访问不会回到最近的行)
> - **chase 随机置换** ⇒ 跨 pass L3 hit 率 ≈ 55%(与 random 相同的 LRU 数学:上一 pass 末尾 4.4M 行仍在 L3,占 8M 的 55%;单 pass 内为 0,因每个节点只访问一次)
> - **random 随机置换** ⇒ L3 hit 率 ≈ 50%(最近 4.4M 个行可能还在 L3,占 8M 工作的 53%;每行每 pass 访问 8 次,增大 hit 窗口)
>
> stride 和 chase/random 的区别在于**访问顺序**:顺序访问的 LRU evict 是确定性的(wrap 后旧行必然不在),随机访问的 LRU evict 是概率性的(MRU 行有概率留存)。

> **compute 的"无 sweet spot"才是 sweet spot 概念的"零向量"**。前 4 个 workload 都是用"频率降 + 吞吐不变"来定义甜点;compute 反向 —— "频率降 + 吞吐同步降"作为 sanity check,告诉其他三个 workload 的甜点是**真实的**。如果 compute_% 不线性,说明整组实验的频率锁没生效,所有甜点数字都作废。这是为什么每个 memfreq_bench 输出都坚持打印 compute 列 —— 它是元数据验证,不是测量结果。
