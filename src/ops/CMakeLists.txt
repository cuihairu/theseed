add_library(theseed_ops
    OpsInspector.cpp
    OpsServer.cpp)

target_link_libraries(
    theseed_ops
    PUBLIC theseed_project_options
    PUBLIC theseed_foundation
    PUBLIC theseed_runtime)

target_include_directories(
    theseed_ops
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
