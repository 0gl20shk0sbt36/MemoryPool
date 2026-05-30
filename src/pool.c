/**
 * @file pool.c
 * @brief 基于页的内存管理系统 — 实现
 *
 * C99, 无系统调用依赖, 适用于单片机裸机环境。
 */

#include "pool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*===========================================================================
 * 内部工具函数
 *===========================================================================*/

/** @brief 位操作：测试指定位（内联，热路径） */
static inline bool bitmap_test(const uint8_t *bitmap, uint32_t page)
{
    return (bitmap[page >> 3u] & (uint8_t)(1u << (page & 7u))) != 0;
}

/** @brief 位操作：设置指定位（内联，热路径） */
static inline void bitmap_set(uint8_t *bitmap, uint32_t page)
{
    bitmap[page >> 3u] |= (uint8_t)(1u << (page & 7u));
}

/** @brief 位操作：清除指定位（内联，热路径） */
static inline void bitmap_clear(uint8_t *bitmap, uint32_t page)
{
    bitmap[page >> 3u] &= (uint8_t)(~(1u << (page & 7u)));
}

/**
 * @brief 标记页为已使用
 * @param cfg        池配置
 * @param start      起始页索引
 * @param count      页数
 * @param handle_idx 所属句柄索引
 */
static void pages_mark(pool_cfg_t *cfg, uint32_t start, uint32_t count, uint16_t handle_idx)
{
    uint8_t  *bm  = cfg->bitmap;
    uint32_t  end = start + count;
    uint32_t  p   = start;

    /* 阶段1: 从 start 对齐到字节边界 */
    while (p < end && (p & 7u) != 0) {
        bm[p >> 3u] |= (uint8_t)(1u << (p & 7u));
        cfg->page_owner[p] = handle_idx;
        p++;
    }

    /* 阶段2: 整字节批量操作（位图写 0xFF + page_owner 循环） */
    while (p + 8u <= end) {
        bm[p >> 3u] = 0xFFu;
        cfg->page_owner[p + 0] = handle_idx;
        cfg->page_owner[p + 1] = handle_idx;
        cfg->page_owner[p + 2] = handle_idx;
        cfg->page_owner[p + 3] = handle_idx;
        cfg->page_owner[p + 4] = handle_idx;
        cfg->page_owner[p + 5] = handle_idx;
        cfg->page_owner[p + 6] = handle_idx;
        cfg->page_owner[p + 7] = handle_idx;
        p += 8u;
    }

    /* 阶段3: 尾部不足一字节，逐位处理 */
    while (p < end) {
        bm[p >> 3u] |= (uint8_t)(1u << (p & 7u));
        cfg->page_owner[p] = handle_idx;
        p++;
    }
}

/**
 * @brief 取消标记页（标记为空闲）
 * @param cfg   池配置
 * @param start 起始页索引
 * @param count 页数
 */
static void pages_unmark(pool_cfg_t *cfg, uint32_t start, uint32_t count)
{
    uint8_t  *bm  = cfg->bitmap;
    uint32_t  end = start + count;
    uint32_t  p   = start;

    /* 阶段1: 从 start 对齐到字节边界 */
    while (p < end && (p & 7u) != 0) {
        bm[p >> 3u] &= (uint8_t)(~(1u << (p & 7u)));
        cfg->page_owner[p] = POOL_PAGE_FREE;
        p++;
    }

    /* 阶段2: 整字节批量清空（位图写 0x00 + page_owner 循环） */
    while (p + 8u <= end) {
        bm[p >> 3u] = 0x00u;
        cfg->page_owner[p + 0] = POOL_PAGE_FREE;
        cfg->page_owner[p + 1] = POOL_PAGE_FREE;
        cfg->page_owner[p + 2] = POOL_PAGE_FREE;
        cfg->page_owner[p + 3] = POOL_PAGE_FREE;
        cfg->page_owner[p + 4] = POOL_PAGE_FREE;
        cfg->page_owner[p + 5] = POOL_PAGE_FREE;
        cfg->page_owner[p + 6] = POOL_PAGE_FREE;
        cfg->page_owner[p + 7] = POOL_PAGE_FREE;
        p += 8u;
    }

    /* 阶段3: 尾部不足一字节，逐位处理 */
    while (p < end) {
        bm[p >> 3u] &= (uint8_t)(~(1u << (p & 7u)));
        cfg->page_owner[p] = POOL_PAGE_FREE;
        p++;
    }
}

