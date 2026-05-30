/**
 * @file pool.h
 * @brief 基于页的内存管理系统 (Page-based Memory Pool)
 *
 * 专为单片机设计，纯C99，不使用任何系统调用。
 * 所有内存由用户在初始化时提供：
 *   - 元数据空间：存放位图、页属主映射、句柄表
 *   - 数据空间：完全交给用户读写，大小为 page_size × page_count
 *
 * 使用流程：
 *   1. pool_init()      — 初始化池配置
 *   2. pool_sys_pack()  — 系统打包（手动指定ID 0~127）
 *      或 pool_user_pack() — 用户打包（自动分配ID 128~25565）
 *   3. pool_alloc_*()   — 分配空间
 *   4. pool_lock()      — 锁定句柄获取地址
 *   5. pool_unlock()    — 解锁句柄
 *   6. pool_free()      — 释放句柄
 *   7. pool_defrag()    — 碎片整理
 *   8. pool_resize()    — 改变句柄大小
 *   9. pool_free_all()  — 释放使用者所有句柄
 */

#ifndef POOL_H
#define POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*===========================================================================
 * 插件字段扩展 — 编译期零开销
 *===========================================================================*/
#ifdef POOL_PLUGINS_ENABLED
#define POOL_FIELD_DEFS_ONLY
#include "pool_plugin_config.h"
#undef POOL_FIELD_DEFS_ONLY
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * 配置宏
 *===========================================================================*/

/**
 * @brief 计算元数据所需空间大小（含对齐填充）
 * @param page_count   总页数
 * @param handle_count 最大句柄数
 * @return 元数据所需字节数（上限，含最大可能对齐填充）
 *
 * 元数据布局（带对齐）:
 *   [0] 页分配位图          ceil(page_count/8) 字节
 *   [+] 对齐填充            最多 (alignof(uint16_t)-1) 字节
 *   [1] 页→句柄索引映射     page_count * sizeof(uint16_t) 字节
 *   [+] 对齐填充            最多 (alignof(entry_t)-1) 字节
 *   [2] 句柄表             handle_count * sizeof(pool_handle_entry_t) 字节
 */
#define POOL_META_SIZE(page_count, handle_count) \
    (  sizeof(uint32_t)                                      /* 初始化标记 */ \
     + ((page_count) + 7u) / 8u                              /* bitmap */ \
     + (sizeof(uint16_t) - 1u)                               /* align pad (2B) */ \
     + (page_count) * sizeof(uint16_t)                       /* page_owner */ \
     + (sizeof(uint32_t) - 1u)                               /* align pad (4B) */ \
     + (handle_count) * sizeof(pool_handle_entry_t) )        /* handle_table */

/*===========================================================================
 * 常量定义
 *===========================================================================*/

/** 系统使用者ID范围: 0 ~ POOL_SYS_ID_MAX */
#define POOL_SYS_ID_MAX      127u

/** 用户使用者ID范围: POOL_USER_ID_MIN ~ POOL_USER_ID_MAX */
#define POOL_USER_ID_MIN     128u
#define POOL_USER_ID_MAX     25565u

/** 空闲句柄/空闲页的标记值 */
#define POOL_HANDLE_FREE     0xFFFFu
#define POOL_PAGE_FREE       0xFFFFu

/** 初始化标记（写入元数据区首 4 字节，用于检测重复初始化） */
#define POOL_INIT_MAGIC      0x504F4F4Cu  /* "POOL" */

/*===========================================================================
 * 内部结构体 (前向声明，由实现定义细节)
 *===========================================================================*/

/** @brief 句柄条目（内部结构，用户不应直接访问） */
typedef struct {
    uint16_t owner_id;      /**< 所属使用者ID，POOL_HANDLE_FREE 表示空闲 */
    uint32_t page_start;    /**< 起始页索引 */
    uint32_t page_count;    /**< 占用页数 */
    uint16_t lock_count;    /**< 锁定计数 */
    uint16_t generation;    /**< 次代数，用于句柄验证 */
#ifdef POOL_HANDLE_ENTRY_PLUGIN_FIELDS
    POOL_HANDLE_ENTRY_PLUGIN_FIELDS
#endif
} pool_handle_entry_t;

