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
> 单次 cache line = 64 B。DRAM 延迟约 100 ns,DRAM 峰值带宽约 70 GB/s(单 channel)。

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
| **chase** | 1 cache line | `-m` MB | miss | miss(串行) | ❌ 链式依赖 | **= 1** | **DRAM 延迟** | **最低**(CPU 闲置) |
| **random** | 1 cache line | `-m` MB | miss | 部分(~50%) | ❌ 索引乱序 | 多个 outstanding | **DRAM 随机带宽** | **中等**(MLP 用得上) |
| **compute** | 0(无内存) | 0 | n/a | n/a | n/a | n/a | **频率基线** | **无 sweet spot**(线性) |
| **flush** | 1 cache line | `-m` MB | prefetched (≈100%) | ≈0%(同 stride,顺序遍历超出 L3 容量) | ✅ 仍有效(stride=8) | 3-6(同 stride) | **clflush 指令开销 + stride** | 与 stride 接近(略高) |

> **核心对比**:stride 测"DRAM 在 prefetch 协同下能跑多快",chase 测"DRAM 一次访问要多久",random 测"DRAM 在乱序访问下能跑多快",compute 测"CPU 不靠内存能跑多快",flush 测"stride + clflush 指令开销"(**不是**裸 DRAM 带宽 —— 详见 §5)。

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

L2 streamer 看到 `i += 8`(步长 64 B = 1 行)识别为线性流,提前 8-32 个 cache line 预拉进 L1/L2。所以 CPU 视角的 load 指令多数是 **L1 hit(prefetched)**,不是真 L3 miss。

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

`pnode` 结构体大小 = 64 B(见 `_Static_assert(sizeof(struct pnode) == CL)`),链表用 Fisher-Yates 洗牌打乱顺序。

### 单次粒度 vs 工作集

- **粒度**:64 B(1 cache line) —— 每个节点 = 1 行。
- **工作集**:`-m` MB / 64 B = `-m × 16 K` 个节点。512 MB → 8 M 个节点。

### Cache 穿透分析(关键区别:MLP = 1)

```text
时间 ───────────────────────────────────────→
load p[0]      load p[X]      load p[Y]
   ↓              ↓              ↓
 等 100 ns      等 100 ns      等 100 ns
   ↓              ↓              ↓
 拿到 next      拿到 next      拿到 next
   ↓              ↓              ↓
 才能发下一个   才能发下一个   才能发下一个
```

**chase 与 stride 面对相同的 cache 容量约束**(L1 < L2 < L3 的容量失守点),但 MLP 完全不同:

- **MLP = 1**:每次 deref 必须等前一次拿到 `next` 指针才能发起下一次 load
- CPU 在 100 ns DRAM 等待里**完全闲置**,没有 outstanding load 在飞
- L1/L2 容量对 chase 来说不重要 —— 你根本没办法 exploit 它们(没有 parallel load 让它们"提前装填"的好处显形)

### Prefetcher 角色(无效)

L2 streamer 试图识别"步长为 X 的线性流" —— 但 chase 链是 **Fisher-Yates 洗牌**过的,`p->next` 跳到随机位置,streamer 看到的步长是"高熵随机",无法预取。

chase 是这个仓库里**唯一能让 prefetcher 完全失效**的 workload。

### MLP(再次强调)

`MLP = 1` 是 chase 最关键的属性,比"L3 miss"还关键。L3 miss + MLP > 1 是 DRAM 带宽测试,L3 miss + MLP = 1 才是纯 DRAM 延迟测试。

### 测什么 → 频率 sweet spot

- **测的是 DRAM 延迟**:每次 deref 都等一次完整的 DRAM round-trip,频率降低对 100 ns DRAM 延迟**几乎无影响**(CPU 时钟和 DDR 时钟完全解耦)。
- **频率 sweet spot 应该是最低**:从 max 频率往下,chase 曲线最先出现平台,甜点频率低于 stride。
- **典型场景**:实测 chase sweet spot 通常比 stride 还低 5-10% max freq —— 因为 stride 多少有点 prefetch 红利能吸收一点频率降,chase 是零红利。

