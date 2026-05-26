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
    InvocationCodec.cpp
    PipedTransport.cpp
    PropertyBlock.cpp
    PropertyReplication.cpp
    RuntimeLoop.cpp
    RuntimeTransport.cpp
    Space.cpp
    SpaceRuntime.cpp
    Witness.cpp
    TickScheduler.cpp)

target_link_libraries(
    theseed_runtime
    PUBLIC theseed_project_options
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_runtime
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