/**
 * @brief 在数据空间中移动页数据
 *
 * 默认使用 memmove。若用户已定义 data_move 宏（例如针对特定硬件的优化实现），
 * 则使用用户的定义。宏签名: data_move(cfg, src_page, dst_page, count)
 */
#ifndef data_move
#define data_move(cfg, src_page, dst_page, count) \
    do { \
        if ((count) != 0 && (src_page) != (dst_page)) { \
            uint8_t *_d = (uint8_t *)(cfg)->data_base; \
            memmove(_d + (size_t)(dst_page) * (cfg)->page_size,  \
                    _d + (size_t)(src_page) * (cfg)->page_size,  \
                    (size_t)(count) * (cfg)->page_size);         \
        } \
    } while(0)
#endif

/**
 * @brief 查找连续的 size 个空闲页（首次适配）
 * @param cfg   池配置
 * @param count 需要的连续页数
 * @return 起始页索引，-1 表示未找到
 */
static int32_t pages_find_free(const pool_cfg_t *cfg, uint32_t count)
{
    uint32_t run_start = 0;
    uint32_t run_len   = 0;

    for (uint32_t i = 0; i < cfg->page_count; i++) {
        /* 字节级加速：整字节全为 1 → 8 页全部已用，跳过 */
        if ((i & 7u) == 0 && cfg->bitmap[i >> 3u] == 0xFFu) {
            i += 7u;        /* +7，循环末尾 i++ = +8 */
            run_len = 0;
            continue;
        }
        if (!bitmap_test(cfg->bitmap, i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= count) return (int32_t)run_start;
        } else {
            run_len = 0;
        }
    }
    return -1;
}

/**
 * @brief 查找空闲句柄槽位
 * @param cfg 池配置
 * @return 句柄索引，-1 表示表已满
 */
static int32_t handle_find_free(pool_cfg_t *cfg)
{
    uint32_t start = cfg->next_handle_hint;

    /* 第一轮: 从 hint 扫描到末尾 */
    for (uint32_t i = start; i < cfg->handle_count; i++) {
        if (cfg->handle_table[i].owner_id == POOL_HANDLE_FREE) {
            cfg->next_handle_hint = (i + 1u) % cfg->handle_count;
            return (int32_t)i;
        }
    }
    /* 第二轮: 从 0 扫描到 hint（环绕） */
    for (uint32_t i = 0; i < start; i++) {
        if (cfg->handle_table[i].owner_id == POOL_HANDLE_FREE) {
            cfg->next_handle_hint = (i + 1u) % cfg->handle_count;
            return (int32_t)i;
        }
    }
    return -1;
}

/**
 * @brief 根据句柄数字查找对应的句柄条目
 *
 * 句柄编码: (generation << index_bits) | index
 *
 * @param cfg    池配置
 * @param handle 句柄数字
 * @return 句柄条目指针，无效时返回 NULL
 */
static pool_handle_entry_t *handle_lookup(const pool_cfg_t *cfg, uint32_t handle)
{
    uint32_t idx = handle & cfg->handle_index_mask;
    if (idx >= cfg->handle_count) return NULL;

    pool_handle_entry_t *e = &cfg->handle_table[idx];
    if (e->owner_id == POOL_HANDLE_FREE) return NULL;

    uint32_t gen = handle >> cfg->handle_index_bits;
    if (gen != e->generation || gen == 0) return NULL;

    return e;
}

