add_library(
    theseed_runtime
    AOI.cpp
    CellRuntime.cpp
    DirtyMask.cpp
    Entity.cpp
    EntityCall.cpp
    EntityDef.cpp
    IORuntime.cpp
    EntityMigration.cpp
    GhostManager.cpp
    InMemoryBytePipe.cpp
    InvocationCodec.cpp
    NetworkTransport.cpp
    NetworkNode.cpp
    PipedTransport.cpp
    PropertyBlock.cpp
    PropertyReplication.cpp
    RuntimeLoop.cpp
    RuntimeTransport.cpp
    Space.cpp
    TransportHub.cpp
    SpaceRuntime.cpp
    TcpConnection.cpp
    TcpListener.cpp
    Witness.cpp
    TickScheduler.cpp)

target_link_libraries(
    theseed_runtime
    PUBLIC theseed_project_options
    PUBLIC theseed_foundation)

if(WIN32)
    target_link_libraries(theseed_runtime PUBLIC ws2_32)
endif()

target_include_directories(
    theseed_runtime
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
