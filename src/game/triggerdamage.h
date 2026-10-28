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

// SV_EntityContact (0x447830) for trigger_disk (shape flag 0x20, spawn 0x529070): the entity stores radius + 64 (0x56f6e0)
// in r.maxs x/y and +-100000 in z, and the contact test subtracts the 64 again, so the box touches when its centre is at
// least radius + half its x width from the axis (the inverse of the trigger_radius test; no z test).
bool triggerDiskContact(float absminX, float absmaxX, float absminY, float absmaxY, float originX, float originY, float radius);

} // namespace iw2::game
