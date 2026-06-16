# T3-LRU 仿真实验规格

## 目标

编写 T3-LRU 驱逐策略的单线程仿真实现，用于验证算法在不同工作负载下的命中率、层级分布、驱逐顺序等指标。

## 简化原则

仿真不关注多线程并发，做以下替换：

| 生产代码 | 仿真替换 |
|---|---|
| `FifoQueue` + `Segment` (46-slot 固定数组 + Michael-Scott 无锁队列) | 每层一个 `Vec<Handle>` 或双向链表 |
| 64-bit atomic `ctrl` 字 CAS 循环 | 普通字段读写 |
| `LOCKED` / `IN_TIER` 标志位 | 不需要，单线程直接操作 |
| TLS `LocalBuffer` + `BatchWriter` 批量写入 | 直接 append 到层级列表 |
| `SegmentPool` 池化 | 不需要 |
| `SimpleSpinLock` 自旋锁 | 不需要 |
| `CacheContext` CRTP 基类 | 不需要，Policy 直接持有所有状态 |

---

## 1. Handle 状态模型

生产代码将所有状态打包进一个 64-bit atomic `ctrl` 字。仿真中拆为显式字段：

### 字段定义

```rust
struct SimHandle {
    key: u64,
    in_cache: bool,
    timestamp: u32,       // 最后访问的 tick
    ref_count: u16,
    usage: u8,            // 0..=15，衰减后的热度计数器
    charge: usize,        // 条目占用空间
    tier: TierId,         // 当前所在层级
}
```

### 字段含义

| 字段 | 类型 | 含义 |
|---|---|---|
| `in_cache` | `bool` | 条目是否在缓存中。驱逐或擦除时置 false |
| `timestamp` | `u32` | 最后一次触发 DecayUsage 的时间（tick） |
| `ref_count` | `u16` | 引用计数。Insert 时初始化为 2（cache 持有 1 + 调用者持有 1）；Lookup 时 +1；Release 时 -1 |
| `usage` | `u8` | 衰减后的热度计数器，范围 0–15。Insert 时初始化为 2；Lookup 时 +1（上限 15）；每次操作前先做半衰期衰减 |
| `charge` | `usize` | 条目占用空间，用于容量计算 |
| `tier` | `TierId` | 当前所在层级 (Cold/Warm/Hot/None) |

### 初始状态（OnInsert 时）

```
in_cache = true
ref_count = 2        // cache 持有 + 调用者持有
usage = 2
tier = Warm          // 新条目一律进入 Warm 层
timestamp = current_tick
```

---

## 2. 半衰期衰减 (Halflife Decay)

### 核心公式

```
ratio = 0.5 ^ (duration / half_life_ticks)
new_usage = round(old_usage * ratio)
```

### DecayTable 构建

预计算 16 项查找表，每项 `{threshold: Duration, decay_ratio: f32}`：

1. 从 `ratio = 0.95` 到 `ratio = 0.05` 等间距生成 16 个 bucket
2. 对每个 ratio 反算 `threshold = ceil(-log2(ratio) * half_life_ticks)`
3. 去重：如果 threshold 与前一项相同，合并（更新前一项的 ratio）
4. `table[0]` 特殊设为 `{table[1].threshold / 2, 1.0}`（短间隔不衰减）
5. 超出所有 threshold 的返回 `MIN_RATIO = 0.01`
6. 剩余位置填入 sentinel `{DURATION_MAX, 0.01}`

### GetRatio 查找

```
for entry in table:
    if entry.threshold >= duration:
        return entry.decay_ratio
return 0.01  // MIN_RATIO
```

线性扫描 16 项，找到第一个 `threshold >= duration` 的项，返回其 `decay_ratio`。

### DecayUsage 逻辑

```
fn decay_usage(handle, now, decay_table):
    duration = now - handle.timestamp
    if duration <= decay_table[0].threshold:
        return  // 时间太短，不衰减
    ratio = get_ratio(decay_table, duration)
    handle.usage = round(handle.usage * ratio)
    handle.timestamp = now
```

---

## 3. 层级判定

```
fn determine_target_tier(usage) -> TierId:
    if usage == 0:  return None   // 应驱逐
    if usage > 10:  return Hot    // kHotThreshold = 10
    if usage > 3:   return Warm   // kWarmThreshold = 3
    return Cold
```

常量：`kHotThreshold=10, kWarmThreshold=3, kUsageMax=15`

---

## 4. 核心操作流程

### 4.1 OnInsert

```
fn insert(key, charge):
    handle = SimHandle {
        key, charge,
        in_cache: true,
        ref_count: 2,
        usage: 2,
        tier: Warm,
        timestamp: current_tick,
    }
    warm_tier.push_back(handle)
    total_usage += charge
    enforce_capacity()
```

### 4.2 OnLookup → IncrRef

