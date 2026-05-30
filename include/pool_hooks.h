/**
 * @file pool_hooks.h
 * @brief 插桩点定义 — 编译期宏展开，零运行时开销
 *
 * 每个 hook 点默认展开为 do{}while(0)。
 * 当 configure.py 扫描到 plugins/ 下有插件实现了该 hook 时，
 * 生成的 pool_plugin_config.h 会覆盖对应的宏定义，展开为插件函数调用链。
 *
 * 用法（在 pool.c 中）:
 *   POOL_HOOK_POST_ALLOC(cfg, owner, page_start, page_count, handle);
 *
 * 插件命名约定:
 *   函数名 = {插件名}_{hook名小写}
 *   例: swap_pre_lock(void *ctx, pool_cfg_t *cfg, ...)
 *
 * 所有 hook 函数签名的第一个参数是 void *ctx（插件上下文），
 * 第二个参数是 pool_cfg_t *cfg，后续按语义传递。
 *
 * Hook 点清单（14 个）:
 *   init 阶段:  POST_INIT
 *   分配阶段:  POST_ALLOC
 *   释放阶段:  PRE_FREE, POST_FREE
 *   锁定阶段:  PRE_LOCK, POST_LOCK
 *   解锁阶段:  PRE_UNLOCK, POST_UNLOCK
 *   调整大小:  PRE_RESIZE, POST_RESIZE
 *   碎片整理:  PRE_DEFRAG, DEFRAG_MOVE, POST_DEFRAG
 *   批量释放:  PRE_FREE_ALL, POST_FREE_ALL
 */

#ifndef POOL_HOOKS_H
#define POOL_HOOKS_H

/*===========================================================================
 * Hook 点默认定义 — 每个都可用 #define 覆盖
 *===========================================================================*/

/*---------------------------------------------------------------------------
 * init 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 池初始化完成后触发
 * @param cfg 池配置（此时所有内部字段已就绪）
 */
#ifndef POOL_HOOK_POST_INIT
#define POOL_HOOK_POST_INIT(cfg) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * alloc 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 分配成功后触发
 * @param cfg        池配置
 * @param owner      使用者上下文
 * @param page_start 分配的起始页索引
 * @param page_count 分配的页数
 * @param handle     返回的句柄
 */
#ifndef POOL_HOOK_POST_ALLOC
#define POOL_HOOK_POST_ALLOC(cfg, owner, page_start, page_count, handle) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * free 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 释放前触发
 *
 * swap 场景: 将脏页刷回交换介质后再释放页面。
 * 此时句柄仍有效，可访问 page_start/page_count/lock_count。
 *
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 即将释放的句柄
 */
#ifndef POOL_HOOK_PRE_FREE
#define POOL_HOOK_PRE_FREE(cfg, owner, handle) \
    do {} while(0)
#endif

/**
 * @brief 释放后触发
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 已释放的句柄（此时 handle_lookup 将返回 NULL）
 */
#ifndef POOL_HOOK_POST_FREE
#define POOL_HOOK_POST_FREE(cfg, owner, handle) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * lock 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 锁定前触发（句柄已验证、所有权已确认，lock_count 尚未递增）
 *
 * swap 场景: 如果目标页在交换介质中，在此换入。
 * 这是 swap 最关键的 hook —— 必须在 lock_count 递增前完成换入，
 * 以保证 lock 成功后数据立即可用。
 *
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 即将锁定的句柄
 */
#ifndef POOL_HOOK_PRE_LOCK
#define POOL_HOOK_PRE_LOCK(cfg, owner, handle) \
    do {} while(0)
#endif

/**
 * @brief 锁定成功后触发（lock_count 已递增，地址已计算）
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 已锁定的句柄
 * @param addr   数据空间地址
 */
#ifndef POOL_HOOK_POST_LOCK
#define POOL_HOOK_POST_LOCK(cfg, owner, handle, addr) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * unlock 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 解锁前触发（lock_count 尚未递减）
 *
 * swap 场景: 标记页面可被换出候选。
 *
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 即将解锁的句柄
 */
#ifndef POOL_HOOK_PRE_UNLOCK
#define POOL_HOOK_PRE_UNLOCK(cfg, owner, handle) \
    do {} while(0)
#endif

/**
 * @brief 解锁后触发（lock_count 已递减）
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param handle 已解锁的句柄
 */
#ifndef POOL_HOOK_POST_UNLOCK
#define POOL_HOOK_POST_UNLOCK(cfg, owner, handle) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * resize 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 大小调整前触发（参数已验证，句柄有效）
 *
 * @param cfg       池配置
 * @param owner     使用者上下文
 * @param handle    句柄
 * @param old_pages 旧页数
 * @param new_pages 新页数
 */
#ifndef POOL_HOOK_PRE_RESIZE
#define POOL_HOOK_PRE_RESIZE(cfg, owner, handle, old_pages, new_pages) \
    do {} while(0)
#endif

/**
 * @brief 大小调整后触发
 *
 * 注意: resize 有多个成功路径（缩小/原地扩展/移动自身/移动后方），
 * 每个路径都会触发此 hook。old_pages 和 new_pages 反映实际变更。
 *
 * @param cfg       池配置
 * @param owner     使用者上下文
 * @param handle    句柄
 * @param old_pages 旧页数
 * @param new_pages 新页数
 */
#ifndef POOL_HOOK_POST_RESIZE
#define POOL_HOOK_POST_RESIZE(cfg, owner, handle, old_pages, new_pages) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * defrag 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 碎片整理开始前触发
 * @param cfg 池配置
 */
#ifndef POOL_HOOK_PRE_DEFRAG
#define POOL_HOOK_PRE_DEFRAG(cfg) \
    do {} while(0)
#endif

/**
 * @brief 碎片搬迁时触发（每次 data_move 后、元数据更新前）
 *
 * swap 场景: 更新交换介质中的页 → 物理地址映射。
 *
 * @param cfg        池配置
 * @param src_page   源起始页
 * @param dst_page   目标起始页
 * @param count      搬迁页数
 * @param handle_idx 句柄表索引
 */
#ifndef POOL_HOOK_DEFRAG_MOVE
#define POOL_HOOK_DEFRAG_MOVE(cfg, src_page, dst_page, count, handle_idx) \
    do {} while(0)
#endif

/**
 * @brief 碎片整理完成后触发
 * @param cfg 池配置
 */
#ifndef POOL_HOOK_POST_DEFRAG
#define POOL_HOOK_POST_DEFRAG(cfg) \
    do {} while(0)
#endif

/*---------------------------------------------------------------------------
 * free_all 阶段
 *---------------------------------------------------------------------------*/

/**
 * @brief 批量释放前触发
 *
 * 非强制模式下，此时已验证所有句柄已解锁。
 *
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param forced 是否强制释放
 */
#ifndef POOL_HOOK_PRE_FREE_ALL
#define POOL_HOOK_PRE_FREE_ALL(cfg, owner, forced) \
    do {} while(0)
#endif

/**
 * @brief 批量释放后触发
 * @param cfg    池配置
 * @param owner  使用者上下文
 * @param forced 是否强制释放
 */
#ifndef POOL_HOOK_POST_FREE_ALL
#define POOL_HOOK_POST_FREE_ALL(cfg, owner, forced) \
    do {} while(0)
#endif

#endif /* POOL_HOOKS_H */
