# User Manual — Page-based Memory Pool

> Version: 2.0 | Date: 2026-05-20

## 1. Overview

A page-based memory management system for embedded MCUs. It divides contiguous memory into fixed-size "pages" managed through an opaque handle system. Zero system calls — all memory is user-provided.

**Core concepts**:

- **Page**: fixed-size block, size must be a power of 2 (e.g. 64, 128, 256, 512, 1024…)
- **Handle**: opaque 32-bit integer representing allocated space. Does not expose addresses
- **Owner**: each task identified by `pool_owner_t`, with ownership isolation
- **Dual-region**: metadata and data are physically separated; data contains no management info

## 2. Constraints (Required Reading)

### 2.1 Parameter Constraints

| Parameter | Constraint | Notes |
|-----------|-----------|-------|
| `page_size` | Must be power of 2 (≥ 2) | 128, 256, 512, 1024 are typical. 3, 5, 6, 7… are rejected |
| `page_count` | Must be multiple of 2 (≥ 2) | Together with page_size ensures 4-byte alignment |
| `handle_count` | ≥ 1 | Maximum number of concurrent handles |

Violations cause `pool_init` to return the corresponding error code.

### 2.2 Memory Constraints

| Constraint | Notes |
|------------|-------|
| Metadata and data must not overlap | `pool_init` checks this; returns ERR_OVERLAP |
| Metadata size ≥ POOL_META_SIZE | Macro computes required space (magic + bitmap + owner map + handle table + padding) |
| Data size = page_size × page_count | Caller's responsibility |
| Metadata must be zero before first init | Or ensure first 4 bytes ≠ `POOL_INIT_MAGIC`. On MCU, `static` variables are zero-initialized |
| Re-init is forbidden | If init magic is already present, returns ERR_ALREADY_INIT |

### 2.3 Alignment Recommendation

```c
/* ✅ Recommended: uint32_t array — compiler ensures 4-byte alignment */
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];

/* ⚠️ Works but no alignment guarantee — data_move falls back to byte copy */
static uint8_t data[PAGE_SIZE * PAGE_COUNT];
```

`page_count` even + `page_size` power-of-2 → total bytes always multiple of 4 → data_move always uses word-copy fast path.

### 2.4 Thread Model

This pool is designed for **single-threaded MCU** use. No locks.

- **Threads with separate data** → use independent pools (separate meta + data)
- **Threads sharing data** → extension point reserved (future spinlock), not implemented

### 2.5 data_move Byte Overflow

Theoretically `count × page_size` could overflow `size_t`. Under real MCU constraints (page_count ≤ 4096, page_size ≤ 4096 → max ~16 MB), no overflow risk on 32-bit platforms.

## 3. Parameter Selection Guide

### page_size

| Value | Use Case |
|-------|----------|
| 64, 128 | Tiny objects, fine-grained allocation |
| 256 | General purpose (default in examples) |
| 512, 1024 | Large buffers, lower metadata overhead |

Larger page_size → lower metadata ratio, but potentially more internal fragmentation.

### page_count

```
total_bytes = page_count × page_size
```

Round up to the nearest even number. Example: need 10 KB, page_size=256 → 40 pages (already even). Need 9 KB → ceil(9216/256) = 36 pages.

### handle_count

Each handle entry is 14 bytes. 16 handles ≈ 224 bytes metadata overhead. Set slightly above expected maximum concurrent handles.

## 4. Error Code Reference

### pool_init Error Codes (9 total)

| Error Code | Value | Meaning |
|-----------|-------|---------|
| POOL_INIT_OK | 0 | Success |
| POOL_INIT_ERR_NULL_PARAM | 1 | cfg / meta / data is NULL |
| POOL_INIT_ERR_PAGE_SIZE | 2 | page_size == 0 |
| POOL_INIT_ERR_PAGE_COUNT | 3 | page_count == 0 |
| POOL_INIT_ERR_HANDLE_COUNT | 4 | handle_count == 0 |
| POOL_INIT_ERR_META_SIZE | 5 | metadata_size insufficient |
| POOL_INIT_ERR_PAGE_SIZE_POW2 | 6 | page_size not a power of 2 |
| POOL_INIT_ERR_PAGE_COUNT_EVEN | 7 | page_count not a multiple of 2 |
| POOL_INIT_ERR_OVERLAP | 8 | metadata and data overlap |
| POOL_INIT_ERR_ALREADY_INIT | 9 | init magic already present |

## 5. Quick Start

```c
#include "pool.h"

#define PAGE_SIZE    256
#define PAGE_COUNT   64          // even ✓
#define HANDLE_COUNT 16

// Metadata (auto zero-initialized via static)
static uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];

// Data (uint32_t array for alignment guarantee)
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];

void app_main(void)
{
    pool_cfg_t cfg;
    if (pool_init(&cfg, meta, sizeof(meta), data,
                  PAGE_SIZE, PAGE_COUNT, HANDLE_COUNT) != POOL_INIT_OK) {
        // error handling (should not happen with validated params)
    }

    pool_owner_t owner;
    pool_user_pack(&owner, &cfg);

    uint32_t handle;
    pool_alloc_pages(&owner, 4, &handle);

    void *addr;
    pool_lock(&owner, handle, &addr);
    // ... use addr ...
    pool_unlock(&owner, handle);

    pool_free(&owner, handle);
}
```

## 6. API Reference

### pool_init

```c
pool_init_err_t pool_init(pool_cfg_t *cfg,
    void *metadata_base, size_t metadata_size,
    void *data_base,
    uint32_t page_size, uint32_t page_count,
    uint32_t handle_count);
```

Execution order: NULL check → parameter validity → power-of-2 check → even check → size check → re-init check → overlap check → layout computation → initialization.

### pool_alloc_pages / pool_alloc_bytes

```c
pool_alloc_err_t pool_alloc_pages(pool_owner_t *owner, uint32_t page_count, uint32_t *handle_out);
pool_alloc_err_t pool_alloc_bytes(pool_owner_t *owner, uint32_t bytes, uint32_t *handle_out);
```

### pool_lock / pool_unlock

```c
pool_lock_err_t pool_lock(pool_owner_t *owner, uint32_t handle, void **addr_out);
pool_unlock_err_t pool_unlock(pool_owner_t *owner, uint32_t handle);
```

Supports recursive locking (same handle can be locked multiple times). Each unlock decrements the count.

### pool_free / pool_free_all

```c
pool_free_err_t pool_free(pool_owner_t *owner, uint32_t handle);
pool_free_all_err_t pool_free_all(pool_owner_t *owner, bool forced);
```

### pool_resize

```c
pool_resize_err_t pool_resize(pool_owner_t *owner, uint32_t handle, uint32_t new_page_count);
```

### pool_defrag

```c
pool_defrag_err_t pool_defrag(pool_owner_t *owner);
```

## 7. License

MIT License
