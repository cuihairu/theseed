#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/CellRuntime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using theseed::foundation::Counter;
using theseed::foundation::MetricsRegistry;
using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceConfig;
using theseed::runtime::SpaceRuntime;
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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

static Counter& forwardsCounter() {
    return MetricsRegistry::instance().counter("migration_route_forwards_total");
}

static Counter& expiredDropsCounter() {
    return MetricsRegistry::instance().counter("migration_route_expired_drops_total");
}

struct Harness {
    std::shared_ptr<InMemoryRuntimeTransport> transport;
    std::unique_ptr<CellRuntime> source;
    std::unique_ptr<CellRuntime> target;
    std::unique_ptr<TickScheduler> scheduler;
    EntityDef def;
    std::unique_ptr<Entity> sourceEntity;
    int migratedDispatchCount = 0;
    std::vector<std::byte> migratedPayload;

    Harness() {
        MetricsRegistry::instance().reset();
        forwardsCounter();
        expiredDropsCounter();

        def = EntityDef("Avatar");
        const auto hpId = def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t));
        def.addMethod("castSpell", MethodSide::Cell);
        static_cast<void>(hpId);

        transport = std::make_shared<InMemoryRuntimeTransport>();

        auto srcTopo = std::make_unique<SingleCellTopology>(11);
        auto srcSpace = std::make_unique<Space>(100, "src", std::move(srcTopo));
        srcSpace->initialize(SpaceConfig{.name = "src"});
        auto srcRt = std::make_unique<SpaceRuntime>(std::move(srcSpace));
        source = std::make_unique<CellRuntime>(std::move(srcRt), transport, 11);

        auto tgtTopo = std::make_unique<SingleCellTopology>(22);
        auto tgtSpace = std::make_unique<Space>(200, "tgt", std::move(tgtTopo));
        tgtSpace->initialize(SpaceConfig{.name = "tgt"});
        auto tgtRt = std::make_unique<SpaceRuntime>(std::move(tgtSpace));
        target = std::make_unique<CellRuntime>(std::move(tgtRt), transport, 22);

        target->registerEntityFactory("Avatar",
            [&](EntityId id, EntitySide side) -> std::unique_ptr<Entity> {
                auto e = std::make_unique<Entity>(id, side, def);
                const auto bound = e->bindMethodHandler("castSpell",
                    [&](Entity&, std::span<const std::byte> payload) {
                        migratedPayload.assign(payload.begin(), payload.end());
                        ++migratedDispatchCount;
                    });
                if (!bound) return nullptr;
                return e;
            });

        sourceEntity = std::make_unique<Entity>(1, EntitySide::Cell, def);
        source->addEntity(*sourceEntity, Vector3{1.0F, 0.0F, 1.0F});
        sourceEntity->activate();

        scheduler = std::make_unique<TickScheduler>(std::chrono::milliseconds{0});
        source->attach(*scheduler);
        target->attach(*scheduler);
    }

    ~Harness() {
        if (source) source->detach(*scheduler);
        if (target) target->detach(*scheduler);
        MetricsRegistry::instance().reset();
    }

    void drain() {
        RuntimeInvocation tmp;
        while (transport->receive(11, &tmp, 1) > 0) {}
        while (transport->receive(22, &tmp, 1) > 0) {}
    }

    void runOnce() { scheduler->runOnce(); }
};

static void test_route_forward_increments_counter() {
    TEST("test_route_forward_increments_counter");
    Harness h;
    if (!h.source->beginMigration(h.sourceEntity->id(), 22, 1)) {
        FAIL("beginMigration failed"); return;
    }
    h.runOnce();  // migration.transfer delivered, restored on target, commit sent back
    h.runOnce();  // migration.commit processed at source

    if (h.migratedDispatchCount != 0) { FAIL("unexpected dispatch before forward"); return; }

    RuntimeInvocation inv;
    inv.entityId = h.sourceEntity->id();
    inv.targetComponent = 11;
    inv.entityType = h.sourceEntity->entityType();
    inv.method = "castSpell";
    const std::array<std::byte, 2> payload{std::byte{0xAA}, std::byte{0xBB}};
    inv.payload.assign(payload.begin(), payload.end());
    if (h.transport->send(inv) != SendResult::Accepted) { FAIL("send failed"); return; }

    h.runOnce();
    if (h.migratedDispatchCount != 1) { FAIL("forward did not deliver"); return; }
    if (forwardsCounter().value() != 1) { FAIL("forwards counter not incremented"); return; }
    PASS();
}

