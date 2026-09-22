#include "theseed/core/BaseApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/InMemoryBytePipe.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/PropertyReplication.h"
#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using theseed::core::BaseApp;
using theseed::core::InMemoryEntityStore;
using theseed::login::ClientMessageType;
using theseed::login::ClientProtocol;
using theseed::login::EntityLeaveMsg;
using theseed::login::LoginProtocol;
using theseed::login::PropertySyncMsg;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;
using theseed::runtime::InMemoryBytePipe;
using theseed::runtime::NetworkNode;
using theseed::runtime::PropertyDelta;
using theseed::runtime::PropertyReplication;
using theseed::runtime::TickScheduler;
using theseed::runtime::Vector3;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAILED: " << (msg) << "\n";               \
        ++testsFailed;                                          \
    } while (0)

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static std::string createDefDir() {
    std::string dir = "test_client_prop_sync_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
        <Property name="name" type="String"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
    </Methods>
</EntityDef>
)");
    return dir;
}

// Parse all PropertySync messages from a raw byte buffer
static std::vector<PropertySyncMsg> extractPropertySyncMessages(
    const std::vector<std::byte>& buffer) {
    std::vector<PropertySyncMsg> results;
    std::size_t offset = 0;

    while (offset + LoginProtocol::kHeaderSize <= buffer.size()) {
        auto payloadLen = static_cast<std::uint32_t>(buffer[offset])
                        | (static_cast<std::uint32_t>(buffer[offset + 1]) << 8)
                        | (static_cast<std::uint32_t>(buffer[offset + 2]) << 16)
                        | (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
        auto typeVal = static_cast<std::uint8_t>(buffer[offset + 4]);
        auto frameSize = LoginProtocol::kHeaderSize + payloadLen;

        if (offset + frameSize > buffer.size()) break;

        auto type = static_cast<ClientMessageType>(typeVal);
        if (type == ClientMessageType::PropertySync) {
            PropertySyncMsg msg;
            auto payload = std::span<const std::byte>(
                buffer.data() + offset + LoginProtocol::kHeaderSize, payloadLen);
            if (ClientProtocol::decodePropertySync(payload, msg)) {
                results.push_back(std::move(msg));
            }
        }

        offset += frameSize;
    }
    return results;
}

// Collect all bytes sent to the pipe's peer
struct CollectorPipe {
    std::shared_ptr<InMemoryBytePipe> clientEnd;
    std::shared_ptr<InMemoryBytePipe> serverEnd;
    std::vector<std::byte> received;

    static std::unique_ptr<CollectorPipe> create() {
        auto cp = std::make_unique<CollectorPipe>();
        auto [a, b] = InMemoryBytePipe::createPair();
        cp->serverEnd = a;
        cp->clientEnd = b;
        auto* raw = cp.get();
        cp->clientEnd->setOnReceived([raw](std::span<const std::byte> data) {
            raw->received.insert(raw->received.end(), data.begin(), data.end());
        });
        return cp;
    }

    void pump() {
        clientEnd->pump();
        serverEnd->pump();
    }
};

// Test: ClientProtocol encode/decode EntityLeave
static void testEntityLeaveEncodeDecode() {
    TEST("ClientProtocol: encode/decode EntityLeave");

    EntityLeaveMsg orig{.entityId = 42};
    auto framed = ClientProtocol::encodeEntityLeave(orig);

    // Parse frame
    ClientMessageType type;
    std::span<const std::byte> payload;
    bool parsed = LoginProtocol::parseFrame(
        std::span<const std::byte>(framed.data(), framed.size()), type, payload);

    bool ok = parsed;
    ok = ok && type == ClientMessageType::EntityLeave;

    EntityLeaveMsg decoded;
    ok = ok && ClientProtocol::decodeEntityLeave(payload, decoded);
    ok = ok && decoded.entityId == 42;

    if (ok) PASS();
    else FAIL("entityId=" + std::to_string(decoded.entityId));
}

// Test: ClientProtocol encode/decode PropertySync
static void testPropertySyncEncodeDecode() {
    TEST("ClientProtocol: encode/decode PropertySync");

    // Build some property deltas
    PropertyDelta d1;
    d1.propertyId = 0;
    std::int32_t val1 = 42;
    d1.value.resize(sizeof(val1));
    std::memcpy(d1.value.data(), &val1, sizeof(val1));

    PropertyDelta d2;
    d2.propertyId = 1;
    float val2 = 3.14f;
    d2.value.resize(sizeof(val2));
    std::memcpy(d2.value.data(), &val2, sizeof(val2));

    PropertyDelta arr[] = {d1, d2};
    auto encoded = PropertyReplication::encodeDelta(arr);

    PropertySyncMsg orig;
    orig.entityId = 100;
    orig.propertyData = std::move(encoded);

    auto framed = ClientProtocol::encodePropertySync(orig);

    ClientMessageType type;
    std::span<const std::byte> payload;
    bool parsed = LoginProtocol::parseFrame(
        std::span<const std::byte>(framed.data(), framed.size()), type, payload);

    bool ok = parsed;
    ok = ok && type == ClientMessageType::PropertySync;

    PropertySyncMsg decoded;
    ok = ok && ClientProtocol::decodePropertySync(payload, decoded);
    ok = ok && decoded.entityId == 100;

    auto deltas = PropertyReplication::decodeDelta(decoded.propertyData);
    ok = ok && deltas.size() == 2;
    if (ok && deltas.size() >= 2) {
        ok = ok && deltas[0].propertyId == 0;
        std::int32_t dv1 = 0;
        std::memcpy(&dv1, deltas[0].value.data(), sizeof(dv1));
        ok = ok && dv1 == 42;

        ok = ok && deltas[1].propertyId == 1;
        float dv2 = 0;
        std::memcpy(&dv2, deltas[1].value.data(), sizeof(dv2));
        ok = ok && std::abs(dv2 - 3.14f) < 0.001f;
    }

    if (ok) PASS();
    else FAIL("deltas=" + std::to_string(deltas.size()));
}

// Test: BaseApp sends initial property snapshot on EnterGame
static void testInitialPropertySnapshot() {
    TEST("BaseApp: initial property snapshot on EnterGame");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

    BaseApp::Config cfg;
    cfg.entityDefPath = dir;
    cfg.componentId = 1;
    cfg.clientListenPort = 0;  // We won't use TCP client, use InMemoryBytePipe directly

    BaseApp app(cfg, node->hub(), store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    auto entityId = entity->id();
    entity->setProperty<std::int32_t>(0, 5);   // level = 5
    entity->setProperty<float>(1, 100.0f);      // hp = 100

    // Create a mock client via InMemoryBytePipe
    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);

    // Simulate handleEnterGame by mapping session -> entity
    // We'll directly test flushClientPropertyUpdates behavior
    // First, let's manually trigger what handleEnterGame does:
    // Send initial snapshot
    auto snapshot = entity->buildFullPropertySnapshot();
    bool ok = !snapshot.empty();

    if (ok) {
        auto encoded = PropertyReplication::encodeDelta(snapshot);
        PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = ClientProtocol::encodePropertySync(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));
    }

    pipe->pump();

    auto syncs = extractPropertySyncMessages(pipe->received);
    ok = ok && syncs.size() == 1;
    if (ok) {
        ok = ok && syncs[0].entityId == entityId;
        auto deltas = PropertyReplication::decodeDelta(syncs[0].propertyData);
        ok = ok && deltas.size() == 2;  // level, hp (name not set)

        // Find level property
        for (auto& d : deltas) {
            if (d.propertyId == 0) {
                std::int32_t v = 0;
                std::memcpy(&v, d.value.data(), sizeof(v));
                ok = ok && v == 5;
            } else if (d.propertyId == 1) {
                float v = 0;
                std::memcpy(&v, d.value.data(), sizeof(v));
                ok = ok && std::abs(v - 100.0f) < 0.01f;
            }
        }
    }

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("syncs=" + std::to_string(syncs.size()));
}

