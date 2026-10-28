// Actor facing helpers: look blocks 0x504950 / 0x5055d0, motion facing 0x505630, the moving-enemy test 0x545580
// (docs/spec/ai.md "Facing blend").
#include "game/world.h"

#include <cfloat>
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


namespace {
int teamIndexOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}
Vec3 originOfE(const ObjectPtr& o) { return vec(o->get("origin")); }
// 0x423f50: a - b brought into [-180, 180].
float angleSub(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}
float clampf(float v, float lo, float hi) { return v > hi ? hi : v < lo ? lo : v; }
} // namespace

// 0x5049a0: the actor's look angles (+0xd8 pitch, +0xdc yaw) and the look axes from them (0x421da0; the forward axis +0xe0
// is the script field lookforward, field table 0x570418: offset 0xe0, vector, read only).
void World::setLookAngles(Entity& e, float pitch, float yaw) {
    ActorAi& ai = e.ai;
    ai.lookAngles[0] = quant(pitch);
    ai.lookAngles[1] = quant(yaw);
    const float p = ai.lookAngles[0] * 0.01745329238474369f, y = ai.lookAngles[1] * 0.01745329238474369f;
    e.obj->set("lookforward", Value(Vec3{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)}));
}

// Spawn (0x4fc7ff): the look angles from the entity angles (the base block is set at spawn, world.cpp 0x4fc7dd).
void World::initActorFacing(Entity& e) {
    const Vec3 a = vec(e.obj->get("angles"));
    setLookAngles(e, a[0], a[1]);
    e.ai.facingInit = true;
}

// 0x53e237 (teleport with angles), 0x54a829 (turret user), 0x54b1f3 (animscripted, every frame): the base block from the entity
// angles (0x504950); the first two also set the look angles (0x5049a0).
void World::faceBlockToAngles(Entity& e, bool look) {
    if (!e.ai.facingInit) initActorFacing(e);
    const Vec3 a = vec(e.obj->get("angles"));
    setFacing(e, false, a[0], a[1]);
    if (look) setLookAngles(e, a[0], a[1]);
}

// 0x504aa0 (pitch delta 0, yaw delta from the animation, called by 0x504160 when the yaw is not 0): in both blocks the look
// pitch is re-quantised (+ the pitch delta 0) and the look yaw and the target become target + yaw delta (the look yaw is
// rebuilt from the target, +0x110 / +0x120, not from itself); the entity yaw += yaw delta with pitch and roll zeroed; the
// look angles += the deltas (0x5049a0).
void World::turnActorByDelta(Entity& e, float yawDelta) {
    ActorAi& ai = e.ai;
    ai.basePitch = quant(ai.basePitch);
    ai.baseYaw = quant(yawDelta + ai.baseTarget);
    ai.baseTarget = quant(yawDelta + ai.baseTarget);
    ai.overridePitch = quant(ai.overridePitch);
    ai.overrideYaw = quant(yawDelta + ai.orientTarget);
    ai.orientTarget = quant(yawDelta + ai.orientTarget);
    const Vec3 a = vec(e.obj->get("angles"));
    e.obj->set("angles", Value(Vec3{0, quant(yawDelta + a[1]), 0}));
    setLookAngles(e, ai.lookAngles[0], ai.lookAngles[1] + yawDelta);
}

// 0x504be0: the look angles turn towards the active block's look pitch / yaw by at most 40 degrees each (0x591f78 / 0x592110).
void World::actorLookTurn(Entity& e) {
    ActorAi& ai = e.ai;
    if (!ai.facingInit) initActorFacing(e);
    const bool ovr = ai.orient != 0;
    const float tp = ovr ? ai.overridePitch : ai.basePitch, ty = ovr ? ai.overrideYaw : ai.baseYaw;
    const float pitch = clampf(angleSub(tp, ai.lookAngles[0]), -40.0f, 40.0f) + ai.lookAngles[0];
    const float yaw = clampf(angleSub(ty, ai.lookAngles[1]), -40.0f, 40.0f) + ai.lookAngles[1];
    setLookAngles(e, pitch, yaw);
}

