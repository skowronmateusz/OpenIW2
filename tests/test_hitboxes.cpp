// Tests for the entity box model 0x3ff (trace, position test, sight) and the bone hit-box trace.

#include "game/collision.h"
#include "game/hitboxes.h"

#include <cmath>
#include <iostream>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

} // namespace

int main() {
    using namespace iw2::game;
    CollisionWorld world; // no map: only the box model is exercised
    const Vec boxMins{-15, -15, 0}, boxMaxs{15, 15, 72};
    const Vec zero{0, 0, 0};

    // Point trace through an actor box from the side: the cylinder test (FUN_00416e70).
    // t = (20000 − 3000) / 40000 + 0.125·100 / −20000.
    TraceResult tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {-100, 0, 36}, {100, 0, 36}, zero, zero,
                                    Contents::BulletMask);
    CHECK(near(tr.fraction, 0.425f - 0.000625f));
    CHECK(near(tr.normal[0], -1) && near(tr.normal[1], 0) && near(tr.normal[2], 0));
    CHECK(tr.contents == Contents::ActorBody);
    // Contents not in the mask: no hit.
    tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {-100, 0, 36}, {100, 0, 36}, zero, zero, Contents::Solid);
    CHECK(tr.fraction == 1.0f);
    // From above onto the top sphere (centre z = 36 + 21): t = (−b − √disc)/a + 0.125·|d|/b.
    tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {0, 0, 157}, {0, 0, -43}, zero, zero, Contents::BulletMask);
    CHECK(near(tr.fraction, 85.0f / 200.0f + 0.125f * 100.0f / -20000.0f));
    CHECK(near(tr.normal[2], 1));
    // Passing beside it: no hit.
    tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {-100, 20, 36}, {100, 20, 36}, zero, zero, Contents::BulletMask);
    CHECK(tr.fraction == 1.0f);
    // Position test inside (FUN_00416120).
    tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {5, 0, 30}, {5, 0, 30}, zero, zero, Contents::BulletMask);
    CHECK(tr.allSolid && tr.startSolid && tr.fraction == 0);
    tr = world.traceBox({0, 0, 0}, boxMins, boxMaxs, Contents::ActorBody, {40, 0, 30}, {40, 0, 30}, zero, zero, Contents::BulletMask);
    CHECK(!tr.allSolid && tr.fraction == 1.0f);

    // Hit boxes: a root bone (torso_upper, box z 40..60), a head bone at z 60 (box 60..70) and a jaw bone with class
    // 0 ("none", priority 1) that inherits the head's class (box 56..60).
    iw2::assets::XModelParts parts;
    parts.rootCount = 1;
    parts.bones.resize(3);
    parts.bones[0].name = "j_spine";
    parts.bones[1].name = "j_head";
    parts.bones[1].parent = 0;
    parts.bones[2].name = "j_jaw";
    parts.bones[2].parent = 1;
    parts.classification = {4, 2, 0};
    auto box = [](float z0, float z1) {
        iw2::assets::XBoneBox b;
        b.mins = {-8, -8, z0};
        b.maxs = {8, 8, z1};
        for (size_t i = 0; i < 3; ++i) b.offset[i] = (b.mins[i] + b.maxs[i]) * 0.5f;
        b.radiusSquared = 64 + 64 + (z1 - b.offset[2]) * (z1 - b.offset[2]);
        return b;
    };
    std::vector<iw2::assets::XBoneBox> boxes = {box(40, 60), box(0, 10), box(-4, 0)};
    HitBoxModel m;
    m.parts = &parts;
    m.boxes = &boxes;
    m.position = {{0, 0, 0}, {0, 0, 60}, {0, 0, 60}};
    m.rotation = {{}, {}, {}};
    // Through the torso from the front.
    HitBoxResult h = traceHitBoxes({m}, {-100, 0, 50}, {100, 0, 50}, kHitPriorityDefault, 1.0f);
    CHECK(h.hit && h.location == 4 && h.bone == 0 && near(h.fraction, 92.0f / 200.0f) && near(h.normal[0], -1));
    // The jaw (class 0) resolves to its parent's class, head.
    h = traceHitBoxes({m}, {-100, 0, 58}, {100, 0, 58}, kHitPriorityDefault, 1.0f);
    CHECK(h.hit && h.location == 4); // box z 40..60 of the torso is reached first at the same priority
    h = traceHitBoxes({m}, {-100, 0, 62}, {100, 0, 62}, kHitPriorityDefault, 1.0f);
    CHECK(h.hit && h.location == 2 && h.bone == 1);
    h = traceHitBoxes({m}, {-100, 0, 58.5f}, {100, 0, 58.5f}, kHitPriorityDefault, 1.0f);
    CHECK(h.hit && h.location == 4);
    h = traceHitBoxes({m}, {0, -100, 57}, {0, 100, 57}, kHitPriorityRifle, 1.0f);
    CHECK(h.hit && h.location == 2 && h.bone == 2); // rifle map: the head class beats the closer torso hit
    // Downwards through head and torso: default map keeps the first box, the rifle map prefers the head.
    h = traceHitBoxes({m}, {0, 0, 100}, {0, 0, 0}, kHitPriorityDefault, 1.0f);
    CHECK(h.hit && h.location == 2 && near(h.fraction, 30.0f / 100.0f) && near(h.normal[2], 1));
    // A hit must beat the current fraction.
    h = traceHitBoxes({m}, {-100, 0, 50}, {100, 0, 50}, kHitPriorityDefault, 0.3f);
    CHECK(!h.hit);
    CHECK(std::string(hitLocationName(4)) == "torso_upper" && std::string(hitLocationName(18)) == "gun");

    if (failures) return 1;
    std::cout << "test_hitboxes: all checks passed\n";
    return 0;
}