/*===========================================================================
 * API 实现
 *===========================================================================*/

pool_init_err_t pool_init(pool_cfg_t *cfg,
                          void *metadata_base, size_t metadata_size,
                          void *data_base,
                          uint32_t page_size, uint32_t page_count,
                          uint32_t handle_count)
{
    if (cfg == NULL || metadata_base == NULL || data_base == NULL) {
        return POOL_INIT_ERR_NULL_PARAM;
    }
    if (page_size == 0)   return POOL_INIT_ERR_PAGE_SIZE;
    if (page_count == 0)  return POOL_INIT_ERR_PAGE_COUNT;
    if (handle_count == 0) return POOL_INIT_ERR_HANDLE_COUNT;

    /* 约束: page_size 必须为 2 的幂 */
    if ((page_size & (page_size - 1u)) != 0) return POOL_INIT_ERR_PAGE_SIZE_POW2;

    /* 约束: page_count 必须为 2 的倍数（保证总字节数为 4 的倍数） */
    if ((page_count & 1u) != 0) return POOL_INIT_ERR_PAGE_COUNT_EVEN;

    size_t required = POOL_META_SIZE(page_count, handle_count);
    if (metadata_size < required) {
        return POOL_INIT_ERR_META_SIZE;
    }

    /* 检测重复初始化: 元数据区前 4 字节若已是标记则拒绝 */
    if (*(const uint32_t *)metadata_base == POOL_INIT_MAGIC) {
        return POOL_INIT_ERR_ALREADY_INIT;
    }

    /* 约束: 元数据区与数据区不可重叠 */
    {
        uintptr_t m_start = (uintptr_t)metadata_base;
        uintptr_t m_end   = m_start + metadata_size;
        uintptr_t d_start = (uintptr_t)data_base;
        uintptr_t d_end   = d_start + (uintptr_t)page_count * page_size;
        if (!(m_end <= d_start || d_end <= m_start)) {
            return POOL_INIT_ERR_OVERLAP;
        }
    }

    /* 填充基础字段 */
    cfg->metadata_base = metadata_base;
    cfg->metadata_size = metadata_size;
    cfg->data_base     = data_base;
    cfg->page_size     = page_size;
    cfg->page_count    = page_count;
    cfg->handle_count  = handle_count;
    cfg->next_user_id     = POOL_USER_ID_MIN;
    cfg->next_handle_hint = 0;

    /* 计算内部指针（带对齐） */
    {
        /*
         * 对齐粒度：
         *  - page_owner: 2 字节 (sizeof(uint16_t))
         *  - handle_table: 4 字节 (uint32_t 对齐要求)
         * 宏 POOL_META_SIZE 已预留最大填充量，此处实际使用可能更少。
         * 偏移 0..3 为初始化标记 (POOL_INIT_MAGIC)。
         */
        #define ALIGN_UP(addr, align) \
            (((addr) + (align) - 1u) & ~((size_t)(align) - 1u))

        uint8_t *meta = (uint8_t *)metadata_base;

        /* 跳过初始化标记 */
        meta += sizeof(uint32_t);

        cfg->bitmap = meta;
        meta += (page_count + 7u) / 8u;

        cfg->page_owner = (uint16_t *)ALIGN_UP((size_t)meta, sizeof(uint16_t));
        meta = (uint8_t *)cfg->page_owner + page_count * sizeof(uint16_t);

        cfg->handle_table = (pool_handle_entry_t *)ALIGN_UP((size_t)meta, sizeof(uint32_t));
        /* meta 指针不再使用 */

        #undef ALIGN_UP
    }

    /* 计算句柄索引位数 */
    uint32_t bits    = 0;
    uint32_t max_idx = handle_count - 1;
    while (max_idx > 0) {
        bits++;
        max_idx >>= 1u;
    }
    if (bits == 0) bits = 1; /* handle_count == 1 */
    cfg->handle_index_bits = bits;
    cfg->handle_index_mask = ((uint32_t)1u << bits) - 1u;

    /* 初始化全部元数据为零 */
    memset(metadata_base, 0, required);

    /* 写入初始化标记（防重复 init） */
    *(uint32_t *)metadata_base = POOL_INIT_MAGIC;

    /* 页属主映射标记为空闲 */
    for (uint32_t i = 0; i < page_count; i++) {
        cfg->page_owner[i] = POOL_PAGE_FREE;
    }

    /* 句柄表标记为空闲 */
    for (uint32_t i = 0; i < handle_count; i++) {
        cfg->handle_table[i].owner_id   = POOL_HANDLE_FREE;
        cfg->handle_table[i].page_start = 0;
        cfg->handle_table[i].page_count = 0;
        cfg->handle_table[i].lock_count = 0;
        cfg->handle_table[i].generation = 0;
    }

    return POOL_INIT_OK;
}