```
fn lookup(key) -> bool:
    handle = find_handle(key)
    if handle == null or !handle.in_cache or handle.ref_count == 0:
        return false  // kFailed

    // 衰减
    decay_usage(handle, current_tick, decay_table)

    // 增加引用和热度
    handle.ref_count += 1
    handle.usage = min(handle.usage + 1, 15)

    // 判定是否需要层级迁移
    target = determine_target_tier(handle.usage)
    if target > handle.tier:          // 只升不降（滞后性）
        transfer_handle(handle)       // 升级到更高层级
    return true
```

### 4.3 OnRelease → DecrRef

```
fn release(key):
    handle = find_handle(key)
    decay_usage(handle, current_tick, decay_table)
    handle.ref_count -= 1

    if handle.ref_count == 0:
        handle.in_cache = false
        transfer_handle(handle)       // ref==0 → 驱逐
    else:
        target = determine_target_tier(handle.usage)
        if target > handle.tier:
            transfer_handle(handle)   // 升级
```

### 4.4 OnErase → DecrRef(ForceRemove)

```
fn erase(key):
    handle = find_handle(key)
    decay_usage(handle, current_tick, decay_table)
    handle.ref_count -= 1
    handle.in_cache = false           // 强制清除
    erase_handle(handle)              // 从层级中移除并驱逐
```

### 4.5 TransferHandle（核心调度）

```
fn transfer_handle(handle):
    current_tier = handle.tier
    remove handle from current_tier's list
    total_usage -= handle.charge      // 暂时减去，如果重新插入会加回

    target = determine_target_tier(handle.usage)

    if target == None or handle.ref_count == 0:
        evict(handle)                 // 调用 evict_callback，记录驱逐
        return handle.charge
    else:
        target_tier_list.push_back(handle)
        handle.tier = target
        return 0
```

### 4.6 EnforceCapacity（容量控制）

```
fn enforce_capacity():
    if total_usage <= capacity:
        return
    enforce_limits_from(Warm)  // 新条目插入 Warm，从 Warm 开始级联

fn enforce_limits_from(tier):
    // Hot 层：超出 soft_limit 时，将头部 segment 降级到 Warm
    if tier >= Hot:
        while hot_charge > hot_soft_limit:
            if hot_tier.is_empty(): break
            demote_head(Hot → Warm)

    // Warm 层：超出 soft_limit 时，将头部降级到 Cold
    if tier >= Warm:
        while warm_charge > warm_soft_limit:
            if warm_tier.is_empty(): break
            demote_head(Warm → Cold)

    // Cold 层：超出 soft_limit 时，驱逐头部条目
    if tier >= Cold:
        while cold_charge > cold_soft_limit:
            if cold_tier.is_empty(): break
            evict_head(Cold)
```

**Demote**: 将源层级头部的一个 handle 移到目标层级尾部
**Cold 驱逐**: 从 Cold 层头部逐个驱逐 handle

### 4.7 Prune（定期清理）

```
fn prune():
    for tier in [Hot, Warm, Cold]:
        for handle in tier:
            if !handle.in_cache:
                continue
            if handle.ref_count != 1:
                continue
            decay_usage(handle, current_tick, decay_table)
            target = determine_target_tier(handle.usage)
            // 只清理 Cold 层中低 usage 的条目
            if tier == Cold and target < Warm:
                handle.ref_count = 0
                handle.in_cache = false
                evict(handle)
```

---

## 5. 数据流全景

```
                    OnInsert
                       │
                       ▼
              ┌─────────────────┐
              │   Warm Tier     │◄─── 新条目入口 (usage=2, ref=2)
              └────────┬────────┘
                       │
         OnLookup ──► IncrRef (usage++, ref++)
                       │  usage > 10?
                       ▼
              ┌─────────────────┐
              │   Hot Tier      │◄─── TransferHandle 升级
              └─────────────────┘

              ┌─────────────────┐
              │   Cold Tier     │◄─── Demote (EnforceSoftLimit)
              └────────┬────────┘
                       │  超出 soft_limit
                       ▼
                   evict_callback ──► 从哈希表删除 + 释放内存

OnRelease ──► DecrRef (ref--)
    ref==0? ──► evict
    usage 下降? ──► 不处理（只在下次操作时通过 DecayUsage 自然降级）

Prune ──► 扫描 Cold 中 ref==1 且低 usage 的条目，主动驱逐
```

---

## 6. 关键算法特性

| 特性 | 说明 |
|---|---|
| **单向升级** | IncrRef/DecrRef 只在 `target > current_tier` 时触发 TransferHandle，不主动降级 |
| **被动降级** | 降级通过 EnforceSoftLimit 的 Demote 实现（从高层级头部移到低层级尾部） |
| **衰减驱动** | 每次操作都先 DecayUsage，长时间不访问的条目 usage 自然衰减到 0 |
| **滞后性** | usage 衰减是渐进的，不会突然从 Hot 掉到 Cold，需要多个半衰期 |
| **容量级联** | EnforceLimitsFrom 从触发层级向下级联：Hot→Warm→Cold→evict |
| **Prune 兜底** | 定期扫描清理 ref==1 的 Cold 条目，防止僵尸条目占用空间 |
| **FIFO 内层级** | 同一层级内是 FIFO 顺序（先入先出），Demote 从头部取、驱逐从头部取 |

