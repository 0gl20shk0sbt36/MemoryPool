/**
 * @file test_pool.c
 * @brief 页内存池单元测试 — 含 hexdump 元数据/数据空间校验
 *
 * 测试覆盖：
 *  - 初始化参数验证
 *  - 系统/用户打包
 *  - 句柄编码正确性
 *  - 分配/释放/锁定/解锁
 *  - 碎片整理（含级联移动）
 *  - 句柄大小调整（含移动自身/移动后方）
 *  - 强制/非强制全部释放
 *  - 边界条件：page_count=1, handle_count=1
 *  - 多使用者交叉操作
 */

#include "pool.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*===========================================================================
 * 测试工具
 *===========================================================================*/

static int g_failures = 0;

#define TEST(name)  static void test_##name(void)
#define RUN(name)   do { test_##name(); } while(0)

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL [%s]: %s\n", __func__, msg); \
        return; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        g_failures++; \
        printf("FAIL [%s]: %s (expected %d, got %d)\n", __func__, msg, (int)(b), (int)(a)); \
        return; \
    } \
} while(0)

/*===========================================================================
 * Hexdump 工具
 *===========================================================================*/

/** 打印一块内存的 hexdump（每行 16 字节，带 ASCII 侧栏） */
static void hexdump(const char *label, const void *ptr, uint32_t len)
{
    const uint8_t *buf = (const uint8_t *)ptr;
    printf("--- %s (%u bytes) ---\n", label, len);
    for (uint32_t i = 0; i < len; i += 16) {
        uint32_t n = (len - i < 16) ? (len - i) : 16;
        printf("%08x  ", i);
        for (uint32_t j = 0; j < n; j++) {
            printf("%02x ", buf[i + j]);
            if (j == 7) putchar(' ');
        }
        if (n < 16) {
            for (uint32_t j = n; j < 16; j++) {
                printf("   ");
                if (j == 7) putchar(' ');
            }
        }
        printf(" |");
        for (uint32_t j = 0; j < n; j++) {
            uint8_t c = buf[i + j];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
    fflush(stdout);
}

/** 打印句柄表（仅活跃条目） */
static void dump_handles(pool_cfg_t *cfg)
{
    printf("--- handle_table (active entries) ---\n");
    uint32_t count = 0;
    for (uint32_t i = 0; i < cfg->handle_count; i++) {
        const pool_handle_entry_t *e = &cfg->handle_table[i];
        if (e->owner_id == POOL_HANDLE_FREE) continue;
        printf("  [%2u] owner=%-5u start=%-2u count=%-2u lock=%u gen=%u\n",
               i, e->owner_id, e->page_start, e->page_count,
               e->lock_count, e->generation);
        count++;
    }
    if (count == 0) printf("  (none)\n");
    fflush(stdout);
}

/** 打印池当前完整状态（元数据 + 数据窗口） */
static void dump_state(const char *tag, pool_cfg_t *cfg)
{
    printf("\n===== STATE [%s] =====\n", tag);

    if (!cfg || !cfg->bitmap || !cfg->page_owner || !cfg->handle_table) {
        printf("  (uninitialized)\n\n");
        fflush(stdout);
        return;
    }

    uint32_t bitmap_bytes = (cfg->page_count + 7u) / 8u;
    hexdump("bitmap", cfg->bitmap, bitmap_bytes);

    /* 打印前 16 个 page_owner 条目 (32 字节) */
    {
        uint32_t show = cfg->page_count < 16 ? cfg->page_count : 16;
        printf("--- page_owner[0..%u] ---\n", show - 1);
        for (uint32_t i = 0; i < show; i++) {
            printf("  page %2u: ", i);
            if (cfg->page_owner[i] == POOL_PAGE_FREE)
                printf("FREE\n");
            else
                printf("handle_idx=%u\n", cfg->page_owner[i]);
        }
        fflush(stdout);
    }

    dump_handles(cfg);

    /* 数据空间：第一个页的前 32 字节 */
    hexdump("data page 0 [0..31]",
            (const uint8_t *)cfg->data_base,
            cfg->page_size < 32 ? cfg->page_size : 32);

    printf("===== END [%s] =====\n\n", tag);
    fflush(stdout);
}

/*===========================================================================
 * 元数据字节级校验宏
 *===========================================================================*/

/** 检查位图中某位的值 */
#define CHECK_BITMAP(cfg_ptr, page, expected_one_or_zero, msg) do { \
    const pool_cfg_t *_c = (const pool_cfg_t *)(cfg_ptr); \
    uint32_t _p = (page); \
    uint8_t  _byte = _c->bitmap[_p / 8]; \
    int      _bit  = (_byte >> (_p % 8)) & 1; \
    if (_bit != (expected_one_or_zero)) { \
        g_failures++; \
        printf("FAIL [%s]: %s (bitmap[page=%u] expected=%d, got=%d, byte=0x%02x)\n", \
               __func__, msg, _p, (int)(expected_one_or_zero), _bit, _byte); \
        return; \
    } \
} while(0)

/** 检查 page_owner 条目 */
#define CHECK_PAGE_OWNER(cfg_ptr, page, expected_idx_or_FREE, msg) do { \
    const pool_cfg_t *_c = (const pool_cfg_t *)(cfg_ptr); \
    uint32_t _p = (page); \
    uint16_t _v = _c->page_owner[_p]; \
    uint16_t _e = (expected_idx_or_FREE); \
    if (_v != _e) { \
        g_failures++; \
        printf("FAIL [%s]: %s (page_owner[page=%u] expected=0x%04x, got=0x%04x)\n", \
               __func__, msg, _p, _e, _v); \
        return; \
    } \
} while(0)

/** 检查句柄表条目字段 */
#define CHECK_HANDLE_IDX(cfg_ptr, idx, field, expected, msg) do { \
    const pool_cfg_t *_c = (const pool_cfg_t *)(cfg_ptr); \
    uint32_t _i = (idx); \
    if (_i >= _c->handle_count) { \
        g_failures++; \
        printf("FAIL [%s]: %s (handle idx %u out of range)\n", __func__, msg, _i); \
        return; \
    } \
    uint32_t _v = (uint32_t)_c->handle_table[_i].field; \
    uint32_t _e = (uint32_t)(expected); \
    if (_v != _e) { \
        g_failures++; \
        printf("FAIL [%s]: %s (handle[%u]." #field " expected=%u, got=%u)\n", \
               __func__, msg, _i, _e, _v); \
        return; \
    } \
} while(0)

#define CHECK_HANDLE_OWNER_IDX(cfg, idx, expected, msg) \
    CHECK_HANDLE_IDX(cfg, idx, owner_id, expected, msg)
#define CHECK_HANDLE_START_IDX(cfg, idx, expected, msg) \
    CHECK_HANDLE_IDX(cfg, idx, page_start, expected, msg)
