// Actor facing helpers: look blocks 0x504950 / 0x5055d0, motion facing 0x505630, the moving-enemy test 0x545580
// (docs/spec/ai.md "Facing blend").
#include "game/world.h"

#include <cmath>

namespace iw2::game {

using namespace script;

bool pathAheadLongerThan(const ActorPath& p, float dist); // 0x543ae0, src/game/actorcover.cpp

namespace {

// 0x5048b0 / 0x504950: trunc(deg * 65536 / 360) & 0xffff back to degrees.
float quant(float deg) { return float(int(deg * 182.044449f) & 0xffff) * 0.0054931640625f; }

// 0x421b30 (vectoangles): pitch -atan2(z, |xy|), yaw atan2(y, x), both 0..360; straight up / down: (270 / 90, 0).
void vecAngles(const Vec3& v, float& pitch, float& yaw) {
    if (v[1] == 0.0f && v[0] == 0.0f) {
        yaw = 0.0f;
        pitch = v[2] > 0.0f ? 270.0f : 90.0f;
        return;
    }
    yaw = std::atan2(v[1], v[0]) * 180.0f / 3.14159274f;
    if (yaw < 0.0f) yaw += 360.0f;
    pitch = std::atan2(v[2], std::sqrt(v[0] * v[0] + v[1] * v[1])) * -180.0f / 3.14159274f;
    if (pitch < 0.0f) pitch += 360.0f;
}

Vec3 vec(const Value& v) { return v.isVector() ? v.asVector() : Vec3{0, 0, 0}; }

} // namespace

void World::setFacing(Entity& e, bool override, float pitch, float yaw) {
    ActorAi& ai = e.ai;
    if (override) {
        ai.overridePitch = quant(pitch);
        ai.overrideYaw = quant(yaw);
        ai.orientTarget = quant(yaw);
    } else {
        ai.basePitch = quant(pitch);
        ai.baseYaw = quant(yaw);
        ai.baseTarget = quant(yaw);
    }
}

void World::setFacingDir(Entity& e, bool override, const Vec3& dir) {
    float pitch = 0.0f, yaw = 0.0f;
    vecAngles(dir, pitch, yaw);
    setFacing(e, override, pitch, yaw);
}

// 0x505630 (eax = actor, arg = look block). Only while a path is followed in anim mode 1:
// - moving (movemode != 0), the path direct (+0x6e8 & 1), the lookahead nearer than 60 (0x591e4c) and the lookahead
//   direction behind lookforward (dot < 0): keep the entity's angles;
// - otherwise the trail sum (0x504e70); zero: keep the angles; moving: face the sum; standing: face it only when it is
//   more than acos(0.9) (0x59200c) away from the block's target, else keep the angles.
// "Keep the angles" is 0x504950 with the entity pitch / yaw (+0xe8 / +0xec).
void World::faceMotion(Entity& e, bool override) {
    const ActorAi& ai = e.ai;
    const ActorPath& p = e.path;
    if (p.count <= 0 || ai.animMode != 1) return;
    const bool moving = ai.movemode != 0;
    const Vec3 ang = vec(e.obj->get("angles"));
    auto keep = [&]() { setFacing(e, override, ang[0], ang[1]); };
    if (moving && (p.flags & 1) && p.lookDist < 60.0f) {
        const Vec3 lf = vec(e.obj->get("lookforward"));
        if (lf[0] * p.lookDir[0] + lf[1] * p.lookDir[1] < 0.0f) return keep();
    }
    auto t = ai.trail.sum();
    if (t[0] == 0.0f && t[1] == 0.0f) return keep();
    // The z the exe hands to 0x5055d0 is an unwritten stack slot (it only feeds the look pitch); 0 here.
    if (moving) return setFacingDir(e, override, {t[0], t[1], 0.0f});
    const float len = std::sqrt(t[0] * t[0] + t[1] * t[1]); // 0x421160
    if (len != 0.0f) t = {t[0] / len, t[1] / len};
    const float target = (override ? ai.orientTarget : ai.baseTarget) * 0.0174532924f;
    if (std::cos(target) * t[0] + std::sin(target) * t[1] < 0.9f) return setFacingDir(e, override, {t[0], t[1], 0.0f});
    keep();
}

// 0x545580 (esi = path, edi = unit direction to the enemy): facing the enemy while moving is allowed when the path was
// planned less than 300 ms ago, or the path ahead is not longer than 128 (0x543ae0), or else when, with h = (1 + dot of
// the lookahead direction and the direction) / 2, the lookahead distance is at least 72 - 48 h and the plan is younger
// than 1500 + 3500 h ms.
bool World::mayFaceEnemyMoving(const Entity& e, const Vec3& dir) const {
    const ActorPath& p = e.path;
    const int64_t age = timeMs_ - p.planMs;
    if (age < 300) return true;
    if (!pathAheadLongerThan(p, 128.0f)) return true;
    const float h = (p.lookDir[1] * dir[1] + dir[0] * p.lookDir[0] + 1.0f) * 0.5f;
    if (72.0f - 48.0f * h > p.lookDist) return false;
    return float(age) < h * 3500.0f + 1500.0f;
}

} // namespace iw2::game