---

## 3. Random(随机置换,需 `-R`)

### 代码

```c
size_t *idx = build_random_index(count);   // Fisher-Yates 洗牌,预先建好
for (size_t i = 0; i < count; i++)
    sum += arr[idx[i]];
```

### 单次粒度 vs 工作集

- **粒度**:64 B(1 cache line)
- **工作集**:`-m` MB 数组 + 一张 `-m` MB 索引表(`random_idx`,每 `uint64_t` 元素对应一个 `size_t` 索引)。**总工作集 = 2× `-m` MB**(如 512 MB 数组 + 512 MB 索引 = 1024 MB)。索引表是顺序访问,对 L3 hit 分析无影响。

### Cache 穿透分析(与 stride 的关键区别)

stride 的 L3 hit 率 ≈ 0(顺序访问一遍覆盖不了 8 M 行),但 random 的 L3 hit 率**显著非零**。

```text
对 array[i] 的一次随机访问:
  - P(在 L1d) ≈ L1/array ≈ 64 KB / 512 MB ≈ 0.012%   (忽略)
  - P(在 L2)  ≈ L2 /array ≈ 1 MB / 512 MB ≈ 0.2%     (忽略)
  - P(在 L3)  ≈ L3 /array ≈ 273 MB / 512 MB ≈ 53%    ← 关键!

→ random 的 demand load 约 50% 命中 L3,50% 打到 DRAM
```

