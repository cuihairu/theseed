# 基础源文件：不含 MySQL 依赖，始终编译。
# 实体存储与 EntityDef 解析族（文件后端/实体数据/查询/定义注册表）由
# theseed_core 迁入：它们是与 SQL/远程后端同族的持久化与实体定义层，
# 不依赖进程骨架，故作为 db 的基础源始终编译。
set(THESEED_DB_SOURCES
    DBApp.cpp
    DBProtocol.cpp
    EntityData.cpp
    EntityDefLoader.cpp
    EntityDefRegistry.cpp
    EntityQuery.cpp
    FileEntityStore.cpp
    RemoteEntityStore.cpp)

add_library(theseed_db ${THESEED_DB_SOURCES})

# MySQL 后端是可选的：libmysql 不可用时回退到 FileEntityStore。
# 通过 THESEED_HAS_MYSQL 宏在编译期裁剪 MySQL 相关代码。
find_package(unofficial-libmysql CONFIG QUIET)
if(unofficial-libmysql_FOUND)
    target_sources(theseed_db PRIVATE
        MySQLConnection.cpp
        MySQLEntityStore.cpp)
    target_compile_definitions(theseed_db PUBLIC THESEED_HAS_MYSQL=1)
    target_link_libraries(theseed_db PRIVATE unofficial::libmysql::libmysql)
    set_property(GLOBAL PROPERTY theseed_mysql_enabled TRUE)
    message(STATUS "theseed_db: MySQL backend enabled (libmysql found)")
else()
    message(STATUS "theseed_db: MySQL backend disabled (libmysql not found, file-only)")
endif()

# PostgreSQL 后端同样是可选的：libpq 不可用时仅回退，不影响其余编译。
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    target_sources(theseed_db PRIVATE
        PostgreSQLConnection.cpp
        PostgreSQLEntityStore.cpp)
    target_compile_definitions(theseed_db PUBLIC THESEED_HAS_POSTGRESQL=1)
    target_link_libraries(theseed_db PRIVATE PostgreSQL::PostgreSQL)
    set_property(GLOBAL PROPERTY theseed_postgresql_enabled TRUE)
    message(STATUS "theseed_db: PostgreSQL backend enabled (libpq found)")
else()
    message(STATUS "theseed_db: PostgreSQL backend disabled (libpq not found)")
endif()

target_link_libraries(
    theseed_db
    PUBLIC theseed_project_options
    PUBLIC theseed_runtime
    PUBLIC theseed_ops
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_db
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