// 0x517e40: nodes a and b see each other (the same node, or the bit min * count + max of the node visibility table).
bool World::nodesSeeEachOther(int a, int b) const {
    if (a == b) return true;
    const auto& bits = paths_.visibilityBits();
    if (bits.empty()) return false;
    const size_t lo = size_t(std::min(a, b)), hi = size_t(std::max(a, b));
    const size_t bit = paths_.size() * lo + hi;
    return bit / 8 < bits.size() && (bits[bit / 8] >> (bit % 8) & 1);
}

// 0x5432f0 (`rec` = the actor's record of the target, or none): the actor's node (0x515040, none -> no result); the target's node is
// the one remembered in the record (+0x24, the target's node when it was last known, 0x500ca2) or else its current node
// (0x515040); none -> no result. If the two nodes see each other (0x517e40) that node is the result. Otherwise 0x547330 searches
// from the target's node over the links (the target team's blocked links skipped, cost = link length, open list sorted without a
// heuristic) for the first node that sees the actor's node; that node, or none.
int World::enemyPositionNode(Entity& e, Entity& target, const SentientInfo* rec) {
    const int mine = sentientNode(e);
    if (mine < 0) return -1;
    int theirs = rec ? rec->lastKnownNode : -1;
    if (theirs < 0) theirs = sentientNode(target);
    if (theirs < 0) return -1;
    if (nodesSeeEachOther(theirs, mine)) return theirs;
    const size_t n = nodes_.size();
    if (search_.size() != n) search_.assign(n, {});
    auto& s = search_;
    static constexpr int kHead = -2;
    const int id = ++searchId_;
    const int team = teamIndexOf(target);
    int head = theirs;
    s[size_t(theirs)] = {id, -1, kHead, -1, 0.0f, 0.0f, true};
    auto setNext = [&](int node, int next) { (node == kHead ? head : s[size_t(node)].next) = next; };
    while (head >= 0) {
        const int cur = head;
        if (nodesSeeEachOther(cur, mine)) return cur;
        head = s[size_t(cur)].next;
        if (head >= 0) s[size_t(head)].prev = kHead;
        const auto& links = paths_.links(size_t(cur));
        for (size_t li = 0; li < links.size(); ++li) {
            if (linkBlocked(size_t(cur), li, team)) continue;
            const int nb = links[li].target;
            auto& sn = s[size_t(nb)];
            const float g = s[size_t(cur)].g + links[li].distance;
            if (sn.id == id) {
                if (sn.g <= g) continue;
                if (sn.queued) {
                    setNext(sn.prev, sn.next);
                    if (sn.next >= 0) s[size_t(sn.next)].prev = sn.prev;
                }
            } else {
                sn.id = id;
                sn.h = 0.0f;
            }
            sn.g = g;
            sn.parent = cur;
            int after = kHead;
            for (int it = head; it >= 0 && s[size_t(it)].h + s[size_t(it)].g < g + sn.h; it = s[size_t(it)].next) after = it;
            sn.prev = after;
            sn.next = after == kHead ? head : s[size_t(after)].next;
            setNext(after, nb);
            if (sn.next >= 0) s[size_t(sn.next)].prev = nb;
            sn.queued = true;
        }
        s[size_t(cur)].queued = false;
    }
    return -1;
}