#define CHECK_HANDLE_COUNT_IDX(cfg, idx, expected, msg) \
    CHECK_HANDLE_IDX(cfg, idx, page_count, expected, msg)
#define CHECK_HANDLE_LOCK_IDX(cfg, idx, expected, msg) \
    CHECK_HANDLE_IDX(cfg, idx, lock_count, expected, msg)
#define CHECK_HANDLE_GEN_IDX(cfg, idx, expected, msg) \
    CHECK_HANDLE_IDX(cfg, idx, generation, expected, msg)
#define CHECK_HANDLE_FREE_IDX(cfg, idx, msg) \
    CHECK_HANDLE_OWNER_IDX(cfg, idx, POOL_HANDLE_FREE, msg)

/** 检查数据空间中指定偏移处的字节值 */
#define CHECK_DATA_BYTE(cfg_ptr, offset, expected, msg) do { \
    const pool_cfg_t *_c = (const pool_cfg_t *)(cfg_ptr); \
    uint32_t _o = (offset); \
    uint8_t  _v = ((const uint8_t *)_c->data_base)[_o]; \
    uint8_t  _e = (expected); \
    if (_v != _e) { \
        g_failures++; \
        printf("FAIL [%s]: %s (data[%u] expected=0x%02x, got=0x%02x)\n", \
               __func__, msg, _o, _e, _v); \
        return; \
    } \
} while(0)

/** 向数据空间写入测试模式 */
#define WRITE_DATA_PATTERN(cfg, offset, value, msg) do { \
    ((uint8_t *)cfg->data_base)[offset] = (uint8_t)(value); \
} while(0)

/*===========================================================================
 * 测试数据区域（64 页标准配置）
 *===========================================================================*/

#define TEST_PAGE_SIZE   256u
#define TEST_PAGE_COUNT  64u
#define TEST_HANDLE_COUNT 16u
#define TEST_META_SIZE   POOL_META_SIZE(TEST_PAGE_COUNT, TEST_HANDLE_COUNT)
#define TEST_DATA_SIZE   (TEST_PAGE_SIZE * TEST_PAGE_COUNT)

static uint8_t   test_meta[TEST_META_SIZE];
static uint8_t   test_data[TEST_DATA_SIZE];
static pool_cfg_t test_cfg;

static void setup_pool(void)
{
    memset(test_meta, 0, sizeof(test_meta));
    memset(test_data, 0xCD, sizeof(test_data));
    pool_init_err_t err = pool_init(&test_cfg,
                                     test_meta, sizeof(test_meta),
                                     test_data,
                                     TEST_PAGE_SIZE, TEST_PAGE_COUNT,
                                     TEST_HANDLE_COUNT);
    (void)err;
}

/*===========================================================================
 * 测试用例
 *===========================================================================*/

/* --- 初始化 --- */

TEST(init_null_params)
{
    pool_cfg_t cfg;
    uint8_t meta[128], data[256];
    CHECK_EQ(pool_init(NULL, meta, sizeof(meta), data, 64, 4, 4),
             POOL_INIT_ERR_NULL_PARAM, "NULL cfg");
    CHECK_EQ(pool_init(&cfg, NULL, sizeof(meta), data, 64, 4, 4),
             POOL_INIT_ERR_NULL_PARAM, "NULL meta");
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), NULL, 64, 4, 4),
             POOL_INIT_ERR_NULL_PARAM, "NULL data");
}

TEST(init_bad_params)
{
    pool_cfg_t cfg;
    uint8_t meta[128], data[256];
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 0, 4, 4),
             POOL_INIT_ERR_PAGE_SIZE, "page_size=0");
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 3, 4, 4),
             POOL_INIT_ERR_PAGE_SIZE_POW2, "page_size=3 not pow2");
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 64, 0, 4),
             POOL_INIT_ERR_PAGE_COUNT, "page_count=0");
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 64, 3, 4),
             POOL_INIT_ERR_PAGE_COUNT_EVEN, "page_count=3 odd");
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 64, 4, 0),
             POOL_INIT_ERR_HANDLE_COUNT, "handle_count=0");
}

TEST(init_overlap)
{
    pool_cfg_t cfg;
    uint8_t buf[1024];
    /* meta 和 data 指向同一块区域 → 重叠 */
    CHECK_EQ(pool_init(&cfg, buf, 512, buf + 256, 64, 4, 4),
             POOL_INIT_ERR_OVERLAP, "meta overlaps data");
}

TEST(init_already_init)
{
    pool_cfg_t cfg;
    uint8_t meta[POOL_META_SIZE(4, 4)];
    uint8_t data[4 * 64];
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 64, 4, 4),
             POOL_INIT_OK, "first init");
    /* 再次 init → 拒绝 */
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 64, 4, 4),
             POOL_INIT_ERR_ALREADY_INIT, "already init");
}

TEST(init_meta_too_small)
{
    pool_cfg_t cfg;
    uint8_t meta[8], data[256];
    CHECK_EQ(pool_init(&cfg, meta, 8, data, 64, 64, 64),
             POOL_INIT_ERR_META_SIZE, "metadata too small");
}

TEST(init_success)
{
    pool_cfg_t cfg;
    uint8_t meta[TEST_META_SIZE], data[TEST_DATA_SIZE];
    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data,
                        TEST_PAGE_SIZE, TEST_PAGE_COUNT, TEST_HANDLE_COUNT),
             POOL_INIT_OK, "init should succeed");
    CHECK(cfg.metadata_base == meta, "metadata_base");
    CHECK(cfg.data_base == data, "data_base");
    CHECK(cfg.page_size == TEST_PAGE_SIZE, "page_size");
    CHECK(cfg.page_count == TEST_PAGE_COUNT, "page_count");
    CHECK(cfg.handle_count == TEST_HANDLE_COUNT, "handle_count");
    CHECK(cfg.next_user_id == POOL_USER_ID_MIN, "next_user_id start");
    CHECK(cfg.handle_index_bits > 0, "index_bits");

    dump_state("init_success", &cfg);
    /* 初始位图应全 0（所有页空闲） */
    {
        uint32_t bbytes = (cfg.page_count + 7u) / 8u;
        for (uint32_t b = 0; b < bbytes; b++)
            if (cfg.bitmap[b] != 0) {
                printf("FAIL [init_success]: bitmap byte %u not zero (0x%02x)\n", b, cfg.bitmap[b]);
                g_failures++;
                return;
            }
    }
    /* page_owner 应全为 POOL_PAGE_FREE */
    for (uint32_t p = 0; p < cfg.page_count; p++)
        if (cfg.page_owner[p] != POOL_PAGE_FREE) {
            printf("FAIL [init_success]: page_owner[%u] not FREE\n", p);
            g_failures++;
            return;
        }
    /* 所有句柄应为空闲 */
    for (uint32_t i = 0; i < cfg.handle_count; i++)
        if (cfg.handle_table[i].owner_id != POOL_HANDLE_FREE) {
            printf("FAIL [init_success]: handle_table[%u] not free\n", i);
            g_failures++;
            return;
        }
    printf("BITMAP: all-zero, PAGE_OWNER: all-FREE, HANDLE_TABLE: all-FREE -- OK\n");
}

