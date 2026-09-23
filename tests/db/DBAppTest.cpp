#include "theseed/core/EntityData.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/db/DBApp.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/db/RemoteEntityStore.h"
#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace theseed::db;
using namespace theseed::core;
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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

int main() {
    std::cout << "DBApp tests:" << std::endl;

    // Create temp store directory
    std::string storeDir = "test_dbapp_store";
    std::filesystem::remove_all(storeDir);
    std::filesystem::create_directory(storeDir);

    TEST("DBProtocol save/load round trip");
    {
        // Test full protocol encode → decode cycle
        EntityData data;
        data.id = 1;
        data.entityType = "Avatar";
        PropertyData prop;
        prop.id = 0;
        prop.name = "level";
        prop.type = DataType::Int32;
        prop.rawValue = {std::byte{42}, std::byte{0}, std::byte{0}, std::byte{0}};
        data.properties.push_back(prop);

        auto req = DBProtocol::encodeSaveRequest(1, data);
        EntityId outId;
        EntityData outData;
        if (!DBProtocol::decodeSaveRequest(
                std::span<const std::byte>(req.data(), req.size()),
                outId, outData))
            FAIL("decode save request failed");
        if (outId != 1) FAIL("id mismatch");
        if (outData.entityType != "Avatar") FAIL("type mismatch");

        auto resp = DBProtocol::encodeSaveResponse(true);
        bool success;
        if (!DBProtocol::decodeSaveResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                success))
            FAIL("decode save response failed");
        if (!success) FAIL("should be success");
    }
    PASS();

    TEST("DBProtocol encode/decode load request");
    {
        auto req = DBProtocol::encodeLoadRequest(42, "Avatar");
        EntityId outId;
        std::string outType;
        if (!DBProtocol::decodeLoadRequest(
                std::span<const std::byte>(req.data(), req.size()),
                outId, outType))
            FAIL("decode failed");
        if (outId != 42) FAIL("id mismatch");
        if (outType != "Avatar") FAIL("type mismatch");
    }
    PASS();

    TEST("DBProtocol encode/decode allocId response");
    {
        auto resp = DBProtocol::encodeAllocIdResponse(123);
        EntityId outId;
        if (!DBProtocol::decodeAllocIdResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outId))
            FAIL("decode failed");
        if (outId != 123) FAIL("id mismatch");
    }
    PASS();

    TEST("DBProtocol encode/decode listIds response");
    {
        std::vector<EntityId> ids = {1, 2, 3};
        auto resp = DBProtocol::encodeListIdsResponse(ids);
        std::vector<EntityId> outIds;
        if (!DBProtocol::decodeListIdsResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outIds))
            FAIL("decode failed");
        if (outIds.size() != 3) FAIL("count mismatch");
        if (outIds[0] != 1 || outIds[1] != 2 || outIds[2] != 3) FAIL("ids mismatch");
    }
    PASS();

    TEST("DBProtocol empty-body request encoders");
    {
        // allocId 与 listTypes 请求没有请求体，编码结果必须为空。
        if (!DBProtocol::encodeAllocIdRequest().empty()) FAIL("allocId request should be empty");
        if (!DBProtocol::encodeListTypesRequest().empty()) FAIL("listTypes request should be empty");
    }
    PASS();

    TEST("DBProtocol encode/decode listTypes response");
    {
        std::vector<std::string> types = {"Avatar", "Monster"};
        auto resp = DBProtocol::encodeListTypesResponse(types);
        std::vector<std::string> outTypes;
        if (!DBProtocol::decodeListTypesResponse(
                std::span<const std::byte>(resp.data(), resp.size()),
                outTypes))
            FAIL("decode failed");
        if (outTypes.size() != 2) FAIL("count mismatch");
        if (outTypes[0] != "Avatar") FAIL("type mismatch");
    }
    PASS();

    // RemoteEntityStore：响应 method 匹配但 payload 为空 → decode 失败路径；
    // receive 首轮空转命中 request() 泵循环回边。
    TEST("RemoteEntityStore fails gracefully on malformed responses");
    {
        class CannedTransport final : public IRuntimeTransport {
        public:
            explicit CannedTransport(RuntimeInvocation resp)
                : resp_(std::move(resp)) {}

            SendResult send(RuntimeInvocation) override { return SendResult::Accepted; }
            std::size_t receive(ComponentId, RuntimeInvocation* out,
                                std::size_t) override {
                if (calls_++ == 0) return 0;  // 首轮空转：响应"还没到"
                *out = resp_;
                return 1;
            }
            std::size_t pendingCount() const override { return 1; }
            void flush() override {}
            TransportStats stats() const override { return {}; }

        private:
            RuntimeInvocation resp_;
            int calls_ = 0;
        };

        // 空 payload 让四个 decode*Response 都返回 false，同时 method
        // 与各自的 Ok 常量匹配，绕过 early-return 直达 decode 分支。
        auto canned = [](const char* method) {
            RuntimeInvocation resp;
            resp.sourceComponent = 10;
            resp.targetComponent = 20;
            resp.method = method;
            return resp;
        };

        EntityData data;
        data.entityType = "Avatar";

        {
            auto transport = std::make_shared<CannedTransport>(canned(DBMethod::kLoadOk));
            RemoteEntityStore store(transport, 10, 20);
            if (store.load(1, "Avatar", data)) FAIL("load should fail on empty payload");
        }
        {
            auto transport = std::make_shared<CannedTransport>(canned(DBMethod::kSaveOk));
            RemoteEntityStore store(transport, 10, 20);
            if (store.save(1, data)) FAIL("save should fail on empty payload");
        }
        {
            auto transport = std::make_shared<CannedTransport>(canned(DBMethod::kRemoveOk));
            RemoteEntityStore store(transport, 10, 20);
            if (store.remove(1)) FAIL("remove should fail on empty payload");
        }
        {
            auto transport = std::make_shared<CannedTransport>(canned(DBMethod::kAllocIdOk));
            RemoteEntityStore store(transport, 10, 20);
            if (store.allocId() != 0) FAIL("allocId should yield 0 on decode failure");
        }
    }
    PASS();

    // 超时语义：DBApp 永不应答时 request() 在 requestTimeout 内返回
    // method 为空的 RuntimeInvocation，各调用方按失败处理而不是挂死。
    TEST("RemoteEntityStore times out when DBApp never responds");
    {
        class NeverRespondTransport final : public IRuntimeTransport {
        public:
            SendResult send(RuntimeInvocation) override { return SendResult::Accepted; }
            std::size_t receive(ComponentId, RuntimeInvocation*, std::size_t) override { return 0; }
            std::size_t pendingCount() const override { return 0; }
            void flush() override {}
            TransportStats stats() const override { return {}; }
        };

        RemoteEntityStore store(std::make_shared<NeverRespondTransport>(), 10, 20,
                                std::chrono::milliseconds{10});

        EntityData data;
        data.entityType = "Avatar";
        if (store.load(1, "Avatar", data)) FAIL("load must time out");
        if (store.save(1, data)) FAIL("save must time out");
        if (store.remove(1)) FAIL("remove must time out");
        if (store.allocId() != 0) FAIL("allocId must yield 0 on timeout");
        if (!store.listIdsByType("Avatar").empty()) FAIL("listIdsByType must be empty on timeout");
        if (!store.listEntityTypes().empty()) FAIL("listEntityTypes must be empty on timeout");
    }
    PASS();

    // 发送失败路径：send 返回 NotConnected 时立即失败，不进入等待循环。
    TEST("RemoteEntityStore fails fast when send is rejected");
    {
        class SendFailTransport final : public IRuntimeTransport {
        public:
            SendResult send(RuntimeInvocation) override { return SendResult::NotConnected; }
            std::size_t receive(ComponentId, RuntimeInvocation*, std::size_t) override { return 0; }
            std::size_t pendingCount() const override { return 0; }
            void flush() override {}
            TransportStats stats() const override { return {}; }
        };

        RemoteEntityStore store(std::make_shared<SendFailTransport>(), 10, 20,
                                std::chrono::milliseconds{10});

        EntityData data;
        data.entityType = "Avatar";
        if (store.load(1, "Avatar", data)) FAIL("load must fail on NotConnected");
        if (store.save(1, data)) FAIL("save must fail on NotConnected");
    }
    PASS();

    // 杂散应答：method 不匹配 "<method>.ok" 的过期消息应被丢弃并继续等待，
    // 直到超时按失败处理。
    TEST("RemoteEntityStore discards stray-method responses until timeout");
    {
        class StrayTransport final : public IRuntimeTransport {
        public:
            SendResult send(RuntimeInvocation inv) override {
                RuntimeInvocation resp;
                resp.sourceComponent = inv.targetComponent;
                resp.targetComponent = inv.sourceComponent;
                resp.method = "db.bogus.ok";  // 与任何请求的 "<method>.ok" 都不匹配
                inbox_.push_back(std::move(resp));
                return SendResult::Accepted;
            }
            std::size_t receive(ComponentId, RuntimeInvocation* out, std::size_t) override {
                if (inbox_.empty()) return 0;
                *out = inbox_.front();
                inbox_.pop_back();
                return 1;
            }
            std::size_t pendingCount() const override { return inbox_.size(); }
            void flush() override {}
            TransportStats stats() const override { return {}; }

        private:
            std::vector<RuntimeInvocation> inbox_;
        };

        RemoteEntityStore store(std::make_shared<StrayTransport>(), 10, 20,
                                std::chrono::milliseconds{10});

        EntityData data;
        data.entityType = "Avatar";
        if (store.load(1, "Avatar", data)) FAIL("stray responses must not satisfy load");
    }
    PASS();

    std::cout << "\nAll DBApp tests passed!" << std::endl;
    return 0;
}
