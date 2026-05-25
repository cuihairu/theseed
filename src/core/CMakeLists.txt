add_library(theseed_core
    MemoryStream.cpp
    TimerWheel.cpp
    ObjectPool.cpp
    MessageHeader.cpp
    Bundle.cpp
    Channel.cpp
    EntityData.cpp
    EntityDefLoader.cpp)

target_link_libraries(theseed_core PUBLIC theseed_project_options)
target_link_libraries(theseed_core PUBLIC theseed_runtime)

target_include_directories(theseed_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
