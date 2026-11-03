# bot 静态库：与 tests/bots 共用（CLI 只是薄壳）。
add_library(theseed_bots STATIC
    BotsConfig.cpp
    BotClient.cpp
    BotManager.cpp)

target_link_libraries(theseed_bots
    PUBLIC theseed_project_options
    PUBLIC theseed_login
    PUBLIC theseed_runtime)

target_include_directories(theseed_bots
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# CLI 可执行文件
add_executable(theseed-bots main.cpp)

target_link_libraries(theseed-bots
    PRIVATE theseed_project_options
    PRIVATE theseed_bots)