/* --- 打包 --- */

TEST(sys_pack_ok)
{
    setup_pool();
    dump_state("sys_pack_ok-BEFORE", &test_cfg);

    pool_owner_t owner;
    CHECK_EQ(pool_sys_pack(&owner, &test_cfg, 0),
             POOL_SYS_PACK_OK, "sys pack id 0");
    CHECK(owner.cfg == &test_cfg, "cfg");
    CHECK(owner.owner_id == 0, "owner_id");

    CHECK_EQ(pool_sys_pack(&owner, &test_cfg, 127),
             POOL_SYS_PACK_OK, "sys pack id 127");
    CHECK(owner.owner_id == 127, "owner_id 127");

    dump_state("sys_pack_ok-AFTER", &test_cfg);
}

TEST(sys_pack_err)
{
    setup_pool();
    pool_owner_t owner;
    CHECK_EQ(pool_sys_pack(&owner, &test_cfg, 128),
             POOL_SYS_PACK_ERR_ID_RANGE, "id too large");
    CHECK_EQ(pool_sys_pack(NULL, &test_cfg, 0),
             POOL_SYS_PACK_ERR_NULL, "NULL owner");
    CHECK_EQ(pool_sys_pack(&owner, NULL, 0),
             POOL_SYS_PACK_ERR_NULL, "NULL cfg");
}

TEST(user_pack_ok)
{
    setup_pool();
    dump_state("user_pack_ok-BEFORE", &test_cfg);

    pool_owner_t owner;
    CHECK_EQ(pool_user_pack(&owner, &test_cfg),
             POOL_USER_PACK_OK, "first user pack");
    CHECK(owner.owner_id == 128, "first id=128");

    CHECK_EQ(pool_user_pack(&owner, &test_cfg),
             POOL_USER_PACK_OK, "second user pack");
    CHECK(owner.owner_id == 129, "second id=129");

    CHECK(test_cfg.next_user_id == 130, "next_user_id=130");
    dump_state("user_pack_ok-AFTER", &test_cfg);
}

TEST(user_pack_exhaust)
{
    setup_pool();
    test_cfg.next_user_id = POOL_USER_ID_MAX + 1;
    pool_owner_t owner;
    CHECK_EQ(pool_user_pack(&owner, &test_cfg),
             POOL_USER_PACK_ERR_NO_ID, "should exhaust");
}

/* --- 分配 --- */

TEST(alloc_pages_basic)
{
    setup_pool();
    dump_state("alloc_pages_basic-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_pages(&owner, 4, &h), POOL_ALLOC_OK, "alloc 4 pages");
    CHECK(h != 0, "handle non-zero");

    /* 验证元数据：位图前 4 位应为 1 */
    uint32_t idx = h & test_cfg.handle_index_mask;
    CHECK_BITMAP(&test_cfg, 0, 1, "page 0 allocated");
    CHECK_BITMAP(&test_cfg, 1, 1, "page 1 allocated");
    CHECK_BITMAP(&test_cfg, 2, 1, "page 2 allocated");
    CHECK_BITMAP(&test_cfg, 3, 1, "page 3 allocated");
    CHECK_BITMAP(&test_cfg, 4, 0, "page 4 free");
    /* page_owner 前 4 页应指向该句柄索引 */
    CHECK_PAGE_OWNER(&test_cfg, 0, (uint16_t)idx, "page 0 owner");
    CHECK_PAGE_OWNER(&test_cfg, 3, (uint16_t)idx, "page 3 owner");
    /* 句柄表验证 */
    CHECK_HANDLE_OWNER_IDX(&test_cfg, idx, 128, "owner=128");
    CHECK_HANDLE_START_IDX(&test_cfg, idx, 0, "start=0");
    CHECK_HANDLE_COUNT_IDX(&test_cfg, idx, 4, "count=4");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, idx, 0, "lock=0");
    CHECK_HANDLE_GEN_IDX(&test_cfg, idx, 1, "gen=1");

    dump_state("alloc_pages_basic-AFTER", &test_cfg);
    printf("CHECK: bitmap[0..3]=1, page=0/3 owner=%u, handle[%u] owner=128 start=0 count=4 -- OK\n",
           idx, idx);
}

TEST(alloc_pages_full)
{
    setup_pool();
    dump_state("alloc_pages_full-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_pages(&owner, TEST_PAGE_COUNT, &h),
             POOL_ALLOC_OK, "alloc all pages");
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h),
             POOL_ALLOC_ERR_NO_SPACE, "no space left");

    /* 位图应全部为 1 */
    {
        uint32_t bbytes = (test_cfg.page_count + 7u) / 8u;
        for (uint32_t b = 0; b < bbytes; b++)
            if (test_cfg.bitmap[b] != 0xFF &&
                !(b == bbytes - 1 && test_cfg.page_count % 8 != 0)) {
                /* 最后一个字节可能不是全 FF */
                uint32_t bits_in_last = test_cfg.page_count % 8;
                uint8_t exp = bits_in_last ? (uint8_t)((1u << bits_in_last) - 1) : 0xFF;
                if (test_cfg.bitmap[b] != exp) {
                    printf("FAIL [alloc_pages_full]: bitmap[%u]=0x%02x\n", b, test_cfg.bitmap[b]);
                    g_failures++;
                    return;
                }
            }
    }
    printf("BITMAP: all 1s -- OK\n");
    dump_state("alloc_pages_full-AFTER", &test_cfg);
}

TEST(alloc_handles_full)
{
    setup_pool();
    dump_state("alloc_handles_full-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t handles[TEST_HANDLE_COUNT];
    for (uint32_t i = 0; i < TEST_HANDLE_COUNT; i++) {
        CHECK_EQ(pool_alloc_pages(&owner, 1, &handles[i]),
                 POOL_ALLOC_OK, "alloc handle");
    }
    uint32_t h;
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h),
             POOL_ALLOC_ERR_NO_HANDLE, "handle table full");

    /* 应占用前 16 页，后 48 页空闲 */
    for (uint32_t p = 0; p < 16; p++)
        CHECK_BITMAP(&test_cfg, p, 1, "allocated page");
    for (uint32_t p = 16; p < 64; p++)
        CHECK_BITMAP(&test_cfg, p, 0, "free page");

    printf("BITMAP: pages 0..15=1, 16..63=0 -- OK\n");
    dump_state("alloc_handles_full-AFTER", &test_cfg);
}

