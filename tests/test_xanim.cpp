// Tests for the animation calc (docs/spec/xanim.md, "Blending") on a synthetic two-bone skeleton.

#include "assets/xanim.h"

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

bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

using namespace iw2::assets;

XModelParts skeleton() {
    XModelParts p;
    p.rootCount = 1;
    p.bones.resize(2);
    p.bones[0].name = "tag_origin";
    p.bones[1].name = "j_mainroot";
    p.bones[1].parent = 0;
    p.bones[1].localOffset = {0, 0, 37};
    p.bones[1].position = {0, 0, 37};
    return p;
}

XAnim moveRoot(float dz, Quat q) {
    XAnim a;
    a.numFrames = 1;
    a.frameRate = 30;
    XAnimBone b;
    b.name = "J_MAINROOT";
    b.rot.keys.push_back(q);
    b.trans.keys.push_back({0, 0, dz});
    a.bones.push_back(b);
    return a;
}

} // namespace

int main() {
    const XModelParts parts = skeleton();
    const float s = std::sqrt(0.5f);

    // Translations are offsets from the bone's base offset.
    const XAnim crouch = moveRoot(-21, {});
    const XPose p = animatePose(parts, crouch, 0);
    CHECK(near(p.position[1][2], 16));

    // Two leaves under the top blend node: raw sums divided by the weight sum.
    const XAnim turn = moveRoot(-1, {0, 0, s, s});
    const XPose b = blendPose(parts, {{&crouch, 0, 1}, {&turn, 0, 1}});
    CHECK(near(b.position[1][2], 37 - 11));
    const float n = std::sqrt(s * s + (1 + s) * (1 + s));
    CHECK(near(b.rotation[1].z, s / n) && near(b.rotation[1].w, (1 + s) / n));

    // A single active layer gets the whole weight.
    const XPose one = blendPose(parts, {{&crouch, 0, 0.25f}, {&turn, 0, 0}});
    CHECK(near(one.position[1][2], 16));

    // A nested blend node adds its normalized result times its weight.
    AnimAccum sub(parts.bones.size()), top(parts.bones.size());
    accumulateAnim(parts, crouch, 0, 3, sub);
    mergeBlend(sub, 0.5f, top);
    CHECK(near(top.bones[1].q[3], 0.5f) && near(top.bones[1].t[2], -10.5f) && near(top.bones[1].w, 0.5f));

    // Bones no animation names keep the base pose.
    const XAnim other = moveRoot(5, {});
    XAnim renamed = other;
    renamed.bones[0].name = "j_spine";
    const XPose base = animatePose(parts, renamed, 0);
    CHECK(near(base.position[1][2], 37) && near(base.rotation[1].w, 1));

    if (failures) return 1;
    std::cout << "xanim: ok\n";
    return 0;
}
