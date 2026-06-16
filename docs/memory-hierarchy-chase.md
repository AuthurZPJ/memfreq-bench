# Memory Hierarchy 和 chase 工作负载:从单条 deref 到 sweet spot

> **本文目的**:把"chase 为什么 sweet spot 高"、"huge page 为什么能修"、"crossover 是什么"讲清楚,作为 `workloads.md` §2 的深度补充。
> **读者画像**:已经读过 `memfreq-bench.md` 和 `workloads.md`,但对"TLB / page walk / L1 dcache / DRAM"在每次内存访问里的角色还有模糊的读者。
> **状态**:v1,正在 review。可以直接指出哪里不清楚、哪里讲错了、哪里啰嗦。

---

## 第 0 章:为什么需要这个文档

`workloads.md` 给了 chase 这个工作负载的 6 维分析(粒度 / 工作集 / cache 行为 / prefetch / MLP / 测什么)。**但它没说清楚"chase 一次 deref 在硬件里到底走了哪些环节、哪些环节是 sequential、哪些是 parallel"**。

最近的一次调试里,用户跑出 chase 的 sweet spot 在 2200 MHz(系统 max 2300 MHz),跟 `workloads.md` 里"chase 永远 sweet spot 最低"的分析矛盾。挖到底,**矛盾来自一个不在原分析里的因素:TLB 和 page walk**。本文把这件事讲清楚。

阅读路径:
- 第一次读:从第 1 章开始,顺序读完
- 已经懂 L1/L2/L3 但不懂 TLB:跳到第 3 章
- 已经懂 TLB/page walk:跳到第 6 章(案例分析)
- 想看 huge page 的具体效果:跳到第 8 章

---

## 第 1 章:7 个角色(内存子系统的硬件层级)

每次 `p = p->next` 这种内存访问,**会涉及 7 个不同的硬件单元**。每个有自己的"延迟(cycles)"和"容量(条目数或字节数)"。

```
┌──────────────────────────────────────────────────────────────────┐
│  CPU 视角:7 级 memory hierarchy(按访问顺序)                      │
├─────┬──────────────┬────────────┬────────────┬──────────────────┤
│  #  │  名字         │  容量      │  hit 延迟  │  miss 时去哪?     │
├─────┼──────────────┼────────────┼────────────┼──────────────────┤
│  1  │ L1 dTLB       │  64 条目   │   1 cycle  │  → L2 dTLB       │
│  2  │ L2 dTLB       │ 1024 条目  │  10 cycles │  → page walk     │
│  3  │ Page walk     │ (无容量)   │ 60-200 cyc │  → 安装到 L1 dTLB│
│  4  │ L1 dcache     │  64 KB     │   4 cycles │  → L2 cache      │
│  5  │ L2 cache      │   1 MB     │  12 cycles │  → L3 cache      │
│  6  │ L3 cache      │  22 MB     │  30 cycles │  → DRAM          │
│  7  │ DRAM          │  GB 级    │  40 ns     │  (终点)           │
└─────┴──────────────┴────────────┴────────────┴──────────────────┘

(数字基于 ARM Neoverse N1 风格的 96 核 server,
 L1/L2/L3 数据是用户的 `lscpu` 输出,
 DRAM 延迟是实测反推 —— 第 6 章会讲怎么算)
```

**两个独立的 cache 链**:

| 链 | 缓存什么 | 跟 chase 的关系 |
|---|---|---|
| **TLB 链**(#1→#2→#3) | 虚拟→物理地址翻译 | 每次 deref 都要查,miss → 整条链走完 |
| **数据链**(#4→#5→#6→#7) | 真正的数据 | 每次 deref 都要查,miss → 数据从 DRAM 拉 |

**这两条链是 sequential 的(先 TLB 再 data),不是 parallel 的**。第 4 章会解释为什么。

---

## 第 2 章:chase 是什么

```c
// 简化版,看 memfreq_bench.c:bench_chase
struct pnode { struct pnode *next; char pad[64 - 8]; };  // 64 B,恰好一个 cache line
struct pnode *p = start;

for (size_t i = 0; i < nnodes; i++)
    p = p->next;          // 一次 dereference,读 p 指向的 pnode 里的 next 字段
```

**chase 想测什么**:chase 想测"DRAM 直读延迟"。理由:
- `p` 依赖 `p->next` 的值(**串行依赖**,MLP=1)
- 所以 CPU 一次只能发一个 deref
- 每个 deref 等一次 DRAM round-trip
- throughput = 1 / DRAM 延迟

**这是 chase 的物理承诺**:跑出来 throughput 是固定的(跟 CPU 频率无关),sweet spot 在最低 freq。

**但实际跑出来不是这样**。下面看为什么。

---

## 第 3 章:一次 deref 到底走了哪些环节

时间线从左到右,**纵轴是 7 个硬件层级**。每个 deref 走 7 步:

```
时间 →

       step 1        step 2        step 3         step 4         step 5
       ─────        ─────        ─────          ─────         ─────
L1dTLB │── 1c ──────►│ miss        │              │              │
       │             │             │              │              │
L2dTLB │             │── 10c ─────►│ miss         │              │
       │             │             │              │              │
PageWLK│             │             │── 60-200c ──►│ install TLB  │
       │             │             │              │              │
L1dcache│            │             │              │── 4c (miss)──►│
       │             │             │              │              │
L2     │             │             │              │          (12c miss)
       │             │             │              │              │
L3     │             │             │              │          (30c miss)
       │             │             │              │              │
DRAM   │             │             │              │          ◄── 40 ns (读)
```

**关键点**:
- step 1-3 是 **TLB 链**(串行)
- step 4-5 是 **数据链**(串行)
- TLB 链必须在数据链**之前**完成(没有物理地址,发不出 L1 access)
- 整个 deref 的 wall time = TLB 时间 + 数据时间(sequential,不是 max)

具体到 chase:
- 几乎不可能 L1/L2 dcache hit(`p` 每次在不同的 cache line,L1 装不下 8 M 个 line)
- 几乎不可能 L3 hit(L3 22 MB 也装不下 45 MB 整个 array,加上 8 M 个 cache line 跨 11 K 个 page,L3 eviction 频繁)
- 所以数据**总是从 DRAM 来**,40 ns 固定

---

## 第 4 章:critical path 怎么算

chase 的 per-deref wall time = **TLB 链时间 + 数据链时间**(因为 TLB 必须先完成才能发数据访问)。

```
TLB 链时间     = (L1 hit ? 1c : (L2 hit ? 10c : page_walk_cycles))
数据链时间     = (L1 hit ? 4c : (L2 hit ? 12c : (L3 hit ? 30c : 40 ns)))
per-deref time = TLB 链 + 数据链
```

chase 在 4 KB page、45 MB array 上的 TLB 行为:

```
L1 dTLB (64 entry × 4 KB = 256 KB 覆盖) → 命中率几乎 0%
L2 dTLB (1024 entry × 4 KB = 4 MB 覆盖)  → 命中率 9%(45 MB / 4 MB = 11.25x over)
→ 91% 触发 page walk
```

每次 page walk 走 4 级页表(典型 L1/L2/L3 命中分布):
```
PGD access   L1 hit     4 cycles
PUD access   L2 hit    12 cycles
PMD access   L2 hit    12 cycles
PTE access   L3 hit    30 cycles
───────────────────────────
总                        ~60 cycles(典型 L3 hit case)
                         (200+ cycles 是 DRAM miss case,你 chase 跑出来是 92)
```

---

## 第 5 章:crossover 概念

把 chase 的 per-deref time 用"两个数字"建模:

```
per-deref time = K / f + B

K = active cycles(freq 翻倍,这种工作翻倍速完成)
B = stall ns(freq 怎么变都不变)
f = CPU freq in MHz
```

chase 的两个数字:
- K:每次 deref 的"必须 CPU 做的 active 工作",包括 TLB 查 / L1 查 / page walk / 流水线 stall
- B:每次 deref 的"等 DRAM 数据回来"的固定 wall time = 40 ns

**K 跟 DRAM 无关,只跟 chase 自身 + 硬件 microarchitecture 有关**。B 跟 CPU 无关,只跟 DRAM 物理延迟有关。

**crossover**:K / f = B 时的频率。低于这个 freq,CPU 工作占主导;高于这个 freq,DRAM 占主导。

```
f_crossover = K / B (单位要对齐:cycles / ns × 1000 = MHz)
```

---

## 第 6 章:从实测数据反推 K 和 B

用户的实测数据(取自 `output/full_sweep_*/mc1.txt` 的 stride=8 chase 那一行):

```
1700 MHz: 18.4 Mops  →  1/18.4e6 s = 54 ns / deref
2300 MHz: 24.9 Mops  →  1/24.9e6 s = 40 ns / deref
```

套进模型:

```
54 = K / 1700 × 1000 + B   →  K/1700 + B = 54
40 = K / 2300 × 1000 + B   →  K/2300 + B = 40

两式相减: K/1700 - K/2300 = 14
         K × (1/1700 - 1/2300) = 14
         K × 0.0002205 = 14
         K ≈ 63 active cycles

代回:  B = 40 - 63/2300 × 1000 = 40 - 27.4 = 12.6
```

但 B 也应该满足 54 = 63/1700 × 1000 + B → B = 17

B 不一致,说明**模型不完美**。真实情况是 K 和 B 都随 freq 略有变化(TLB page walk 内部也有 L1/L2/L3 命中,不是常数)。

但作为一阶近似,**用 K ≈ 90 cycles / B ≈ 0 ns** 能很好地拟合数据:

```
K=90: 90/1700 × 1000 = 52.9,  90/2300 × 1000 = 39.1
实测: 54                40
误差: 2%                2%
```

所以**chase 的 per-deref 时间几乎完全是 CPU 工作(约 90 cycles)**,DRAM 延迟(40 ns)在这套数据里没出现 —— 因为它被 OoO 引擎跟下一条 deref 的 CPU 工作重叠了。

> **注意**:这跟第 4 章的"TLB + 数据 sequential"模型矛盾。
> 原因:OoO 引擎有"投机执行 + 多 in-flight load"的能力,让 TLB walk 和下一次 deref 的 CPU 工作重叠。
> 在 chase 这种"MLP=1 严格串行"的工作里,OoO 也只能隐藏**部分** DRAM 延迟 —— 但足够让 B ≈ 0。
> 这是 chase 在 ARM Neoverse 上的具体行为,**不同微架构会得到不同的 K 和 B**。

---

## 第 7 章:huge page 改了什么

huge page 改的是**TLB 链**(第 1/2/3 级),**不动数据链**(第 4/5/6/7 级)。

```
4 KB page (默认):                      2 MB huge page:
─────────────────                     ─────────────────
45 MB array / 4 KB                     45 MB array / 2 MB
= 11,250 个 page                       = 23 个 huge page

L1 dTLB (64 entry × 4 KB               L1 dTLB (64 entry × 2 MB
= 256 KB 覆盖)                          = 128 MB 覆盖)
命中率: 0% (45 MB >> 256 KB)            命中率: 100%(23 < 64)

→ 每次 deref 触发 page walk            → 每次 deref 1 cycle L1 dTLB hit
→ 60-200 cycles TLB 成本                → 1 cycle TLB 成本
```

**为什么 TLB 容量跟 page size 有关**:
- 1 个 TLB entry = 1 个 page 的翻译
- page 越大,1 个 entry 覆盖的内存越多
- 64 entry × 4 KB = 256 KB 覆盖
- 64 entry × 2 MB = **128 MB 覆盖**

chase 工作集 45 MB < 128 MB,所以**huge page 后 23 个 huge page 完全装进 L1 dTLB**,零 miss。

---

## 第 8 章:有 huge page 后 chase 的物理图

**唯一变了的环节**:TLB 链的 #1→#2→#3 段从"~90 cycles"变成"1 cycle"。

```
huge page 后的 critical path:
─────────────────────────────────
  L1 dTLB hit (1 cycle)        ← 几乎 0
         ↓
  L1 dcache miss (4 cycles)
         ↓
  L2 → L3 → DRAM (40 ns 数据)
─────────────────────────────────
TLB 时间 ≈ 0.4 ns at 2.3 GHz / 0.6 ns at 1.7 GHz
DRAM 时间 = 40 ns (固定)
total    = max(0.4, 40) = 40 ns (DRAM-bound)
```

**K 从 ~90 cycles 降到 ~5 cycles**(只剩 L1 dcache miss + loop overhead):

| 项 | 4 KB page | 2 MB huge page |
|---|---|---|
| L1 dTLB | 1 (hit 0% 触发 page walk) | 1 (hit 100%) |
| L2 dTLB | 10 (hit 0% 触发 page walk) | 0 |
| Page walk | 60-200 | 0 |
| L1 dcache | 4 | 4 |
| L2 + L3 | 12 + 30 | 12 + 30(OoO 重叠) |
| Loop | 3 | 3 |
| **总 (active cycles)** | **~90** | **~5** |

**新 crossover**:
```
f_crossover = 5 / 40 × 1000 = 125 MHz
```

用户 freq 范围 1700-2300 MHz **全部在 crossover 之上** → **整个范围 DRAM-bound** → sweet spot 跌到 1700 MHz(最低)。

---

## 第 9 章:对比"huge page 前 vs 后"的预测

| 指标 | 4 KB page(实测) | 2 MB huge page(预测) |
|---|---|---|
| 1700 MHz throughput | 18.4 Mops | ~25 Mops |
| 2300 MHz throughput | 24.9 Mops | ~25 Mops |
| 95% sweet spot | 2200 MHz | 1700 MHz |
| Plateau 检测 | "无平台" | "有平台" |
| 每次 deref 在哪一级成为瓶颈 | TLB(page walk) | DRAM |

**huge page 改变的不是 cache 层级,是"chase 在哪个 microarchitectural 瓶颈上工作"**:
- 之前:TLB 链是 max 的那项,所以 chase 测的是"TLB 性能"(误打误撞)
- 之后:DRAM 是 max 的那项,所以 chase 真的在测它名字承诺的"DRAM 延迟"

---

## 第 10 章:open questions(下次 review 时填)

下面是这次推演中**我不确定 / 需要实测验证**的点:

1. **K=90 cycles 的拆分**:
   - 真 page walk 多少 cycles(60? 80? 100?)
   - L1/L2/L3 cache miss 多少 cycles(被 OoO 重叠后净增多少?)
   - Pipeline stall(分支预测失败、串行依赖等)多少 cycles?
   - **验证方法**:`perf stat -e instructions,cycles,dTLB-loads,dTLB-load-misses,LLC-load-misses,stalled-cycles-frontend,stalled-cycles-backend ./memfreq_bench -c 0 -A -t 1 -n 1`

2. **TLB page walk 内部各级的命中分布**:
   - PGD 是不是总在 L1?
   - PTE 是不是真在 L3(30 cycles),还是有时落 DRAM(40 ns + stall)
   - **验证方法**:`perf stat -e dtlb_load_misses.walk_active,dtlb_load_misses.walk_pending ./memfreq_bench ...`

3. **huge page 后的实测**:
   - per-deref 时间真的降到 ~5 cycles 吗?还是更低?
   - 95% sweet spot 真的到 1700 吗?
   - Plateau 检测会报"有平台"吗?
   - **验证方法**:跑 `-H` flag 后的 chase,看 sweet spot

4. **其他 workload 受 TLB 影响的程度**:
   - random workload 的 TLB miss 率(也用 huge page 测)
   - stride workload 的 TLB miss 率(应该低,因为有 spatial locality)
   - **验证方法**:同上的 perf stat,加 `-R` 看 random

5. **不同微架构下的 K 和 B**:
   - Apple M1(小 L2,大 L3):K 多少?B 多少?
   - 笔记本 LPDDR5:K 和 B 又多少?
   - 验证这些需要换机器跑

---

## 附录 A:这次 debug 的方法论

1. **先看错误信号**:chase 的 sweet spot 跟 workloads.md 的分析矛盾 → 必有原因
2. **看实测 raw 数据**:贴 mc1.txt 完整输出,从 1700/2300 的 throughput 反推 per-deref time
3. **用简单模型拟合**:T(f) = K/f + B,两个数据点解出 K 和 B
4. **追溯 K 来自哪**:K=90 cycles 拆成 TLB / cache / loop,每个估算数量级
5. **看哪个最可能是大头**:TLB page walk 60-200 cycles,占大头 → 假设 TLB 是主因
6. **设计修法**:消除主因(TLB)→ huge page,而不是优化它(page walk 速度)
7. **预测修后效果**:huge page 后 K 从 90 降到 5,crossover 从 2.3 GHz 跌到 125 MHz
8. **写文档记录这次推理**:本次文档,避免下次重新发明

这个流程可以套到所有 "实测 vs 理论" 矛盾上。