/** @brief 池配置 */
typedef struct {
    /* --- 用户提供的原始参数 --- */
    void     *metadata_base;    /**< 元数据空间基址 */
    size_t    metadata_size;    /**< 元数据空间大小 */
    void     *data_base;        /**< 数据空间基址（用户读写区） */
    uint32_t  page_size;        /**< 页大小（字节） */
    uint32_t  page_count;       /**< 总页数 */
    uint32_t  handle_count;     /**< 最大句柄数 */

    /* --- 内部管理字段（由 pool_init 设置） --- */
    uint32_t  next_user_id;     /**< 下一个自动分配的用户ID */
    uint32_t  next_handle_hint; /**< 空闲句柄查找提示（内部） */
    /* 内部指针 */
    uint8_t  *bitmap;           /**< 页分配位图指针 */
    uint16_t *page_owner;       /**< 页→句柄索引映射指针 */
    pool_handle_entry_t *handle_table; /**< 句柄表指针 */
    uint32_t  handle_index_mask;/**< 句柄索引掩码 */
    uint32_t  handle_index_bits;/**< 句柄索引位数 */
#ifdef POOL_CFG_PLUGIN_FIELDS
    POOL_CFG_PLUGIN_FIELDS
#endif
} pool_cfg_t;

/** @brief 使用者上下文（打包结构体，作为所有操作的第一个参数） */
typedef struct {
    pool_cfg_t *cfg;        /**< 指向池配置的指针 */
    uint16_t    owner_id;   /**< 使用者ID */
#ifdef POOL_OWNER_PLUGIN_FIELDS
    POOL_OWNER_PLUGIN_FIELDS
#endif
} pool_owner_t;

/*===========================================================================
 * 错误码枚举 — 每个函数独立
 *===========================================================================*/

/** pool_init 错误码 */
typedef enum {
    POOL_INIT_OK                = 0,
    POOL_INIT_ERR_NULL_PARAM    = 1,    /**< cfg/metadata_base/data_base 为 NULL */
    POOL_INIT_ERR_PAGE_SIZE     = 2,    /**< page_size 为 0 */
    POOL_INIT_ERR_PAGE_COUNT    = 3,    /**< page_count 为 0 */
    POOL_INIT_ERR_HANDLE_COUNT  = 4,    /**< handle_count 为 0 */
    POOL_INIT_ERR_META_SIZE     = 5,    /**< metadata_size 不足 */
    POOL_INIT_ERR_PAGE_SIZE_POW2 = 6,   /**< page_size 非 2 的幂 */
    POOL_INIT_ERR_PAGE_COUNT_EVEN = 7,  /**< page_count 非 2 的倍数 */
    POOL_INIT_ERR_OVERLAP       = 8,    /**< 元数据区与数据区重叠 */
    POOL_INIT_ERR_ALREADY_INIT  = 9,    /**< 元数据区已有初始化标记（禁止重复 init） */
} pool_init_err_t;

/** pool_sys_pack 错误码 */
typedef enum {
    POOL_SYS_PACK_OK            = 0,
    POOL_SYS_PACK_ERR_NULL      = 1,    /**< owner/cfg 为 NULL */
    POOL_SYS_PACK_ERR_ID_RANGE  = 2,    /**< owner_id 不在 0~127 范围 */
} pool_sys_pack_err_t;

/** pool_user_pack 错误码 */
typedef enum {
    POOL_USER_PACK_OK           = 0,
    POOL_USER_PACK_ERR_NULL     = 1,    /**< owner/cfg 为 NULL */
    POOL_USER_PACK_ERR_NO_ID    = 2,    /**< 无可用用户ID (已耗尽 128~25565) */
} pool_user_pack_err_t;

