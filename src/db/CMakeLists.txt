add_library(
    theseed_db
    DBApp.cpp
    DBProtocol.cpp
    RemoteEntityStore.cpp)

target_link_libraries(
    theseed_db
    PUBLIC theseed_project_options
    PUBLIC theseed_core
    PUBLIC theseed_runtime
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_db
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
