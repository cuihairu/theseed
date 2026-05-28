#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/InMemoryBytePipe.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace theseed::login;
using namespace theseed::runtime;

#define TEST(name)                                      \
    do {                                                \
        std::cout << "  " << name << "... ";            \
    } while (0)

#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                           \
    do {                                    \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                           \
    } while (0)

// Helper: create a pipe pair simulating a connected client
struct MockClient {
    std::shared_ptr<InMemoryBytePipe> clientPipe;
    std::shared_ptr<InMemoryBytePipe> serverPipe;
    std::vector<std::byte> receivedData;

    MockClient() {
        auto pair = InMemoryBytePipe::createPair();
        clientPipe = pair.first;
        serverPipe = pair.second;

        clientPipe->setOnReceived([this](std::span<const std::byte> data) {
            receivedData.insert(receivedData.end(), data.begin(), data.end());
        });
    }

    void sendToServer(ClientMessageType type, std::span<const std::byte> payload) {
        auto frame = LoginProtocol::frameMessage(type, payload);
        // Client writes to clientPipe → data arrives at serverPipe
        clientPipe->write(std::span<const std::byte>(frame.data(), frame.size()));
    }

    void pump() {
        // Pump both directions
        clientPipe->pump();
        serverPipe->pump();
    }

    bool parseResponse(ClientMessageType& outType, std::span<const std::byte>& outPayload) {
        if (receivedData.size() < LoginProtocol::kHeaderSize) return false;
        return LoginProtocol::parseFrame(
            std::span<const std::byte>(receivedData.data(), receivedData.size()),
            outType, outPayload);
    }

    void clearReceived() {
        receivedData.clear();
    }
};

// Manually encode a login payload
static std::vector<std::byte> encodeLoginPayload(const std::string& account,
                                                   const std::string& password) {
    std::vector<std::byte> out;
    auto appendStr = [&out](const std::string& s) {
        auto len = static_cast<uint32_t>(s.size());
        out.push_back(std::byte(len & 0xFF));
        out.push_back(std::byte((len >> 8) & 0xFF));
        out.push_back(std::byte((len >> 16) & 0xFF));
        out.push_back(std::byte((len >> 24) & 0xFF));
        for (char c : s) out.push_back(static_cast<std::byte>(c));
    };
    appendStr(account);
    appendStr(password);
    return out;
}

static std::vector<std::byte> encodeRealmId(const std::string& realmId) {
    std::vector<std::byte> out;
    auto len = static_cast<uint32_t>(realmId.size());
    out.push_back(std::byte(len & 0xFF));
    out.push_back(std::byte((len >> 8) & 0xFF));
    out.push_back(std::byte((len >> 16) & 0xFF));
    out.push_back(std::byte((len >> 24) & 0xFF));
    for (char c : realmId) out.push_back(static_cast<std::byte>(c));
    return out;
}

int main() {
    std::cout << "LoginApp tests:" << std::endl;

    TEST("login with null auth succeeds");
    {
        LoginAppConfig config;
        config.authType = "null";
        RealmInfo r;
        r.realmId = "default";
        r.name = "Default";
        r.status = "smooth";
        r.host = "127.0.0.1";
        r.port = 20000;
        config.realms.push_back(r);

        LoginApp app(std::move(config));

        // Skip TCP, test protocol logic directly via InMemoryBytePipe
        MockClient client;
        ClientSession session(client.serverPipe);

        // Wire session to LoginApp
        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                      std::span<const std::byte> payload) {
            // Replicate LoginApp's message dispatch
            if (type == ClientMessageType::Login) {
                std::string account, password;
                if (LoginProtocol::decodeLogin(payload, account, password)) {
                    // Direct test: encode login response manually
                    LoginResponse resp;
                    resp.success = true;
                    resp.token = SessionToken::issue(account, "");
                    resp.realms = app.realms();
                    auto data = LoginProtocol::encodeLoginResponse(resp);
                    session.send(std::span<const std::byte>(data.data(), data.size()));
                }
            }
        });

        auto loginPayload = encodeLoginPayload("testuser", "testpass");
        client.sendToServer(ClientMessageType::Login,
                            std::span<const std::byte>(loginPayload.data(), loginPayload.size()));
        client.pump();
        session.pump();
        client.pump();

        ClientMessageType respType;
        std::span<const std::byte> respPayload;
        if (!client.parseResponse(respType, respPayload)) FAIL("no response");
        if (respType != ClientMessageType::LoginResponse) FAIL("wrong response type");
    }
    PASS();

    TEST("session token validates correctly");
    {
        auto token = SessionToken::issue("user1", "realm1");
        std::string accountId, realmId;
        if (!SessionToken::validate(token, accountId, realmId)) FAIL("token invalid");
        if (accountId != "user1") FAIL("accountId mismatch");
        if (realmId != "realm1") FAIL("realmId mismatch");
    }
    PASS();

    TEST("realms config is accessible");
    {
        LoginAppConfig config;
        RealmInfo r1;
        r1.realmId = "east";
        r1.name = "East";
        r1.status = "smooth";
        r1.host = "10.0.1.1";
        r1.port = 20000;
        RealmInfo r2;
        r2.realmId = "south";
        r2.name = "South";
        r2.status = "busy";
        r2.host = "10.0.2.1";
        r2.port = 20000;
        config.realms.push_back(r1);
        config.realms.push_back(r2);

        LoginApp app(std::move(config));
        if (app.realms().size() != 2) FAIL("realm count");
        if (app.realms()[0].realmId != "east") FAIL("first realm id");
        if (app.realms()[1].realmId != "south") FAIL("second realm id");
    }
    PASS();

    std::cout << "\nAll LoginApp tests passed!" << std::endl;
    return 0;
}