/** pool_alloc_pages / pool_alloc_bytes 错误码 */
typedef enum {
    POOL_ALLOC_OK               = 0,
    POOL_ALLOC_ERR_NULL         = 1,    /**< owner/handle_out 为 NULL */
    POOL_ALLOC_ERR_SIZE         = 2,    /**< 请求大小为 0 */
    POOL_ALLOC_ERR_NO_SPACE     = 3,    /**< 无足够连续空闲页 */
    POOL_ALLOC_ERR_NO_HANDLE    = 4,    /**< 句柄表已满 */
} pool_alloc_err_t;

/** pool_lock 错误码 */
typedef enum {
    POOL_LOCK_OK                = 0,
    POOL_LOCK_ERR_NULL          = 1,    /**< owner/addr_out 为 NULL */
    POOL_LOCK_ERR_INVALID       = 2,    /**< 句柄无效（不存在/已释放/次代数不匹配） */
    POOL_LOCK_ERR_OWNER         = 3,    /**< 句柄不属于此使用者 */
    POOL_LOCK_ERR_OVERFLOW      = 4,    /**< 锁定计数溢出 */
} pool_lock_err_t;

/** pool_unlock 错误码 */
typedef enum {
    POOL_UNLOCK_OK              = 0,
    POOL_UNLOCK_ERR_NULL        = 1,    /**< owner 为 NULL */
    POOL_UNLOCK_ERR_INVALID     = 2,    /**< 句柄无效 */
    POOL_UNLOCK_ERR_OWNER       = 3,    /**< 句柄不属于此使用者 */
    POOL_UNLOCK_ERR_NOT_LOCKED  = 4,    /**< 锁定计数已为 0 */
} pool_unlock_err_t;

/** pool_free 错误码 */
typedef enum {
    POOL_FREE_OK                = 0,
    POOL_FREE_ERR_NULL          = 1,    /**< owner 为 NULL */
    POOL_FREE_ERR_INVALID       = 2,    /**< 句柄无效 */
    POOL_FREE_ERR_OWNER         = 3,    /**< 句柄不属于此使用者 */
    POOL_FREE_ERR_LOCKED        = 4,    /**< 句柄仍被锁定，须先解锁 */
} pool_free_err_t;

/** pool_resize 错误码 */
typedef enum {
    POOL_RESIZE_OK              = 0,
    POOL_RESIZE_ERR_NULL        = 1,    /**< owner 为 NULL */
    POOL_RESIZE_ERR_INVALID     = 2,    /**< 句柄无效 */
    POOL_RESIZE_ERR_OWNER       = 3,    /**< 句柄不属于此使用者 */
    POOL_RESIZE_ERR_SIZE        = 4,    /**< 新大小为 0 */
    POOL_RESIZE_ERR_NO_SPACE    = 5,    /**< 无法扩展（无足够空间且无法移动） */
    POOL_RESIZE_ERR_LOCKED      = 6,    /**< 需要移动自身但句柄被锁定 */
} pool_resize_err_t;

/** pool_defrag 错误码 */
typedef enum {
    POOL_DEFRAG_OK              = 0,
    POOL_DEFRAG_ERR_NULL        = 1,    /**< owner 为 NULL */
} pool_defrag_err_t;

/** pool_free_all 错误码 */
typedef enum {
    POOL_FREE_ALL_OK            = 0,
    POOL_FREE_ALL_ERR_NULL      = 1,    /**< owner 为 NULL */
    POOL_FREE_ALL_ERR_LOCKED    = 2,    /**< 非强制模式下存在被锁定的句柄 */
} pool_free_all_err_t;

/** pool_query_* 查询 API 错误码 */
typedef enum {
    POOL_QUERY_OK               = 0,
    POOL_QUERY_ERR_NULL         = 1,    /**< owner/out 指针为 NULL */
    POOL_QUERY_ERR_INVALID      = 2,    /**< 句柄无效 */
    POOL_QUERY_ERR_OWNER        = 3,    /**< 句柄不属于此使用者 */
} pool_query_err_t;