/*-------------------------------------------------------------------------*/

pool_sys_pack_err_t pool_sys_pack(pool_owner_t *owner, pool_cfg_t *cfg, uint16_t owner_id)
{
    if (owner == NULL || cfg == NULL) return POOL_SYS_PACK_ERR_NULL;
    if (owner_id > POOL_SYS_ID_MAX)   return POOL_SYS_PACK_ERR_ID_RANGE;

    owner->cfg      = cfg;
    owner->owner_id = owner_id;
    return POOL_SYS_PACK_OK;
}

/*-------------------------------------------------------------------------*/

pool_user_pack_err_t pool_user_pack(pool_owner_t *owner, pool_cfg_t *cfg)
{
    if (owner == NULL || cfg == NULL) return POOL_USER_PACK_ERR_NULL;
    if (cfg->next_user_id > POOL_USER_ID_MAX) return POOL_USER_PACK_ERR_NO_ID;

    owner->cfg      = cfg;
    owner->owner_id = (uint16_t)cfg->next_user_id;
    cfg->next_user_id++;
    return POOL_USER_PACK_OK;
}

/*-------------------------------------------------------------------------*/

pool_alloc_err_t pool_alloc_pages(pool_owner_t *owner, uint32_t page_count, uint32_t *handle_out)
{
    if (owner == NULL || handle_out == NULL) return POOL_ALLOC_ERR_NULL;
    if (page_count == 0) return POOL_ALLOC_ERR_SIZE;

    pool_cfg_t *cfg = owner->cfg;

    /* 找空闲句柄槽 */
    int32_t h_idx = handle_find_free(cfg);
    if (h_idx < 0) return POOL_ALLOC_ERR_NO_HANDLE;

    /* 找连续空闲页 */
    int32_t pg_start = pages_find_free(cfg, page_count);
    if (pg_start < 0) return POOL_ALLOC_ERR_NO_SPACE;

    /* 标记页 */
    pages_mark(cfg, (uint32_t)pg_start, page_count, (uint16_t)h_idx);

    /* 填充句柄条目 */
    pool_handle_entry_t *e = &cfg->handle_table[h_idx];
    e->owner_id   = owner->owner_id;
    e->page_start = (uint32_t)pg_start;
    e->page_count = page_count;
    e->lock_count = 0;
    /* 递增次代数并防止绕回 0（gen==0 永远无效） */
    e->generation++;
    if (e->generation == 0) e->generation = 1;

    /* 编码句柄 */
    *handle_out = (((uint32_t)e->generation) << cfg->handle_index_bits) | (uint32_t)h_idx;
    return POOL_ALLOC_OK;
}

/*-------------------------------------------------------------------------*/