// 0x505790 (no enemy, modes 3..5): returns whether it handled the facing. Without a team to fight ({0, 2, 1, 0, 0}) false.
// Until the timer +0x204c passes, true (the block stays). Then the nearest living sentient of the fought team (3D distance²,
// a player's (client) taken * 0.25, the first of equals) is picked; none -> false. The timer becomes now + 3000, the target's
// position node (0x5432f0 without a record) is looked up (none -> false) and the block faces it when it is 1 unit or more away (2D).
bool World::faceThreat(Entity& e, bool override) {
    static constexpr int kEnemy[5] = {0, 2, 1, 0, 0};
    const int fought = kEnemy[teamIndexOf(e)];
    if (fought == 0) return false;
    if (e.ai.threatCheckMs - timeMs_ > 0) return true;
    const Vec3 o = originOfE(e.obj);
    Entity* best = nullptr;
    float bestD = FLT_MAX;
    for (const auto& ep : entities_) {
        Entity& s = *ep;
        if (&s == &e || !isSentient(s) || teamIndexOf(s) != fought) continue;
        const Vec3 so = originOfE(s.obj);
        const float dx = so[0] - o[0], dy = so[1] - o[1], dz = so[2] - o[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (s.isPlayer) d *= 0.25f;
        if (d < bestD) {
            bestD = d;
            best = &s;
        }
    }
    if (!best) return false;
    e.ai.threatCheckMs = timeMs_ + 3000;
    const int node = enemyPositionNode(e, *best, nullptr);
    if (node < 0) return false;
    const Vec3 np = originOfE(nodes_[size_t(node)].obj);
    const Vec3 d{np[0] - o[0], np[1] - o[1], np[2] - o[2]};
    if (d[0] * d[0] + d[1] * d[1] >= 1.0f) setFacingDir(e, override, d);
    return true;
}

// 0x504450 (unlinked actor) without the mode-1 path walk, which the states run through moveActor: anim mode 8 (nophysics) does
// nothing at all. Modes 2..7 take this frame's animation delta (0x503e40 -> 0x478550, goal weights per +0x220) and its yaw
// (0x423900 of the relative rotation, 2 / (a² + b²) with 0x591ed0, degrees by 0x591fd8): 2 xy delta (kind 2, no yaw), 3 no
// delta (kind 1), 4 xy delta (kind 2), 5 3D delta (kind 3), 6 3D delta (kind 4), 7 xy delta (kind 5); modes 0 and 1 have no yaw.
// After the turn the physics (0x501d00) rotates the delta by desiredangle (+0x124; v = x * forward - y * right + z) for kinds
// other than 1 and moves the block (World::moveActorKind).
void World::actorMoveFrame(Entity& e) {
    ActorAi& ai = e.ai;
    const int mode = ai.animMode;
    if (mode == 8) return;
    float yaw = 0.0f;
    Vec3 d{0, 0, 0};
    int kind = 0;
    // The port's path following forces anim mode 1 for its walk (World::followActorPath) while setActorAnimScript may then store
    // an animmode override; a frame that already walked the path is treated as mode 1 so the actor does not move twice.
    // TODO(spec): whether 0x552350 stores 1 directly or through 0x53ac90 (which applies the override +0x21c).
    const bool walked = ai.walkedMs == timeMs_;
    if (!walked && mode >= 2 && mode <= 7 && e.anim) {
        const AnimDelta ad = e.anim->delta(ai.animDeltaGoalWeights);
        if (mode != 2) {
            const float sq = ad.rot[0] * ad.rot[0] + ad.rot[1] * ad.rot[1];
            if (sq > 0.0f) {
                const float k = 2.0f / sq;
                yaw = std::atan2(ad.rot[1] * ad.rot[0] * k, 1.0f - k * ad.rot[0] * ad.rot[0]) * 57.2957763671875f;
            }
        }
        switch (mode) {
        case 2: d = {ad.trans[0], ad.trans[1], 0}; kind = 2; break;
        case 3: kind = 1; break;
        case 4: d = {ad.trans[0], ad.trans[1], 0}; kind = 2; break;
        case 5: d = {ad.trans[0], ad.trans[1], ad.trans[2]}; kind = 3; break;
        case 6: d = {ad.trans[0], ad.trans[1], ad.trans[2]}; kind = 4; break;
        case 7: d = {ad.trans[0], ad.trans[1], 0}; kind = 5; break;
        default: break;
        }
    }
    // The tail of 0x504160: a non-zero yaw turns the actor (0x504aa0), then the selector 0x505d80, the body turn 0x504ca0 and the
    // look turn 0x504be0 (World::actorFacing).
    if (!ai.facingInit) initActorFacing(e);
    if (yaw != 0.0f) turnActorByDelta(e, yaw);
    actorFacing(e);
    if (kind == 0) return;
    if (kind == 1) {
        moveActorKind(e, 1, {0, 0, 0}, 0.05f);
        return;
    }
    const Value dv = e.obj->get("desiredangle");
    const float y = (dv.isNumber() ? dv.asFloat() : 0.0f) * 0.01745329238474369f;
    const float c = std::cos(y), s = std::sin(y);
    moveActorKind(e, kind, {d[0] * c - d[1] * s, d[0] * s + d[1] * c, d[2]}, 0.05f);
}

} // namespace iw2::game
