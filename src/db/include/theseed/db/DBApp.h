#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/db/IAccountStore.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace theseed::db {

class DBApp {
public:
    struct OpsConfig final {
        bool enabled = false;
        std::string host = "127.0.0.1";
        std::uint16_t port = 20030;
        std::size_t maxConnections = 8;
    };

    struct Config {
        std::string listenHost = "0.0.0.0";
        std::uint16_t listenPort = 20003;
        // FileEntityStore 的根目录（storeBackend == "file" 时使用）
        std::string storePath = "data/entities";
        // 存储后端："file"（默认，FileEntityStore）、"mysql"（MySQLEntityStore）
        // 或 "postgresql"（PostgreSQLEntityStore）
        std::string storeBackend = "file";
        // SQL 后端连接配置（storeBackend == "mysql"/"postgresql" 时共用）。
        // dbPort 默认值跟随 MySQL；dbapp 在 --backend postgresql 且未显式
        // 指定端口时会改用 5432。
        std::string dbHost = "127.0.0.1";
        std::uint16_t dbPort = 3306;
        std::string dbUser = "theseed";
        std::string dbPassword;
        std::string dbDatabase = "theseed";
        bool dbAutoCreateSchema = true;
        runtime::ComponentId componentId = 10;
        OpsConfig ops;
    };

    explicit DBApp(Config config);
    ~DBApp();

    DBApp(const DBApp&) = delete;
    DBApp& operator=(const DBApp&) = delete;

    bool init();
    void tick();
    void stop();

private:
    void acceptConnections();
    void processMessages();
    void handleInvocation(runtime::RuntimeInvocation& inv);

    void handleLoad(const runtime::RuntimeInvocation& inv);
    void handleSave(const runtime::RuntimeInvocation& inv);
    void handleRemove(const runtime::RuntimeInvocation& inv);
    void handleAllocId(const runtime::RuntimeInvocation& inv);
    void handleListIds(const runtime::RuntimeInvocation& inv);
    void handleListTypes(const runtime::RuntimeInvocation& inv);
    void handleQueryAccount(const runtime::RuntimeInvocation& inv);
    void handleCreateAccount(const runtime::RuntimeInvocation& inv);

    void sendResponse(runtime::ComponentId target,
                      const std::string& method,
                      std::span<const std::byte> payload);

    Config config_;
    runtime::TcpListener listener_;
    // 后端无关的存储句柄。init() 根据 storeBackend 选择 FileEntityStore
    // 或 MySQLEntityStore。FileEntityStore 不实现 IAccountStore，
    // 此时 accountStore_ 为 nullptr，DBApp 回退到基于 store_ 的线性扫描。
    std::shared_ptr<core::IEntityStore> store_;
    std::shared_ptr<IAccountStore> accountStore_;
    std::shared_ptr<runtime::TransportHub> hub_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::db
