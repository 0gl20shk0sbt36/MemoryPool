# Development Manual — Page-based Memory Pool Internals

[English](dev_manual.md) | [中文](../zh/dev_manual.md) | [← Back to README](../../README.md)

> Version: 2.0 | Date: 2026-05-20
> Source: include/pool.h (~340 lines) + src/pool.c (~740 lines)

---

## Contents

1. Architecture Overview
2. Dual-Region Memory Model
3. Metadata Layout (Byte-Level)
4. Handle Encoding System
5. Owner ID System
6. Core Algorithms (Per-Function)
7. Resize Strategy Decision Tree
8. Defrag Algorithm Step-by-Step
9. Internal Utility Functions
10. Implemented Performance Optimizations
11. Known Issues & Future Directions

Appendices: A. File Index / B. Vulnerability Analysis

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      Public API Layer                        │
│  init / alloc / lock / unlock / free / resize / defrag      │
├─────────────────────────────────────────────────────────────┤
│                      Internal Utilities                      │
│  Bitmap ops (test/set/clear)                                │
│  Page ops   (mark/unmark)                                   │
│  Data move  (word-copy optimized)                           │
│  Lookups    (pages_find_free, handle_find_free, handle_lookup)│
├──────────────┬──────────────────────────────────────────────┤
│   Metadata    │               Data Region                    │
│  ┌─────────┐ │  ┌─────┬─────┬─────┬─────┬──────────┐      │
│  │ Magic    │ │  │Page0│Page1│Page2│ ... │Page N-1  │      │
│  ├─────────┤ │  └─────┴─────┴─────┴─────┴──────────┘      │
│  │ Bitmap   │ │                                             │
│  ├─────────┤ │   Address = data_base + page * page_size    │
│  │ Owner map│ │                                             │
│  ├─────────┤ │   page_size: power of 2                     │
│  │Handle tbl│ │   page_count: multiple of 2                │
│  └─────────┘ │                                             │
└──────────────┴──────────────────────────────────────────────┘
```

Core design principles: zero syscalls, ownership isolation, opaque handles, use-after-free prevention via generation counters.

## 2. Dual-Region Memory Model

Two separate memory regions provided by the user at `pool_init`:

| Region | Purpose | Provided By |
|--------|---------|-------------|
| Metadata | Bitmap, page→handle map, handle table, init magic | User @ pool_init |
| Data | Actual readable/writable memory | User @ pool_init |

The data region contains zero management overhead — users get raw pointers.

## 3. Metadata Layout

```
Offset 0:    uint32_t init_magic     (4 bytes, "POOL" 0x504F4F4C)
Offset 4:    bitmap[ceil(N/8)]       (1 bit per page)
             alignment pad (0~1 byte, to 2-byte boundary)
             page_owner[N]           (2 bytes each, handle index or 0xFFFF)
             alignment pad (0~3 bytes, to 4-byte boundary)
             handle_table[H]         (14 bytes each)
```

The init magic at offset 0 prevents accidental re-initialization.

## 4. Handle Encoding

```
handle = (generation << index_bits) | index

index_bits = ceil(log2(handle_count)), minimum 1
generation ∈ [1, 65535], 0 is permanently invalid
```

Decoding: `idx = handle & mask; gen = handle >> bits`. Validation rejects gen=0, index out of range, stale generations, and freed slots.

Use-after-free prevention: freed slots retain their generation. Reallocation increments generation → old handles become invalid.

**Generation wrap-around (fixed)**: gen wraps 65535→0 after increment. Fixed by detecting gen==0 and resetting to 1 in `pool_alloc_pages`.

## 5. Owner ID System

| Range | Type | Assignment |
|-------|------|------------|
| 0–127 | System | Manual (pool_sys_pack) |
| 128–25565 | User | Auto-increment (pool_user_pack) |
| 0xFFFF | Free marker | Never assigned |

All mutating operations verify `handle_table[idx].owner_id == owner->owner_id` before proceeding.

## 6. Core Algorithms

### pool_init
NULL checks → power-of-2 check → even check → size check → re-init (magic) check → overlap check → compute pointers with alignment → zero metadata → write magic → init page_owner + handle_table.

### pool_alloc_pages
Find free handle slot (hint-based) → find contiguous free pages (byte-skip bitmap scan) → mark pages → fill handle entry → encode handle.

### pool_lock / pool_unlock / pool_free
lock: validate handle → check lock overflow → increment lock_count → return address.
unlock: validate → check not already 0 → decrement.
free: validate → check lock_count==0 → unmark pages → free slot (keep generation).

**Reentrancy**: `pool_lock` supports recursive locking — the same handle can be locked multiple times, each call increments lock_count, each unlock decrements it. Lock count is capped at 65535 (uint16_t). The handle cannot be freed until fully unlocked (lock_count==0). Locked handles are also immune to defrag relocation. This is useful for nested function calls that all need to access the same buffer.

### pool_defrag
Left-to-right scan: find free gaps → for each gap, find the smallest-page-start unlocked handle that fits → move it forward → repeat. Locked handles are never moved.

**Owner parameter**: the `pool_owner_t *owner` argument is used only to obtain the `cfg` pointer. Defrag operates on the entire pool across all owners. Any valid owner pointing to the same pool is equivalent.

### pool_resize
Shrink: free trailing pages in-place.
Expand: try in-place first → collect following handles → decide who moves (self or followers) based on lock state and total sizes → move data and update metadata.

## 7. Resize Decision Tree

```
Need to expand
  ├─ Space immediately after is free? → in-place expand ✓
  └─ No
      ├─ Following handles locked? → MUST move self
      ├─ Following total > self? → move self (less data)
      └─ Otherwise → move followers, then in-place expand

