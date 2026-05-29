#include "theseed/runtime/AOI.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using theseed::runtime::CoordinateNode;
using theseed::runtime::CoordinateSystem;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyType;
using theseed::runtime::RangeTrigger;
using theseed::runtime::Vector3;

namespace {

int fail(const char* stage) {
    std::cerr << "cross_list_aoi_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

// Verify X-sorted order by walking from headX
bool verifyXOrder(const CoordinateSystem& cs) {
    auto* curr = cs.headX();
    if (curr == nullptr) return true;
    float prev = curr->x();
    curr = curr->nextX();
    while (curr != nullptr) {
        if (curr->x() < prev) return false;
        prev = curr->x();
        curr = curr->nextX();
    }
    return true;
}

// Verify Z-sorted order by walking from headZ
bool verifyZOrder(const CoordinateSystem& cs) {
    auto* curr = cs.headZ();
    if (curr == nullptr) return true;
    float prev = curr->z();
    curr = curr->nextZ();
    while (curr != nullptr) {
        if (curr->z() < prev) return false;
        prev = curr->z();
        curr = curr->nextZ();
    }
    return true;
}

// Collect all X positions in sorted order
std::vector<float> collectX(const CoordinateSystem& cs) {
    std::vector<float> result;
    auto* curr = cs.headX();
    while (curr != nullptr) {
        result.push_back(curr->x());
        curr = curr->nextX();
    }
    return result;
}

std::vector<float> collectZ(const CoordinateSystem& cs) {
    std::vector<float> result;
    auto* curr = cs.headZ();
    while (curr != nullptr) {
        result.push_back(curr->z());
        curr = curr->nextZ();
    }
    return result;
}

// Verify doubly-linked list consistency for X axis
bool verifyXLinks(const CoordinateSystem& cs) {
    auto* curr = cs.headX();
    if (curr == nullptr) return true;
    if (curr->prevX() != nullptr) return false;
    while (curr->nextX() != nullptr) {
        if (curr->nextX()->prevX() != curr) return false;
        curr = curr->nextX();
    }
    return true;
}

bool verifyZLinks(const CoordinateSystem& cs) {
    auto* curr = cs.headZ();
    if (curr == nullptr) return true;
    if (curr->prevZ() != nullptr) return false;
    while (curr->nextZ() != nullptr) {
        if (curr->nextZ()->prevZ() != curr) return false;
        curr = curr->nextZ();
    }
    return true;
}

class RecordingTrigger final : public RangeTrigger {
public:
    using RangeTrigger::RangeTrigger;

    std::vector<std::uint64_t> entered;
    std::vector<std::uint64_t> left;

private:
    void onEnter(CoordinateNode& node, float distance) override {
        static_cast<void>(distance);
        entered.push_back(node.entityId());
    }

    void onLeave(CoordinateNode& node) override {
        left.push_back(node.entityId());
    }
};

}  // namespace

int main() {
    std::cout << "Cross-linked list AOI tests:" << std::endl;

    EntityDef def("Mob");
    static_cast<void>(def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));

    // --- Test 1: Sorted insertion order ---
    std::cout << "  sorted insertion... ";
    {
        Entity e1(1, EntitySide::Cell, def);
        Entity e2(2, EntitySide::Cell, def);
        Entity e3(3, EntitySide::Cell, def);
        Entity e4(4, EntitySide::Cell, def);
        Entity e5(5, EntitySide::Cell, def);

        CoordinateNode n1(e1, Vector3{10.0F, 0.0F, 5.0F});
        CoordinateNode n2(e2, Vector3{3.0F, 0.0F, 20.0F});
        CoordinateNode n3(e3, Vector3{7.0F, 0.0F, 1.0F});
        CoordinateNode n4(e4, Vector3{15.0F, 0.0F, 10.0F});
        CoordinateNode n5(e5, Vector3{1.0F, 0.0F, 15.0F});

        CoordinateSystem cs;
        cs.insert(n1);
        cs.insert(n2);
        cs.insert(n3);
        cs.insert(n4);
        cs.insert(n5);

        // X order should be: 1, 3, 2, 10, 15
        auto xs = collectX(cs);
        if (xs.size() != 5 || xs[0] != 1.0F || xs[1] != 3.0F || xs[2] != 7.0F ||
            xs[3] != 10.0F || xs[4] != 15.0F) {
            return fail("x_order");
        }

        // Z order should be: 1, 5, 10, 15, 20
        auto zs = collectZ(cs);
        if (zs.size() != 5 || zs[0] != 1.0F || zs[1] != 5.0F || zs[2] != 10.0F ||
            zs[3] != 15.0F || zs[4] != 20.0F) {
            return fail("z_order");
        }

        if (!verifyXOrder(cs) || !verifyZOrder(cs)) return fail("sorted_verify");
        if (!verifyXLinks(cs) || !verifyZLinks(cs)) return fail("link_consistency");
    }
    std::cout << "OK" << std::endl;

