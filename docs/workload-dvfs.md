# Workload-Aware DVFS：动机与目标

## 动机

### 当前 DVFS 的盲区

动态电压频率调节（DVFS）是现代处理器节能的核心技术。Linux 内核提供了多种 cpufreq governor（schedutil、ondemand、performance、powersave、conservative），其中动态调频 governor（schedutil、ondemand）都基于 CPU 利用率做频率决策：高利用率升频，低利用率降频。这个策略隐含一个假设——**CPU 忙的时候需要高性能，闲的时候可以省点电**。

但这个假设忽略了一个关键事实：**不同工作负载对频率的敏感度完全不同**。

### 计算密集型 vs 访存密集型

两种极端工作负载表现截然不同：

- **计算密集型**（矩阵乘法、编译器优化、视频编码）：CPU 满负荷执行算术逻辑运算，吞吐线性正比于频率。频率减半，吞吐减半；这类负载对频率高度敏感。
- **访存密集型**（数据库查询、图遍历、稀疏矩阵）：CPU 大部分时间在等内存。假设一次 DRAM 访问 x 纳秒，而一个 2.3 GHz CPU 周期只有 0.43 纳秒——这 x 纳秒里 CPU 无论跑什么频率都只能干等。频率翻倍，等待时间不变；频率减半，吞吐几乎不变。

现实中绝大多数生产负载介于两者之间。

### 当前问题

问题在于：当 governor 看到 CPU 利用率 100% 时，它会认为"这个任务需要最高性能"，把频率拉到最高。但若任务是访存密集型，从 1200 MHz 升到 2300 MHz 性能几乎没有变化，功耗却增加数倍。Governor 的利用率启发式把这些节能空间全部浪费了。

### 功耗的物理现实

CPU 动态功耗遵循 **P ∝ V² × f**（功耗与电压平方成正比，与频率成正比）。DVFS 通常同步调节 V 与 f：在理想 V ∝ f 模型下，P ∝ f³。

举例（V 与 f 同比下降）：

| f / f_max | V / V_max | P / P_max | 节电 |
|-----------|-----------|-----------|------|
| 100% | 100% | 100% | 0% |
| 70%  | 70%  | 34%  | 66% |
| 50%  | 50%  | 12.5% | 87.5% |
| 30%  | 30%  | 2.7% | 97.3% |

**实际硬件约束**：电压有下限 V_min，低于此值电路无法稳定工作。CPPC接口把这一约束暴露为 `lowest_nonlinear_freq`——频率低于此值时，电压不再随频率下降而下降，继续降频只能节省 f 不再节省 V²，因此功耗下降趋缓。内核 governor 即以此为下限做节能控制。表中 f=30% 时 97.3% 的节电是 V ∝ f 模型的物理上限，实际 SoC 能达到的典型节电在 70-85% 区间——但仍远高于"按 CPU 利用率调频"能拿到的节电。

如果一个任务的甜点在最大频率的 30%，那么实际可节省 60-80% 功耗，性能损失不到 5%。但前提是——**你得知道甜点在哪里**。

### 为什么现有 Governor 找不到甜点？

主流 Linux cpufreq governor 都是**利用率驱动**：以"CPU 忙不忙"为依据，不区分"在忙什么"。逐一分析：

- **ondemand / conservative**：按固定周期采样 CPU 忙闲比例，忙就升频、闲就降频。一个 100% 忙的访存密集型任务和一个 100% 忙的计算密集型任务在 governor 看来完全一样，都被拉到最大频率——前者的甜点机会被完全错过。conservative 是温和版（逐步升频），反应更慢，对短负载追不上、对长负载依然过度配置。

- **schedutil**：目前 Linux 默认 governor，和 ondemand 共享同一个根本盲区——把"CPU 忙"当作"需要最高频率"。信号换成 PELT `util_avg`、通过 `cpufreq_update_util()` 挂在调度器事件上，粒度从 10ms 压到 1ms 级——这是它取代 ondemand 的原因，但盲区依旧：mem-bound 任务的 `util_avg` 贴上限，照样被推到 max_freq。

