add_library(theseed_core
    EntityData.cpp
    EntityDefLoader.cpp
    FileEntityStore.cpp
    BaseRuntime.cpp
    EntityDefRegistry.cpp
    BaseApp.cpp
    CellApp.cpp)

target_link_libraries(theseed_core PUBLIC theseed_project_options)
target_link_libraries(theseed_core PUBLIC theseed_login)
target_link_libraries(theseed_core PUBLIC theseed_runtime)
target_link_libraries(theseed_core PUBLIC theseed_foundation)

target_include_directories(theseed_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
