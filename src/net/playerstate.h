// Player-state delta coder of snapshots, implemented from the spec (docs/spec/net.md section 7.3).
#pragma once

#include <cstdint>

#include "net/delta.h"
#include "net/msg.h"

namespace iw2::net {

// Byte offsets of the extra sections inside the player-state layout (kPlayerStateSize bytes).
constexpr int kPsStatsOffset = 300;        // 6 int32
constexpr int kPsAmmoOffset = 324;         // 64 int32 in 4 groups of 16
constexpr int kPsAmmoClipOffset = 836;     // 64 int32 in 4 groups of 16
constexpr int kPsObjectivesOffset = 1508;  // 16 objectives of kObjectiveSize bytes
// The 1.3 server keeps archived HUD elements (the default) at +1960 (observed, docs/spec/net.md section 11); on the
// wire the array at +5928 comes first.
constexpr int kPsHudElemsOffset = 1960;    // 31 hud elements of kHudElemSize bytes
constexpr int kPsArchivalHudElemsOffset = 5928;  // 31 more, sent first
constexpr int kPsHudElemCount = 31;
constexpr int kPsObjectiveCount = 16;

// The base command the first user command of a packet is coded against (docs/spec/net.md section 6).
UserCmd usercmdBase(const uint8_t* ps);

// from may be nullptr (all zero).
void writeDeltaPlayerState(Msg& msg, const uint8_t* from, const uint8_t* to);
void readDeltaPlayerState(Msg& msg, const uint8_t* from, uint8_t* to);

} // namespace iw2::net