---

## 7. 仿真数据结构

```rust
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum TierId { None = 0, Cold = 1, Warm = 2, Hot = 3 }

struct SimHandle {
    key: u64,
    in_cache: bool,
    timestamp: u32,
    ref_count: u16,
    usage: u8,           // 0..=15
    charge: usize,
    tier: TierId,
}

struct DecayTable {
    entries: Vec<(u32, f32)>,  // (threshold, decay_ratio), sorted by threshold asc
}

struct SimT3LruPolicy {
    capacity: usize,
    usage: usize,
    cold_soft_limit: usize,
    warm_soft_limit: usize,
    hot_soft_limit: usize,
    decay_table: DecayTable,
    half_life_ticks: u32,

    // 每层一个有序列表，头部是先插入的（FIFO）
    cold: Vec<SimHandle>,      // 或 VecDeque / LinkedList
    warm: Vec<SimHandle>,
    hot: Vec<SimHandle>,

    // 索引：key → (tier, index) 用于 O(1) 查找
    index: HashMap<u64, (TierId, usize)>,

    // 统计
    evicted: Vec<(u64, u32)>,  // (key, evicted_at_tick)
    current_tick: u32,
    lookup_count: u64,
    hit_count: u64,
}
```

---

## 8. 仿真 API

```rust
impl SimT3LruPolicy {
    fn new(capacity: usize,
           cold_ratio: f64, warm_ratio: f64, hot_ratio: f64,
           half_life_ticks: u32) -> Self;

    fn insert(&mut self, key: u64, charge: usize);
    fn lookup(&mut self, key: u64) -> bool;
    fn release(&mut self, key: u64);
    fn erase(&mut self, key: u64);
    fn prune(&mut self);
    fn advance_tick(&mut self, delta: u32);   // 推进仿真时钟

    // 统计
    fn hit_rate(&self) -> f64;
    fn tier_distribution(&self) -> (usize, usize, usize);  // (cold, warm, hot) count
    fn tier_charge(&self) -> (usize, usize, usize);        // (cold, warm, hot) charge
    fn total_charge(&self) -> usize;
    fn eviction_count(&self) -> usize;
}
```

---

## 9. 仿真实验场景

### 9.1 Zipf 工作负载

- Key 访问频率服从 Zipf 分布（s=1.0）
- 观察：热 key 是否升到 Hot、冷 key 是否被驱逐
- 指标：命中率 vs capacity、层级分布随时间变化

### 9.2 扫描抵抗

- 先插入 N 个热 key（高频访问，usage 升到 Hot）
- 然后顺序扫描大量冷 key
- 观察：原有热 key 是否保留在 Hot，扫描 key 是否被快速驱逐
- 指标：扫描后热 key 保留率

### 9.3 容量压力

- 插入量远超 capacity（如 10x）
- 观察：驱逐顺序是否符合 LRU 语义（先入先出 + usage 低者优先）
- 指标：驱逐顺序中 usage 分布

### 9.4 衰减速率对比

- 固定工作负载，调整 half_life_ticks（如 30s / 300s / 3000s）
- 观察：不同衰减速率下的层级分布变化、命中率变化
- 指标：命中率 vs half_life 曲线

### 9.5 Prune 效果

- 插入一批 key 后，长时间不访问（advance_tick 大量推进）
- 触发 Prune
- 观察：僵尸条目清理比例、Prune 前后 total_usage 变化
- 指标：Prune 清理数量、清理后 usage 占 capacity 比例

### 9.6 层级迁移追踪

- 单个 key 的完整生命周期追踪
- Insert(Warm) → 多次 Lookup(升 Hot) → 长时间不访问(衰减) → Prune/EnforceCapacity(降 Cold) → 驱逐
- 输出：每步操作后的 {tier, usage, ref_count, timestamp} 快照

---

## 10. 生产代码参考路径

| 组件 | 文件 |
|---|---|
| 类型定义、TierId、DetermineTargetTier | `include/t3_lru/common.hpp` |
| Handle ctrl 字布局、IncrRef/DecrRef/DecayUsage | `include/t3_lru/handle.hpp` |
| DecayTable 创建、GetRatio | `src/t3_lru_policy.cpp` |
| TierGroup 层级容器、Demote、EnforceSoftLimit | `include/t3_lru/tier_group.hpp` |
| TierManager OnInsert/TransferHandle/Erase/Prune | `include/t3_lru/tier_manager.hpp` |
| T3LruPolicy 编排层 OnInsert/OnLookup/OnRelease/OnErase | `include/t3_lru/t3_lru_policy.hpp` |
| EvictionPolicy 抽象接口 | `include/t3_lru/eviction_policy.hpp` |
