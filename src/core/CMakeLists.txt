add_library(theseed_core INTERFACE)
target_link_libraries(theseed_core INTERFACE theseed_project_options)
target_include_directories(
    theseed_core
    INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
