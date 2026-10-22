// AI perception: per-target sight info and enemy selection (docs/spec/ai.md, "Perception").
#pragma once

#include "script/value.h"

#include <cstdint>

namespace iw2::game {

// What an actor knows about one sentient (actor +0x1774 + sentient index * 0x44).
struct SentientInfo {
    bool visible = false;         // +0x00 result of the last sight check
    int64_t lastCheckMs = 0;      // +0x04
    int64_t lastSeenMs = 0;       // +0x08
    int64_t lastAttackedMs = 0;   // +0x0c (TODO: set when the sentient attacks the actor)
    int64_t lastKnownMs = 0;      // +0x10 time of the last known position
    int64_t ignoreUntilMs = 0;    // +0x14
    script::Vec3 lastKnownPos{};  // +0x18 the sentient's origin then
    script::Vec3 ownPosThen{};    // +0x28 the actor's origin then
};

} // namespace iw2::game
