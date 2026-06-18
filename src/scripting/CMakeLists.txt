add_library(theseed_scripting STATIC
    HotUpdate.cpp)

target_link_libraries(theseed_scripting
    PUBLIC theseed_project_options)

target_include_directories(theseed_scripting
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