pool_alloc_err_t pool_alloc_bytes(pool_owner_t *owner, uint32_t bytes, uint32_t *handle_out)
{
    if (owner == NULL || handle_out == NULL) return POOL_ALLOC_ERR_NULL;
    if (bytes == 0) return POOL_ALLOC_ERR_SIZE;

    pool_cfg_t *cfg = owner->cfg;
    /* 向上取整到页 */
    uint32_t pages = (bytes + cfg->page_size - 1u) / cfg->page_size;
    return pool_alloc_pages(owner, pages, handle_out);
}

/*-------------------------------------------------------------------------*/

pool_lock_err_t pool_lock(pool_owner_t *owner, uint32_t handle, void **addr_out)
{
    if (owner == NULL || addr_out == NULL) return POOL_LOCK_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    pool_handle_entry_t *e = handle_lookup(cfg, handle);
    if (e == NULL)                    return POOL_LOCK_ERR_INVALID;
    if (e->owner_id != owner->owner_id) return POOL_LOCK_ERR_OWNER;
    if (e->lock_count == 0xFFFFu)     return POOL_LOCK_ERR_OVERFLOW;

    e->lock_count++;
    *addr_out = (uint8_t *)cfg->data_base + (size_t)e->page_start * cfg->page_size;
    return POOL_LOCK_OK;
}

/*-------------------------------------------------------------------------*/

pool_unlock_err_t pool_unlock(pool_owner_t *owner, uint32_t handle)
{
    if (owner == NULL) return POOL_UNLOCK_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    pool_handle_entry_t *e = handle_lookup(cfg, handle);
    if (e == NULL)                    return POOL_UNLOCK_ERR_INVALID;
    if (e->owner_id != owner->owner_id) return POOL_UNLOCK_ERR_OWNER;
    if (e->lock_count == 0)           return POOL_UNLOCK_ERR_NOT_LOCKED;

    e->lock_count--;
    return POOL_UNLOCK_OK;
}

/*-------------------------------------------------------------------------*/

pool_free_err_t pool_free(pool_owner_t *owner, uint32_t handle)
{
    if (owner == NULL) return POOL_FREE_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    pool_handle_entry_t *e = handle_lookup(cfg, handle);
    if (e == NULL)                    return POOL_FREE_ERR_INVALID;
    if (e->owner_id != owner->owner_id) return POOL_FREE_ERR_OWNER;
    if (e->lock_count > 0)            return POOL_FREE_ERR_LOCKED;

    /* 释放页面 */
    pages_unmark(cfg, e->page_start, e->page_count);

    /* 释放句柄槽（保留 generation 以验证旧句柄） */
    e->owner_id   = POOL_HANDLE_FREE;
    e->page_start = 0;
    e->page_count = 0;
    /* lock_count 和 generation 保持不变 */

    /* 更新句柄查找提示（使释放的槽可被优先复用） */
    {
        uint32_t freed_idx = handle & cfg->handle_index_mask;
        if (freed_idx < cfg->next_handle_hint) {
            cfg->next_handle_hint = freed_idx;
        }
    }

    return POOL_FREE_OK;
}

/*-------------------------------------------------------------------------*/

