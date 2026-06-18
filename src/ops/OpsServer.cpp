#include "theseed/ops/OpsServer.h"

#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace theseed::ops {

namespace {

constexpr std::string_view kContentText = "text/plain; version=0.0.4";
constexpr std::string_view kContentJson = "application/json";

}  // namespace

OpsServer::OpsServer(Config config, OpsInspector& inspector)
    : config_(std::move(config)), inspector_(inspector) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

OpsServer::~OpsServer() {
    stop();
}

bool OpsServer::start() {
    return listener_.listen(config_.host, config_.port);
}

void OpsServer::stop() {
    listener_.close();
    pending_.clear();
}

bool OpsServer::isListening() const {
    return listener_.isListening();
}

std::uint16_t OpsServer::localPort() const {
    return listener_.localPort();
}

void OpsServer::tick() {
    acceptNew();
    servicePending();
}

void OpsServer::acceptNew() {
    while (pending_.size() < config_.maxConnections) {
        auto conn = listener_.accept();
        if (!conn) break;

        PendingConnection pc;
        pc.conn = conn;
        pc.recvBuffer = std::make_shared<std::vector<std::byte>>();
        auto buffer = pc.recvBuffer;
        conn->setOnReceived([buffer](std::span<const std::byte> data) {
            buffer->insert(buffer->end(), data.begin(), data.end());
        });
        pending_.push_back(std::move(pc));
    }
}

void OpsServer::servicePending() {
    std::vector<std::size_t> completed;

    for (std::size_t i = 0; i < pending_.size(); ++i) {
        auto& pc = pending_[i];
        if (!pc.conn || !pc.conn->isConnected()) {
            completed.push_back(i);
            continue;
        }

        pc.conn->pump();

        if (pc.recvBuffer->empty()) {
            continue;
        }

        const auto view = std::string_view(
            reinterpret_cast<const char*>(pc.recvBuffer->data()),
            pc.recvBuffer->size());

        const auto eoh1 = view.find("\r\n\r\n");
        const auto eoh2 = view.find("\n\n");
        if (eoh1 == std::string_view::npos && eoh2 == std::string_view::npos) {
            if (pc.recvBuffer->size() > 8192) {
                respondBadRequest(pc);
                completed.push_back(i);
            }
            continue;
        }

        const auto eol = view.find('\n');
        const auto line = view.substr(0, eol);
        const auto sp1 = line.find(' ');
        const auto sp2 = line.find(' ', sp1 == std::string_view::npos ? 0 : sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
            respondBadRequest(pc);
            completed.push_back(i);
            continue;
        }
        std::string method(line.substr(0, sp1));
        std::string path(line.substr(sp1 + 1, sp2 - sp1 - 1));

        if (method != "GET") {
            respondBadRequest(pc);
            completed.push_back(i);
            continue;
        }

        if (auto q = path.find('?'); q != std::string::npos) {
            path = path.substr(0, q);
        }

        if (path == "/metrics") {
            respond(pc, inspector_.renderMetrics(), kContentText.data());
        } else if (path == "/health") {
            respond(pc, inspector_.renderHealthJson(), kContentJson.data());
        } else if (path == "/inspect") {
            respond(pc, inspector_.renderInspectJson(), kContentJson.data());
        } else if (path == "/entities") {
            respond(pc, inspector_.renderEntitiesJson(), kContentJson.data());
        } else {
            std::ostringstream body;
            body << "{\"error\":\"not found\",\"path\":\"" << path << "\"}";
            respond(pc, body.str(), kContentJson.data());
        }
        completed.push_back(i);
    }

    std::sort(completed.rbegin(), completed.rend());
    for (std::size_t i : completed) {
        if (i < pending_.size()) {
            if (pending_[i].conn) pending_[i].conn->close();
            pending_.erase(pending_.begin() + i);
        }
    }
}

void OpsServer::respond(PendingConnection& pc, std::string body, const char* contentType) {
    std::ostringstream resp;
    resp << "HTTP/1.0 200 OK\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;

    const std::string out = resp.str();
    pc.conn->write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(out.data()), out.size()));
}

void OpsServer::respondBadRequest(PendingConnection& pc) {
    static constexpr std::string_view kBody = "{\"error\":\"bad request\"}";
    std::ostringstream resp;
    resp << "HTTP/1.0 400 Bad Request\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << kBody.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << kBody;

    const std::string out = resp.str();
    pc.conn->write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(out.data()), out.size()));
}

}  // namespace theseed::ops
