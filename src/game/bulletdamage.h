// Bullet damage by weapon, target and distance (exe 0x531090, called from Bullet_Fire_Extended at 0x531787).
#pragma once

#include <map>
#include <string>

namespace iw2::game {

// weaponClass (weapondef +0x7c) turret with a client target (entity +0xfc): playerDamage (+0x1f0) and
// minPlayerDamage (+0x594); otherwise damage (+0x1ec) and minDamage (+0x590). distance < maxDamageRange (+0x598):
// the damage; distance >= minDamageRange (+0x59c): the minimum; between them (and with different ranges) the
// linear blend damage·(1−t) + min·t, t = (distance − max)/(min − max), truncated (0x565994 is _ftol2).
// The caller multiplies the result by its float factor (0x80(%esp) at 0x531787): 1.0 for the normal shot
// (0x5312b8 pushes 0x3f800000), the other values (rifleBullet refire) are TODO(spec).
int bulletDamageAtDistance(const std::map<std::string, std::string>& weaponDef, bool targetIsClient, float distance);

} // namespace iw2::game
