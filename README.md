# Page-based Memory Pool (页内存池)

A page-based memory management system designed for MCU (microcontrollers).

## Features

- **C99 pure standard**: no system calls, suitable for bare-metal environments
- **Dual-region memory model**: metadata and user data are completely separated, user data contains no management information
- **Handle system**: handle = generation + index, safe reuse, prevents use-after-free
- **Ownership isolation**: 0~127 system IDs (manual), 128~25565 user IDs (auto)
- **Full API**: allocate, lock/unlock, free, resize, defrag

## Project Structure

```
├── CMakeLists.txt          — CMake build entry
├── .gitignore              — excludes build/
├── include/
│   └── pool.h              — public API header
├── src/
│   └── pool.c              — implementation
├── test/
│   ├── test_pool.c         — 29 unit tests (optional POOL_DEBUG)
│   └── CMakeLists.txt      — test subdirectory
├── .tmp/                   — project memory files
└── build/                  — build artifacts (git ignored)
```

## Quick Start

```c
#include "pool.h"

// 1. Define memory regions
#define PAGE_SIZE    256
#define PAGE_COUNT   64
#define HANDLE_COUNT 16
uint8_t meta[POOL_META_SIZE(PAGE_COUNT, HANDLE_COUNT)];
uint8_t data[PAGE_SIZE * PAGE_COUNT];

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

Requires cmake + a C99 compiler (e.g. gcc).

### Mode 1: Static library only (default)

```sh
cmake -B build
cmake --build build
# output: build/libpool.a
```

### Mode 2: Library + tests

```sh
cmake -B build -DPOOL_BUILD_TEST=ON
cmake --build build
ctest --test-dir build
```

Tests are compiled with `POOL_DEBUG`, which enables hexdump output of bitmap,
page_owner map, handle table, and data window.

### Clean

```sh
rm -rf build
```

## Integration into Other Projects

In your project's `CMakeLists.txt`:

```cmake
add_subdirectory(path/to/pool)
target_link_libraries(your_target PRIVATE pool)
target_include_directories(your_target PRIVATE path/to/pool/include)
```

## License

MIT License