TEST(alloc_bytes)
{
    setup_pool();
    dump_state("alloc_bytes-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_bytes(&owner, 256, &h), POOL_ALLOC_OK, "256 bytes");
    /* 256 字节 = 1 页 */
    {
        uint32_t idx = h & test_cfg.handle_index_mask;
        CHECK_HANDLE_COUNT_IDX(&test_cfg, idx, 1, "256 bytes -> 1 page");
    }
    CHECK_EQ(pool_alloc_bytes(&owner, 257, &h), POOL_ALLOC_OK, "257 bytes");
    {
        uint32_t idx = h & test_cfg.handle_index_mask;
        CHECK_HANDLE_COUNT_IDX(&test_cfg, idx, 2, "257 bytes -> 2 pages");
    }

    dump_state("alloc_bytes-AFTER", &test_cfg);
    printf("CHECK: first handle 1 page, second handle 2 pages -- OK\n");
}

/* --- 锁/解锁 --- */

TEST(lock_unlock)
{
    setup_pool();
    dump_state("lock_unlock-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 2, &h);
    uint32_t idx = h & test_cfg.handle_index_mask;

    void *addr1, *addr2;
    CHECK_EQ(pool_lock(&owner, h, &addr1), POOL_LOCK_OK, "lock first");
    CHECK(addr1 != NULL, "addr non-null");
    CHECK(addr1 == test_data, "addr at data start");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, idx, 1, "lock_count=1");

    CHECK_EQ(pool_lock(&owner, h, &addr2), POOL_LOCK_OK, "lock second");
    CHECK(addr1 == addr2, "same address each lock");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, idx, 2, "lock_count=2");

    CHECK_EQ(pool_unlock(&owner, h), POOL_UNLOCK_OK, "unlock");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, idx, 1, "lock_count=1");
    CHECK_EQ(pool_unlock(&owner, h), POOL_UNLOCK_OK, "unlock last");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, idx, 0, "lock_count=0");

    CHECK_EQ(pool_unlock(&owner, h), POOL_UNLOCK_ERR_NOT_LOCKED, "over unlock");

    dump_state("lock_unlock-AFTER", &test_cfg);
    printf("CHECK: lock_count 0->1->2->1->0 -- OK\n");
}

TEST(lock_invalid)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);

    void *addr;
    CHECK_EQ(pool_lock(&owner, 0xDEADBEEF, &addr), POOL_LOCK_ERR_INVALID, "invalid handle");
    uint32_t h;
    pool_alloc_pages(&owner, 1, &h);
    pool_lock(&owner, h, &addr);

    pool_owner_t other;
    pool_user_pack(&other, &test_cfg);
    CHECK_EQ(pool_lock(&other, h, &addr), POOL_LOCK_ERR_OWNER, "cross owner lock");
}

/* --- 释放 --- */

TEST(free_basic)
{
    setup_pool();
    dump_state("free_basic-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 4, &h);
    uint32_t idx = h & test_cfg.handle_index_mask;

    CHECK_EQ(pool_free(&owner, h), POOL_FREE_OK, "free");
    void *addr;
    CHECK_EQ(pool_lock(&owner, h, &addr), POOL_LOCK_ERR_INVALID, "stale handle");

    /* 释放后：页应空闲，句柄应空闲，generation 保留 */
    CHECK_HANDLE_FREE_IDX(&test_cfg, idx, "handle slot free");
    for (uint32_t p = 0; p < 4; p++)
        CHECK_BITMAP(&test_cfg, p, 0, "freed page");
    CHECK_HANDLE_GEN_IDX(&test_cfg, idx, 1, "generation preserved");

    dump_state("free_basic-AFTER", &test_cfg);
    printf("CHECK: pages 0..3 freed, handle free, gen=1 -- OK\n");
}

TEST(free_while_locked)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 1, &h);
    void *addr;
    pool_lock(&owner, h, &addr);
    CHECK_EQ(pool_free(&owner, h), POOL_FREE_ERR_LOCKED, "free while locked");
}

/* --- 句柄复用 --- */

