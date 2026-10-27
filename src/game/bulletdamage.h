// Bullet damage by weapon, target and distance (exe 0x531090, called from Bullet_Fire_Extended at 0x531787).
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

// weaponClass (weapondef +0x7c) turret with a client target (entity +0xfc): playerDamage (+0x1f0) and
// minPlayerDamage (+0x594); otherwise damage (+0x1ec) and minDamage (+0x590). distance < maxDamageRange (+0x598):
// the damage; distance >= minDamageRange (+0x59c): the minimum; between them (and with different ranges) the
// linear blend damage·(1−t) + min·t, t = (distance − max)/(min − max), truncated (0x565994 is _ftol2).
// The caller multiplies the result by its float factor (0x80(%esp) at 0x531787): 1.0 for the normal shot
// (0x5312b8 pushes 0x3f800000), the other values (rifleBullet refire) are TODO(spec).
int bulletDamageAtDistance(const std::map<std::string, std::string>& weaponDef, bool targetIsClient, float distance);

// AI weapon accuracy graph (accuracy/aivsai|aivsplayer/<name>.accu, "WEAPONACCUFILE", a point count, then x y pairs).
struct AccuracyGraph {
    std::vector<std::pair<float, float>> points;
};
bool parseAccuracyGraph(const std::string& text, AccuracyGraph& out);

// 0x420b40 over the graph input of 0x54f9c0 (x = distance · 0.00025, at most 1): the first point with x <= px[i]
// (i >= 1) gives the linear blend between points i−1 and i; −1 when no point qualifies or there are fewer than 2.
float evalAccuracyGraph(const AccuracyGraph& g, float distance);

// Hit chance of an AI shot at an enemy (0x54fde0 → 0x54fb90; ai_vsPlayerUseAccuGraphOnly default 1 and AI targets):
// graph(distance) · actor accuracy · the shoot() accuracy argument, clamped to 0..1. The shot hits when this is larger
// than a random number in [0, 1) (0x550bad).
float aiHitChance(const AccuracyGraph& g, float distance, float actorAccuracy, float shootAccuracyArg);

} // namespace iw2::game
