# theseed_core 仅承载 Base/Cell 进程骨架（BaseApp/BaseRuntime/CellApp）。
# 实体存储与 EntityDef 解析族已迁往 theseed_db（持久化/实体定义层），
# 故 core 依赖 db 而非反向。核心头文件（BaseRuntime.h/BaseApp.h/CellApp.h）
# 包含 theseed/db/*.h，PUBLIC 传递 db 头路径给下游。
add_library(theseed_core
    BaseRuntime.cpp
    BaseApp.cpp
    CellApp.cpp
)

target_link_libraries(theseed_core PUBLIC theseed_project_options)
target_link_libraries(theseed_core PUBLIC theseed_db)
target_link_libraries(theseed_core PUBLIC theseed_login)
target_link_libraries(theseed_core PUBLIC theseed_runtime)
target_link_libraries(theseed_core PUBLIC theseed_ops)
target_link_libraries(theseed_core PUBLIC theseed_scripting)
target_link_libraries(theseed_core PUBLIC theseed_foundation)

target_include_directories(theseed_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
