#pragma once

#include "theseed/ops/OpsInspector.h"
#include "theseed/runtime/TcpListener.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::ops {

// Read-only HTTP/1.0 server exposing the OpsInspector.
//
// Endpoints (GET only):
//   /metrics   — Prometheus text
//   /health    — startup/liveness/readiness JSON
//   /inspect   — process + runtime JSON
//   /entities  — entity listing JSON
//
// Designed to be polled from the app's tick loop (non-blocking accept +
// per-connection read/parse/respond in a single tick).
class OpsServer final {
public:
    struct Config final {
        std::string host = "127.0.0.1";
        std::uint16_t port = 20010;
        std::size_t maxConnections = 8;
    };

    OpsServer(Config config, OpsInspector& inspector);
    ~OpsServer();

    OpsServer(const OpsServer&) = delete;
    OpsServer& operator=(const OpsServer&) = delete;

    bool start();
    void stop();
    void tick();

    bool isListening() const;
    std::uint16_t localPort() const;

private:
    struct PendingConnection {
        std::shared_ptr<runtime::TcpConnection> conn;
        std::shared_ptr<std::vector<std::byte>> recvBuffer;
    };

    void acceptNew();
    void servicePending();
    void respond(PendingConnection& pc, std::string body, const char* contentType,
                 const char* statusLine = "HTTP/1.0 200 OK");
    void respondBadRequest(PendingConnection& pc);

    Config config_;
    OpsInspector& inspector_;
    runtime::TcpListener listener_;
    std::vector<PendingConnection> pending_;
};

}  // namespace theseed::ops
