# 页内存池 (Page-based Memory Pool)

[English](README.md) | [中文](README_zh.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-C99-blue)]()
[![Tests](https://img.shields.io/badge/tests-18%20passed-brightgreen)]()

基于页的内存管理系统，专为嵌入式 MCU 设计。纯 C99，零系统调用，所有内存由用户静态提供。

## 特性

- **纯 C99** — 不使用任何系统调用，适用于裸机环境
- **双区内存模型** — 元数据区与数据区物理分离，数据区零管理开销
- **句柄系统** — 不透明 32 位句柄，代次计数器防 use-after-free
- **所有权隔离** — 0~127 系统 ID（手动分配），128~25565 用户 ID（自动分配）
- **全功能 API** — 分配、锁定/解锁、释放、改变大小、碎片整理、批量释放
- **插件系统** — 编译期零开销的插桩系统，支持事件日志、swap 等

## 文档

| 文档 | 语言 |
|------|------|
| [用户手册](doc/zh/user_manual.md) | 中文 / [EN](doc/en/user_manual.md) |
| [开发手册](doc/zh/dev_manual.md) | 中文 / [EN](doc/en/dev_manual.md) |

- **用户手册** — 快速上手、API 参考、参数选择指南、错误码速查
- **开发手册** — 内部架构、元数据布局、句柄编码、核心算法、插件系统

## 文件结构

```
├── CMakeLists.txt                 — CMake 构建入口
├── configure.py                   — 插件扫描与配置生成器
├── pool_plugin_config.cmake       — 插件配置（由 configure.py --gen-cmake 生成，用户可编辑）
├── include/
│   ├── pool.h                     — 公共 API 头文件 (~340行)
│   ├── pool_hooks.h               — 14 个编译期 hook 点定义
│   └── pool_plugin_config.h       — 生成的 hook 重定向（.gitignore 忽略）
├── src/
│   └── pool.c                     — 实现 (~740行)
├── test/
│   ├── test_pool.c                — 18 项单元测试
│   └── CMakeLists.txt
├── lib/                           — 外部 submodule（如 pool_log）
├── doc/
│   ├── en/                        — 英文手册
│   │   ├── user_manual.md
│   │   └── dev_manual.md
│   └── zh/                        — 中文手册
│       ├── user_manual.md
│       └── dev_manual.md
├── LICENSE
└── .gitignore
```

## 快速上手

```c
#include "pool.h"

// 1. 定义内存区域
#define PAGE_SIZE    256
#define PAGE_COUNT   64
#define HANDLE_COUNT 16
static uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];  // uint32_t 保证对齐

// 2. 初始化池
pool_cfg_t cfg;
pool_init(&cfg, meta, sizeof(meta), data, PAGE_SIZE, PAGE_COUNT, HANDLE_COUNT);

// 3. 获取使用者上下文
pool_owner_t owner;
pool_user_pack(&owner, &cfg);  // 自动分配用户ID

// 4. 分配空间
uint32_t handle;
pool_alloc_pages(&owner, 4, &handle);  // 分配 4 页

// 5. 锁定获取地址
void *addr;
pool_lock(&owner, handle, &addr);

// 6. 使用 addr ...

// 7. 解锁
pool_unlock(&owner, handle);

// 8. 释放
pool_free(&owner, handle);
```

## 插件系统

插件在编译期接入 pool 的各个事件点（init/alloc/free/lock/unlock/resize/defrag/free_all），无插件时零运行时开销。

### 添加插件

```sh
# 1. 添加 submodule
git submodule add <url> lib/<插件名>

# 2. 扫描并生成配置骨架
python3 configure.py --gen-cmake --scan-dir lib

# 3. 编辑 option（ON/OFF）
vim pool_plugin_config.cmake

# 4. 生成 hook 头文件
python3 configure.py

# 5. 编译
cmake -B build
cmake --build build
```

### 可用插件

| 插件 | 说明 | 来源 |
|------|------|------|
| **logging** | 事件日志，支持等级过滤和时间间隔 | [pool_log](https://github.com/0gl20shk0sbt36/pool_log) |

> 想写一个插件？参见[开发手册 — 插件系统](doc/zh/dev_manual.md)了解 hook API 和约定。

## 构建

需要 CMake ≥ 3.10 和 C99 编译器（GCC、Clang 等）。

```sh
# 仅编译库（无插件）
cmake -B build
cmake --build build
# 产物: build/libpool.a

# 库 + 测试（插件按 pool_plugin_config.cmake 启用）
cmake -B build -DPOOL_BUILD_TEST=ON
cmake --build build
ctest --test-dir build
```

## 集成 (CMake)

```cmake
add_subdirectory(path/to/MemoryPool)
target_link_libraries(your_target PRIVATE pool)
target_include_directories(your_target PRIVATE path/to/MemoryPool/include)
```

## 核心设计决策

- **所有内存由用户提供** — 无 `malloc`，无动态分配
- **句柄在锁定前不暴露地址** — 支持碎片整理时的安全搬迁
- **代次计数器防 use-after-free** — 释放后的句柄永久失效
- **锁定保护搬移** — `pool_defrag` 跳过已锁定句柄
- **data_move 可覆盖** — 在 `#include "pool.c"` 前定义 `data_move` 宏即可使用平台专属拷贝

## 许可

MIT License
