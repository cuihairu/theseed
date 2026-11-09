// RemoteEntityStore 直测：IEntityStore 的远程实现，把六个存储操作翻译成对
// DBApp 组件的 RuntimeInvocation 请求-应答。既有 DBAppE2ETest 从完整 TCP
// tick 循环视角端到端驱动过它，这里用脚本化 transport 逐臂锚定：请求构造
// （组件路由/method/payload 可解回）、六操作正常往返、杂散应答丢弃后仍命
// 中、静默超时降级（不挂死）、发送拒绝立即降级、pumpFn 在等待循环中被驱动。
#include "theseed/db/RemoteEntityStore.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/db/EntityData.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace theseed::core;
using namespace theseed::db;
using namespace theseed::runtime;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                   \
    do {                                            \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                   \
    } while (0)

namespace {

constexpr ComponentId kDbComponent = 7;
constexpr ComponentId kLocalComponent = 3;

// 超时臂用短预算：六操作各等满 30ms 也在 200ms 量级，不拖 suite；
// 仍远大于调度毛刺，超时判定不吃环境负载。
constexpr auto kShortTimeout = std::chrono::milliseconds{30};

EntityData sampleEntity(EntityId id) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";

    PropertyData level;
    level.id = 0;
    level.name = "level";
    level.type = DataType::Int32;
    level.rawValue = {std::byte{42}, std::byte{0}, std::byte{0}, std::byte{0}};
    data.properties.push_back(level);

    PropertyData name;
    name.id = 1;
    name.name = "name";
    name.type = DataType::String;
    name.rawValue = {std::byte{'a'}, std::byte{'v'}, std::byte{'a'}};
    data.properties.push_back(name);

    return data;
}

// 全字段一致性判定：false = 已打印失配明细，调用方沿 main 的 FAIL 路径退出。
bool expectSameEntity(const EntityData& expected, const EntityData& actual,
                      const char* what) {
    if (actual.id != expected.id) {
        std::cout << "FAILED: " << what << ": id mismatch" << std::endl;
        return false;
    }
    if (actual.entityType != expected.entityType) {
        std::cout << "FAILED: " << what << ": entityType mismatch" << std::endl;
        return false;
    }
    if (actual.properties.size() != expected.properties.size()) {
        std::cout << "FAILED: " << what << ": property count mismatch" << std::endl;
        return false;
    }
    for (std::size_t i = 0; i < expected.properties.size(); ++i) {
        const auto& e = expected.properties[i];
        const auto& a = actual.properties[i];
        if (a.id != e.id || a.name != e.name || a.type != e.type ||
            a.rawValue != e.rawValue) {
            std::cout << "FAILED: " << what << ": property " << i
                      << " mismatch" << std::endl;
            return false;
        }
    }
    return true;
}

// 脚本化 transport：send 按 mode 决定入站应答（正确 .ok / 先杂散后正确 /
// 静默吞掉 / 拒收），receive 从 inbox 弹出。单线程假设——store 的等待循
// 环在同一线程内 send/receive，无锁。
class ScriptedDbTransport final : public IRuntimeTransport {
public:
    enum class Mode { Canned, StrayThenOk, Silent, Closed, Malformed };

    SendResult send(RuntimeInvocation invocation) override {
        ++sends;
        lastInvocation = invocation;
        if (mode == Mode::Closed) return SendResult::Closed;
        if (mode == Mode::Silent) return SendResult::Accepted;

        RuntimeInvocation reply;
        reply.sourceComponent = kDbComponent;
        reply.targetComponent = lastInvocation.sourceComponent;
        reply.entityId = lastInvocation.entityId;
        // P2 深审回归：服务端必须原样回传 requestId，store 按它关联响应。
        reply.requestId = lastInvocation.requestId;
        reply.method = lastInvocation.method + ".ok";
        if (mode == Mode::Malformed) {
            // method 正确但 payload 为空/截断：触发各 decode*Response 的
            // 失败臂（读不足即 false）。非 0xFF 单字节——部分解码器宽松
            // 读 u8 success 会接受，必须用不足长度。
            reply.payload = {};
        } else {
            reply.payload = cannedReply(lastInvocation.method);
        }

        if (mode == Mode::StrayThenOk) {
            RuntimeInvocation stray = reply;
            stray.method = lastInvocation.method + ".bogus.ok";
            inbox_.push_back(std::move(stray));
        }
        inbox_.push_back(std::move(reply));
        return SendResult::Accepted;
    }

