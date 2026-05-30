cmake_minimum_required(VERSION 3.10)
project(pool C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ================================================================
# 插件系统 — 自动发现与配置生成
# ================================================================

# 运行 configure.py 生成 pool_plugin_config.h
execute_process(
    COMMAND python3 "${CMAKE_SOURCE_DIR}/configure.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE CONFIGURE_RESULT
    OUTPUT_VARIABLE CONFIGURE_OUT
    ERROR_VARIABLE  CONFIGURE_OUT
)

# 检测 plugins/ 下是否有非父目录条目
file(GLOB PLUGIN_DIRS RELATIVE "${CMAKE_SOURCE_DIR}/plugins"
     "${CMAKE_SOURCE_DIR}/plugins/*")
list(LENGTH PLUGIN_DIRS PLUGIN_COUNT)

if(PLUGIN_COUNT GREATER 0)
    message(STATUS "Plugins enabled: ${PLUGIN_DIRS}")
    add_compile_definitions(POOL_PLUGINS_ENABLED)
else()
    message(STATUS "Plugins: none (hooks disabled, zero overhead)")
endif()

# ================================================================
# 静态库（默认） — 供其他项目链接
# ================================================================

# 收集插件源文件
set(PLUGIN_SOURCES)
if(PLUGIN_COUNT GREATER 0)
    file(GLOB_RECURSE PLUGIN_SRCS "${CMAKE_SOURCE_DIR}/plugins/*.c")
    list(APPEND PLUGIN_SOURCES ${PLUGIN_SRCS})
endif()

add_library(pool STATIC
    src/pool.c
    ${PLUGIN_SOURCES}
)
target_include_directories(pool
    PUBLIC include
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
