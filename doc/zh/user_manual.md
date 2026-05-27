# 用户手册 — 页内存池

[中文](user_manual.md) | [English](../en/user_manual.md) | [← 返回 README](../../README_zh.md)

> 版本: 2.0 | 日期: 2026-05-20

## 1. 概述

页内存池是一个面向嵌入式 MCU 的内存管理系统。它将一块连续内存划分为固定大小的"页"，通过句柄系统管理分配和回收，零系统调用。

**核心概念**：

- **页（Page）**：固定大小的内存块，大小为 2 的幂（如 64, 128, 256, 512, 1024…）
- **句柄（Handle）**：不透明的 32 位整数，代表一块已分配空间。不直接暴露地址
- **使用者（Owner）**：每个任务通过 `pool_owner_t` 标识，享有所有权隔离
- **双区模型**：元数据区 + 数据区物理分离，数据区不含管理信息

## 2. 约束与限制（必读）

### 2.1 参数约束

| 参数 | 约束 | 说明 |
|------|------|------|
| `page_size` | 须为 2 的幂（≥ 2） | 128, 256, 512, 1024 是典型值。禁止 3, 5, 6, 7… |
| `page_count` | 须为 2 的倍数（≥ 2） | 配合 page_size 保证每页地址 4 字节对齐 |
| `handle_count` | ≥ 1 | 决定同时存在的最大句柄数 |

违反上述约束时 `pool_init` 返回对应错误码。

### 2.2 内存约束

| 约束 | 说明 |
|------|------|
| 元数据区与数据区不可重叠 | `pool_init` 检测，重叠则返回 ERR_OVERLAP |
| 元数据区大小 ≥ POOL_META_SIZE | 宏自动计算所需空间（含初始标记 + 位图 + 属主映射 + 句柄表 + 对齐填充） |
| 数据区大小 = page_size × page_count | 用户自行保证 |
| 首次 init 前元数据区须为零 | 或保证前 4 字节不为 `POOL_INIT_MAGIC`。MCU 上推荐 `static` 变量（自动零初始化） |
| 禁止重复 init | 若元数据区已有初始化标记，返回 ERR_ALREADY_INIT |

### 2.3 数据对齐建议

```c
/* ✅ 推荐：uint32_t 数组，编译器保证 4 字节对齐 */
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];

/* ⚠️ 可用但无对齐保证：若地址不对齐，data_move 退化为逐字节复制 */
static uint8_t data[PAGE_SIZE * PAGE_COUNT];
```

page_count 为 2 的倍数 + page_size 为 2 的幂 → 总字节数必为 4 的倍数，data_move 始终走按字复制快速路径。

### 2.4 线程模型

本池为**单线程 MCU 设计**，无锁保护。

- **线程间不共享数据** → 各自使用独立的内存池（两套 meta + data）
- **线程间需共享数据** → 预留扩展点（未来 spinlock），当前不做实现

### 2.5 data_move 字节数溢出

理论上 count × page_size 可能溢出 `size_t`。在 MCU 实际约束下（page_count ≤ 4096, page_size ≤ 4096 → 最大 16 MB），32 位平台无溢出风险。

## 3. 参数选择指南

### page_size

| 值 | 适用场景 |
|----|----------|
| 64, 128 | 极小对象，高精度分配 |
| 256 | 通用（本手册示例的默认值） |
| 512, 1024 | 大块缓冲，减少元数据开销 |

page_size 越大 → 元数据占比越低，但内部碎片可能增多。

### page_count

```
总字节数 = page_count × page_size
```

算好后向上取整到 2 的倍数。例如需要 10 KB，page_size=256：
→ 40 页（已是偶数，可用）

需要 9 KB，page_size=256：
→ 36 页（ceil(9216/256) = 36，已是偶数）

### handle_count

每个句柄条目 14 字节。16 个句柄 ≈ 224 字节元数据开销。建议设略大于预计最大同时句柄数。

## 4. 错误码速查

### pool_init 错误码（9 种）

| 错误码 | 值 | 含义 |
|--------|-----|------|
| POOL_INIT_OK | 0 | 成功 |
| POOL_INIT_ERR_NULL_PARAM | 1 | cfg / meta / data 为 NULL |
| POOL_INIT_ERR_PAGE_SIZE | 2 | page_size == 0 |
| POOL_INIT_ERR_PAGE_COUNT | 3 | page_count == 0 |
| POOL_INIT_ERR_HANDLE_COUNT | 4 | handle_count == 0 |
| POOL_INIT_ERR_META_SIZE | 5 | metadata_size 不足 |
| POOL_INIT_ERR_PAGE_SIZE_POW2 | 6 | page_size 非 2 的幂 |
| POOL_INIT_ERR_PAGE_COUNT_EVEN | 7 | page_count 非 2 的倍数 |
| POOL_INIT_ERR_OVERLAP | 8 | 元数据区与数据区重叠 |
| POOL_INIT_ERR_ALREADY_INIT | 9 | 元数据区已有初始化标记 |

## 5. 快速上手

```c
#include "pool.h"

#define PAGE_SIZE    256
#define PAGE_COUNT   64          // 偶数 ✓
#define HANDLE_COUNT 16

// 元数据区（自动零初始化，static 保证）
static uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];

// 数据区（推荐 uint32_t 数组，对齐保证）
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];

void app_main(void)
{
    pool_cfg_t cfg;
    if (pool_init(&cfg, meta, sizeof(meta), data,
                  PAGE_SIZE, PAGE_COUNT, HANDLE_COUNT) != POOL_INIT_OK) {
        // 错误处理（不会发生，上面参数已验证）
    }

    pool_owner_t owner;
    pool_user_pack(&owner, &cfg);

    uint32_t handle;
    pool_alloc_pages(&owner, 4, &handle);

    void *addr;
    pool_lock(&owner, handle, &addr);
    // ... 使用 addr ...
    pool_unlock(&owner, handle);

    pool_free(&owner, handle);
}
```

## 6. API 参考

### pool_init — 初始化

```c
pool_init_err_t pool_init(pool_cfg_t *cfg,
    void *metadata_base, size_t metadata_size,
    void *data_base,
    uint32_t page_size, uint32_t page_count,
    uint32_t handle_count);
```

执行顺序：NULL 检查 → 参数合法性 → 2 的幂检查 → 偶数检查 → 大小检查 → 重复 init 检查 → 重叠检查 → 布局计算 → 初始化。

### pool_alloc_pages / pool_alloc_bytes — 分配

```c
pool_alloc_err_t pool_alloc_pages(pool_owner_t *owner, uint32_t page_count, uint32_t *handle_out);
pool_alloc_err_t pool_alloc_bytes(pool_owner_t *owner, uint32_t bytes, uint32_t *handle_out);
```

### pool_lock / pool_unlock — 锁定/解锁

```c
pool_lock_err_t pool_lock(pool_owner_t *owner, uint32_t handle, void **addr_out);
pool_unlock_err_t pool_unlock(pool_owner_t *owner, uint32_t handle);
```

支持递归锁（同一句柄可多次锁定），每次解锁递减计数。

### pool_free / pool_free_all — 释放

```c
pool_free_err_t pool_free(pool_owner_t *owner, uint32_t handle);
pool_free_all_err_t pool_free_all(pool_owner_t *owner, bool forced);
```

### pool_resize — 改变大小

```c
pool_resize_err_t pool_resize(pool_owner_t *owner, uint32_t handle, uint32_t new_page_count);
```

### pool_defrag — 碎片整理

```c
pool_defrag_err_t pool_defrag(pool_owner_t *owner);
```

## 7. 许可

MIT License