pool_resize_err_t pool_resize(pool_owner_t *owner, uint32_t handle, uint32_t new_page_count)
{
    if (owner == NULL)     return POOL_RESIZE_ERR_NULL;
    if (new_page_count == 0) return POOL_RESIZE_ERR_SIZE;

    pool_cfg_t *cfg = owner->cfg;

    pool_handle_entry_t *e = handle_lookup(cfg, handle);
    if (e == NULL)                    return POOL_RESIZE_ERR_INVALID;
    if (e->owner_id != owner->owner_id) return POOL_RESIZE_ERR_OWNER;

    uint32_t cur_pages = e->page_count;
    if (new_page_count == cur_pages) return POOL_RESIZE_OK;

    /* --- 缩小：原地释放尾部页面 --- */
    if (new_page_count < cur_pages) {
        uint32_t extra = cur_pages - new_page_count;
        pages_unmark(cfg, e->page_start + new_page_count, extra);
        e->page_count = new_page_count;
        return POOL_RESIZE_OK;
    }

    /* --- 扩大 --- */
    uint32_t extra_needed = new_page_count - cur_pages;
    uint32_t old_end      = e->page_start + cur_pages;

    /* 1. 尝试原地扩展 */
    if (old_end + extra_needed <= cfg->page_count) {
        bool can_extend = true;
        for (uint32_t p = old_end; p < old_end + extra_needed; p++) {
            if (bitmap_test(cfg->bitmap, p)) {
                can_extend = false;
                break;
            }
        }
        if (can_extend) {
            uint16_t self_idx = (uint16_t)(handle & cfg->handle_index_mask);
            pages_mark(cfg, old_end, extra_needed, self_idx);
            e->page_count = new_page_count;
            return POOL_RESIZE_OK;
        }
    }

    /* 2. 收集覆盖扩展区域的后方句柄 */
    #define RESIZE_MAX_FOLLOWING 32
    uint16_t following_idx[RESIZE_MAX_FOLLOWING];
    uint32_t following_cnt   = 0;
    bool     any_locked      = false;
    uint32_t following_total = 0;

    uint32_t scan_end = old_end + extra_needed;
    if (scan_end > cfg->page_count) scan_end = cfg->page_count;

    for (uint32_t p = old_end; p < scan_end; p++) {
        if (!bitmap_test(cfg->bitmap, p)) continue;
        uint16_t h_idx = cfg->page_owner[p];
        if (h_idx == POOL_PAGE_FREE) continue;

        /* 是否已记录 */
        bool already = false;
        for (uint32_t j = 0; j < following_cnt; j++) {
            if (following_idx[j] == h_idx) { already = true; break; }
        }
        if (already) continue;

        if (following_cnt >= RESIZE_MAX_FOLLOWING) return POOL_RESIZE_ERR_NO_SPACE;

        following_idx[following_cnt++] = h_idx;
        pool_handle_entry_t *fe = &cfg->handle_table[h_idx];
        following_total += fe->page_count;
        if (fe->lock_count > 0) any_locked = true;
    }

    /* 如果扩展区域超出总页数且无可移动空间 */
    if (following_cnt == 0 && old_end + extra_needed > cfg->page_count) {
        return POOL_RESIZE_ERR_NO_SPACE;
    }

    /* 3. 判断移动策略 */
    bool must_move_self = false;

    if (following_cnt == 0) {
        /* 理论上已被原地扩展捕获，这里是扩展超过边界 */
        must_move_self = true;
    } else if (any_locked) {
        must_move_self = true;
    } else {
        if (following_total > cur_pages) {
            must_move_self = true;
        }
        /* else: 移动后方句柄 */
    }

    /* 4. 移动自己 */
    if (must_move_self) {
        if (e->lock_count > 0) return POOL_RESIZE_ERR_LOCKED;

        int32_t new_start = pages_find_free(cfg, new_page_count);
        if (new_start < 0) return POOL_RESIZE_ERR_NO_SPACE;

        /* 移动数据 */
        data_move(cfg, e->page_start, (uint32_t)new_start, cur_pages);

        /* 更新元数据 */
        uint16_t self_idx = (uint16_t)(handle & cfg->handle_index_mask);
        pages_unmark(cfg, e->page_start, cur_pages);
        pages_mark(cfg, (uint32_t)new_start, new_page_count, self_idx);
        e->page_start = (uint32_t)new_start;
        e->page_count = new_page_count;
        return POOL_RESIZE_OK;
    }

    /* 5. 移动后方句柄，清空 [old_end, old_end+extra_needed) 区域 */
    {
        uint32_t scan_limit = old_end + extra_needed;
        if (scan_limit > cfg->page_count) scan_limit = cfg->page_count;

        for (uint32_t iteration = 0; iteration < cfg->handle_count * 2; iteration++) {
            /* 扫描区域查找仍占用的句柄 */
            int32_t occupant_idx = -1;
            for (uint32_t p = old_end; p < scan_limit; p++) {
                if (bitmap_test(cfg->bitmap, p)) {
                    uint16_t oi = cfg->page_owner[p];
                    if (oi != POOL_PAGE_FREE &&
                        oi != (uint16_t)(handle & cfg->handle_index_mask)) {
                        occupant_idx = (int32_t)oi;
                        break;
                    }
                }
            }

            if (occupant_idx < 0) break; /* 区域已清空 */

            /* 安全上限（理论上不可达） */
            if (iteration + 1 >= cfg->handle_count * 2) return POOL_RESIZE_ERR_NO_SPACE;

            pool_handle_entry_t *fe = &cfg->handle_table[occupant_idx];

            if (fe->lock_count > 0) return POOL_RESIZE_ERR_NO_SPACE;

            int32_t new_start = pages_find_free(cfg, fe->page_count);
            if (new_start < 0) return POOL_RESIZE_ERR_NO_SPACE;

            data_move(cfg, fe->page_start, (uint32_t)new_start, fe->page_count);
            pages_unmark(cfg, fe->page_start, fe->page_count);
            pages_mark(cfg, (uint32_t)new_start, fe->page_count, (uint16_t)occupant_idx);
            fe->page_start = (uint32_t)new_start;
        }
    }

    /* 6. 原地扩展自身 */
    {
        uint16_t self_idx = (uint16_t)(handle & cfg->handle_index_mask);
        pages_mark(cfg, old_end, extra_needed, self_idx);
        e->page_count = new_page_count;
    }

    return POOL_RESIZE_OK;
}

