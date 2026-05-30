cmake_minimum_required(VERSION 3.10)
project(pool C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ================================================================
# 插件配置 — 由 configure.py --gen-cmake 生成，用户可编辑
# ================================================================

# 生成方法：
#   python3 configure.py --gen-cmake --scan-dir lib
#   编辑 pool_plugin_config.cmake 中的 ON/OFF
#   python3 configure.py           → 生成 .h
#   cmake -B build && cmake --build build

include("${CMAKE_SOURCE_DIR}/pool_plugin_config.cmake")

# 若启用任一插件，设置编译宏
list(LENGTH POOL_PLUGIN_NAMES PLUGIN_COUNT)
if(PLUGIN_COUNT GREATER 0)
    message(STATUS "Plugins enabled: ${POOL_PLUGIN_NAMES}")
    add_compile_definitions(POOL_PLUGINS_ENABLED)
else()
    message(STATUS "Plugins: none (hooks disabled, zero overhead)")
endif()

# ================================================================
# 静态库（默认） — 供其他项目链接
# ================================================================

add_library(pool STATIC
    src/pool.c
    ${POOL_PLUGIN_SOURCES}
)
target_include_directories(pool
    PUBLIC include
    PUBLIC ${POOL_PLUGIN_INCLUDES}
)
# 插件头文件的 include 路径（通过生成的 config.h 间接引用）
target_include_directories(pool
    PRIVATE "${CMAKE_SOURCE_DIR}"
)

# ================================================================
# 测试（可选） — cmake -B build -DPOOL_BUILD_TEST=ON
# ================================================================
option(POOL_BUILD_TEST "Build test executable" OFF)

if(POOL_BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
