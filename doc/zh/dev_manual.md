# 开发手册 — 页内存池内部机制

[中文](dev_manual.md) | [English](../en/dev_manual.md) | [← 返回 README](../../README_zh.md)

> 版本: 1.0 | 日期: 2026-05-20
> 对应源码: include/pool.h (~340行) + src/pool.c (~740行)

---

## 目录

1. [架构概览](#1-架构概览)
2. [双区内存模型](#2-双区内存模型)
3. [元数据布局（字节级精确）](#3-元数据布局字节级精确)
4. [句柄编码系统](#4-句柄编码系统)
5. [使用者ID体系](#5-使用者id体系)
6. [核心算法逐函数详解](#6-核心算法逐函数详解)
7. [resize 移动策略决策树](#7-resize-移动策略决策树)
8. [defrag 算法步骤分解](#8-defrag-算法步骤分解)
9. [内部工具函数](#9-内部工具函数)
10. [已实施的性能优化](#10-已实施的性能优化)
11. [潜在问题与改进方向](#11-潜在问题与改进方向)
12. [查询 API](#12-查询-api)
13. [插件与 Hook 系统](#13-插件与-hook-系统)

## 附录

- [附录 A: 文件索引](#附录-文件索引)
- [附录 B: 项目漏洞分析](#附录-b-项目漏洞分析)

---

## 1. 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                      使用者 API 层                          │
│  init / alloc / lock / unlock / free / resize / defrag      │
├─────────────────────────────────────────────────────────────┤
│                      内部工具函数                            │
│  位图操作 (bitmap_test/set/clear)                           │
│  页操作   (pages_mark/unmark)                               │
│  数据搬移 (data_move)                                       │
│  查找逻辑 (pages_find_free, handle_find_free, handle_lookup)│
├──────────────┬──────────────────────────────────────────────┤
│  元数据区     │              数据区                          │
│  ┌─────────┐ │  ┌─────┬─────┬─────┬─────┬──────────┐      │
│  │ 位图     │ │  │Page0│Page1│Page2│ ... │Page N-1  │      │
│  ├─────────┤ │  └─────┴─────┴─────┴─────┴──────────┘      │
│  │页属主映射│ │                                             │
│  ├─────────┤ │   每页大小由用户定义（如 256 字节）            │
│  │句柄表    │ │   地址计算: data_base + page * page_size    │
│  └─────────┘ │                                             │
└──────────────┴──────────────────────────────────────────────┘
```

核心设计原则：
- **零系统调用** — 不使用 malloc/free/mmap
- **所有权隔离** — 每个句柄有 owner_id，跨使用者操作被拒绝
- **句柄不透明** — 句柄是 32 位编码值，不直接暴露内部索引
- **防 use-after-free** — 次代数机制确保释放后旧句柄立即失效

---

## 2. 双区内存模型

### 2.1 设计动机

在单片机裸机环境中，有两个约束：
1. 不能动态分配，所有内存必须预先规划
2. 用户数据不应混入管理信息（零开销读取）

解决方案：用户提供两块独立内存区域：

| 区域 | 用途 | 提供方 |
|------|------|--------|
| 元数据区 (metadata) | 位图、页属主映射、句柄表 | 用户 @ pool_init |
| 数据区 (data) | 用户实际可读写的内存 | 用户 @ pool_init |

用户读写数据区时，不需要任何间接访问或元数据查询，直接获得裸指针。

### 2.2 内存关系图

```
用户提供:
  metadata_base ──→ [位图][填充]...[页属主映射][填充]...[句柄表]
  data_base     ──→ [Page 0][Page 1]...[Page N-1]

地址转换公式:
  addr_of_page(n) = data_base + n * page_size
```

元数据区完全由 pool_init 初始化，用户不应直接访问元数据内部字段。

---

## 3. 元数据布局（字节级精确）

### 3.1 三大区域

```
metadata_base 偏移量:
┌───────────┬──────────────┬────────────┬──────────────────┬──────────────────┐
│ 初始标记   │    位图       │  对齐填充   │   页属主映射       │  对齐填充  │ 句柄表 │
│ 4 字节     │ ceil(N/8)字节 │ (0~1字节)  │  N × sizeof(u16)  │ (0~3字节)  │ H×14  │
└───────────┴──────────────┴────────────┴──────────────────┴──────────┴───────┘
```

### 3.2 位图 (Page Bitmap)

每个页占用 1 bit：
- `bitmap_set(page)` → `bitmap[page/8] |= (1 << (page%8))`
- `bitmap_test(page)` → `(bitmap[page/8] >> (page%8)) & 1`

位图用途：快速查找连续空闲页（首次适配扫描）。

### 3.3 页属主映射 (Page → Handle Index)

`uint16_t page_owner[N]` — 每个页记录它属于哪个句柄（句柄表索引）。

- `page_owner[p] == 0xFFFF` → 页空闲
- `page_owner[p] == handle_idx` → 页属于 `handle_table[handle_idx]`

此映射用于 O(1) 定位某页的句柄，在 defrag 和 resize 中至关重要。

### 3.4 句柄表 (Handle Table)

```c
typedef struct {
    uint16_t owner_id;      // 0xFFFF=空闲，否则=使用者ID
    uint32_t page_start;    // 起始页索引
    uint32_t page_count;    // 占用页数
    uint16_t lock_count;    // 递归锁定计数
    uint16_t generation;    // 次代数（防 use-after-free）
} pool_handle_entry_t;      // 每条目 14 字节
```

### 3.5 对齐策略

```
page_owner 对齐要求: sizeof(uint16_t) = 2 字节
handle_table 对齐要求: sizeof(uint32_t) = 4 字节

ALIGN_UP(addr, align) = ((addr) + (align) - 1) & ~((size_t)(align) - 1)

示例(pool_init 内):
  bitmap_offset  = 0
  meta += (page_count + 7) / 8    // 跳过位图
  page_owner = ALIGN_UP(meta, 2)  // 对齐到 2 字节
  meta = page_owner + page_count * 2
  handle_table = ALIGN_UP(meta, 4) // 对齐到 4 字节
```

POOL_META_SIZE 宏预留最大可能的对齐填充量，确保用户分配的空间不会小于所需。

### 3.6 具体示例

page_count=64, handle_count=16:

| 区域 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| 位图 | 0 | 8 字节 | (64+7)/8=8 |
| 填充 | 8 | 0 字节 | 8 已满足 2 对齐 |
| 页属主 | 8 | 128 字节 | 64×2 |
| 填充 | 136 | 0 字节 | 136 已满足 4 对齐 |
| 句柄表 | 136 | 224 字节 | 16×14 |
| **总计** | | **360 字节** | 实际占用 |
| POOL_META_SIZE | | 365 字节 | 预留上限（含 4 字节填充） |

---

## 4. 句柄编码系统

### 4.1 编码公式

```
handle = (generation << index_bits) | index

其中:
  index      = 句柄表索引 [0, handle_count-1]
  generation = 次代数 [1, 65535]，0 永远无效
  index_bits = ceil(log2(handle_count))，最小为 1
```

### 4.2 index_bits 计算

```c
bits = 0;
max_idx = handle_count - 1;
while (max_idx > 0) { bits++; max_idx >>= 1; }
if (bits == 0) bits = 1;   // handle_count == 1
```

示例：

| handle_count | max_idx | index_bits | 掩码 |
|-------------|---------|------------|------|
| 1 | 0 | 1 | 0x1 |
| 4 | 3 | 2 | 0x3 |
| 8 | 7 | 3 | 0x7 |
| 16 | 15 | 4 | 0xF |
| 64 | 63 | 6 | 0x3F |

### 4.3 解码与验证

```c
index = handle & handle_index_mask;
generation = handle >> index_bits;

// 验证步骤:
1. index >= handle_count          → 无效
2. handle_table[index].owner_id == POOL_HANDLE_FREE  → 无效
3. generation != handle_table[index].generation       → 无效
4. generation == 0                                    → 无效
```

`generation == 0` 永远无效，因为初始化时句柄表全零，首次分配时 generation 从 0 递增到 1。

### 4.4 use-after-free 防护

```
时间线:
  T1: 分配句柄 H1 = (gen=1 << bits) | idx=0  → 例如 H1 = 0x0010
  T2: 释放 H1 → owner_id=FREE, page_start=0, generation=1 (保留)
  T3: 分配句柄 H2 = (gen=2 << bits) | idx=0  → 例如 H2 = 0x0020
  T4: 尝试使用 H1 → handle_lookup 检查 gen:
      H1 的 gen=1, handle_table[0].gen=2 → 不匹配！返回 NULL
```

旧句柄因 generation 不匹配而冻结，防止悬垂指针。

### 4.5 generation 绕回问题（已知限制）

generation 是 uint16_t，范围 0~65535。65535 次后回绕到 0：
```
65535 → 0 → 1 → 2 → ...
```

当绕回到 0 时，新句柄的 gen=0 会被 handle_lookup 拒绝（gen==0 永远无效）。这是已知 bug，当前未修复。

修复方案：在 `pool_alloc_pages` 中检测 `generation == 0` 时重置为 1。

---

## 5. 使用者ID体系

### 5.1 ID 分配规则

| 范围 | 类型 | 分配方式 |
|------|------|----------|
| 0~127 | 系统 ID | 手动（pool_sys_pack） |
| 128~25565 | 用户 ID | 自动递增（pool_user_pack） |
| 0xFFFF | 空闲标记 | 不可分配 |

### 5.2 所有权验证

所有可写操作（lock/unlock/free/resize）在执行前验证：

```c
if (e->owner_id != owner->owner_id) {
    return POOL_XXX_ERR_OWNER;
}
```

这意味着使用者 A 无法释放/解锁使用者 B 的句柄。

### 5.3 多使用者共享池

同一 `pool_cfg_t` 可以关联多个 `pool_owner_t`：

```c
pool_owner_t a, b;
pool_user_pack(&a, &cfg);  // ID=128
pool_user_pack(&b, &cfg);  // ID=129
```

每个使用者独立管理自己的句柄，但共享同一数据空间。碎片整理（defrag）作用于全局。

---

## 6. 核心算法逐函数详解

### 6.1 pool_init

```
输入: cfg, metadata_base, metadata_size, data_base, page_size, page_count, handle_count

1. 参数校验
   - cfg/metadata_base/data_base NULL → NULL_PARAM
   - page_size/page_count/handle_count == 0 → 各自错误

2. 元数据大小校验
   - 计算 required = POOL_META_SIZE(page_count, handle_count)
   - metadata_size < required → META_SIZE

3. 填充基础字段
   - cfg->metadata_base, cfg->metadata_size
   - cfg->data_base
   - cfg->page_size, cfg->page_count, cfg->handle_count
   - cfg->next_user_id = POOL_USER_ID_MIN (128)

4. 内部指针计算（带对齐）
   - bitmap = metadata_base
   - 跳过位图: meta += (page_count + 7) / 8
   - page_owner = ALIGN_UP(meta, sizeof(uint16_t))
   - 跳过页属主: meta = page_owner + page_count * sizeof(uint16_t)
   - handle_table = ALIGN_UP(meta, sizeof(uint32_t))

5. 计算 handle_index_bits 和 handle_index_mask

6. 清零元数据
   - memset(bitmap, 0, bitmap_bytes)
   - page_owner[i] = POOL_PAGE_FREE (0xFFFF)
   - handle_table[i].owner_id = POOL_HANDLE_FREE (0xFFFF)
```

### 6.2 pool_alloc_pages

```
输入: owner, page_count, handle_out

1. 参数校验
   - owner/handle_out NULL → NULL
   - page_count == 0 → SIZE

2. 查找空闲句柄槽
   - 扫描 handle_table，找第一个 owner_id == POOL_HANDLE_FREE
   - 未找到 → NO_HANDLE

3. 查找连续空闲页（位图首次适配）
   - 扫描 0..page_count-1，统计连续空闲位
   - 达到 page_count 时记录起始页
   - 未找到 → NO_SPACE

4. 标记占用
   - pages_mark(start, page_count, h_idx)
   - 设置位图 + page_owner

5. 填充句柄条目
   - owner_id = owner->owner_id
   - page_start = start, page_count
   - lock_count = 0
   - generation++ (从0→1)
   - 编码 handle = (generation << bits) | h_idx
```

### 6.3 pool_lock

```
输入: owner, handle, addr_out

1. 参数校验
   - owner/addr_out NULL → NULL

2. 句柄解码验证 (handle_lookup)
   - err → INVALID/OWNER

3. 锁定计数检查
   - lock_count == 0xFFFF → OVERFLOW

4. 递增锁定计数
   - ++lock_count

5. 返回地址
   - *addr_out = data_base + page_start * page_size
```

**可重入性分析**

`pool_lock` 支持递归/可重入锁定。同一句柄可被多次锁定：

```c
pool_lock(&owner, handle, &addr1);  // lock_count: 0→1
pool_lock(&owner, handle, &addr2);  // lock_count: 1→2, addr1 == addr2
pool_unlock(&owner, handle);        // lock_count: 2→1
pool_unlock(&owner, handle);        // lock_count: 1→0
pool_free(&owner, handle);          // ✓ 允许（lock_count==0）
```

设计动机与约束：

| 特性 | 说明 |
|------|------|
| 同一使用者多次锁定 | ✅ 允许，lock_count 递增 |
| 不同使用者锁定同一句柄 | ✅ 允许（所有权在 alloc 时校验，不在 lock 时校验） |
| lock_count 上限 | 65535（uint16_t），溢出 → OVERFLOW |
| 释放前必须完全解锁 | lock_count 非零时 pool_free 返回 LOCKED |
| defrag 保护 | lock_count > 0 的句柄不会被移动 |
| 数据地址 | 每次 lock 返回相同地址（句柄位置不变）|

实际场景示例——任务嵌套使用同一缓冲区：

```c
void process_buffer(pool_owner_t *o, uint32_t h)
{
    void *buf;
    pool_lock(o, h, &buf);    // 外层锁定
    read_data(buf);
    
    transform_buffer(o, h);   // 内层也会锁定同一句柄
    
    pool_unlock(o, h);        // 外层解锁
}

void transform_buffer(pool_owner_t *o, uint32_t h)
{
    void *buf;
    pool_lock(o, h, &buf);    // 重入锁定 (lock_count++)
    process(buf);
    pool_unlock(o, h);        // 重入解锁 (lock_count--)
}
```

**与 defrag 的交互**：锁定期间句柄不会被 `pool_defrag` 移动。如果某句柄在 defrag 进行中被锁定，defrag 会跳过它 —— 这是安全设计而非缺陷。碎片整理操作者需注意：紧持锁定的句柄会阻碍整理效果。

### 6.4 pool_unlock

```
1. 参数校验 + 句柄验证 (同 lock)
2. lock_count == 0 → NOT_LOCKED
3. --lock_count
```

### 6.5 pool_free

```
1. 参数校验 + 句柄验证
2. lock_count > 0 → LOCKED
3. pages_unmark(page_start, page_count)
   - 清位图 + page_owner = POOL_PAGE_FREE
4. 清句柄条目
   - owner_id = POOL_HANDLE_FREE
   - page_start = 0, page_count = 0, lock_count = 0
   - generation 保留不变（防 use-after-free）
```

### 6.6 pool_free_all

```
输入: owner, forced  (false=非强制, true=强制)

1. 非强制模式: 扫描 handle_table
   - owner_id == oid && lock_count > 0 → LOCKED

2. 扫描 handle_table
   - owner_id == oid → pages_unmark + 清句柄条目
```

### 6.7 pool_resize

见[第 7 章](#7-resize-移动策略决策树)。

### 6.8 pool_defrag

见[第 8 章](#8-defrag-算法步骤分解)。

---

## 7. resize 移动策略决策树

resize 是本系统最复杂的函数。以下是扩大时的完整决策流程：

```
输入: handle, new_page_count (new > cur)

1. 原地扩展尝试
   old_end = page_start + cur_pages
   IF old_end + extra_needed <= page_count:
     扫描 [old_end, old_end+extra_needed) 是否全空闲
     IF 全空闲:
       pages_mark(old_end, extra_needed) → 成功，返回 OK

2. 收集扩展区域的后方句柄
   扫描 [old_end, old_end+extra_needed):
     IF 位图已标记:
       记录句柄索引、是否锁定、总大小

3. 决策: 移动谁？
   IF 超出总页数边界 && 无后方句柄:
     must_move_self = true
   ELSE IF 任何后方句柄被锁定:
     must_move_self = true          ← ★ 因为被锁定的不能动
   ELSE:
     IF 后方句柄总大小 > 自身当前大小:
       must_move_self = true        ← 移动数据量更小的
     ELSE:
       移动后方句柄                 ← 后方更小

4. 执行移动
   移动自身:
     IF self.lock_count > 0 → ERR_LOCKED   ← ★ 自身锁定不能移
     查找新位置 pages_find_free(new_size) → NO_SPACE?
     data_move(old → new)
     更新 page_start, page_count

   移动后方句柄 (循环清空策略):
     FOR iteration = 0..handle_count*2:
       扫描 [old_end, old_end+extra_needed) 找仍有句柄占用的页
       IF 已清空 → BREAK
       该句柄锁定 → NO_SPACE
       为该句柄找新位置
       data_move + 更新元数据

5. 原地扩展自身
   pages_mark(old_end, extra_needed)
   page_count = new
```

### 决策流程图

```
                  ┌─────────────┐
                  │ 需要扩大     │
                  └──────┬──────┘
                         │
                  ┌──────▼──────┐
                  │ 紧后有空闲？ │
                  └──┬──────┬──┘
                  Yes│      │No
            ┌────────▼─┐  ┌─▼──────────────┐
            │ 原地扩展  │  │ 收集后方句柄信息 │
            │ (成功)    │  └───────┬─────────┘
            └──────────┘          │
                   ┌──────────────┼──────────────┐
                   │              │              │
              ┌────▼────┐   ┌────▼────┐   ┌─────▼─────┐
              │后方锁定  │   │后方总大小│   │后方总大小  │
              │→移自己   │   │> 自身   │   │≤ 自身     │
              └────┬────┘   │→移自己  │   │→移后方    │
                   │        └────┬────┘   └─────┬─────┘
            ┌──────▼──────┐      │              │
            │ 自己锁定?    │      │       ┌──────▼──────┐
            ├──Yes──►ERR  │      │       │ 循环清空区域 │
            │  No   MOVED │      │       │ + 原地扩展   │
            └─────────────┘      │       └─────────────┘
                          ┌──────▼──────┐
                          │ 自己锁定?    │
                          ├──Yes──►ERR  │
                          │ No   MOVED  │
                          └─────────────┘
```

### 循环清空策略（关键修复）

移动后方句柄时，搬走一个句柄 → 该句柄的旧页变为空闲 → 下一个句柄的 `pages_find_free` 可能返回这些刚空闲的页 → 新位置恰好落在 [old_end, old_end+extra_needed) 中 → 扩展区域又被占！

**修复**：用 `for(iteration = 0..handle_count*2)` 循环反复扫描扩展区域，直到区域完全清空（无句柄占据）。

---

## 8. defrag 算法步骤分解

### 8.1 算法伪代码

```
cursor = 0
WHILE cursor < page_count:
  // 找到空闲间隙
  跳过已用页 (bitmap = 1)

  gap_start = cursor
  WHILE cursor < page_count AND bitmap[cursor] = 0:
    cursor++
  gap_end = cursor
  gap_size = gap_end - gap_start

  // 填充间隙
  WHILE gap_size > 0:
    找到候选句柄:
      扫描 handle_table，条件:
        - page_start > gap_start
        - lock_count == 0
        - page_count <= gap_size
      选择 page_start 最小的一个

    IF 未找到 → BREAK (无法再填充)

    // 搬移数据
    data_move(旧位置 → gap_start, page_count * page_size)

    // 更新元数据
    pages_unmark(旧位置)
    pages_mark(gap_start, page_count, h_idx)
    handle_table[h_idx].page_start = gap_start

    // 缩小间隙
    gap_start += page_count
    gap_size  -= page_count

  cursor = gap_end
```

### 8.2 逐步示例

初始状态: 5 个句柄，每块 4 页。
```
页: 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
    ───────  ───────  ────────  ────────  ─────────
       ha       hb       hc        hd        he
```

释放 hb 和 hd：
```
页: 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
    ───────           ────────              ─────────
       ha              hc                     he
          └─间隙1─┘           └──间隙2──┘
```

defrag 扫描：

**Step 1**: gap_start=4, gap_end=8, gap_size=4
- 候选: hc.page_start=8 (最小且 > 4), size=4 ≤ 4 ✓
- 搬移 hc 到页 4
- 结果: ha(0..3), hc(4..7)

**Step 2**: gap_start=8, gap_end=12, gap_size=4
- 候选: he.page_start=16 (最小且 > 8), size=4 ≤ 4 ✓
- 搬移 he 到页 8
- 结果: ha(0..3), hc(4..7), he(8..11)

最终：
```
页: 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 ...
    ───────  ───────  ────────
       ha       hc       he
                           └── 全空闲 ────────────────
```

### 8.3 算法特性

- **简单可靠** — 从左到右单次扫描，每次填充当前间隙
- **非最优** — 未考虑搬移后产生的新间隙是否能被后续句柄利用
- **跳过锁定句柄** — 锁定中的句柄不会被移动
- **时间复杂度** — O(page_count × handle_count)，对 MCU 足够

### 8.4 owner 参数语义

`pool_defrag` 接受一个 `pool_owner_t *owner` 参数，但这个参数**仅用于获取 `cfg` 指针**。碎片整理作用于整个池中的所有使用者，不限于 owner 对应的使用者。

```c
// 以下两种调用等价（假设 a 和 b 都指向同一个 cfg）：
pool_defrag(&owner_a);
pool_defrag(&owner_b);
```

这意味着：
- 可以用**任意有效的 owner** 调用 defrag，不影响操作范围
- 不需要"root"或"管理员"所有者来执行整理
- `lock_count > 0` 的句柄（任何所有者）都会被跳过
- 移动数据后只更新元数据，不通知句柄持有者 —— 句柄持有者通过 `pool_lock` 重新获取地址即可感知新位置

---

## 9. 内部工具函数

### 9.1 位图操作

```c
bitmap_test(bitmap, page)  → (bitmap[page/8] >> (page%8)) & 1
bitmap_set(bitmap, page)   → bitmap[page/8] |= 1 << (page%8)
bitmap_clear(bitmap, page) → bitmap[page/8] &= ~(1 << (page%8))
```

**位序与大小端（Endianness）说明**

位图操作使用 `(page % 8)` 作为位偏移（0 = LSB, 7 = MSB），即**字节内 bit 0 对应 page 0，bit 7 对应 page 7**。此约定与 CPU 大小端无关——位操作（`<<`、`>>`、`&`、`|`、`~`）在 C 语言层面是**值语义**，不受内存字节序影响。

```
bitmap 字节布局示例（page_count=16, bitmap 占 2 字节）:
  byte 0: [p7 p6 p5 p4 p3 p2 p1 p0]  ← bit0=page0, bit7=page7
  byte 1: [p15 p14 p13 p12 p11 p10 p9 p8]  ← bit0=page8, bit7=page15
```

不过，当 bitmap 被整体视为 `uint16_t` 或 `uint32_t` 读取时（如调试器中以字为单位查看），字节内的位与页的对应关系在不同大小端平台上可能有不同视图。**代码中始终以字节为单位通过 `bitmap[page >> 3]` 和位运算访问，因此对大小端透明。** 仅在需要将 bitmap 整体导出或跨平台传输时才需关注字节序。

### 9.2 页标记

```c
pages_mark(cfg, start, count, handle_idx)
  // 循环: bitmap_set + page_owner = handle_idx

pages_unmark(cfg, start, count)
  // 循环: bitmap_clear + page_owner = POOL_PAGE_FREE
```

### 9.3 数据搬移 (data_move)

```c
data_move(cfg, src_page, dst_page, page_count)
  // 计算字节数 = page_count * page_size
  // 处理重叠: src > dst → 正向复制, src < dst → 反向复制
  // 纯 C99 逐字节循环，无 memmove 依赖
```

### 9.4 查找函数

```c
pages_find_free(cfg, need_count) → 起始页索引 或 -1
  // 首次适配扫描位图，找到 need_count 个连续空闲位

handle_find_free(cfg) → 句柄索引 或 -1
  // 扫描 handle_table，找第一个 owner_id == POOL_HANDLE_FREE

handle_lookup(cfg, handle) → pool_handle_entry_t* 或 NULL
  // 解码 handle → 验证 generation + owner_id

defrag_find_candidate(cfg, min_page, max_pages) → 句柄索引 或 -1
  // 扫描 handle_table，找 page_start>min_page, 未锁定, 能放入
  // 选择 page_start 最小的
```

---

## 10. 已实施的性能优化

### 10.1 位图字节级跳过（pages_find_free）

当前扫描空闲页时，逐位测试。如果位图的某个整字节为 `0xFF`（8 页全部已用），直接跳过 8 页：

```c
if ((i & 7u) == 0 && cfg->bitmap[i / 8u] == 0xFFu) {
    i += 7u;        // +7，循环末尾 i++ = +8
    run_len = 0;
    continue;
}
```

效果：64 页池从最多 64 次循环降至最多 8 次（位图只有 8 字节）。

### 10.2 位图批量操作（pages_mark / pages_unmark）

标记/取消标记大量页时，整字节直接写 `0xFF` 或 `0x00`，page_owner 展开为连续 8 次赋值（避免函数调用开销）。仅起始和末尾不对齐的字节才逐位操作。

### 10.3 数据搬移按字复制（data_move）

检测 src/dst 地址是否 4 字节对齐，若是则用 `uint32_t*` 按字复制，速度提升约 4 倍。未对齐时回退到逐字节复制。正确处理重叠区域。

### 10.4 句柄查找提示（handle_find_free）

在 `pool_cfg_t` 中增加 `next_handle_hint` 字段。每次分配从句柄表 hint 位置开始搜索，而非从 0 开始。释放时若槽位索引小于 hint 则更新 hint 指向该位置，使释放的槽优先被复用。

效果：摊销 O(1)，缓解线性扫描的累积开销。

---

## 11. 潜在问题与改进方向

### 11.1 generation 绕回（已修复）

问题: generation 为 uint16_t，分配 65536 次后绕回到 0，新句柄立即被拒绝。
修复: `pool_alloc_pages` 中 `generation++` 后检测 `==0` 则重置为 1。
状态: ✅ 已修复。

### 11.2 未对齐数据搬运（已优化）

逐字节复制在 `data_move` 中已替换为 4 字节对齐时按字复制（4× 加速）。未对齐地址回退到逐字节。详见第 10 章。

### 11.3 线程安全

当前无任何锁保护，仅适合单线程 MCU。

MCU 多线程场景的两条路径：
- **线程间不共享数据** → 各自使用独立的内存池（不同的 meta/data 区域）
- **线程间需共享数据** → 预留方案（未来），可考虑 per-pool 自旋锁或临界区

当前不做实现。两池隔离方案是推荐做法。

### 11.4 碎片整理算法扩展

当前 defrag 是单次从左到右扫描。对绝大多数 MCU 场景足够。

未来如有需要（极端碎片化、追求最优压缩），可用宏切换算法：

```c
#define POOL_DEFRAG_ALGO  0   // 0=当前算法, 1=最优压缩, 2=快速整理
```

新算法实现后编译时切换，不改 API。

### 11.5 测试覆盖缺口

✅ 全部已补。新增 8 个测试：

| 测试 | 覆盖路径 |
|------|----------|
| `resize_move_following_nospace` | 移动后方句柄但池满 → NO_SPACE |
| `resize_same_size` | new == cur → OK, 无变化 |
| `alloc_bytes_zero_size` | 0 字节 → ERR_SIZE |
| `defrag_full_pool` | 池满 + 全锁定 → OK（无操作） |
| `free_all_no_handles` | 所有者无句柄 → OK |
| `multi_owner_cross_reuse` | A 释放后 B 分配同区域 + 数据改写 |
| `generation_wrap` | 手动设 gen=65535 → alloc 后 gen=1 |
| `defrag_mixed_blocks` | 不同大小块 (8/2/4/6页) + 级联移动 |

当前测试总数：**42**，全部通过。详见 `test/test_pool.c`。

### 11.6 pool_lock 可重入性风险

lock_count 为 uint16_t（上限 65535）。在单线程 MCU 环境下，在同一调用链中超出 65535 次嵌套锁定几乎不可能。但若通过中断服务例程（ISR）意外重入锁定循环，可能导致 lock_count 溢出并返回 `POOL_LOCK_ERR_OVERFLOW`。此风险在单线程裸机环境下为理论风险。

### 11.7 pool_defrag 全局作用域

`pool_defrag` 接收一个 `pool_owner_t *` 参数，但作用域为整个池（所有使用者）。此设计简化了 API（不需要额外的"超级使用者"概念），但多使用者场景中，任何使用者均可触发全局碎片整理。这是设计意图，非缺陷。详见 [8.4 节](#84-owner-参数语义)。

---

## 12. 查询 API

查询 API 是只读操作，不修改池状态。用于监控、调试和自适应分配策略。

### 12.1 pool_query_free_pages

```c
pool_query_err_t pool_query_free_pages(pool_owner_t *owner, uint32_t *out_free);
```

遍历位图，统计空闲页数。

| 参数 | 方向 | 说明 |
|------|------|------|
| owner | in | 任意有效的使用者上下文（仅用于访问 cfg） |
| out_free | out | 返回空闲页数 |

返回值：
| 错误码 | 值 | 含义 |
|--------|-----|------|
| POOL_QUERY_OK | 0 | 成功 |
| POOL_QUERY_ERR_NULL | 1 | owner/out_free 为 NULL |

复杂度：O(page_count)。

### 12.2 pool_query_handle_size

```c
pool_query_err_t pool_query_handle_size(pool_owner_t *owner, uint32_t handle,
                                         uint32_t *out_pages, uint32_t *out_bytes);
```

查询指定句柄当前占用的页数和字节数。需要句柄属于当前使用者。

| 参数 | 方向 | 说明 |
|------|------|------|
| owner | in | 使用者上下文（需与句柄所有者匹配） |
| handle | in | 要查询的句柄 |
| out_pages | out | 返回占用页数（传 NULL 表示不关心） |
| out_bytes | out | 返回占用字节数（传 NULL 表示不关心） |

返回值：
| 错误码 | 值 | 含义 |
|--------|-----|------|
| POOL_QUERY_OK | 0 | 成功 |
| POOL_QUERY_ERR_NULL | 1 | owner 为 NULL |
| POOL_QUERY_ERR_INVALID | 2 | 句柄无效（不存在/已释放/代次不匹配） |
| POOL_QUERY_ERR_OWNER | 3 | 句柄不属于此使用者 |

复杂度：O(1)。

### 12.3 pool_query_owner_info

```c
pool_query_err_t pool_query_owner_info(pool_owner_t *owner, uint16_t target_owner,
                                        uint32_t *out_handles, uint32_t *out_pages);
```

查询指定使用者的资源占用统计。

| 参数 | 方向 | 说明 |
|------|------|------|
| owner | in | 任意有效的使用者上下文（仅用于访问 cfg） |
| target_owner | in | 要查询的使用者 ID |
| out_handles | out | 返回活跃句柄数（传 NULL 表示不关心） |
| out_pages | out | 返回占用总页数（传 NULL 表示不关心） |

返回值：
| 错误码 | 值 | 含义 |
|--------|-----|------|
| POOL_QUERY_OK | 0 | 成功 |
| POOL_QUERY_ERR_NULL | 1 | owner 为 NULL |

复杂度：O(handle_count)。

### 12.4 使用示例

```c
// 监控：打印池使用率
uint32_t free_pages;
pool_query_free_pages(&owner, &free_pages);
printf("Free: %u / %u pages (%.1f%%)\n",
       free_pages, cfg.page_count,
       100.0f * (cfg.page_count - free_pages) / cfg.page_count);

// 调试：查询某句柄大小
uint32_t pages, bytes;
pool_query_handle_size(&owner, h, &pages, &bytes);
printf("Handle %u: %u pages, %u bytes\n", h, pages, bytes);

// 审计：统计系统使用者的资源占用
uint32_t sys_handles, sys_pages;
pool_query_owner_info(&owner, 0, &sys_handles, &sys_pages);
printf("System owner(0): %u handles, %u pages\n", sys_handles, sys_pages);
```

---

## 13. 插件与 Hook 系统

### 13.1 设计理念

插桩（Hook）系统允许在不修改 `pool.c` 的情况下，将外部逻辑注入到池的核心操作路径中。设计目标：

- **零默认开销**：未启用插件时，所有 hook 宏展开为 `do{}while(0)`，编译器完全消除
- **编译期展开**：启用插件后，hook 展开为直接函数调用（非函数指针），编译器可内联
- **外部发现**：插件通过 `lib/*/` submodule 提供，每个插件目录包含 `plugin.cmake` 元数据文件
- **手动工作流**：`configure.py --gen-cmake` 扫描发现插件，生成用户可编辑的 `.cmake`；用户编辑 ON/OFF 后，`configure.py` 生成 `pool_plugin_config.h`
- **纯 C99**：不依赖弱符号、函数指针表或运行时注册

### 13.2 架构

```
lib/pool_log/                   pool_hooks.h
┌────────────────────┐          ┌──────────────────────┐
│ src/pool_log.c     │          │ #ifndef POOL_HOOK_*  │
│ include/pool_log.h │          │ #define ... do{}...  │← 默认空（零开销）
│ plugins/logging/   │          │ #endif               │
│  ├ plugin.h        │          └──────────────────────┘
│  ├ plugin.c        │                   ↑ 被覆盖
│  └ plugin.cmake ───┼── configure.py ──→│  (生成)
└────────────────────┘          │
                                │
     pool_plugin_config.cmake   │    pool_plugin_config.h
     (生成，用户可编辑)         │    ┌──────────────────────┐
     ┌──────────────────────┐   │    │ #undef POOL_HOOK_*   │
     │ option(USE_LOGGING   │   │    │ #define POOL_HOOK_*  │
     │   "..." ON)          │   │    │   logging_*(&ctx,..) │
     │ list(APPEND SOURCES  │   │    └──────────────────────┘
     │   lib/pool_log/...)  │   │              ↓ 被 include
     └──────────────────────┘   │         src/pool.c
                                │    ┌──────────────────────┐
                                │    │ POOL_HOOK_PRE_LOCK(  │
                                │    │   cfg, owner, h);    │
                                └───→│   logging_pre_lock(  │
                                     │     &logging_ctx,...)│
                                     └──────────────────────┘
```

### 13.3 全部 Hook 点（14 个）

| 编号 | Hook 宏 | 触发位置 | 参数 |
|------|---------|----------|------|
| 1 | `POOL_HOOK_POST_INIT` | `pool_init` 完成 | `cfg` |
| 2 | `POOL_HOOK_POST_ALLOC` | `pool_alloc_pages` 成功 | `cfg, owner, start, count, handle` |
| 3 | `POOL_HOOK_PRE_FREE` | `pool_free` 验证后、释放前 | `cfg, owner, handle` |
| 4 | `POOL_HOOK_POST_FREE` | `pool_free` 释放后 | `cfg, owner, handle` |
| 5 | `POOL_HOOK_PRE_LOCK` | `pool_lock` 验证后、递增前 | `cfg, owner, handle` |
| 6 | `POOL_HOOK_POST_LOCK` | `pool_lock` 递增后 | `cfg, owner, handle, addr` |
| 7 | `POOL_HOOK_PRE_UNLOCK` | `pool_unlock` 递减前 | `cfg, owner, handle` |
| 8 | `POOL_HOOK_POST_UNLOCK` | `pool_unlock` 递减后 | `cfg, owner, handle` |
| 9 | `POOL_HOOK_PRE_RESIZE` | `pool_resize` 验证后 | `cfg, owner, h, old, new` |
| 10 | `POOL_HOOK_POST_RESIZE` | `pool_resize` 完成（4 个路径） | `cfg, owner, h, old, new` |
| 11 | `POOL_HOOK_PRE_DEFRAG` | `pool_defrag` 开始 | `cfg` |
| 12 | `POOL_HOOK_DEFRAG_MOVE` | 每次 `data_move` 后 | `cfg, src, dst, cnt, idx` |
| 13 | `POOL_HOOK_POST_DEFRAG` | `pool_defrag` 完成 | `cfg` |
| 14 | `POOL_HOOK_PRE_FREE_ALL` | `pool_free_all` 验证后 | `cfg, owner, forced` |
| 15 | `POOL_HOOK_POST_FREE_ALL` | `pool_free_all` 完成 | `cfg, owner, forced` |

### 13.4 使用流程

**第一步：编写插件**

创建插件的目录结构：

```
lib/pool_myplugin/
├── plugins/myplugin/
│   ├── plugin.h
│   ├── plugin.c
│   └── plugin.cmake     ← configure.py 的元数据文件
├── src/                 ← 可选：独立核心库
├── include/             ← 可选：核心库头文件
└── CMakeLists.txt
```

在 `lib/pool_myplugin/plugins/myplugin/plugin.h` 中声明 hook 函数：

```c
// lib/pool_myplugin/plugins/myplugin/plugin.h
#include "pool.h"

extern int my_ctx;  // 插件自己的上下文

void my_pre_lock(void *ctx, pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle);
void my_post_alloc(void *ctx, pool_cfg_t *cfg, pool_owner_t *owner,
                   uint32_t start, uint32_t count, uint32_t handle);
```

函数命名必须遵循 `{插件名}_{hook名}` 约定，使 `configure.py` 能自动识别。

**第二步：实现函数**

```c
// lib/pool_myplugin/plugins/myplugin/plugin.c
#include "plugin.h"

int my_ctx = 0;

void my_pre_lock(void *ctx, pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle)
{
    // 自定义逻辑
    (void)ctx; (void)cfg; (void)owner; (void)handle;
}
// ...
```

**第三步：编写 plugin.cmake**

```cmake
# lib/pool_myplugin/plugins/myplugin/plugin.cmake
set(POOL_PLUGIN_MYPLUGIN_SOURCES   src/mylib.c plugins/myplugin/plugin.c)
set(POOL_PLUGIN_MYPLUGIN_INCLUDES  include)
```

路径相对于插件的库根目录（`lib/pool_myplugin/`）。

**第四步：注册到 MemoryPool**

```sh
# 添加 submodule（首次）
cd path/to/MemoryPool
git submodule add <url> lib/pool_myplugin

# 扫描并生成配置骨架
python3 configure.py --gen-cmake --scan-dir lib

# 编辑 pool_plugin_config.cmake — 设置 ON/OFF
vim pool_plugin_config.cmake

# 生成 hook 头文件
python3 configure.py

# 编译
cmake -B build
cmake --build build
```

**第五步：验证**

```
Plugins enabled: myplugin logging
→ POOL_HOOK_* 宏重定向到 myplugin_xxx(ctx, ...)
```

**无插件启用**（`pool_plugin_config.cmake` 中全部 OFF）：

```
Plugins: none (hooks disabled, zero overhead)
→ POOL_PLUGINS_ENABLED 未定义
→ 所有 POOL_HOOK_* 宏 = do{}while(0)
→ 编译产物零额外指令
```

### 13.5 关键 Hook 使用场景

**swap（页交换）**：`PRE_LOCK` 换入 → `PRE_FREE` 刷回脏页 → `DEFRAG_MOVE` 更新索引 → `PRE_UNLOCK` 标记换出候选

**log（日志）**：`POST_ALLOC`、`POST_FREE`、`POST_LOCK`、`POST_UNLOCK` 记录操作

**perf（性能计数器）**：`PRE_*` + `POST_*` 成对使用，测量每个操作耗时

**guard（安全校验）**：`PRE_LOCK` 检查地址合法性，失败时通过全局标志阻止操作（hook 无返回值，通过标志位通信）

### 13.6 插件编写规范

| 规则 | 说明 |
|------|------|
| 库根目录 = submodule 名 | `lib/<libname>/plugins/<plugin>/` |
| 函数命名 | `{plugin}_{hook}` 例如 `logging_post_init` |
| 上下文变量 | `extern {plugin}_ctx_t {plugin}_ctx` |
| 第一个参数 | `void *ctx` — 透传上下文指针 |
| 第二个参数 | `pool_cfg_t *cfg` — 池配置指针 |
| 无副作用 | 插件可读取池状态或管理外部资源，但禁止修改池内部结构 |
| 头文件依赖 | 必须 `#include "pool.h"` 使用 `pool_cfg_t` 等类型 |
| 元数据文件 | 每个插件需提供 `plugin.cmake` 供 `configure.py --gen-cmake` 发现 |

---

## 附录 A: 文件索引

| 文件 | 行数 | 内容 |
|------|------|------|
| include/pool.h | 333 | 公共 API、结构体、枚举、宏 |
| src/pool.c | 649 | 完整实现 |
| test/test_pool.c | ~1120 | 29 个单元测试（含 hexdump） |
| CMakeLists.txt | 24 | CMake 双模式构建 |
| test/CMakeLists.txt | 15 | 测试子目录 |

---

## 附录 B: 项目漏洞分析

### B.1 输入校验

| 检查项 | 状态 | 说明 |
|--------|------|------|
| NULL owner | ✅ | 所有函数首行检查 |
| NULL cfg (per-function) | ✅ | pool_init 检查，其余通过 owner 间接引用 |
| page_size == 0 | ✅ | pool_init 返回 ERR |
| page_count == 0 | ✅ | pool_init 返回 ERR |
| handle_count == 0 | ✅ | pool_init 返回 ERR |
| metadata_size 不足 | ✅ | pool_init 返回 ERR |
| new_page_count == 0 | ✅ | pool_resize 返回 ERR |
| bytes == 0 | ✅ | pool_alloc_bytes 返回 ERR（已测试） |
| handle 无效 | ✅ | handle_lookup 验证 generation + owner_id |

### B.2 整数溢出

| 风险点 | 严重性 | 分析 |
|--------|--------|------|
| generation 绕回 | 🟢 已修复 | gen=0 自动复位为 1 |
| lock_count 溢出 | ✅ 已防御 | lock_count == 0xFFFF → ERR_OVERFLOW |
| data_move bytes 溢出 | 🟡 理论存在 | count × page_size 在极端值下溢出 size_t。实际 MCU 场景（page_count ≤ 1024, page_size ≤ 4096）不可能触发 |
| handle 编码溢出 | ✅ 无风险 | index_bits ≤ 16（handle_count ≤ 65536），generation 占高 16 位，总 ≤ 32 位 |
| page_start + page_count | ✅ 无风险 | pages_find_free 只返回 page_count 内有效范围 |
| following_idx 数组 | ✅ 有上限 | RESIZE_MAX_FOLLOWING=32，超过返回 NO_SPACE |

### B.3 数组越界

| 访问点 | 边界检查 | 安全 |
|--------|----------|------|
| bitmap[page/8] | 仅 bitmap_test 中 page < page_count 保证 | ✅ |
| page_owner[page] | pages_mark/unmark 由调用者保证合法范围 | ✅ |
| handle_table[idx] | handle_lookup 验证 idx < handle_count | ✅ |
| following_idx[] | following_cnt < RESIZE_MAX_FOLLOWING 时写入 | ✅ |
| defrag_find_candidate 扫描 | 限于 handle_count 范围 | ✅ |

### B.4 元数据一致性与隐含假设

| 条件 | 风险 | 说明 |
|------|------|------|
| 用户提供 meta/data 区域重叠 | 🟡 未检测 | pool_init 不验证两块内存是否重叠。若重叠会破坏元数据。文档中说明"双区模型"隐含不重叠。后续可加 static_assert 风格的地址范围检查 |
| page_size 非 2 的幂 | 🟡 无检查 | 非 2 的幂的 page_size 不破坏正确性，但 data_move 的 4 字节对齐优化无法生效。MCU 场景几乎总是 2 的幂 |
| data_base 未对齐 | 🟡 影响性能 | data_base 未 4 字节对齐时 data_move 退回逐字节。MCU 链接器通常保证对齐 |
| handle_count 非 2 的幂 | ✅ 已处理 | pool_init 动态计算 bits（ceil(log2)） |
| 同一 pool_cfg_t 多次 init | 🟡 未检测 | 重复 init 会覆盖元数据且旧句柄全部失效。使用者需自行保证。此为设计约束 |

### B.5 防御性校验

当前实现中存在的防御性检查，在正常流程中理论上不会触发，但提供额外保护：

| 位置 | 检查 | 意义 |
|------|------|------|
| pool_alloc_pages | `generation == 0` 后重置 | 防止绕回后 new handle 立即无效 |
| pool_lock | `lock_count == 0xFFFF` | 防止递归锁溢出后未定义行为 |
| pool_resize Step 5 | `fe->lock_count > 0` → NO_SPACE | 后方句柄在策略选择后又被锁定（单线程下不可达，但保留安全网） |
| pool_resize Step 5 | `iteration >= handle_count*2` → NO_SPACE | 防止无限循环 |
| handle_lookup | `generation == 0` 永远无效 | 初始化和解引用前的安全栅栏 |
| handle_lookup | `idx >= handle_count` | 掩码提取可能越界（handle_count 非 2 的幂时） |

### B.6 未覆盖的边界路径

| 场景 | 影响 | 风险评级 |
|------|------|----------|
| 65535 个使用者 ID 耗尽后 pool_user_pack | 已处理: ERR_NO_ID | 🟢 安全 |
| pool_defrag 时所有句柄均锁定 | 算法退出，无操作 | 🟢 安全（已测试） |
| pool_resize 缩小到 0 | 已拒绝: ERR_SIZE | 🟢 安全 |
| pool_init 时提供过大的 metadata_size | 仅使用 required 字节，其余不触碰 | 🟢 安全 |
| pool_free_all 释放部分属于其他 owner 的句柄 | 仅释放 owner_id 匹配的 | 🟢 安全 |

### B.7 风险评估总结

```
安全性:    ✅ 无已知可利用漏洞
健壮性:    ✅ 参数校验完整，错误码统一
边界条件:  ✅ 主要路径已覆盖（42 测试）
性能:      ✅ 位图批量/字节跳过/按字搬移/hint 搜索
内存安全:  ✅ 零动态分配，无悬垂指针（handle 机制保护）
线程安全:  ⬜ 单线程 MCU 设计，共享池需额外保护（已知限制）
代码质量:  ✅ C99 纯标准，-Wall -Wextra -Wpedantic 零警告
```

---

> 本文档基于对 `src/pool.c` 源码的逐行分析，所有算法描述与实现一一对应。
> 最后更新: 2026-05-20