/*-------------------------------------------------------------------------*/

/**
 * @brief 在空闲页后方查找最小 page_start 的未锁定句柄（用于碎片整理）
 *
 * 查找条件：
 *  - page_start > min_page
 *  - 未锁定 (lock_count == 0)
 *  - 页数 <= max_pages
 *  返回满足条件的 page_start 最小者的索引，-1 表示未找到。
 */
static int32_t defrag_find_candidate(const pool_cfg_t *cfg,
                                     uint32_t min_page, uint32_t max_pages)
{
    int32_t  best_idx   = -1;
    uint32_t best_start = 0xFFFFFFFFu; /* 比任何有效 page_start 都大 */

    for (uint32_t i = 0; i < cfg->handle_count; i++) {
        const pool_handle_entry_t *e = &cfg->handle_table[i];
        if (e->owner_id == POOL_HANDLE_FREE) continue;
        if (e->page_start <= min_page) continue;
        if (e->lock_count > 0) continue;
        if (e->page_count > max_pages) continue;
        if (e->page_start < best_start) {
            best_start = e->page_start;
            best_idx   = (int32_t)i;
        }
    }
    return best_idx;
}

/*-------------------------------------------------------------------------*/

pool_defrag_err_t pool_defrag(pool_owner_t *owner)
{
    if (owner == NULL) return POOL_DEFRAG_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    uint32_t cursor = 0;
    while (cursor < cfg->page_count) {
        /* 跳过已用页 */
        while (cursor < cfg->page_count && bitmap_test(cfg->bitmap, cursor)) {
            cursor++;
        }
        if (cursor >= cfg->page_count) break;

        /* 找到空闲间隙 */
        uint32_t gap_start = cursor;
        uint32_t gap_end   = gap_start;
        while (gap_end < cfg->page_count && !bitmap_test(cfg->bitmap, gap_end)) {
            gap_end++;
        }
        uint32_t gap_size = gap_end - gap_start;

        /* 尝试用后方未锁定句柄填充此间隙 */
        while (gap_size > 0) {
            int32_t h_idx = defrag_find_candidate(cfg, gap_start, gap_size);
            if (h_idx < 0) break;

            pool_handle_entry_t *he = &cfg->handle_table[h_idx];

            /* 移动数据 */
            data_move(cfg, he->page_start, gap_start, he->page_count);
            /* 更新页映射 */
            pages_unmark(cfg, he->page_start, he->page_count);
            pages_mark(cfg, gap_start, he->page_count, (uint16_t)h_idx);
            /* 更新句柄 */
            he->page_start = gap_start;

            gap_start += he->page_count;
            gap_size  -= he->page_count;
        }

        cursor = gap_end;
    }

    return POOL_DEFRAG_OK;
}