TEST(handle_reuse)
{
    setup_pool();
    dump_state("handle_reuse-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1;
    pool_alloc_pages(&owner, 1, &h1);
    uint32_t idx1 = h1 & test_cfg.handle_index_mask;
    CHECK_HANDLE_GEN_IDX(&test_cfg, idx1, 1, "first gen=1");

    pool_free(&owner, h1);

    uint32_t h2;
    pool_alloc_pages(&owner, 1, &h2);
    uint32_t idx2 = h2 & test_cfg.handle_index_mask;
    CHECK(idx1 == idx2, "same index reused");
    CHECK(h1 != h2, "handle differs (generation changed)");
    CHECK_HANDLE_GEN_IDX(&test_cfg, idx2, 2, "second gen=2");

    dump_state("handle_reuse-AFTER", &test_cfg);
    printf("CHECK: same slot, gen 1->2, handles differ -- OK\n");
}

/* --- 碎片整理 --- */

TEST(defrag_simple)
{
    setup_pool();
    dump_state("defrag_simple-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2, h3;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    pool_alloc_pages(&owner, 4, &h3);
    uint32_t i1 = h1 & test_cfg.handle_index_mask;
    uint32_t i2 = h2 & test_cfg.handle_index_mask;
    uint32_t i3 = h3 & test_cfg.handle_index_mask;

    /* 在数据区写模式以便验证移动后数据一致性 */
    for (uint32_t i = 0; i < 4 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = (uint8_t)(i1 + 0xA0);
    for (uint32_t i = 4 * TEST_PAGE_SIZE; i < 8 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = (uint8_t)(i2 + 0xA0);
    for (uint32_t i = 8 * TEST_PAGE_SIZE; i < 12 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = (uint8_t)(i3 + 0xA0);

    pool_free(&owner, h2);
    dump_state("defrag_simple-AFTER-FREE", &test_cfg);

    CHECK_EQ(pool_defrag(&owner), POOL_DEFRAG_OK, "defrag");

    pool_handle_entry_t *e1 = &test_cfg.handle_table[i1];
    pool_handle_entry_t *e3 = &test_cfg.handle_table[i3];
    CHECK(e1->page_start == 0, "h1 still at 0");
    CHECK(e3->page_start == 4, "h3 moved to page 4");

    /* 验证数据已正确移动 */
    CHECK_DATA_BYTE(&test_cfg, 0,          (uint8_t)(i1 + 0xA0), "h1 data intact at 0");
    CHECK_DATA_BYTE(&test_cfg, 4*256 - 1,  (uint8_t)(i1 + 0xA0), "h1 data intact end");
    CHECK_DATA_BYTE(&test_cfg, 4*256,      (uint8_t)(i3 + 0xA0), "h3 data at page 4");
    CHECK_DATA_BYTE(&test_cfg, 8*256 - 1,  (uint8_t)(i3 + 0xA0), "h3 data intact end");

    dump_state("defrag_simple-AFTER", &test_cfg);
    printf("CHECK: h3 moved 8->4, data verified -- OK\n");
}

TEST(defrag_no_move_locked)
{
    setup_pool();
    dump_state("defrag_no_move_locked-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    uint32_t i2 = h2 & test_cfg.handle_index_mask;

    void *addr;
    pool_lock(&owner, h2, &addr);
    pool_free(&owner, h1);

    pool_defrag(&owner);

    pool_handle_entry_t *e2 = &test_cfg.handle_table[i2];
    CHECK(e2->page_start == 4, "locked h2 stays at page 4");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, i2, 1, "h2 still locked");

    dump_state("defrag_no_move_locked-AFTER", &test_cfg);
    printf("CHECK: h2 locked, stays at page 4 -- OK\n");
}

/** 级联碎片整理：3 块交替，验证多次移动 */
TEST(defrag_cascade)
{
    setup_pool();
    dump_state("defrag_cascade-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t ha, hb, hc, hd, he;
    pool_alloc_pages(&owner, 4, &ha);  /* pages 0..3  */
    pool_alloc_pages(&owner, 4, &hb);  /* pages 4..7  */
    pool_alloc_pages(&owner, 4, &hc);  /* pages 8..11 */
    pool_alloc_pages(&owner, 4, &hd);  /* pages 12..15 */
    pool_alloc_pages(&owner, 4, &he);  /* pages 16..19 */

    uint32_t ic = hc & test_cfg.handle_index_mask;
    uint32_t ie = he & test_cfg.handle_index_mask;

    /* 释放 hb 和 hd → 产生两个 4 页空隙 */
    pool_free(&owner, hb);
    pool_free(&owner, hd);

    dump_state("defrag_cascade-AFTER-FREE", &test_cfg);

    CHECK_EQ(pool_defrag(&owner), POOL_DEFRAG_OK, "cascade defrag");

    /* defrag 从左到右填充空隙：
     *   空隙1: pages 4..7 ← hc (原 8..11)
     *   空隙2: pages 8..11 ← he (原 16..19)
     *   最终: ha 0..3, hc 4..7, he 8..11, 其余空闲 */
    CHECK(test_cfg.handle_table[ic].page_start == 4,  "hc moved to page 4");
    CHECK(test_cfg.handle_table[ie].page_start == 8,  "he moved to page 8");

    for (uint32_t p = 0;  p < 4;  p++) CHECK_BITMAP(&test_cfg, p, 1, "ha pages");
    for (uint32_t p = 4;  p < 8;  p++) CHECK_BITMAP(&test_cfg, p, 1, "hc pages");
    for (uint32_t p = 8;  p < 12; p++) CHECK_BITMAP(&test_cfg, p, 1, "he pages");
    for (uint32_t p = 12; p < 64; p++) CHECK_BITMAP(&test_cfg, p, 0, "free after compact");

    dump_state("defrag_cascade-AFTER", &test_cfg);
    printf("CHECK: hc->page4, he->page8, bitmap compact -- OK\n");
}

/* --- 改变大小 --- */

TEST(resize_shrink)
{
    setup_pool();
    dump_state("resize_shrink-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 8, &h);
    uint32_t idx = h & test_cfg.handle_index_mask;
    pool_handle_entry_t *e = &test_cfg.handle_table[idx];

    /* 写数据标记 */
    for (uint32_t i = 0; i < 8 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = (uint8_t)(i % 256);

    CHECK_EQ(pool_resize(&owner, h, 4), POOL_RESIZE_OK, "shrink");
    CHECK(e->page_count == 4, "page_count=4");
    CHECK(e->page_start == 0, "stays at 0");

    /* 第 4..7 页应已释放 */
    CHECK_BITMAP(&test_cfg, 4, 0, "page 4 freed after shrink");
    CHECK_BITMAP(&test_cfg, 7, 0, "page 7 freed after shrink");
    /* 前 4 页数据不变 */
    CHECK_DATA_BYTE(&test_cfg, 0,       0,   "data[0] preserved");
    CHECK_DATA_BYTE(&test_cfg, 4*256-1, 255, "data[4*256-1] preserved");

    uint32_t h2;
    CHECK_EQ(pool_alloc_pages(&owner, 4, &h2), POOL_ALLOC_OK, "alloc freed pages");
    /* 新分配应使用刚释放的第 4..7 页 */
    uint32_t i2 = h2 & test_cfg.handle_index_mask;
    CHECK_HANDLE_START_IDX(&test_cfg, i2, 4, "h2 reuses freed pages 4..7");
    CHECK_BITMAP(&test_cfg, 4, 1, "page 4 reallocated");

    dump_state("resize_shrink-AFTER", &test_cfg);
    printf("CHECK: shrink 8->4, pages 4..7 freed then reused -- OK\n");
}

TEST(resize_expand_inplace)
{
    setup_pool();
    dump_state("resize_expand_inplace-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 2, &h);
    uint32_t idx = h & test_cfg.handle_index_mask;

    CHECK_EQ(pool_resize(&owner, h, 6), POOL_RESIZE_OK, "expand in-place");
    pool_handle_entry_t *e = &test_cfg.handle_table[idx];
    CHECK(e->page_count == 6, "page_count=6");
    CHECK(e->page_start == 0, "still at page 0");
    CHECK_BITMAP(&test_cfg, 2, 1, "page 2 now allocated");
    CHECK_BITMAP(&test_cfg, 5, 1, "page 5 now allocated");
    CHECK_BITMAP(&test_cfg, 6, 0, "page 6 still free");
    CHECK_HANDLE_COUNT_IDX(&test_cfg, idx, 6, "count=6");

    dump_state("resize_expand_inplace-AFTER", &test_cfg);
    printf("CHECK: expand 2->6 in-place, pages 2..5 now allocated -- OK\n");
}

TEST(resize_move_self)
{
    /* h2 锁定 → must_move_self=true; h1 未锁 → 成功移动自身到新位置 */
    setup_pool();
    dump_state("resize_move_self-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    uint32_t i1 = h1 & test_cfg.handle_index_mask;
    uint32_t i2 = h2 & test_cfg.handle_index_mask;

    /* 写数据标记 */
    for (uint32_t i = 0; i < 4 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = 0xA1;
    for (uint32_t i = 4 * TEST_PAGE_SIZE; i < 8 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = 0xA2;

    void *addr;
    pool_lock(&owner, h2, &addr);  /* h2 锁定，h1 不能原地扩展 */

    dump_state("resize_move_self-BEFORE-RESIZE", &test_cfg);

    CHECK_EQ(pool_resize(&owner, h1, 8), POOL_RESIZE_OK, "move self");

    /* h1 应在新位置（不是原地） */
    pool_handle_entry_t *e1 = &test_cfg.handle_table[i1];
    CHECK(e1->page_count == 8, "h1 now 8 pages");
    CHECK(e1->page_start != 0, "h1 moved away from page 0");
    CHECK(e1->page_start == 8, "h1 at page 8 (first free block)");
    CHECK_HANDLE_START_IDX(&test_cfg, i2, 4, "h2 unchanged");

    /* 旧页 0..3 应已释放 */
    CHECK_BITMAP(&test_cfg, 0, 0, "old h1 page 0 freed");
    CHECK_BITMAP(&test_cfg, 3, 0, "old h1 page 3 freed");
    /* 新位置页 8..15 应已分配 */
    CHECK_BITMAP(&test_cfg, 8, 1, "new h1 page 8");
    CHECK_BITMAP(&test_cfg, 15, 1, "new h1 page 15");
    /* h2 仍在 4..7 */
    CHECK_BITMAP(&test_cfg, 4, 1, "h2 page 4");

    /* 数据一致性 */
    CHECK_DATA_BYTE(&test_cfg, 0,           0xA1, "h1 data moved to new location");
    CHECK_DATA_BYTE(&test_cfg, 4 * 256,     0xA2, "h2 data preserved");

    dump_state("resize_move_self-AFTER", &test_cfg);
    printf("CHECK: h1 moved 0->8 pages, 8 pages, h2 stays at 4 -- OK\n");
}

TEST(resize_move_self_locked)
{
    setup_pool();
    dump_state("resize_move_self_locked-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    uint32_t i1 = h1 & test_cfg.handle_index_mask;

    void *addr;
    pool_lock(&owner, h1, &addr);
    pool_lock(&owner, h2, &addr);
    CHECK_EQ(pool_resize(&owner, h1, 8), POOL_RESIZE_ERR_LOCKED, "cannot move self while locked");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, i1, 1, "h1 still locked");

    dump_state("resize_move_self_locked-AFTER", &test_cfg);
    printf("CHECK: ERR_LOCKED returned, h1 unchanged -- OK\n");
}

TEST(resize_move_following)
{
    /* h2 未锁、following_total(4) <= cur_pages(4) → 移动后方句柄 */
    setup_pool();
    dump_state("resize_move_following-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    uint32_t i1 = h1 & test_cfg.handle_index_mask;
    uint32_t i2 = h2 & test_cfg.handle_index_mask;

    /* 写数据标记 */
    for (uint32_t i = 0; i < 4 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = 0xB1;
    for (uint32_t i = 4 * TEST_PAGE_SIZE; i < 8 * TEST_PAGE_SIZE; i++)
        ((uint8_t *)test_data)[i] = 0xB2;

    dump_state("resize_move_following-BEFORE-RESIZE", &test_cfg);

    CHECK_EQ(pool_resize(&owner, h1, 8), POOL_RESIZE_OK, "move following");

    /* h1 原地扩展至 0..7 */
    CHECK_HANDLE_START_IDX(&test_cfg, i1, 0, "h1 still at 0");
    CHECK_HANDLE_COUNT_IDX(&test_cfg, i1, 8, "h1 now 8 pages");
    /* h2 被移走，应在其他位置 */
    pool_handle_entry_t *e2 = &test_cfg.handle_table[i2];
    CHECK(e2->page_start != 4, "h2 moved away from page 4");
    CHECK(e2->page_count == 4, "h2 still 4 pages");
    CHECK_HANDLE_LOCK_IDX(&test_cfg, i2, 0, "h2 not locked");
    /* h1 占用 0..7（h2 被移走，h1 原地扩展） */
    CHECK_BITMAP(&test_cfg, 0, 1, "h1 page 0");
    CHECK_BITMAP(&test_cfg, 7, 1, "h1 page 7");
    /* h2 的旧页 4..7 现属于 h1 */
    CHECK_PAGE_OWNER(&test_cfg, 4, (uint16_t)i1, "page 4 now owned by h1");

    /* 数据一致性 */
    CHECK_DATA_BYTE(&test_cfg, 0,       0xB1, "h1 data at page 0");
    CHECK_DATA_BYTE(&test_cfg, 4*256,   0xB2, "h2 data at new location");

    dump_state("resize_move_following-AFTER", &test_cfg);
    printf("CHECK: h1 expands 0..7 in-place, h2 moved away, page 4 now h1 -- OK\n");
}

TEST(resize_no_space)
{
    setup_pool();
    dump_state("resize_no_space-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);

    uint32_t remaining = 64 - 8;
    uint32_t h3;
    pool_alloc_pages(&owner, remaining, &h3);

    void *addr;
    pool_lock(&owner, h2, &addr);
    CHECK_EQ(pool_resize(&owner, h1, 8), POOL_RESIZE_ERR_NO_SPACE, "no room to move");

    /* 状态不变 */
    CHECK_HANDLE_START_IDX(&test_cfg, h1 & test_cfg.handle_index_mask, 0, "h1 still at 0");
    CHECK_HANDLE_COUNT_IDX(&test_cfg, h1 & test_cfg.handle_index_mask, 4, "h1 still 4 pages");

    dump_state("resize_no_space-AFTER", &test_cfg);
    printf("CHECK: ERR_NO_SPACE, h1 unchanged -- OK\n");
}

/* --- 释放全部 --- */

TEST(free_all_forced)
{
    setup_pool();
    dump_state("free_all_forced-BEFORE", &test_cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 2, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    uint32_t i1 = h1 & test_cfg.handle_index_mask;
    uint32_t i2 = h2 & test_cfg.handle_index_mask;
    void *addr;
    pool_lock(&owner, h2, &addr);

    CHECK_EQ(pool_free_all(&owner, true), POOL_FREE_ALL_OK, "forced free all");
    CHECK_EQ(pool_lock(&owner, h1, &addr), POOL_LOCK_ERR_INVALID, "h1 gone");
    CHECK_EQ(pool_lock(&owner, h2, &addr), POOL_LOCK_ERR_INVALID, "h2 gone");

    /* 所有页应空闲 */
    for (uint32_t p = 0; p < 64; p++)
        CHECK_BITMAP(&test_cfg, p, 0, "all pages freed");
    CHECK_HANDLE_FREE_IDX(&test_cfg, i1, "h1 slot free");
    CHECK_HANDLE_FREE_IDX(&test_cfg, i2, "h2 slot free");

    dump_state("free_all_forced-AFTER", &test_cfg);
    printf("CHECK: all pages freed, handles invalid -- OK\n");
}

TEST(free_all_unforced_locked)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 1, &h);
    void *addr;
    pool_lock(&owner, h, &addr);
    CHECK_EQ(pool_free_all(&owner, false), POOL_FREE_ALL_ERR_LOCKED, "unforced with lock");
}

/* --- 边界测试 --- */

/* pool with minimum pages (2 — 强制偶数) */
TEST(page_count_min)
{
    uint8_t meta[POOL_META_SIZE(2, 4)];
    uint8_t data[2 * 256];
    pool_cfg_t cfg;

    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 256, 2, 4),
             POOL_INIT_OK, "init 2 pages");
    dump_state("page_count_min-INIT", &cfg);

    pool_owner_t owner;
    pool_user_pack(&owner, &cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h), POOL_ALLOC_OK, "alloc 1 page");
    CHECK(h != 0, "handle non-zero");

    void *addr;
    CHECK_EQ(pool_lock(&owner, h, &addr), POOL_LOCK_OK, "lock");
    CHECK(addr == data, "addr == data_base");

    CHECK_EQ(pool_unlock(&owner, h), POOL_UNLOCK_OK, "unlock");
    CHECK_EQ(pool_free(&owner, h), POOL_FREE_OK, "free");

    /* 再分配应成功（复用）*/
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h), POOL_ALLOC_OK, "re-alloc");
    CHECK(h != 0, "handle non-zero after realloc");

    /* 分配全部 2 页 → 成功 */
    pool_free(&owner, h);
    CHECK_EQ(pool_alloc_pages(&owner, 2, &h), POOL_ALLOC_OK, "alloc all 2 pages");

    /* 分配第 3 页 → 失败 */
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h), POOL_ALLOC_ERR_NO_SPACE, "no space for 3rd page");

    dump_state("page_count_min-AFTER", &cfg);
    printf("CHECK: 2-page min pool: alloc/lock/unlock/free/realloc/full OK -- OK\n");
}

/* pool with only 1 handle slot */
TEST(handle_count_1)
{
    uint8_t meta[POOL_META_SIZE(64, 1)];
    uint8_t data[64 * 256];
    pool_cfg_t cfg;

    CHECK_EQ(pool_init(&cfg, meta, sizeof(meta), data, 256, 64, 1),
             POOL_INIT_OK, "init 1 handle");
    dump_state("handle_count_1-INIT", &cfg);

    /* index_bits 应为 1（handle_count=1 → max_idx=0 → bits=1）*/
    CHECK(cfg.handle_index_bits == 1, "index_bits=1 for 1 handle");
    CHECK(cfg.handle_index_mask == 1, "index_mask=1");

    pool_owner_t owner;
    pool_user_pack(&owner, &cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h), POOL_ALLOC_OK, "alloc 1 handle");

    uint32_t idx_mask = cfg.handle_index_mask;
    uint32_t idx = h & idx_mask;
    CHECK(idx == 0, "handle index must be 0");

    /* 再分配应失败：句柄表满 */
    CHECK_EQ(pool_alloc_pages(&owner, 1, &h), POOL_ALLOC_ERR_NO_HANDLE, "no handle slot left");

    dump_state("handle_count_1-AFTER", &cfg);
    printf("CHECK: 1-handle pool: alloc + table-full err -- OK\n");
}

/* --- 多使用者交叉操作 --- */
TEST(multi_owner)
{
    setup_pool();
    dump_state("multi_owner-BEFORE", &test_cfg);

    pool_owner_t a, b;
    pool_user_pack(&a, &test_cfg);  /* id=128 */
    pool_user_pack(&b, &test_cfg);  /* id=129 */

    /* a 分配 4 页，b 分配 4 页 */
    uint32_t ha, hb;
    pool_alloc_pages(&a, 4, &ha);       /* a gets pages 0..3 */
    pool_alloc_pages(&b, 4, &hb);       /* b gets pages 4..7 */
    uint32_t ia = ha & test_cfg.handle_index_mask;
    uint32_t ib = hb & test_cfg.handle_index_mask;

    CHECK_HANDLE_OWNER_IDX(&test_cfg, ia, 128, "a's handle");
    CHECK_HANDLE_OWNER_IDX(&test_cfg, ib, 129, "b's handle");

    /* a 不能解锁 b 的句柄 */
    CHECK_EQ(pool_unlock(&a, hb), POOL_UNLOCK_ERR_OWNER, "a cannot unlock b's handle");
    /* b 不能释放 a 的句柄 */
    CHECK_EQ(pool_free(&b, ha), POOL_FREE_ERR_OWNER, "b cannot free a's handle");

    /* a 能正常使用自己的句柄 */
    void *addr;
    CHECK_EQ(pool_lock(&a, ha, &addr), POOL_LOCK_OK, "a locks own");
    CHECK(addr == test_data, "a data at 0");
    CHECK_EQ(pool_unlock(&a, ha), POOL_UNLOCK_OK, "a unlocks");   /* 解锁后 free_all */

    /* b 能正常使用自己的句柄 */
    CHECK_EQ(pool_lock(&b, hb, &addr), POOL_LOCK_OK, "b locks own");
    CHECK(addr == test_data + 4 * TEST_PAGE_SIZE, "b data at page 4");
    CHECK_EQ(pool_unlock(&b, hb), POOL_UNLOCK_OK, "b unlocks");

    /* free_all(a) 只释放 a 的句柄，b 不受影响 */
    CHECK_EQ(pool_free_all(&a, false), POOL_FREE_ALL_OK, "free_all a");
    CHECK_EQ(pool_lock(&a, ha, &addr), POOL_LOCK_ERR_INVALID, "ha invalid after free_all");
    /* b 的句柄仍在 */
    CHECK_EQ(pool_lock(&b, hb, &addr), POOL_LOCK_OK, "hb still valid");

    CHECK_HANDLE_FREE_IDX(&test_cfg, ia, "a's slot freed");
    CHECK_HANDLE_OWNER_IDX(&test_cfg, ib, 129, "b's slot still owned");

    /* a 释放后的页面 0..3 可被 b 使用 */
    uint32_t hb2;
    CHECK_EQ(pool_alloc_pages(&b, 4, &hb2), POOL_ALLOC_OK, "b reuses freed pages");
    {
        uint32_t ib2 = hb2 & test_cfg.handle_index_mask;
        CHECK_HANDLE_START_IDX(&test_cfg, ib2, 0, "b's new handle at page 0");
    }

    dump_state("multi_owner-AFTER", &test_cfg);
    printf("CHECK: cross-owner isolation, free_all a leaves b intact -- OK\n");
}

/* --- 补充覆盖 --- */

/* resize 移动后方句柄时池满 → NO_SPACE */
TEST(resize_move_following_nospace)
{
    /* 未锁定后方句柄，但选择移动后方时池满无处可移 */
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2;
    pool_alloc_pages(&owner, 4, &h1);
    pool_alloc_pages(&owner, 4, &h2);
    /* 填满剩余空间 */
    uint32_t h3;
    pool_alloc_pages(&owner, 64 - 8, &h3);

    /* h2 未锁 → must_move_self=false → 移动后方，但无空间 */
    CHECK_EQ(pool_resize(&owner, h1, 8), POOL_RESIZE_ERR_NO_SPACE, "no space for following");
}
/* 保持相同大小 → OK，无变化 */
TEST(resize_same_size)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 4, &h);
    uint32_t idx = h & test_cfg.handle_index_mask;
    CHECK_EQ(pool_resize(&owner, h, 4), POOL_RESIZE_OK, "same size");
    CHECK_HANDLE_COUNT_IDX(&test_cfg, idx, 4, "unchanged");
}

/* 按字节分配 0 字节 → ERR_SIZE */
TEST(alloc_bytes_zero_size)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    CHECK_EQ(pool_alloc_bytes(&owner, 0, &h), POOL_ALLOC_ERR_SIZE, "0 bytes illegal");
}

/* defrag of completely full pool → OK, no change */
TEST(defrag_full_pool)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h;
    pool_alloc_pages(&owner, 64, &h);
    void *addr;
    pool_lock(&owner, h, &addr);
    CHECK_EQ(pool_defrag(&owner), POOL_DEFRAG_OK, "defrag full pool");
}

/* free_all with no handles → OK */
TEST(free_all_no_handles)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    /* 无分配 → free_all 应正常返回 */
    CHECK_EQ(pool_free_all(&owner, false), POOL_FREE_ALL_OK, "free_all empty");
    CHECK_EQ(pool_free_all(&owner, true), POOL_FREE_ALL_OK, "forced free_all empty");
}

/* 多使用者: A free_all 后 B 交叉使用 A 释放的页 */
TEST(multi_owner_cross_reuse)
{
    setup_pool();
    pool_owner_t a, b;
    pool_user_pack(&a, &test_cfg);
    pool_user_pack(&b, &test_cfg);
    uint32_t ha;
    pool_alloc_pages(&a, 8, &ha);
    /* 写数据标记 */
    {
        void *addr;
        pool_lock(&a, ha, &addr);
        memset(addr, 0xCA, 8 * TEST_PAGE_SIZE);
        pool_unlock(&a, ha);
    }
    pool_free_all(&a, false);

    /* B 分配 A 释放的页面，验证数据区域可读写 */
    uint32_t hb;
    pool_alloc_pages(&b, 8, &hb);
    void *addr;
    pool_lock(&b, hb, &addr);
    CHECK(addr == test_data, "b reuses same base");
    memset(addr, 0xFE, 8 * TEST_PAGE_SIZE);
    pool_unlock(&b, hb);
}

/* generation 绕回：65535 → 0 → 重置为 1 */
TEST(generation_wrap)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);

    /* 分配并释放一个句柄，获取其槽位索引 */
    uint32_t h;
    pool_alloc_pages(&owner, 1, &h);   /* gen=1 */
    uint32_t idx = h & test_cfg.handle_index_mask;
    pool_free(&owner, h);

    /* 手动设置 generation 为 65535 */
    test_cfg.handle_table[idx].generation = 65535;

    /* 重新分配 → generation++ → 0 → 重置为 1 */
    pool_alloc_pages(&owner, 1, &h);
    CHECK_HANDLE_GEN_IDX(&test_cfg, idx, 1, "gen wrapped 65535→1 (not 0)");

    /* 验证句柄有效 */
    void *addr;
    CHECK_EQ(pool_lock(&owner, h, &addr), POOL_LOCK_OK, "handle valid after wrap");
}

