#pragma once

#include "theseed/core/FileEntityStore.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <cstdint>
#include <memory>
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
        std::string storePath = "data/entities";
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
    std::shared_ptr<core::FileEntityStore> store_;
    std::shared_ptr<runtime::TransportHub> hub_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::db