    // --- Test 2: Repositioning after update ---
    std::cout << "  reposition after update... ";
    {
        Entity e1(1, EntitySide::Cell, def);
        Entity e2(2, EntitySide::Cell, def);
        Entity e3(3, EntitySide::Cell, def);

        CoordinateNode n1(e1, Vector3{0.0F, 0.0F, 0.0F});
        CoordinateNode n2(e2, Vector3{5.0F, 0.0F, 5.0F});
        CoordinateNode n3(e3, Vector3{10.0F, 0.0F, 10.0F});

        CoordinateSystem cs;
        cs.insert(n1);
        cs.insert(n2);
        cs.insert(n3);

        // Move e2 from (5,0,5) to (12,0,12) — should bubble past e3
        cs.update(2, Vector3{12.0F, 0.0F, 12.0F});
        auto xs = collectX(cs);
        if (xs[0] != 0.0F || xs[1] != 10.0F || xs[2] != 12.0F) return fail("reposition_x");
        auto zs = collectZ(cs);
        if (zs[0] != 0.0F || zs[1] != 10.0F || zs[2] != 12.0F) return fail("reposition_z");

        // Move e2 to head position (−1, 0, −1)
        cs.update(2, Vector3{-1.0F, 0.0F, -1.0F});
        xs = collectX(cs);
        if (xs[0] != -1.0F || xs[1] != 0.0F || xs[2] != 10.0F) return fail("reposition_x_head");
        zs = collectZ(cs);
        if (zs[0] != -1.0F || zs[1] != 0.0F || zs[2] != 10.0F) return fail("reposition_z_head");

        if (!verifyXOrder(cs) || !verifyZOrder(cs)) return fail("reposition_sorted");
        if (!verifyXLinks(cs) || !verifyZLinks(cs)) return fail("reposition_links");
    }
    std::cout << "OK" << std::endl;

    // --- Test 3: Remove and list integrity ---
    std::cout << "  remove preserves integrity... ";
    {
        Entity e1(1, EntitySide::Cell, def);
        Entity e2(2, EntitySide::Cell, def);
        Entity e3(3, EntitySide::Cell, def);

        CoordinateNode n1(e1, Vector3{1.0F, 0.0F, 3.0F});
        CoordinateNode n2(e2, Vector3{2.0F, 0.0F, 2.0F});
        CoordinateNode n3(e3, Vector3{3.0F, 0.0F, 1.0F});

        CoordinateSystem cs;
        cs.insert(n1);
        cs.insert(n2);
        cs.insert(n3);

        // Remove middle node
        cs.remove(2);
        auto xs = collectX(cs);
        if (xs.size() != 2 || xs[0] != 1.0F || xs[1] != 3.0F) return fail("remove_x");
        auto zs = collectZ(cs);
        if (zs.size() != 2 || zs[0] != 1.0F || zs[1] != 3.0F) return fail("remove_z");

        // Remove head node
        cs.remove(1);
        xs = collectX(cs);
        if (xs.size() != 1 || xs[0] != 3.0F) return fail("remove_head_x");
        zs = collectZ(cs);
        if (zs.size() != 1 || zs[0] != 1.0F) return fail("remove_head_z");

        // Remove last node
        cs.remove(3);
        if (cs.headX() != nullptr || cs.headZ() != nullptr) return fail("remove_all");
    }
    std::cout << "OK" << std::endl;