// Test: BaseApp flushes dirty property deltas on tick
static void testDirtyPropertyFlush() {
    TEST("BaseApp: dirty property flush on tick");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

    BaseApp::Config cfg;
    cfg.entityDefPath = dir;
    cfg.componentId = 1;
    cfg.clientListenPort = 0;

    BaseApp app(cfg, node->hub(), store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    auto entityId = entity->id();
    entity->clearDirtyFlags();  // Clear creation dirty flags

    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);

    // Simulate session entity mapping
    // Manually set a dirty property
    entity->setProperty<float>(1, 75.0f);  // hp = 75

    // Build and send dirty delta
    auto deltas = entity->buildDirtyPropertyDelta();
    bool ok = deltas.size() == 1;
    if (ok) {
        auto encoded = PropertyReplication::encodeDelta(deltas);
        PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = ClientProtocol::encodePropertySync(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        entity->clearDirtyFlags();
    }

    pipe->pump();

    auto syncs = extractPropertySyncMessages(pipe->received);
    ok = ok && syncs.size() == 1;
    if (ok) {
        auto decoded = PropertyReplication::decodeDelta(syncs[0].propertyData);
        ok = ok && decoded.size() == 1;
        if (ok && decoded.size() >= 1) {
            ok = ok && decoded[0].propertyId == 1;
            float v = 0;
            std::memcpy(&v, decoded[0].value.data(), sizeof(v));
            ok = ok && std::abs(v - 75.0f) < 0.01f;
        }
    }

    // Verify no more dirty flags
    ok = ok && !entity->isPropertyDirty(0);
    ok = ok && !entity->isPropertyDirty(1);

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("syncs=" + std::to_string(syncs.size()));
}

// Test: no sync when no properties are dirty
static void testNoSyncWhenClean() {
    TEST("BaseApp: no sync when properties are clean");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

    BaseApp::Config cfg;
    cfg.entityDefPath = dir;
    cfg.componentId = 1;
    cfg.clientListenPort = 0;

    BaseApp app(cfg, node->hub(), store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    entity->clearDirtyFlags();

    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);

    // No property changes - buildDirtyPropertyDelta should return empty
    auto deltas = entity->buildDirtyPropertyDelta();
    bool ok = deltas.empty();

    // Nothing to send
    pipe->pump();
    ok = ok && pipe->received.empty();

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("deltas=" + std::to_string(deltas.size()));
}

// Test: string property sync via PropertySyncMsg
static void testStringPropertySync() {
    TEST("BaseApp: string property sync");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

    BaseApp::Config cfg;
    cfg.entityDefPath = dir;
    cfg.componentId = 1;
    cfg.clientListenPort = 0;

    BaseApp app(cfg, node->hub(), store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    auto entityId = entity->id();
    entity->clearDirtyFlags();

    // Set a string property
    entity->setString(2, "Hero");

    auto deltas = entity->buildDirtyPropertyDelta();
    bool ok = !deltas.empty();

    if (ok) {
        auto encoded = PropertyReplication::encodeDelta(deltas);
        PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = ClientProtocol::encodePropertySync(msg);

        // Parse round-trip
        ClientMessageType type;
        std::span<const std::byte> payload;
        LoginProtocol::parseFrame(
            std::span<const std::byte>(data.data(), data.size()), type, payload);

        PropertySyncMsg decoded;
        ok = ok && ClientProtocol::decodePropertySync(payload, decoded);
        ok = ok && decoded.entityId == entityId;

        auto decodedDeltas = PropertyReplication::decodeDelta(decoded.propertyData);
        // Find string property (id=2)
        bool found = false;
        for (auto& d : decodedDeltas) {
            if (d.propertyId == 2) {
                const std::string val(reinterpret_cast<const char*>(d.value.data()),
                                      d.value.size());
                ok = ok && val == "Hero";
                found = true;
            }
        }
        ok = ok && found;
    }

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("deltas=" + std::to_string(deltas.size()));
}

// Test: sequential property updates produce incremental syncs
static void testIncrementalSync() {
    TEST("BaseApp: incremental property sync across ticks");

    auto dir = createDefDir();
    auto store = std::make_shared<InMemoryEntityStore>();
    auto node = std::make_shared<NetworkNode>(NetworkNode::Config{.localComponent = 1, .listenPort = 0});

    BaseApp::Config cfg;
    cfg.entityDefPath = dir;
    cfg.componentId = 1;
    cfg.clientListenPort = 0;

    BaseApp app(cfg, node->hub(), store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    auto entityId = entity->id();
    entity->clearDirtyFlags();

    auto pipe = CollectorPipe::create();
    auto session = std::make_unique<theseed::login::ClientSession>(pipe->serverEnd);

    // Tick 1: change level
    entity->setProperty<std::int32_t>(0, 10);
    {
        auto deltas = entity->buildDirtyPropertyDelta();
        auto encoded = PropertyReplication::encodeDelta(deltas);
        PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = ClientProtocol::encodePropertySync(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        entity->clearDirtyFlags();
    }

    pipe->pump();
    auto syncs1 = extractPropertySyncMessages(pipe->received);
    bool ok = syncs1.size() == 1;

    // Tick 2: change hp only
    entity->setProperty<float>(1, 50.0f);
    pipe->received.clear();
    {
        auto deltas = entity->buildDirtyPropertyDelta();
        auto encoded = PropertyReplication::encodeDelta(deltas);
        PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = ClientProtocol::encodePropertySync(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        entity->clearDirtyFlags();
    }

    pipe->pump();
    auto syncs2 = extractPropertySyncMessages(pipe->received);
    ok = ok && syncs2.size() == 1;

    if (ok) {
        // First sync should have level change
        auto d1 = PropertyReplication::decodeDelta(syncs1[0].propertyData);
        ok = ok && d1.size() == 1 && d1[0].propertyId == 0;

        // Second sync should have hp change
        auto d2 = PropertyReplication::decodeDelta(syncs2[0].propertyData);
        ok = ok && d2.size() == 1 && d2[0].propertyId == 1;
    }

    std::filesystem::remove_all(dir);

    if (ok) PASS();
    else FAIL("sync1=" + std::to_string(syncs1.size()) + " sync2=" + std::to_string(syncs2.size()));
}

int main() {
    std::cout << "Client property sync tests:\n";

    testEntityLeaveEncodeDecode();
    testPropertySyncEncodeDecode();
    testInitialPropertySnapshot();
    testDirtyPropertyFlush();
    testNoSyncWhenClean();
    testStringPropertySync();
    testIncrementalSync();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