    std::size_t receive(ComponentId, RuntimeInvocation* out,
                        std::size_t capacity) override {
        std::size_t delivered = 0;
        while (delivered < capacity && !inbox_.empty()) {
            *out++ = std::move(inbox_.front());
            inbox_.pop_front();
            ++delivered;
        }
        return delivered;
    }

    std::size_t pendingCount() const override { return inbox_.size(); }
    void flush() override { ++flushes; }
    theseed::runtime::TransportStats stats() const override { return {}; }

    void injectInbox(RuntimeInvocation inv) { inbox_.push_back(std::move(inv)); }

    Mode mode = Mode::Canned;
    int sends = 0;
    int flushes = 0;
    RuntimeInvocation lastInvocation;
    EntityData cannedData;
    EntityId cannedAllocId = 77;
    std::vector<EntityId> cannedIds;
    std::vector<std::string> cannedTypes;

private:
    std::vector<std::byte> cannedReply(const std::string& method) const {
        if (method == DBMethod::kLoad)
            return DBProtocol::encodeLoadResponse(true, cannedData);
        if (method == DBMethod::kSave) return DBProtocol::encodeSaveResponse(true);
        if (method == DBMethod::kRemove)
            return DBProtocol::encodeRemoveResponse(true);
        if (method == DBMethod::kAllocId)
            return DBProtocol::encodeAllocIdResponse(cannedAllocId);
        if (method == DBMethod::kListIds)
            return DBProtocol::encodeListIdsResponse(cannedIds);
        if (method == DBMethod::kListTypes)
            return DBProtocol::encodeListTypesResponse(cannedTypes);
        return {};
    }

    std::deque<RuntimeInvocation> inbox_;
};

// 栈上 fake + 空删除器：store 只在 request 期间持引用，生命周期由 main
// 作用域保证。
RemoteEntityStore makeStore(ScriptedDbTransport& transport,
                            std::chrono::milliseconds requestTimeout =
                                std::chrono::milliseconds{5000}) {
    return RemoteEntityStore(
        std::shared_ptr<IRuntimeTransport>(&transport, [](IRuntimeTransport*) {}),
        kDbComponent, kLocalComponent, requestTimeout);
}

}  // namespace

