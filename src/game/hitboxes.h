// Bullet traces against the bone hit boxes of a DObj (docs/spec/collision.md, "Hit boxes").
#pragma once

#include "assets/xmodel.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iw2::game {

// Hit locations in the order of the name table at 0x5a9538.
constexpr int kHitLocationCount = 19;
const char* hitLocationName(int location);

// Priority maps (byte per hit location): 0x5a9584 for most bullets, 0x5a9598 when the weapon has rifleBullet.
extern const uint8_t kHitPriorityDefault[kHitLocationCount];
extern const uint8_t kHitPriorityRifle[kHitLocationCount];

// One model of the DObj (body first, then attachments), posed in entity space.
struct HitBoxModel {
    const assets::XModelParts* parts = nullptr;
    const std::vector<assets::XBoneBox>* boxes = nullptr;
    int attachBone = -1; // global bone index of the tag the model hangs on (DObj +0x3c), -1 for none
    bool hidden = false;
    std::vector<std::array<float, 3>> position; // per bone
    std::vector<assets::Quat> rotation;
};

struct HitBoxResult {
    bool hit = false;
    float fraction = 1.0f;
    std::array<float, 3> normal{}; // entity space
    int model = -1, bone = -1;     // bone index within the model
    int location = 0;              // hit location (partGroup)
};

// FUN_00473d20: start/end in entity space; `fraction` is the trace's current fraction, which a hit must beat.
HitBoxResult traceHitBoxes(const std::vector<HitBoxModel>& models, const std::array<float, 3>& start,
                           const std::array<float, 3>& end, const uint8_t* priority, float fraction);

} // namespace iw2::game
