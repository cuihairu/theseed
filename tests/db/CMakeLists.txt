add_executable(theseed_db_app_test DBAppTest.cpp)

target_link_libraries(
    theseed_db_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_db)

add_test(NAME DBAppTest COMMAND theseed_db_app_test)

# 回环探针用 POSIX socket（arpa/inet.h、非阻塞 recv），Windows 暂不纳入；
# 其余单元测试全平台跑。
if(NOT WIN32)
    # DBApp 端到端测试：真实 TCP 回环驱动完整 tick 循环。
    # mysql/postgresql 场景由运行时环境变量门控，无需外部依赖即可跑 file 部分。
    add_executable(theseed_dbapp_e2e_test DBAppE2ETest.cpp)

    target_link_libraries(
        theseed_dbapp_e2e_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME DBAppE2ETest COMMAND theseed_dbapp_e2e_test)
    # 三个数据库测试共用同一套 theseed_test 库，建库/删库互踩，必须串行
    set_tests_properties(DBAppE2ETest PROPERTIES RUN_SERIAL ON)
endif()

# MySQL 集成测试：需要运行中的 MySQL 实例，未配置 THESEED_MYSQL_HOST 时测试自身跳过。
# 仅在 theseed_db 启用了 MySQL 后端（THESEED_MYSQL_ENABLED 全局属性为 TRUE）时构建。
get_property(THESEED_MYSQL_ENABLED GLOBAL PROPERTY theseed_mysql_enabled)
if(THESEED_MYSQL_ENABLED)
    add_executable(theseed_mysql_store_test MySqlEntityStoreTest.cpp)

    target_link_libraries(
        theseed_mysql_store_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME MySQLEntityStoreTest COMMAND theseed_mysql_store_test)
    set_tests_properties(MySQLEntityStoreTest PROPERTIES RUN_SERIAL ON)

    add_executable(theseed_mysql_connection_test MySQLConnectionTest.cpp)

    target_link_libraries(
        theseed_mysql_connection_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME MySQLConnectionTest COMMAND theseed_mysql_connection_test)
    set_tests_properties(MySQLConnectionTest PROPERTIES RUN_SERIAL ON)
endif()

# PostgreSQL 集成测试：需要运行中的 PostgreSQL 实例，未配置 THESEED_PG_HOST 时跳过。
get_property(THESEED_POSTGRESQL_ENABLED GLOBAL PROPERTY theseed_postgresql_enabled)
if(THESEED_POSTGRESQL_ENABLED)
    add_executable(theseed_pg_store_test PostgreSQLEntityStoreTest.cpp)

    target_link_libraries(
        theseed_pg_store_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME PostgreSQLEntityStoreTest COMMAND theseed_pg_store_test)
    set_tests_properties(PostgreSQLEntityStoreTest PROPERTIES RUN_SERIAL ON)

    add_executable(theseed_pg_connection_test PostgreSQLConnectionTest.cpp)

    target_link_libraries(
        theseed_pg_connection_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME PostgreSQLConnectionTest COMMAND theseed_pg_connection_test)
    set_tests_properties(PostgreSQLConnectionTest PROPERTIES RUN_SERIAL ON)
endif()