Self locked while must_move_self → ERR_LOCKED
Nowhere to move → ERR_NO_SPACE
```

## 8. Defrag Step-by-Step Example

```
Initial:  ha(0..3) hb(4..7) hc(8..11) hd(12..15) he(16..19)
Free hb, hd:
         ha(0..3) [GAP 4..7] hc(8..11) [GAP 12..15] he(16..19)

Step 1: gap 4..7, candidate hc(8)→4 → ha(0..3) hc(4..7) [GAP 8..11] he(16..19)
Step 2: gap 8..11, candidate he(16)→8 → ha(0..3) hc(4..7) he(8..11) [FREE 12..]
```

## 9. Internal Utilities

- `bitmap_test/set/clear` — single-bit operations (endian-agnostic: bit shifts operate on values, not memory layout)
- `pages_mark/unmark` — bulk mark/unmark with byte-level bitmap optimization
- `data_move` — word-copy when 4-byte aligned, byte-copy fallback
- `pages_find_free` — first-fit with byte-skip (full bytes of 0xFF skip 8 pages)
- `handle_find_free` — hint-based search starting from `next_handle_hint`
- `handle_lookup` — decode + validate handle

## 10. Implemented Optimizations

| Optimization | Location | Effect |
|-------------|----------|--------|
| Byte-skip in bitmap scan | pages_find_free | 64 pages: 64→8 max iterations |
| Byte-level bulk bitmap | pages_mark/unmark | Direct byte writes for aligned ranges |
| Word-copy data move | data_move | ~4× speedup when aligned |
| Handle find hint | handle_find_free | Amortized O(1) slot lookup |
| Generation wrap fix | pool_alloc_pages | 65535→0→1, safe |

## 11. Known Issues & Future

- **Thread safety**: single-threaded design. Multi-thread isolation: separate pools. Shared pool: spinlock extension point reserved.
- **Defrag algorithm**: current single-pass left-to-right is sufficient for MCU. Macro `POOL_DEFRAG_ALGO` reserved for future algorithm switching.
- **Meta/data overlap**: not detected in pool_init (already checked since v2.0).
- **page_size non-power-of-2**: rejected since v2.0.
- **Lock reentrancy**: supports recursive locking. lock_count capped at 65535 — overflow returns ERR_OVERFLOW. Theoretical ISR risk in single-thread MCU environments.
- **Defrag global scope**: defrag operates on the entire pool. The owner parameter only provides the cfg pointer; any valid owner works equivalently.
- **Query API added**: three read-only query functions for monitoring/debugging: `pool_query_free_pages`, `pool_query_handle_size`, `pool_query_owner_info`. See section 12.

---

## 12. Query API

Read-only, non-mutating functions for monitoring, debugging, and adaptive allocation strategies.

### pool_query_free_pages
```c
pool_query_err_t pool_query_free_pages(pool_owner_t *owner, uint32_t *out_free);
```
Counts free pages by scanning the bitmap. Accepts any valid owner. O(page_count).

### pool_query_handle_size
```c
pool_query_err_t pool_query_handle_size(pool_owner_t *owner, uint32_t handle,
                                         uint32_t *out_pages, uint32_t *out_bytes);
```
Returns page count and byte count for a handle (must belong to owner). Accepts NULL for either output parameter. O(1).

### pool_query_owner_info
```c
pool_query_err_t pool_query_owner_info(pool_owner_t *owner, uint16_t target_owner,
                                        uint32_t *out_handles, uint32_t *out_pages);
```
Returns handle count and total page count for a given owner ID. Accepts any valid owner for the cfg pointer; target_owner can be any owner ID. Accepts NULL for either output parameter. O(handle_count).

### Error codes
| Code | Value | Meaning |
|------|-------|---------|
| POOL_QUERY_OK | 0 | Success |
| POOL_QUERY_ERR_NULL | 1 | owner/out pointer is NULL |
| POOL_QUERY_ERR_INVALID | 2 | Handle invalid (only for handle_size) |
| POOL_QUERY_ERR_OWNER | 3 | Handle does not belong to this owner |

---

## Appendix A: File Index

| File | Lines | Content |
|------|-------|---------|
| include/pool.h | ~340 | Public API, structs, enums, macros |
| src/pool.c | ~780 | Full implementation |
| test/test_pool.c | ~1350 | 42 unit tests (POOL_DEBUG hexdump) |
| CMakeLists.txt | 24 | Dual-mode CMake build |
| test/CMakeLists.txt | 15 | Test subdirectory |
| doc/zh/ | | Chinese manuals |
| doc/en/ | | English manuals |

## Appendix B: Vulnerability Analysis (Summary)

| Category | Rating | Notes |
|----------|--------|-------|
| Input validation | ✅ Complete | All NULL/zero/invalid checks present |
| Integer overflow | ✅ Safe | gen wrap fixed, lock_count guarded, data_move theoretical only |
| Array bounds | ✅ Safe | All accesses bounded by page_count/handle_count |
| Metadata integrity | ✅ Guarded | Re-init magic, overlap check, generation validation |
| Memory safety | ✅ | Zero dynamic allocation, no dangling pointers (handle system) |
| Thread safety | ⬜ | Single-threaded design (documented) |
| Code quality | ✅ | C99, -Wall -Wextra -Wpedantic: zero warnings, 42 tests pass |

---

> This document is based on line-by-line analysis of `src/pool.c` source.
> Last updated: 2026-05-20