/* defrag 混合块大小 + 空隙 */
TEST(defrag_mixed_blocks)
{
    setup_pool();
    pool_owner_t owner;
    pool_user_pack(&owner, &test_cfg);
    uint32_t h1, h2, h3, h4;
    pool_alloc_pages(&owner, 8, &h1);   /* 0..7   */
    pool_alloc_pages(&owner, 2, &h2);   /* 8..9   */
    pool_alloc_pages(&owner, 4, &h3);   /* 10..13 */
    pool_alloc_pages(&owner, 6, &h4);   /* 14..19 */

    uint32_t i1 = h1 & test_cfg.handle_index_mask;
    uint32_t i2 = h2 & test_cfg.handle_index_mask;
    uint32_t i4 = h4 & test_cfg.handle_index_mask;

    pool_free(&owner, h2);  /* 空隙 8..9 (2页) */
    pool_free(&owner, h3);  /* 空隙 10..13 (4页) */

    pool_defrag(&owner);

    /* h1 不变，h4 应前移到第一个空隙 8..9 */
    CHECK(test_cfg.handle_table[i1].page_start == 0, "h1 at 0");
    CHECK(test_cfg.handle_table[i4].page_start == 8, "h4 moves to page 8");
}

/*===========================================================================
 * main
 *===========================================================================*/

