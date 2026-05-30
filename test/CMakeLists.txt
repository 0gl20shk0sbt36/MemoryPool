# 测试子目录 — 仅在根 CMakeLists.txt 中 POOL_BUILD_TEST=ON 时编译

include("${CMAKE_SOURCE_DIR}/pool_plugin_config.cmake")

# 将插件源文件和 include 路径转为绝对路径（相对于 CMAKE_SOURCE_DIR）
set(_POOL_PLUGIN_SOURCES "")
foreach(_s ${POOL_PLUGIN_SOURCES})
    if(NOT IS_ABSOLUTE "${_s}")
        list(APPEND _POOL_PLUGIN_SOURCES "${CMAKE_SOURCE_DIR}/${_s}")
    else()
        list(APPEND _POOL_PLUGIN_SOURCES "${_s}")
    endif()
endforeach()

set(_POOL_PLUGIN_INCLUDES "")
foreach(_i ${POOL_PLUGIN_INCLUDES})
    if(NOT IS_ABSOLUTE "${_i}")
        list(APPEND _POOL_PLUGIN_INCLUDES "${CMAKE_SOURCE_DIR}/${_i}")
    else()
        list(APPEND _POOL_PLUGIN_INCLUDES "${_i}")
    endif()
endforeach()

add_executable(test_pool
    test_pool.c
    ../src/pool.c
    ${_POOL_PLUGIN_SOURCES}
)
target_compile_definitions(test_pool
    PRIVATE POOL_DEBUG
)
target_include_directories(test_pool
    PRIVATE ../include
    PRIVATE ${_POOL_PLUGIN_INCLUDES}
    PRIVATE "${CMAKE_SOURCE_DIR}"
)

add_test(NAME pool_test COMMAND test_pool)