int main() {
    std::cout << "RemoteEntityStoreTest:" << std::endl;

    TEST("load round-trips entity data and captures request construction");
    {
        ScriptedDbTransport transport;
        transport.cannedData = sampleEntity(88);
        auto store = makeStore(transport);

        EntityData out;
        if (!store.load(88, "Avatar", out)) FAIL("load failed on canned reply");
        if (!expectSameEntity(transport.cannedData, out, "load")) return 1;

        // 请求构造：路由到 DB 组件、回指本地组件、method 与 payload 可解回。
        const auto& inv = transport.lastInvocation;
        if (inv.targetComponent != kDbComponent)
            FAIL("request must target the db component");
        if (inv.sourceComponent != kLocalComponent)
            FAIL("request must identify the local component");
        if (inv.method != DBMethod::kLoad) FAIL("load must send db.load");
        EntityId id = 0;
        std::string type;
        if (!DBProtocol::decodeLoadRequest(inv.payload, id, type))
            FAIL("load request payload undecodable");
        if (id != 88 || type != "Avatar")
            FAIL("load request payload mismatch");
        PASS();
    }

    TEST("save round-trips id and entity data");
    {
        ScriptedDbTransport transport;
        auto store = makeStore(transport);
        const auto data = sampleEntity(42);

        if (!store.save(42, data)) FAIL("save failed on canned reply");
        if (transport.lastInvocation.method != DBMethod::kSave)
            FAIL("save must send db.save");
        EntityId id = 0;
        EntityData out;
        if (!DBProtocol::decodeSaveRequest(transport.lastInvocation.payload, id, out))
            FAIL("save request payload undecodable");
        if (id != 42) FAIL("save request id mismatch");
        if (!expectSameEntity(data, out, "save request payload")) return 1;
        PASS();
    }

    TEST("remove round-trips id");
    {
        ScriptedDbTransport transport;
        auto store = makeStore(transport);

        if (!store.remove(0xA5A5A5A5A5A5A5A5ull))
            FAIL("remove failed on canned reply");
        if (transport.lastInvocation.method != DBMethod::kRemove)
            FAIL("remove must send db.remove");
        EntityId id = 0;
        if (!DBProtocol::decodeRemoveRequest(transport.lastInvocation.payload, id))
            FAIL("remove request payload undecodable");
        if (id != 0xA5A5A5A5A5A5A5A5ull) FAIL("remove request id mismatch");
        PASS();
    }

    TEST("allocId returns the id from db.allocId.ok");
    {
        ScriptedDbTransport transport;
        transport.cannedAllocId = 0x123456789ABCDEF0ull;
        auto store = makeStore(transport);

        if (store.allocId() != 0x123456789ABCDEF0ull)
            FAIL("allocId round-trip mismatch");
        if (transport.lastInvocation.method != DBMethod::kAllocId)
            FAIL("allocId must send db.allocId");
        PASS();
    }

    TEST("listIdsByType round-trips id list and request entityType");
    {
        ScriptedDbTransport transport;
        transport.cannedIds = {0, 5, 0xFFFFFFFFFFFFFFFFull};
        auto store = makeStore(transport);

        const auto ids = store.listIdsByType("Avatar");
        if (ids != transport.cannedIds) FAIL("listIdsByType round-trip mismatch");
        if (transport.lastInvocation.method != DBMethod::kListIds)
            FAIL("listIdsByType must send db.listIds");
        {
            // 请求载荷是长度前缀字符串（协议无专用解码器，DBApp 直接读流）。
            theseed::foundation::MemoryStream ms;
            ms.writeBytes(transport.lastInvocation.payload.data(),
                          transport.lastInvocation.payload.size());
            ms.resetRead();
            if (ms.readString() != "Avatar")
                FAIL("listIdsByType request entityType mismatch");
        }
        PASS();
    }

    TEST("listEntityTypes round-trips type list");
    {
        ScriptedDbTransport transport;
        transport.cannedTypes = {"Avatar", "Account"};
        auto store = makeStore(transport);

        const auto types = store.listEntityTypes();
        if (types != transport.cannedTypes)
            FAIL("listEntityTypes round-trip mismatch");
        if (transport.lastInvocation.method != DBMethod::kListTypes)
            FAIL("listEntityTypes must send db.listTypes");
        PASS();
    }

    // P2 深审回归：旧实现按 method（"db.load.ok"）关联响应，不校验
    // requestId——上一轮超时后迟到的 db.load.ok 会被新请求误认领。必须
    // 按 requestId 关联：先注入一个 method 相同但 requestId 不同的陈旧
    // 响应，再注入真正的应答，store 应丢弃陈旧响应并消费真应答。
    TEST("stale same-method reply with wrong requestId is discarded");
    {
        ScriptedDbTransport transport;
        transport.mode = ScriptedDbTransport::Mode::Canned;
        transport.cannedData = sampleEntity(123);
        auto store = makeStore(transport);

        // 注入陈旧响应：method 是 db.load.ok，但 requestId 与本次请求不同。
        RuntimeInvocation stale;
        stale.sourceComponent = kDbComponent;
        stale.targetComponent = kLocalComponent;
        stale.entityId = 999;
        stale.requestId = 0xDEADBEEF;  // 任意非本次 requestId
        stale.method = std::string(DBMethod::kLoad) + ".ok";
        stale.payload = DBProtocol::encodeLoadResponse(true, sampleEntity(999));
        transport.injectInbox(std::move(stale));

        EntityData out;
        if (!store.load(123, "Avatar", out))
            FAIL("load failed when a stale same-method reply was queued first");
        if (!expectSameEntity(transport.cannedData, out, "stale-then-real load"))
            return 1;
        PASS();
    }

    TEST("stray reply is discarded and the real reply still wins");
    {
        ScriptedDbTransport transport;
        transport.mode = ScriptedDbTransport::Mode::StrayThenOk;
        transport.cannedData = sampleEntity(9);
        auto store = makeStore(transport);

        EntityData out;
        if (!store.load(9, "Avatar", out))
            FAIL("load failed with a stray reply queued first");
        if (!expectSameEntity(transport.cannedData, out, "stray-then-ok load"))
            return 1;
        if (transport.sends != 1)
            FAIL("stray discard must not retransmit (sends=" +
                 std::to_string(transport.sends) + ")");
        PASS();
    }

    TEST("silence degrades every operation without hanging");
    {
        ScriptedDbTransport transport;
        transport.mode = ScriptedDbTransport::Mode::Silent;
        auto store = makeStore(transport, kShortTimeout);

        const auto begin = std::chrono::steady_clock::now();
        EntityData out;
        if (store.load(1, "Avatar", out)) FAIL("load must degrade on timeout");
        if (store.save(1, sampleEntity(1))) FAIL("save must degrade on timeout");
        if (store.remove(1)) FAIL("remove must degrade on timeout");
        if (store.allocId() != 0) FAIL("allocId must degrade to 0 on timeout");
        if (!store.listIdsByType("Avatar").empty())
            FAIL("listIdsByType must degrade to empty on timeout");
        if (!store.listEntityTypes().empty())
            FAIL("listEntityTypes must degrade to empty on timeout");
        // 六个操作各等满一份预算：耗时下界钉住「真的在等」，而不是碰巧
        // 拿到空结果。上界不设——负载毛刺只拖慢，不该判死。
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        if (elapsed < 6 * kShortTimeout.count() - 10)
            FAIL("timeout budget not consumed (elapsed " +
                 std::to_string(elapsed) + "ms)");
        PASS();
    }

    TEST("malformed ok-payload degrades each op without waiting for timeout");
    {
        ScriptedDbTransport transport;
        transport.mode = ScriptedDbTransport::Mode::Malformed;
        auto store = makeStore(transport);  // 默认 5s 预算：若误入等待会远超

        const auto begin = std::chrono::steady_clock::now();
        EntityData out;
        // method 匹配 .ok 但 payload 畸形：request 立即返回该响应，
        // decode*Response 失败臂（return false / 0）被覆盖——不消耗预算。
        if (store.load(1, "Avatar", out)) FAIL("load must degrade on malformed payload");
        if (store.save(1, sampleEntity(1))) FAIL("save must degrade on malformed payload");
        if (store.remove(1)) FAIL("remove must degrade on malformed payload");
        if (store.allocId() != 0) FAIL("allocId must degrade to 0 on malformed payload");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        // 不应为每个操作各等满 5s：decode 失败即时返回。
        if (elapsed >= 4 * 1000)
            FAIL("malformed payload must not consume the full timeout budget");
        PASS();
    }

    TEST("send rejection degrades immediately without waiting");
    {
        ScriptedDbTransport transport;
        transport.mode = ScriptedDbTransport::Mode::Closed;
        auto store = makeStore(transport);  // 默认 5s 预算：若误入等待循环会远超

        const auto begin = std::chrono::steady_clock::now();
        EntityData out;
        if (store.load(1, "Avatar", out)) FAIL("load must degrade on send rejection");
        if (store.allocId() != 0) FAIL("allocId must degrade to 0 on send rejection");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        if (elapsed > 1000)
            FAIL("send rejection must not wait (elapsed " +
                 std::to_string(elapsed) + "ms)");
        PASS();
    }

    TEST("pump function is driven while waiting for the reply");
    {
        ScriptedDbTransport transport;
        transport.cannedData = sampleEntity(3);
        auto store = makeStore(transport);

        int pumpCalls = 0;
        store.setPumpFunction([&pumpCalls] { ++pumpCalls; });
        EntityData out;
        if (!store.load(3, "Avatar", out)) FAIL("load failed with pump set");
        if (pumpCalls == 0) FAIL("pump function never invoked");
        PASS();
    }

    TEST("requests flush the transport outbound queue");
    {
        ScriptedDbTransport transport;
        auto store = makeStore(transport);

        (void)store.allocId();
        if (transport.flushes == 0) FAIL("request must flush the transport");
        PASS();
    }

    std::cout << "\nAll RemoteEntityStore tests passed!" << std::endl;
    return 0;
}