/*===========================================================================
 * API 函数声明
 *===========================================================================*/

/**
 * @brief 初始化内存池配置
 *
 * 检查参数合法性，计算并设置所有内部字段。
 * metadata_base 的前 POOL_META_SIZE(page_count, handle_count) 字节将被此函数初始化。
 *
 * 约束条件：
 *  - page_size 必须为 2 的幂（≥ 2）
 *  - page_count 必须为 2 的倍数
 *  - 元数据区与数据区不可重叠
 *  - 首次 init 前元数据区须清零（或保证首 4 字节不为 POOL_INIT_MAGIC）
 *  - 建议 data_base 声明为 uint32_t 数组以确保 4 字节对齐
 *
 * @param cfg            [out] 池配置结构体
 * @param metadata_base  [in]  元数据空间基址
 * @param metadata_size  [in]  元数据空间大小（字节）
 * @param data_base      [in]  数据空间基址（用户数据区，建议 uint32_t 对齐）
 * @param page_size      [in]  每页大小（字节），须为 2 的幂且 > 0
 * @param page_count     [in]  总页数，须为 2 的倍数且 > 0
 * @param handle_count   [in]  最大句柄数，须 > 0
 * @return POOL_INIT_OK 或相应错误码
 */
pool_init_err_t pool_init(pool_cfg_t *cfg,
                          void *metadata_base, size_t metadata_size,
                          void *data_base,
                          uint32_t page_size, uint32_t page_count,
                          uint32_t handle_count);

/**
 * @brief 系统打包 — 手动指定使用者ID
 *
 * 用于系统代码，手动指定 0~127 范围内的ID。
 *
 * @param owner    [out] 使用者上下文
 * @param cfg      [in]  池配置指针
 * @param owner_id [in]  使用者ID (0~127)
 * @return POOL_SYS_PACK_OK 或相应错误码
 */
pool_sys_pack_err_t pool_sys_pack(pool_owner_t *owner, pool_cfg_t *cfg, uint16_t owner_id);

/**
 * @brief 用户打包 — 自动分配使用者ID
 *
 * 自动从 cfg->next_user_id 分配一个ID (128~25565)。
 *
 * @param owner [out] 使用者上下文
 * @param cfg   [in]  池配置指针
 * @return POOL_USER_PACK_OK 或相应错误码
 */
pool_user_pack_err_t pool_user_pack(pool_owner_t *owner, pool_cfg_t *cfg);

/**
 * @brief 按页数分配空间
 *
 * @param owner      [in]  使用者上下文
 * @param page_count [in]  请求页数（> 0）
 * @param handle_out [out] 返回的句柄
 * @return POOL_ALLOC_OK 或相应错误码
 */
pool_alloc_err_t pool_alloc_pages(pool_owner_t *owner, uint32_t page_count, uint32_t *handle_out);

/**
 * @brief 按字节数分配空间（自动向上取整到页）
 *
 * @param owner      [in]  使用者上下文
 * @param bytes      [in]  请求字节数（> 0）
 * @param handle_out [out] 返回的句柄
 * @return POOL_ALLOC_OK 或相应错误码
 */
pool_alloc_err_t pool_alloc_bytes(pool_owner_t *owner, uint32_t bytes, uint32_t *handle_out);

/**
 * @brief 锁定句柄，递增锁定计数，返回数据地址
 *
 * 支持可重入锁定：同一句柄可以被同一（或不同）使用者多次锁定，
 * 每次锁定递增 lock_count，每次解锁递减。lock_count 回到 0 后方可释放。
 * 锁定计数上限为 65535，溢出时返回 POOL_LOCK_ERR_OVERFLOW。
 *
 * @param owner    [in]  使用者上下文
 * @param handle   [in]  句柄
 * @param addr_out [out] 返回的数据空间地址
 * @return POOL_LOCK_OK 或相应错误码
 */
pool_lock_err_t pool_lock(pool_owner_t *owner, uint32_t handle, void **addr_out);

