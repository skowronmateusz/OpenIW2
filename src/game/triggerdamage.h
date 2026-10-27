// trigger_damage rules (spawn 0x529540, action 0x5293c0, damage-kind filter 0x529350), free of the world.
#pragma once

#include <string>

namespace iw2::game {

constexpr int kTriggerDamageFullHealth = 32000; // 0x7d00: the health a trigger_damage starts with and returns to

// The means-of-death number of a name, in the table order of 0x5a95ac (MOD_UNKNOWN = 0 ... MOD_EXPLOSIVE = 14).
int modNumber(const std::string& mod);

// 0x529350: false when a spawnflag excludes the means of death (1 pistol, 2 rifle, 4 grenade/projectile 3-6, 8 explosive 14,
// 0x10 splash 4 and 6, 0x20 melee 7, 0x100 the others: unknown and 9-13).
bool triggerDamageAccepts(int spawnflags, int mod);

// 0x5293c0: whether a hit fires the trigger. Below a positive threshold nothing happens; the means of death must be
// accepted; with an accumulate value the damage taken since the last firing (32000 - health) must reach it.
bool triggerDamageFires(int threshold, int accumulate, int spawnflags, int damage, int mod, int health);

} // namespace iw2::game