- **performance / powersave**：前者永远最大频率、后者永远最小频率。前者浪费所有甜点、后者在计算密集型任务上欠频。

- **userspace**：完全由用户态自己定频率。机制上可以做 workload-aware——但没有任何内置机制告诉你"如何决定"，没有分类、没有 profile、没有反馈通道，等于一个空白接口。

**根本盲区**：所有 governor 都把"利用率"当作"工作负载类型"的代理。这两个概念在访存密集型任务上完全脱钩：100% 利用率的 chase 工作负载和 100% 利用率的矩阵乘法看起来一模一样，频率需求却不相同。要填这个洞，必须在 governor 之外引入 workload-aware 信号。

## 目标

Workload-Aware DVFS 的核心思想：**离线分析工作负载的频率特性，在线利用分析结果指导频率决策**。

### 目标一：定位 mem-bound 工作负载的甜点频率

Mem-bound 是 DVFS 节能空间最大的场景——频率翻倍吞吐几乎不变。要拿到这部分节能，首先得知道每类 mem-bound 负载的甜点在哪。

**机制**：离线基准测试。在目标 SoC 上对一组代表性 mem-bound 工作负载（顺序遍历、随机链表、指针追踪等）做频率扫描，记录每频率点的吞吐，找到满足"≥ 95% 峰值吞吐"的最低频率。同时给出甜点的 95% bootstrap 置信区间和平台期起点，量化决策的鲁棒性。

**验收**：
- 甜点定位误差：与全频率扫描 ground truth 相比 ≤ 50 MHz
- 95% bootstrap CI 宽度：在低噪声负载上 ≤ 100 MHz
- 覆盖范围：顺序访问、随机访问、混合访问三类

### 目标二：通过 SCP + AMU 在线识别 mem-bound 并应用甜点

有了离线甜点表，下一步是在运行时识别"当前负载是不是 mem-bound"，并把频率设到对应甜点。

**机制**：
- **AMU 采样**：在 SCP 小核固件周期性读 AMU 计数器（instructions retired、memory stalls 等）
- **分类**：基于 AMU 指标判断当前负载是否 mem-bound。v1 简化版可只做"mem-bound vs 非 mem-bound"二分类，不必细分三类
- **查表下决策**：判定 mem-bound 后查目标一产出的 profile 表得到对应甜点，通过 SCP 固件下发频率

**验收**：
- AMU 分类准确率：在标准 benchmark 上 ≥ 90%（二分类 mem-bound vs 非）
- 节电效果：mem-bound 工作负载下系统功耗降低 ≥ 30% 且性能损失 ≤ 5%

### 目标三：EPP 风格的轻量级 hint 集成

仿照 Linux 已有的 **Energy Performance Preference (EPP)** 机制，让用户掌控是性能优先还是能耗优先，然后交给硬件处理。

**机制**：
- **信号复用 + EPP 映射**：直接消费目标二产出的 AMU 分类结果，映射到 EPP 数值（0-255）——mem-bound 偏 power 一端（高 EPP）、compute-bound 偏 performance 一端（低 EPP）。通过 `cpu*/cpufreq/energy_performance_preference` sysfs 写入或在 CPPC 系统上写 CPPC 表的 EPP 字段
- **轻量化体现**：不接管频率决策，硬件 HWP/CPPC 仍按 EPP 偏好自主选频；不与 schedutil 路径冲突，不引入新 governor

**验收**：
- EPP hint 正确反映检测到的负载类型
- 不引入频率抖动

## 总结

Workload-Aware DVFS 的本质：**用离线分析换在线智能**，三个目标对应三个环节：

1. **离线（目标一）**：在目标 SoC 上扫描代表性 mem-bound 负载
2. **在线直接（目标二）**：通过 SCP + AMU 实时识别 mem-bound，
3. **在线间接（目标三）**：把 workload-aware 信号作为 EPP hint 暴露给现有 governor

**最终目标**：**在 benchmark 中性能基本不变，但功耗显著下降**。这是整个项目的成功判据——目标一/二/三都是手段，最终用 benchmark 上的"性能 - 功耗"曲线相对 schedutil baseline 的对比来衡量。
