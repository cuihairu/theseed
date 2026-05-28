#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/realm/RealmApp.h"
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
using namespace theseed::realm;
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

    void sendQueryRealms() {
        std::vector<std::byte> empty;
        auto frame = LoginProtocol::frameMessage(ClientMessageType::QueryRealms,
                                                  std::span<const std::byte>(empty.data(), 0));
        clientPipe->write(std::span<const std::byte>(frame.data(), frame.size()));
    }

    void pump() {
        clientPipe->pump();
        serverPipe->pump();
    }

    bool parseResponse(ClientMessageType& outType, std::span<const std::byte>& outPayload) {
        if (receivedData.size() < LoginProtocol::kHeaderSize) return false;
        return LoginProtocol::parseFrame(
            std::span<const std::byte>(receivedData.data(), receivedData.size()),
            outType, outPayload);
    }
};

int main() {
    std::cout << "RealmApp tests:" << std::endl;

    TEST("query realms returns configured list");
    {
        RealmAppConfig config;
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
        r2.port = 20001;
        config.realms.push_back(r1);
        config.realms.push_back(r2);

        RealmApp app(std::move(config));
        if (app.realms().size() != 2) FAIL("realm count");
        if (app.realms()[0].realmId != "east") FAIL("first realm id");
        if (app.realms()[1].realmId != "south") FAIL("second realm id");
    }
    PASS();

    TEST("client receives realm list via pipe");
    {
        RealmAppConfig config;
        RealmInfo r;
        r.realmId = "default";
        r.name = "Default";
        r.status = "smooth";
        r.host = "127.0.0.1";
        r.port = 20000;
        config.realms.push_back(r);

        RealmApp app(std::move(config));

        MockClient client;
        ClientSession session(client.serverPipe);

        session.setMessageCallback([&app, &session](ClientMessageType type,
                                                      std::span<const std::byte> payload) {
            if (type == ClientMessageType::QueryRealms) {
                auto data = LoginProtocol::encodeRealmList(app.realms());
                session.send(std::span<const std::byte>(data.data(), data.size()));
            }
        });

        client.sendQueryRealms();
        client.pump();
        session.pump();
        client.pump();

        ClientMessageType respType;
        std::span<const std::byte> respPayload;
        if (!client.parseResponse(respType, respPayload)) FAIL("no response");
        if (respType != ClientMessageType::QueryRealmsResponse) FAIL("wrong response type");
    }
    PASS();

    TEST("empty realm list");
    {
        RealmAppConfig config;
        RealmApp app(std::move(config));
        if (app.realms().size() != 0) FAIL("should be empty");
    }
    PASS();

    std::cout << "\nAll RealmApp tests passed!" << std::endl;
    return 0;
}
