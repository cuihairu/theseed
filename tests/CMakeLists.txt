# 测试侧豁免：Config 聚合大量使用指定初始化器并有意省略字段（取默认值），
# -Wextra 的 -Wmissing-field-initializers 族对此是噪声；产品码保持全量检查。
# 用法：测试目标按序链接 `theseed_project_options theseed_test_options`——
# 顺序保证豁免落在 -Wextra 之后生效。
add_library(theseed_test_options INTERFACE)
include(CheckCXXCompilerFlag)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(theseed_test_options INTERFACE -Wno-missing-field-initializers)
    # -Wmissing-designated-field-initializers 是较新 GCC 的 -Wextra 派生警
    # 告；老编译器（CI 的 gcc-13/clang-18）不认识该旗标，而 -Werror 下
    # 「-Wno-未知选项」本身就是错误——探测通过才加，编译器与版本无关。
    # 探测用正向旗标 + -Werror：clang 对未知 -Wno- 只告警不失败，负向探测不可靠。
    set(CMAKE_REQUIRED_FLAGS "-Werror")
    check_cxx_compiler_flag("-Wmissing-designated-field-initializers"
                            CXX_W_MISSING_DESIGNATED_FIELD_INITIALIZERS)
    unset(CMAKE_REQUIRED_FLAGS)
    if(CXX_W_MISSING_DESIGNATED_FIELD_INITIALIZERS)
        target_compile_options(
            theseed_test_options INTERFACE -Wno-missing-designated-field-initializers)
    endif()
endif()

add_executable(theseed_smoke_test smoke/SmokeTest.cpp)

target_link_libraries(
    theseed_smoke_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_smoke_test COMMAND theseed_smoke_test)

add_subdirectory(foundation)
add_subdirectory(core)
add_subdirectory(runtime)
add_subdirectory(login)
add_subdirectory(realm)
add_subdirectory(db)
add_subdirectory(ops)
add_subdirectory(control)
add_subdirectory(codegen)
add_subdirectory(scripting)
