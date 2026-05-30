# 测试子目录 — 仅在根 CMakeLists.txt 中 POOL_BUILD_TEST=ON 时编译

# 收集插件源文件（与根 CMakeLists.txt 一致）
file(GLOB PLUGIN_SRCS "${CMAKE_SOURCE_DIR}/plugins/*.c")
file(GLOB_RECURSE PLUGIN_DEEP_SRCS "${CMAKE_SOURCE_DIR}/plugins/**/*.c")
list(APPEND PLUGIN_SRCS ${PLUGIN_DEEP_SRCS})
list(REMOVE_DUPLICATES PLUGIN_SRCS)

add_executable(test_pool
    test_pool.c
    ../src/pool.c
    ${PLUGIN_SRCS}
)
target_compile_definitions(test_pool
    PRIVATE POOL_DEBUG
)
target_include_directories(test_pool
    PRIVATE ../include
    PRIVATE "${CMAKE_SOURCE_DIR}"
)

add_test(NAME pool_test COMMAND test_pool)
