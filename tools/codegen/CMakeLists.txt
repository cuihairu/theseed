# CSharpEmitter 静态库：被 CLI 和测试共用，避免重复编译。
add_library(theseed_codegen STATIC
    CSharpEmitter.cpp)

target_link_libraries(theseed_codegen
    PUBLIC theseed_project_options
    PUBLIC theseed_runtime
    PUBLIC theseed_core)

target_include_directories(theseed_codegen
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# CLI 可执行文件
add_executable(theseed_codegen_cli main.cpp)

target_link_libraries(theseed_codegen_cli
    PRIVATE theseed_project_options
    PRIVATE theseed_codegen)

set_target_properties(theseed_codegen_cli PROPERTIES OUTPUT_NAME theseed-codegen)
