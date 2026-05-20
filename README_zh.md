# 页内存池 (Page-based Memory Pool)

基于页的内存管理系统，专为单片机（MCU）设计。

## 特性

- **C99 纯标准**：不使用任何系统调用，适用于裸机环境
- **双区内存模型**：元数据区与用户数据区完全分离，用户数据区不含任何管理信息
- **句柄系统**：句柄 = 次代数 + 索引，安全复用，防止 use-after-free
- **所有权隔离**：0~127 系统 ID（手动分配），128~25565 用户 ID（自动分配）
- **全功能 API**：分配、锁定/解锁、释放、调整大小、碎片整理

## 文件结构

```
├── CMakeLists.txt          — CMake 构建入口
├── .gitignore              — 排除 build/
├── include/
│   └── pool.h              — 公共 API 头文件
├── src/
│   └── pool.c              — 实现
├── test/
│   ├── test_pool.c         — 29 个单元测试（可选 POOL_DEBUG）
│   └── CMakeLists.txt      — 测试子目录
├── .tmp/                   — 项目记忆文件
└── build/                  — 编译产物（git 已忽略）
```

## 快速使用

```c
#include "pool.h"

// 1. 定义内存区域
#define PAGE_SIZE    256
#define PAGE_COUNT   64
#define HANDLE_COUNT 16
uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];
uint8_t data[PAGE_SIZE * PAGE_COUNT];

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

## 构建

需要 cmake + C99 编译器（如 gcc）。

### 模式1：编译静态库（默认）

```sh
cmake -B build
cmake --build build
# 产物: build/libpool.a
```

### 模式2：编译库 + 测试

```sh
cmake -B build -DPOOL_BUILD_TEST=ON
cmake --build build
ctest --test-dir build
```

测试启用 `POOL_DEBUG` 宏，输出位图 hexdump + page_owner 映射 + 句柄表 + 数据窗口。

### 清理

```sh
rm -rf build
```

## 作为依赖集成到其他项目

在第三方项目的 `CMakeLists.txt` 中：

```cmake
add_subdirectory(path/to/pool)
target_link_libraries(your_target PRIVATE pool)
target_include_directories(your_target PRIVATE path/to/pool/include)
```

## 许可

MIT License
