// Bullet damage by weapon, target and distance (exe 0x531090, called from Bullet_Fire_Extended at 0x531787).
#pragma once

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

// weaponClass (weapondef +0x7c) turret with a client target (entity +0xfc): playerDamage (+0x1f0) and
// minPlayerDamage (+0x594); otherwise damage (+0x1ec) and minDamage (+0x590). distance < maxDamageRange (+0x598):
// the damage; distance >= minDamageRange (+0x59c): the minimum; between them (and with different ranges) the
// linear blend damage·(1−t) + min·t, t = (distance − max)/(min − max), truncated (0x565994 is _ftol2).
// The caller multiplies the result by a float factor (arg 5 of Bullet_Fire_Extended, 0x80(%esp) at 0x531797): 1.0 from
// every initial caller (player 0x4eea17, vehicle 0x51481b, AI / turret through 0x5312b0), halved by the refire after a
// rifleBullet hit on a sentient (0x53195f), forwarded unchanged by the other recursive calls.
int bulletDamageAtDistance(const std::map<std::string, std::string>& weaponDef, bool targetIsClient, float distance);

// The damage flags of a bullet (SP 0x531379..0x5313a3, MP docs/spec/mp-pmove.md "Bullets"): rifleBullet gives 0x40 in SP and 0x20
// in MP (the gametypes' iDFLAGS_PASSTHRU); armorPiercing adds 2.
inline int bulletDamageFlags(bool rifleBullet, bool armorPiercing, bool multiplayer) {
    return (rifleBullet ? (multiplayer ? 0x20 : 0x40) : 0) | (armorPiercing ? 2 : 0);
}
// 0x53187d..0x5318e9: true when a hit `offset` from the player's origin lies more than 8 units (0x592010) from the
// player's vertical axis, measured along the horizontal perpendicular (dir.y, -dir.x, 0) of the shot direction `dir`.
bool bulletMissesPlayerAxis(const std::array<float, 3>& dir, const std::array<float, 3>& offset);

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

// Where an AI shot is aimed once the hit roll is decided (0x550360 → 0x54ff90 hit, 0x550390 miss; both build the
// unit axis c = normalize(v × up), v = unit muzzle → target point T, up = the shooter's third frame axis (ctx +0xf8;
// the actor's look frame, aiShooterUp)). Random inputs are u = rand() / 32768 draws in order
// (the exe's LCG 0x343fd / 0x269ec3, state >> 17).
struct Vec3f {
    float x, y, z;
};
// Hit (0x54ff90): a random point of the body box around T. Player target: lateral half width 8, height from +10 down
// 44 (T is the eye); actor: width ((1−acc)·0.9 + 0.1)·15, height −44·((1−acc)+1)/2 from +6.
// point = T + (2·u1 − 1)·W·c + (zBase + u2·H)·up.
Vec3f aiHitPoint(Vec3f T, Vec3f c, Vec3f up, bool targetIsClient, float accuracy, float u1, float u2);
// Miss (0x550390): T − 16·v (maxs.x + 1 of the player box) beside the target: sign · M · c with sign +1 when u1 > 0.5;
// player target M = 8·(1 + u2), height (10 − 44·u3)·up; actor target M = sqrt(1350), height (2·u2 − 1)·12·up.
Vec3f aiMissPoint(Vec3f T, Vec3f v, Vec3f c, Vec3f up, bool targetIsClient, float u1, float u2, float u3);
// c = normalize(v × up) as the exe computes it (0x550027); zero when v ∥ up.
Vec3f aiSideAxis(Vec3f v, Vec3f up);
// The shooter's up axis: the third axis of the actor's look frame, built from the look pitch (+0xd8) and look yaw (+0xdc) by 0x5049a0 (0x421da0 with
// roll 0, stored at +0xf8) and copied into the shot context by 0x54fee0; (sin p cos y, sin p sin y, cos p) for pitch p, yaw y in degrees.
Vec3f aiShooterUp(float pitchDeg, float yawDeg);

} // namespace iw2::game