/**
 * @brief 解锁句柄，递减锁定计数
 *
 * @param owner  [in] 使用者上下文
 * @param handle [in] 句柄
 * @return POOL_UNLOCK_OK 或相应错误码
 */
pool_unlock_err_t pool_unlock(pool_owner_t *owner, uint32_t handle);

/**
 * @brief 释放句柄（须先完全解锁）
 *
 * @param owner  [in] 使用者上下文
 * @param handle [in] 句柄
 * @return POOL_FREE_OK 或相应错误码
 */
pool_free_err_t pool_free(pool_owner_t *owner, uint32_t handle);

/**
 * @brief 改变句柄对应空间的大小
 *
 * - 缩小: 原地缩小，释放尾部页面
 * - 扩大: 优先原地扩展；若紧后空间不足则尝试移动
 *
 * @param owner          [in] 使用者上下文
 * @param handle         [in] 句柄
 * @param new_page_count [in] 新的大小（页数），必须 > 0
 * @return POOL_RESIZE_OK 或相应错误码
 */
pool_resize_err_t pool_resize(pool_owner_t *owner, uint32_t handle, uint32_t new_page_count);

/**
 * @brief 碎片整理
 *
 * 从前往后扫描，将未锁定的句柄向低地址方向移动，消除碎片。
 * 锁定中的句柄不会被移动。
 *
 * **注意**：此函数操作的是整个池（所有使用者），owner 参数仅用于获取
 * cfg 指针。传入任意有效的 pool_owner_t 均可，不影响操作范围。
 *
 * @param owner [in] 使用者上下文（仅用于访问 cfg，不影响操作范围）
 * @return POOL_DEFRAG_OK 或相应错误码
 */
pool_defrag_err_t pool_defrag(pool_owner_t *owner);

/**
 * @brief 释放指定使用者的所有句柄
 *
 * @param owner  [in] 使用者上下文
 * @param forced [in] true=强制释放（忽略锁定状态），false=非强制（有锁定则报错）
 * @return POOL_FREE_ALL_OK 或相应错误码
 */
pool_free_all_err_t pool_free_all(pool_owner_t *owner, bool forced);

/*===========================================================================
 * 查询 API（只读，不修改池状态）
 *===========================================================================*/

/**
 * @brief 查询池中当前空闲页数
 *
 * @param owner    [in]  使用者上下文（仅用于访问 cfg，任意有效 owner 均可）
 * @param out_free [out] 返回的空闲页数
 * @return POOL_QUERY_OK 或相应错误码
 */
pool_query_err_t pool_query_free_pages(pool_owner_t *owner, uint32_t *out_free);

/**
 * @brief 查询指定句柄占用的页数和字节数
 *
 * 注意：此函数需要句柄属于当前使用者（校验 owner_id）。
 *
 * @param owner       [in]  使用者上下文
 * @param handle      [in]  句柄
 * @param out_pages   [out] 返回占用的页数（可为 NULL，不关心则传 NULL）
 * @param out_bytes   [out] 返回占用的字节数（可为 NULL）
 * @return POOL_QUERY_OK 或相应错误码
 */
pool_query_err_t pool_query_handle_size(pool_owner_t *owner, uint32_t handle,
                                         uint32_t *out_pages, uint32_t *out_bytes);

/**
 * @brief 查询某使用者占用的资源统计
 *
 * @param owner         [in]  使用者上下文（仅用于访问 cfg，任意有效 owner 均可）
 * @param target_owner  [in]  要查询的使用者 ID
 * @param out_handles   [out] 返回该使用者的活跃句柄数（可为 NULL）
 * @param out_pages     [out] 返回该使用者占用的总页数（可为 NULL）
 * @return POOL_QUERY_OK 或相应错误码
 */
pool_query_err_t pool_query_owner_info(pool_owner_t *owner, uint16_t target_owner,
                                        uint32_t *out_handles, uint32_t *out_pages);

#ifdef __cplusplus
}
#endif

#endif /* POOL_H */
