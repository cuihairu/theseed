#include "theseed/runtime/AOI.h"
#include "theseed/runtime/Witness.h"

#include <cstdlib>
#include <iostream>

using theseed::runtime::CoordinateNode;
using theseed::runtime::CoordinateSystem;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyType;
using theseed::runtime::Vector3;
using theseed::runtime::Witness;
using theseed::runtime::WitnessViewTrigger;

namespace {

int fail(const char* stage) {
    std::cerr << "aoi_witness_integration_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    EntityDef def("Avatar");
    static_cast<void>(def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t)));

    Entity owner(1, EntitySide::Cell, def);
    Entity nearEntity(2, EntitySide::Cell, def);
    Entity farEntity(3, EntitySide::Cell, def);

    CoordinateSystem coordinateSystem;
    CoordinateNode ownerNode(owner, Vector3{0.0F, 0.0F, 0.0F});
    CoordinateNode nearNode(nearEntity, Vector3{3.0F, 0.0F, 4.0F});
    CoordinateNode farNode(farEntity, Vector3{20.0F, 0.0F, 0.0F});
    coordinateSystem.insert(ownerNode);
    coordinateSystem.insert(nearNode);
    coordinateSystem.insert(farNode);

    Witness witness;
    witness.attach(owner);
    witness.setDetailDistanceBands(10.0F, 30.0F);

    WitnessViewTrigger trigger(witness, owner, 10.0F);
    trigger.install(coordinateSystem);

    if (!witness.entityInView(nearEntity.id()) || witness.entityInView(farEntity.id())) {
        return fail("initial_view");
    }

    const auto initialView = witness.snapshotView();
    if (initialView.size() != 1 || initialView[0].distance != 5.0F || initialView[0].detailLevel != 0) {
        return fail("initial_distance_detail");
    }

    coordinateSystem.update(farEntity.id(), Vector3{0.0F, 0.0F, 9.0F});
    trigger.refresh();
    if (!witness.entityInView(farEntity.id())) {
        return fail("enter_after_move");
    }

    const auto updatedView = witness.snapshotView();
    if (updatedView.size() != 2) {
        return fail("updated_view_size");
    }

    coordinateSystem.update(nearEntity.id(), Vector3{15.0F, 0.0F, 0.0F});
    trigger.refresh();
    if (witness.entityInView(nearEntity.id())) {
        return fail("leave_after_move");
    }

    return EXIT_SUCCESS;
}