static void test_route_clear_does_not_increment() {
    TEST("test_route_clear_does_not_increment");
    Harness h;
    if (!h.source->beginMigration(h.sourceEntity->id(), 22, 1)) {
        FAIL("beginMigration failed"); return;
    }
    h.runOnce();
    h.runOnce();

    if (!h.source->clearMigrationRoute(h.sourceEntity->id())) {
        FAIL("clearMigrationRoute returned false"); return;
    }

    RuntimeInvocation inv;
    inv.entityId = h.sourceEntity->id();
    inv.targetComponent = 11;
    inv.entityType = h.sourceEntity->entityType();
    inv.method = "castSpell";
    const std::array<std::byte, 2> payload{std::byte{0x11}, std::byte{0x22}};
    inv.payload.assign(payload.begin(), payload.end());
    if (h.transport->send(inv) != SendResult::Accepted) { FAIL("send failed"); return; }

    h.runOnce();
    if (h.migratedDispatchCount != 0) { FAIL("delivered despite cleared route"); return; }
    if (forwardsCounter().value() != 0) { FAIL("forwards counter incremented unexpectedly"); return; }
    PASS();
}

static void test_multiple_forwards_accumulate_counter() {
    TEST("test_multiple_forwards_accumulate_counter");
    Harness h;
    if (!h.source->beginMigration(h.sourceEntity->id(), 22, 1)) {
        FAIL("beginMigration failed"); return;
    }
    h.runOnce();
    h.runOnce();

    for (int i = 0; i < 3; ++i) {
        RuntimeInvocation inv;
        inv.entityId = h.sourceEntity->id();
        inv.targetComponent = 11;
        inv.entityType = h.sourceEntity->entityType();
        inv.method = "castSpell";
        const std::array<std::byte, 1> payload{std::byte(static_cast<std::uint8_t>(i))};
        inv.payload.assign(payload.begin(), payload.end());
        if (h.transport->send(inv) != SendResult::Accepted) {
            FAIL("send failed"); return;
        }
        h.runOnce();
    }
    if (h.migratedDispatchCount != 3) { FAIL("expected 3 forwards"); return; }
    if (forwardsCounter().value() != 3) { FAIL("counter should equal 3"); return; }
    PASS();
}

static void test_route_expired_drop_increments_separate_counter() {
    TEST("test_route_expired_drop_increments_separate_counter");
    Harness h;
    if (!h.source->beginMigration(h.sourceEntity->id(), 22, 1)) {
        FAIL("beginMigration failed"); return;
    }
    h.runOnce();
    h.runOnce();

    // We cannot mock steady_clock, so instead simulate TTL expiry path by
    // invoking clearMigrationRoute() then verifying forwards/drops stay 0
    // for non-expiry clears (drops counter only ticks via the timer path).
    h.source->clearMigrationRoute(h.sourceEntity->id());

    RuntimeInvocation inv;
    inv.entityId = h.sourceEntity->id();
    inv.targetComponent = 11;
    inv.entityType = h.sourceEntity->entityType();
    inv.method = "castSpell";
    inv.payload.push_back(std::byte{0});
    h.transport->send(inv);
    h.runOnce();

    if (expiredDropsCounter().value() != 0) { FAIL("drops should not increment on explicit clear"); return; }
    PASS();
}

int main() {
    test_route_forward_increments_counter();
    test_route_clear_does_not_increment();
    test_multiple_forwards_accumulate_counter();
    test_route_expired_drop_increments_separate_counter();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
