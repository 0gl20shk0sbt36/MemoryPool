# Page-based Memory Pool

[English](README.md) | [中文](README_zh.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-C99-blue)]()
[![Tests](https://img.shields.io/badge/tests-29%20passed-brightgreen)]()

A page-based memory management system designed for embedded MCUs. C99 pure standard — zero system calls, all memory user-provided.

## Features

- **C99 only** — no system calls, suitable for bare-metal environments
- **Dual-region memory model** — metadata and user data physically separated; data region contains zero management overhead
- **Handle system** — opaque 32-bit handles with generation-based use-after-free protection
- **Ownership isolation** — 0–127 system IDs (manual), 128–25565 user IDs (auto-assigned)
- **Full API** — allocate, lock/unlock, free, resize, defrag, free_all

## Documentation

| Document | Language |
|----------|----------|
| [User Manual](doc/en/user_manual.md) | EN / [中文](doc/zh/user_manual.md) |
| [Development Manual](doc/en/dev_manual.md) | EN / [中文](doc/zh/dev_manual.md) |

- **User Manual** — Quick start, API reference, parameter selection guide, error codes
- **Development Manual** — Internal architecture, metadata layout, handle encoding, core algorithms, optimizations

## Project Structure

```
├── CMakeLists.txt          — CMake build entry
├── include/
│   └── pool.h              — public API header (~340 lines)
├── src/
│   └── pool.c              — implementation (~740 lines)
├── test/
│   ├── test_pool.c         — 29 unit tests
│   └── CMakeLists.txt
├── doc/
│   ├── en/                 — English manuals
│   │   ├── user_manual.md
│   │   └── dev_manual.md
│   └── zh/                 — Chinese manuals
│       ├── user_manual.md
│       └── dev_manual.md
├── LICENSE
└── .gitignore
```

## Quick Start

```c
#include "pool.h"

// 1. Define memory regions
#define PAGE_SIZE    256
#define PAGE_COUNT   64
#define HANDLE_COUNT 16
static uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];
static uint32_t data[PAGE_SIZE / 4 * PAGE_COUNT];  // uint32_t for alignment

// 2. Initialize pool
pool_cfg_t cfg;
pool_init(&cfg, meta, sizeof(meta), data, PAGE_SIZE, PAGE_COUNT, HANDLE_COUNT);

// 3. Get owner context
pool_owner_t owner;
pool_user_pack(&owner, &cfg);  // auto-assigns user ID

// 4. Allocate space
uint32_t handle;
pool_alloc_pages(&owner, 4, &handle);  // allocates 4 pages

// 5. Lock to get address
void *addr;
pool_lock(&owner, handle, &addr);

// 6. Use addr ...

// 7. Unlock
pool_unlock(&owner, handle);

// 8. Free
pool_free(&owner, handle);
```

## Build

Requires CMake ≥ 3.10 and a C99 compiler (GCC, Clang, etc.).

```sh
# Library only
cmake -B build
cmake --build build
# Output: build/libpool.a

# Library + tests
cmake -B build -DPOOL_BUILD_TEST=ON
cmake --build build
ctest --test-dir build
```

## Integration (CMake)

```cmake
add_subdirectory(path/to/MemoryPool)
target_link_libraries(your_target PRIVATE pool)
target_include_directories(your_target PRIVATE path/to/MemoryPool/include)
```

## Key Design Decisions

- **All memory is user-provided** — no `malloc`, no dynamic allocation
- **Handles never expose addresses until locked** — enables safe relocation during defrag
- **Generation counters prevent use-after-free** — freed handles are permanently invalidated
- **Locking protects against relocation** — `pool_defrag` skips locked handles
- **data_move is overridable** — define `data_move` macro before `#include "pool.c"` for platform-specific copy

## License

MIT License
