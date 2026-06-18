add_library(theseed_foundation
    MemoryStream.cpp
    TimerWheel.cpp
    ObjectPool.cpp
    MessageHeader.cpp
    Bundle.cpp
    Channel.cpp
    ChannelRouter.cpp
    MetricsRegistry.cpp
    Logger.cpp
    Tracing.cpp
    RedisProvider.cpp
    SessionStore.cpp
    RateLimiter.cpp)

target_link_libraries(theseed_foundation PUBLIC theseed_project_options)

target_include_directories(theseed_foundation
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
