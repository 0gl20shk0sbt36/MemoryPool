cmake_minimum_required(VERSION 3.10)
project(pool C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ================================================================
# 静态库（默认） — 供其他项目链接
# ================================================================
add_library(pool STATIC
    src/pool.c
)
target_include_directories(pool
    PUBLIC include
)

# ================================================================
# 测试（可选） — cmake -B build -DPOOL_BUILD_TEST=ON
# ================================================================
option(POOL_BUILD_TEST "Build test executable" OFF)

if(POOL_BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