int main(void)
{
    printf("=== pool tests (with hexdump) ===\n\n");

    RUN(init_null_params);
    RUN(init_bad_params);
    RUN(init_overlap);
    RUN(init_already_init);
    RUN(init_meta_too_small);
    RUN(init_success);

    RUN(sys_pack_ok);
    RUN(sys_pack_err);
    RUN(user_pack_ok);
    RUN(user_pack_exhaust);

    RUN(alloc_pages_basic);
    RUN(alloc_pages_full);
    RUN(alloc_handles_full);
    RUN(alloc_bytes);

    RUN(lock_unlock);
    RUN(lock_invalid);

    RUN(free_basic);
    RUN(free_while_locked);

    RUN(handle_reuse);

    RUN(defrag_simple);
    RUN(defrag_no_move_locked);
    RUN(defrag_cascade);

    RUN(resize_shrink);
    RUN(resize_expand_inplace);
    RUN(resize_move_self);
    RUN(resize_move_self_locked);
    RUN(resize_move_following);
    RUN(resize_no_space);

    RUN(free_all_forced);
    RUN(free_all_unforced_locked);

    RUN(page_count_min);
    RUN(handle_count_1);

    RUN(multi_owner);

    /* --- 补充覆盖 --- */
    RUN(resize_move_following_nospace);
    RUN(resize_same_size);
    RUN(alloc_bytes_zero_size);
    RUN(defrag_full_pool);
    RUN(free_all_no_handles);
    RUN(multi_owner_cross_reuse);
    RUN(generation_wrap);
    RUN(defrag_mixed_blocks);

    printf("\n--- %d failures ---\n", g_failures);
    return g_failures ? 1 : 0;
}