/*-------------------------------------------------------------------------*/

pool_free_all_err_t pool_free_all(pool_owner_t *owner, bool forced)
{
    if (owner == NULL) return POOL_FREE_ALL_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;
    uint16_t oid    = owner->owner_id;

    /* 非强制模式：检查所有句柄是否已解锁 */
    if (!forced) {
        for (uint32_t i = 0; i < cfg->handle_count; i++) {
            pool_handle_entry_t *e = &cfg->handle_table[i];
            if (e->owner_id == oid && e->lock_count > 0) {
                return POOL_FREE_ALL_ERR_LOCKED;
            }
        }
    }

    /* 释放所有属于此使用者的句柄 */
    for (uint32_t i = 0; i < cfg->handle_count; i++) {
        pool_handle_entry_t *e = &cfg->handle_table[i];
        if (e->owner_id == oid) {
            pages_unmark(cfg, e->page_start, e->page_count);
            e->owner_id   = POOL_HANDLE_FREE;
            e->page_start = 0;
            e->page_count = 0;
            e->lock_count = 0;
            /* generation 保留 */
            if (i < cfg->next_handle_hint) cfg->next_handle_hint = i;
        }
    }

    return POOL_FREE_ALL_OK;
}

/*===========================================================================
 * 查询 API 实现（只读，不修改池状态）
 *===========================================================================*/

pool_query_err_t pool_query_free_pages(pool_owner_t *owner, uint32_t *out_free)
{
    if (owner == NULL || out_free == NULL) return POOL_QUERY_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    uint32_t free_count = 0;
    for (uint32_t i = 0; i < cfg->page_count; i++) {
        if (!bitmap_test(cfg->bitmap, i)) {
            free_count++;
        }
    }
    *out_free = free_count;
    return POOL_QUERY_OK;
}

pool_query_err_t pool_query_handle_size(pool_owner_t *owner, uint32_t handle,
                                         uint32_t *out_pages, uint32_t *out_bytes)
{
    if (owner == NULL) return POOL_QUERY_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    pool_handle_entry_t *e = handle_lookup(cfg, handle);
    if (e == NULL)                    return POOL_QUERY_ERR_INVALID;
    if (e->owner_id != owner->owner_id) return POOL_QUERY_ERR_OWNER;

    if (out_pages != NULL) *out_pages = e->page_count;
    if (out_bytes != NULL) *out_bytes = e->page_count * cfg->page_size;
    return POOL_QUERY_OK;
}

pool_query_err_t pool_query_owner_info(pool_owner_t *owner, uint16_t target_owner,
                                        uint32_t *out_handles, uint32_t *out_pages)
{
    if (owner == NULL) return POOL_QUERY_ERR_NULL;
    pool_cfg_t *cfg = owner->cfg;

    uint32_t handle_cnt = 0;
    uint32_t page_cnt   = 0;

    for (uint32_t i = 0; i < cfg->handle_count; i++) {
        const pool_handle_entry_t *e = &cfg->handle_table[i];
        if (e->owner_id == target_owner) {
            handle_cnt++;
            page_cnt += e->page_count;
        }
    }

    if (out_handles != NULL) *out_handles = handle_cnt;
    if (out_pages   != NULL) *out_pages   = page_cnt;
    return POOL_QUERY_OK;
}