但 L3 命中与否对 **DRAM 流量** 几乎没影响(L3 命中的数据原本就在 DRAM 喂出来,只是被复用),**对 latency 影响显著**(L3 hit ~30 ns vs DRAM ~100 ns)。

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
- **频率 sweet spot 介于 stride 和 chase 之间**:MLP > 1 让 CPU 能并行填充 DRAM 总线,所以"砍频率降吞吐"的程度比 chase 大,比 stride 也略大(因为 stride 有 prefetch 帮缓)。
- **典型场景**:random sweet spot 通常在 35-45% max freq,比 stride 高 5-15 个百分点。

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
1 M 迭代 = 4 ms @ 3 GHz
→ 频率翻 3 倍,迭代时间变 1/3,吞吐变 3 倍
```

**频率敏感度 = 100%**。这是"线性"行为的物理根源。

### 测什么 → 频率 sweet spot

- **测的是 CPU 计算单元的频率响应**(无 sweet spot)
- **用途是 sanity check**:如果 `compute_%` 不等于频率比,说明:
  - `compute_%` 异常高(> 频率比)→ 频率没锁住,turbo 漏过
  - `compute_%` 异常低(< 频率比)→ 该频点 `set_freq` 失败,实际频率低于 target
- **频率 sweet spot = 无**(compute 永远需要最大频率才能保持性能)

详见 [memfreq-bench.md](memfreq-bench.md#compute_-作为-sanity-check) 的 compute_% sanity check 段。

---

## 5. Stride + Flush(显式驱逐,需 `-f`)

### 代码

```c
for (size_t i = 0; i < count; i += stride) {
    sum += arr[i];
    flush_cacheline(&arr[i]);   // x86: clflush | ARM: dc cvac
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
  └─ clflush &arr[i]  → ~50 cycles (显式驱逐,serializing)
                       ──────────
                       ≈ 52 cycles / iteration  (vs stride 的 ~1 cycle)
```

也就是说,stride+flush 跑得**比 plain stride 慢约 50 倍** —— 不是因为 cache miss 变多,而是每个迭代多花了 50 cycle 的 clflush 指令成本。

> **常见误读**:把 flush 理解为"强制 L3 miss 后测裸 DRAM 带宽"。这是错的 —— flush 不改变 L3 miss rate(取决于 stride vs prefetch 距离),它改变的是**每条 load 指令的额外开销**。

### Prefetcher 角色(被 flush 削弱但仍然有效)

- L2 streamer 看到 `i += 8`(步长 1 line)继续识别为线性流
- 提前 8-32 行预取,所以 **stride 8 下每次 load 仍是 L1 hit (prefetched)**
- 跟 plain stride 一样有 100% L1 hit from prefetch

只有当 stride > prefetch 距离时(K > stride 不成立),flush 才会真正强制 L1 miss —— 但那种情况下本来 prefetch 就跟不上,stride 已经接近 miss-everywhere 状态。

### MLP(同 stride)

flush 不引入新的依赖,MLP 仍是 3-6(同 stride)。

### 测什么 → 频率 sweet spot

- **测的是"stride + clflush 指令开销"**,**不是"裸 DRAM 带宽"**
- 真实测的量是 **clflush 指令吞吐量**(在 ~50 cycle / line 的固定开销下,每秒能完成多少次 evict)
- **频率 sweet spot 应该与 stride 接近**:flush 主要瓶颈仍是 DRAM 喂数据(虽然加了 clflush 开销),CPU 频率降低的影响仍然是次要因素
- **典型场景**(估算,需以 `run_full_sweep.sh` 实际输出为准):flush sweet spot 与 stride 接近(都在 27-35% max freq 区间),但因 clflush 成本与频率部分耦合,flush 的 sweet spot 略高于 stride
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

> **与 [run_full_sweep.sh Suite G](run_full_sweep.md#suite-g-cache-hierarchy-sweep5-个测试) (Cache Hierarchy) 的关系**:Suite G 的 2×L2 测试已经跑了类似大小的数组,但 `-2` 直接提取 `stride_l3 sweet spot`,不需要手动从 cache hierarchy 数据中推断。

---

## 横向对比:两个轴统一分类

把 5 个内存型工作负载放在 **MLP × prefetcher 友好度** 这两个轴的 2D 平面上:

```text
                 prefetcher 友好               prefetcher 无效
              ┌──────────────────────┬──────────────────────┐
  MLP > 1     │  stride              │  random              │
  (带宽测试)   │  (顺序流,DRAM 带宽)  │  (乱序流,DRAM 带宽)  │
              │  flush(同象限,       │                      │
              │   多 ~50 cycle 开销) │                      │
              ├──────────────────────┼──────────────────────┤
  MLP = 1     │  (本仓库未覆盖)      │  chase               │
  (延迟测试)   │  假想:顺序链表       │  (纯 DRAM 延迟)      │
              └──────────────────────┴──────────────────────┘
```

**flush**:与 stride 同属 MLP>1 + prefetcher 友好象限,但每个迭代多 ~50 cycle clflush 成本,所以测的是 stride 带宽与 clflush 指令开销的混合(详见 §5)。

**compute**:不在 2D 平面里(零内存访问),作为频率基线对照。

---

## 频率 sweet spot 矩阵

把 5 个工作负载 + 核心数组合展开,典型的频率 sweet spot(占 max 频率百分比)如下。

> **免责声明**:以下数字是**估算值**而非实测。准确数字以 `run_full_sweep.sh` 实际输出为准(`output/full_sweep_*/data.csv` 里有每 workload 的 `stride_sweet_MHz` 等字段)。具体数值因 CPU 微架构(L1/L2 关联度、prefetch 策略)、DRAM 时序、内存控制器调度策略而异,典型 ±5% 偏差。

| Workload × 核心数 | 1 核 | 4 核 | 96 核 |
|---|---|---|---|
| **stride** | ~27% | ~35% | ~50% |
| **chase** | ~25% | ~25% | ~25% |
| **random** | ~38% | ~45% | ~60% |
| **compute** | **无 sweet spot**(线性) | **无** | **无** |
| **flush** | ~30% | ~38% | ~52% |

趋势解读:

- **stride** sweet spot 随核心数**升高** —— 核多 → MC 带宽压力大 → 不能再随便降频
- **chase** sweet spot 几乎不随核心数变化 —— 串行依赖让 MC 永远不饱和
- **random** 介于 stride 和 chase 之间
- **compute** 不适用

这个矩阵是 `run_full_sweep.sh` 跑全套 8 个 suite 的设计依据 —— 它要测的就是这套"workload × core count"组合下的 sweet spot 漂移,给出不同 DVFS 场景下的合理频率。

---

## 如何验证这些分析

本文的 cache 行为分析可以通过 `perf stat` 验证:

```bash
# stride: 预期 L1 hit 高(prefetch 有效),L3 miss 高(超出 L3)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
    ./memfreq_bench -c 0 -A -t 1 -n 1

# chase: 预期 L1 miss 高(随机跳转),L3 miss 高(串行依赖)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -C -R

# random: 预期 L1 miss 高,L3 miss ~50%(如本文分析)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -C

# flush: 预期 L1 hit 高(同 stride),但 IPC 显著降低(clflush 开销)
sudo perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,instructions,cycles \
    ./memfreq_bench -c 0 -A -t 1 -n 1 -f
```

验证要点:

| Workload | 预期 L1 miss rate | 预期 L3 miss rate | 预期 IPC |
|----------|:---:|:---:|:---:|
| stride | <5%(prefetch 有效) | ~100%(超出 L3) | 中等 |
| chase | ~100%(随机跳转) | ~100%(串行依赖) | 极低(大量 stall) |
| random | ~100%(随机索引) | ~50%(如 L3/array 比) | 中等(MLP 帮助) |
| flush | <5%(同 stride) | ~100% | 低(clflush 开销) |
| compute | 极低(寄存器操作) | 极低 | 高(纯计算) |

> **提示**:[run_full_sweep.sh Suite 7 (Cache Hierarchy)](run_full_sweep.md#suite-g-cache-hierarchy-sweep5-个测试) 通过跑不同数组大小(½×L2, 2×L2, ½×L3, 2×L3, 4×L3)直接观察吞吐在不同缓存层级的阶跃,可以验证本文"stride 在 L3 失守后打 DRAM"的分析。

---

## 附录:为什么这 5 个放在一起刚好

任何内存访问模式都可以用"MLP + prefetcher 友好度"两个轴分类。完整矩阵见上方[横向对比](#横向对比)。本仓库 5 个工作负载覆盖了 4 个象限中的 3.5 个(stride+flush 共享一个象限):

- **stride + flush**:MLP>1, prefetcher 友好 → DRAM 带宽
- **random**:MLP>1, prefetcher 无效 → DRAM 随机带宽
- **chase**:MLP=1, prefetcher 无效 → 纯 DRAM 延迟
- **未覆盖**:MLP=1, prefetcher 友好 → 顺序链表(prefetcher 可预测但串行依赖)

`compute` 不在 2D 平面里(它根本没内存访问),它在这个矩阵上方,作为"频率基线"。

## Insights

> **"MLP × prefetcher"二维分类法**是分析 memory-bound 性能最实用的工具。带宽测试要求 MLP > 1(否则测不到带宽上限),延迟测试要 MLP = 1(否则隐藏了延迟);prefetcher 友好表示"实际负载可以借助硬件加速",不友好表示"最坏情况"。这两个轴的 4 个象限对应 4 种典型性能瓶颈场景,而本仓库 5 个 workload 几乎覆盖全。

> **"工作集 > cache ⇒ 必然 evict"是 cache 行为的铁律**。理解了它,stride/chase/random 三者的 cache hit rate 差异就只是 LRU 替换的推论:
> - **stride 顺序** ⇒ L3 hit 率近 0(每次 wrap 后旧行已 evict)
> - **chase 串行** ⇒ L3 hit 率也近 0(被前面 1M 次访问就 evict 了)
> - **random 随机** ⇒ L3 hit 率 ≈ 50%(最近 4.4M 个行可能还在 L3,占 8M 工作的 53%)
>
> 这就是 LRU 数学,不是经验值。

> **compute 的"无 sweet spot"才是 sweet spot 概念的"零向量"**。前 4 个 workload 都是用"频率降 + 吞吐不变"来定义甜点;compute 反向 —— "频率降 + 吞吐同步降"作为 sanity check,告诉其他三个 workload 的甜点是**真实的**。如果 compute_% 不线性,说明整组实验的频率锁没生效,所有甜点数字都作废。这是为什么每个 memfreq_bench 输出都坚持打印 compute 列 —— 它是元数据验证,不是测量结果。
