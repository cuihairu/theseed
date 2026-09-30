add_library(
    theseed_control
    machine/HostProbe.cpp
    machine/MachineAgent.cpp
    machine/MachineDaemon.cpp
    machine/MachineSnapshotCodec.cpp
    machine/OtlpMetricsExporter.cpp
    machine/OtlpTraceExporter.cpp
    machine/ProcessPortScanner.cpp
    machine/ProcessSupervisor.cpp
    ops/OpsControlCenter.cpp)

target_link_libraries(
    theseed_control
    PUBLIC theseed_project_options
    PUBLIC theseed_core
    PUBLIC theseed_runtime)

if(WIN32)
    # HostProbe 的网络探针走 IP Helper API（GetIfTable2）
    target_link_libraries(theseed_control PUBLIC iphlpapi)
endif()

target_include_directories(
    theseed_control
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