    // --- Test 4: Efficient range query ---
    std::cout << "  range query (cross-list scan)... ";
    {
        // Place entities on a grid. Query center with small radius should only find nearby.
        std::vector<Entity*> entities;
        std::vector<CoordinateNode*> nodes;
        CoordinateSystem cs;

        for (int i = 0; i < 100; ++i) {
            float x = static_cast<float>(i % 10) * 10.0F;
            float z = static_cast<float>(i / 10) * 10.0F;
            auto* e = new Entity(static_cast<std::uint64_t>(i) + 1, EntitySide::Cell, def);
            auto* n = new CoordinateNode(*e, Vector3{x, 0.0F, z});
            entities.push_back(e);
            nodes.push_back(n);
            cs.insert(*n);
        }

        // Query at (45, 0, 45) with radius 15 — should find entities in the 4x4 grid cell
        auto inRange = cs.entitiesInRange(Vector3{45.0F, 0.0F, 45.0F}, 15.0F);
        // At (40,0,40), (40,0,50), (50,0,40), (50,0,50), (45,0,45 doesn't exist but nearby do)
        bool found40_40 = false;
        bool found50_50 = false;
        for (auto* e : inRange) {
            auto pos = cs.find(e->id())->position();
            if (pos.x == 40.0F && pos.z == 40.0F) found40_40 = true;
            if (pos.x == 50.0F && pos.z == 50.0F) found50_50 = true;
        }
        if (!found40_40 || !found50_50) return fail("range_query_missed");
        // Should not find distant entities
        bool found0_0 = false;
        for (auto* e : inRange) {
            if (e->id() == 1) found0_0 = true;
        }
        if (found0_0) return fail("range_query_false_positive");

        if (!verifyXOrder(cs) || !verifyZOrder(cs)) return fail("range_query_order");

        for (auto* n : nodes) delete n;
        for (auto* e : entities) delete e;
    }
    std::cout << "OK" << std::endl;

    // --- Test 5: RangeTrigger with cross-linked list ---
    std::cout << "  trigger incremental events... ";
    {
        Entity owner(1, EntitySide::Cell, def);
        Entity e2(2, EntitySide::Cell, def);
        Entity e3(3, EntitySide::Cell, def);
        Entity e4(4, EntitySide::Cell, def);

        CoordinateNode n1(owner, Vector3{0.0F, 0.0F, 0.0F});
        CoordinateNode n2(e2, Vector3{3.0F, 0.0F, 3.0F});    // distance ~4.24
        CoordinateNode n3(e3, Vector3{50.0F, 0.0F, 50.0F});   // far
        CoordinateNode n4(e4, Vector3{-2.0F, 0.0F, -2.0F});   // distance ~2.83

        CoordinateSystem cs;
        cs.insert(n1);
        cs.insert(n2);
        cs.insert(n3);
        cs.insert(n4);

        RecordingTrigger trigger(owner, 10.0F);
        trigger.install(cs);

        // Should detect e2 and e4 in range initially
        if (trigger.entered.size() != 2) return fail("trigger_initial_count");
        bool found2 = false, found4 = false;
        for (auto id : trigger.entered) {
            if (id == 2) found2 = true;
            if (id == 4) found4 = true;
        }
        if (!found2 || !found4) return fail("trigger_initial_ids");

        // Move e3 into range
        cs.update(3, Vector3{5.0F, 0.0F, 5.0F});
        trigger.refresh();
        if (trigger.entered.size() != 3) return fail("trigger_enter_after_move");
        if (trigger.entered.back() != 3) return fail("trigger_enter_id");

        // Move e4 out of range
        cs.update(4, Vector3{100.0F, 0.0F, 100.0F});
        trigger.refresh();
        if (trigger.left.size() != 1 || trigger.left[0] != 4) return fail("trigger_leave_after_move");
    }
    std::cout << "OK" << std::endl;

    // --- Test 6: Same-position nodes ---
    std::cout << "  same position nodes... ";
    {
        Entity e1(1, EntitySide::Cell, def);
        Entity e2(2, EntitySide::Cell, def);
        Entity e3(3, EntitySide::Cell, def);

        CoordinateNode n1(e1, Vector3{5.0F, 0.0F, 5.0F});
        CoordinateNode n2(e2, Vector3{5.0F, 0.0F, 5.0F});
        CoordinateNode n3(e3, Vector3{5.0F, 0.0F, 5.0F});

        CoordinateSystem cs;
        cs.insert(n1);
        cs.insert(n2);
        cs.insert(n3);

        if (!verifyXLinks(cs) || !verifyZLinks(cs)) return fail("same_pos_links");

        // Range query should find all 3
        auto inRange = cs.entitiesInRange(Vector3{5.0F, 0.0F, 5.0F}, 1.0F);
        if (inRange.size() != 3) return fail("same_pos_range");

        // Remove middle one
        cs.remove(2);
        inRange = cs.entitiesInRange(Vector3{5.0F, 0.0F, 5.0F}, 1.0F);
        if (inRange.size() != 2) return fail("same_pos_remove");
        if (!verifyXLinks(cs) || !verifyZLinks(cs)) return fail("same_pos_remove_links");
    }
    std::cout << "OK" << std::endl;

    std::cout << "\nAll cross-linked list AOI tests passed!" << std::endl;
    return EXIT_SUCCESS;
}
