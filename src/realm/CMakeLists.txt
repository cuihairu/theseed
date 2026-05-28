add_library(
    theseed_realm
    RealmApp.cpp)

target_link_libraries(
    theseed_realm
    PUBLIC theseed_project_options
    PUBLIC theseed_login
    PUBLIC theseed_runtime
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_realm
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
