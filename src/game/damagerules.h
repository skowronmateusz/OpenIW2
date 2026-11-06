// Decisions of G_Damage (VAs in the patch 1.3 SP executable; docs/spec/weapons.md "G_Damage order").
#pragma once

#include <cstdint>

namespace iw2::game {

// 0x512390: a client riding a vehicle (pm_flags 0x20000 without 0x80000) takes no damage unless the vehicle's type
// word (vehicle info +0x40: table 0x1c7fa80, stride 0x270) is 4, "artillery". The vehicle itself takes the hits.
inline bool riderIgnoresDamage(uint32_t pmFlags, bool riding, int vehicleType) {
    return riding && (pmFlags & 0x20000u) && !(pmFlags & 0x80000u) && vehicleType != 4;
}

// 0x5282c0..0x5282c9: past the god check an amount below 1 becomes 1. Actors never get here with one (0x528143 left
// on <= 0), so this acts on the player and on entities without an actor.
inline int damageFloor(int amount) { return amount < 1 ? 1 : amount; }

// 0x4ff360: the part of a lethal hit an actor does not take while its state stack holds the animscripted (7) or
// negotiation (9) state (0x4ff310) and allowdeath (+0xb0) is 0. The actor keeps 1 health and +0xb4 is set.
inline int actorDamageReduction(int amount, int health, bool allowDeath, bool scriptedOnStack) {
    if (amount < health || allowDeath || !scriptedOnStack) return 0;
    return amount - health + 1;
}

} // namespace iw2::game
