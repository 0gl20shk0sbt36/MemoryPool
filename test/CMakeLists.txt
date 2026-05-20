# 测试子目录 — 仅在根 CMakeLists.txt 中 POOL_BUILD_TEST=ON 时编译

add_executable(test_pool
    test_pool.c
    ../src/pool.c
)
target_compile_definitions(test_pool
    PRIVATE POOL_DEBUG
)
target_include_directories(test_pool
    PRIVATE ../include
)

add_test(NAME pool_test COMMAND test_pool)
