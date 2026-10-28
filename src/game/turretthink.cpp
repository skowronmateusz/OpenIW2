// Turret think (0x51d250 and helpers, docs/spec/turrets.md "Turret think"): target choice, convergence, firing
// state and turning for automatic turrets and AI gunners, and shootturret.
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

namespace iw2::game {

using namespace script;

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0 ? scale(v, 1.0f / l) : v;
}

// Wrapped difference a − b in [−180, 180] (0x423f50).
float angleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Pitch/yaw of a direction (0x421c90 / vectoangles).
Vec3 toAngles(const Vec3& v) {
    if (v[0] == 0.0f && v[1] == 0.0f) return {v[2] > 0.0f ? 270.0f : 90.0f, 0.0f, 0.0f};
    float yaw = std::atan2(v[1], v[0]) * 180.0f / float(M_PI);
    if (yaw < 0) yaw += 360.0f;
    float pitch = std::atan2(v[2], std::sqrt(v[0] * v[0] + v[1] * v[1])) * -180.0f / float(M_PI);
    if (pitch < 0) pitch += 360.0f;
    return {pitch, yaw, 0.0f};
}

Vec3 forwardOf(const Vec3& a) {
    const float p = a[0] * float(M_PI) / 180.0f, y = a[1] * float(M_PI) / 180.0f;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}

float weaponNumber(const std::map<std::string, std::string>* w, const char* key) {
    if (!w) return 0.0f;
    auto it = w->find(key);
    return it == w->end() ? 0.0f : float(std::atof(it->second.c_str()));
}

std::mt19937& turretRng() {
    static std::mt19937 rng(0x269ec3);
    return rng;
}
float rand11() { return std::uniform_real_distribution<float>(-1.0f, 1.0f)(turretRng()); }

} // namespace

// World position of a model tag on the turret (bind/animation pose through the entity angles; the turret bone
// rotation the client applies is not modelled, TODO).
bool World::turretTag(Entity& t, const char* tag, Vec3& pos) {
    const auto bones = entityBones(t);
    auto it = bones.find(tag);
    if (it == bones.end()) return false;
    pos = it->second.position;
    return true;
}

// Relative pitch/yaw from tag_flash to a point, inside the arcs (0x51b940).
bool World::turretAnglesTo(Entity& t, const Vec3& point, Vec3& angles) {
    Vec3 pivot;
    if (!turretTag(t, "tag_flash", pivot)) return false;
    const TurretState& ts = *t.turret;
    const Vec3 a = toAngles(sub(point, pivot)), base = t.obj->get("angles").asVector();
    angles = {angleDelta(a[0], base[0]), angleDelta(a[1], base[1]), 0};
    return angles[0] <= ts.bottomArc && angles[0] >= ts.topArc && angles[1] <= ts.leftArc && angles[1] >= ts.rightArc;
}

// Aim at a sentient (0x51ba20): its eye when inside the arcs, else its origin + 2 (while firing first, else the
// other way round), else the eye with the pitch clamped (pitch 0 and the pivot height).
bool World::turretAimSentient(Entity& t, Entity& target, Vec3& point, Vec3& angles) {
    const Vec3 eye = sightEye(target, nullptr);
    Vec3 pivot;
    if (!turretTag(t, "tag_flash", pivot)) return false;
    const TurretState& ts = *t.turret;
    const Vec3 base = t.obj->get("angles").asVector();
    const Vec3 ea = toAngles(sub(eye, pivot));
    angles[1] = angleDelta(ea[1], base[1]);
    angles[2] = 0;
    if (angles[1] > ts.leftArc || angles[1] < ts.rightArc) return false;
    const Vec3 o = target.obj->get("origin").asVector();
    const Vec3 feet{o[0], o[1], o[2] + 2.0f};
    const float eyePitch = angleDelta(ea[0], base[0]);
    const float feetPitch = angleDelta(toAngles(sub(feet, pivot))[0], base[0]);
    auto inPitch = [&](float p) { return p <= ts.bottomArc && p >= ts.topArc; };
    if (ts.state == 1) {
        if (inPitch(eyePitch)) { point = eye; angles[0] = eyePitch; return true; }
        if (inPitch(feetPitch)) { point = feet; angles[0] = feetPitch; return true; }
    } else {
        if (inPitch(feetPitch)) { point = feet; angles[0] = feetPitch; return true; }
        if (inPitch(eyePitch)) { point = eye; angles[0] = eyePitch; return true; }
    }
    point = {eye[0], eye[1], pivot[2]};
    angles[0] = 0.0f;
    return true;
}

// A sentient the turret can engage (0x51c2c0): within maxRange², aimable and in clear sight from the barrel.
bool World::turretTargetValid(Entity& t, Entity& target, float* distSq) {
    const Vec3 d = sub(target.obj->get("origin").asVector(), t.obj->get("origin").asVector());
    const float dsq = dot(d, d);
    if (dsq >= t.turret->maxRangeSq) return false;
    Vec3 point, angles;
    if (!turretAimSentient(t, target, point, angles)) return false;
    Vec3 pivot;
    if (!turretTag(t, "tag_flash", pivot)) return false;
    // TODO: the 0x48b090 visibility value (≥ 0.2) is not checked.
    const TraceResult tr = collision_->trace(pivot, point, {0, 0, 0}, {0, 0, 0}, 0x801);
    if (tr.fraction < 1.0f) return false;
    if (distSq) *distSq = dsq;
    return true;
}

void World::turretSetState(Entity& t, int state) {
    TurretState& ts = *t.turret;
    if (ts.state == state) return;
    ts.state = state;
    vm_.notify(t.obj, "turretstatechange");
}

// Aiming state machine (0x51bd40): idle → aiming (2) when the barrel is within 5° of the wanted angles or 250 ms
// after engagement started; aiming → firing (1) once the yaw convergence time has passed.
void World::turretUpdateState(Entity& t, bool aiming, const Vec3& point, const Vec3& angles) {
    TurretState& ts = *t.turret;
    if (!aiming) {
        turretSetState(t, 0);
    } else if (ts.state == 0) {
        const bool early = timeMs_ < ts.engageStartMs + 250;
        const bool off = std::fabs(angleDelta(ts.pitch, angles[0])) >= 5.0f || std::fabs(angleDelta(ts.yaw, angles[1])) >= 5.0f;
        if (!(early && off)) {
            turretSetState(t, 2);
            if (ts.engageStartMs + int64_t(ts.yawConvergenceTime * 1000.0f) - timeMs_ < 0) turretSetState(t, 1);
        }
    } else if (ts.state == 2) {
        if (ts.engageStartMs + int64_t(ts.yawConvergenceTime * 1000.0f) - timeMs_ < 0) turretSetState(t, 1);
    }
    ts.flags |= 0x40u;
    ts.aimPoint = point;
    ts.wantAngles = angles;
}

// Engage a point or non-sentient entity (0x51c1e0 / 0x51be80).
bool World::turretEngagePoint(Entity& t, Entity* target, const Vec3& point) {
    TurretState& ts = *t.turret;
    if (target && ts.engaged != target->obj) {
        ts.engaged = target->obj;
        ts.engageStartMs = timeMs_;
        ts.flags &= ~8u;
    }
    Vec3 angles;
    if (!turretAnglesTo(t, point, angles)) {
        ts.flags &= ~0x40u;
        return false;
    }
    turretUpdateState(t, true, point, angles);
    return true;
}

// Engage a sentient with the convergence miss (0x51bef0): until the convergence time has passed, aim beside the
// target (63 units off the line of fire, on the side the target faces), closing in linearly, then at it.
bool World::turretEngageSentient(Entity& t, Entity& target) {
    TurretState& ts = *t.turret;
    if (ts.engaged != target.obj) {
        ts.engaged = target.obj;
        ts.engageStartMs = timeMs_;
        ts.flags &= ~8u;
    }
    Vec3 point, angles;
    if (!turretAimSentient(t, target, point, angles)) return false;
    const int conv = int(ts.yawConvergenceTime * 1000.0f);
    if (0 < conv - timeMs_ + ts.engageStartMs) {
        Vec3 pivot;
        if (!turretTag(t, "tag_flash", pivot)) return false;
        const Vec3 o = target.obj->get("origin").asVector();
        if (!(ts.flags & 8u)) {
            ts.flags |= 8u;
            Vec3 side = normalized(Vec3{point[1] - pivot[1], pivot[0] - point[0], 0}); // perpendicular in xy
            side = scale(side, 63.0f);
            const Value ang = target.obj->get("angles");
            const float yaw = ang.isVector() ? ang.asVector()[1] * float(M_PI) / 180.0f : 0.0f;
            const Vec3 facing{std::cos(yaw), std::sin(yaw), 0};
            Vec3 off = dot(facing, side) < 0.0f ? scale(side, -1.0f) : side;
            for (float z : {o[2], point[2]}) {
                const Vec3 p{point[0] + off[0], point[1] + off[1], z};
                if (turretEngagePoint(t, nullptr, p)) {
                    ts.aimOffset = normalized(sub(p, point));
                    if (z == point[2]) ts.aimOffset[2] = 0;
                    return true;
                }
            }
            ts.aimOffset = {0, 0, 0};
        } else {
            const float f = (float(ts.engageStartMs - timeMs_ + conv) / float(conv)) * 30.0f;
            const Vec3 p = add(point, add(scale(ts.aimOffset, 33.0f), scale(ts.aimOffset, f)));
            if (turretEngagePoint(t, nullptr, p)) return true;
        }
    }
    turretUpdateState(t, true, point, angles);
    return true;
}

// No target: drop the engaged target and stop (0x51b850).
void World::turretIdle(Entity& t) {
    TurretState& ts = *t.turret;
    ts.engaged.reset();
    ts.flags &= ~0x48u;
    turretSetState(t, 0);
}

// Turning towards the wanted angles (0x51b610 / 0x51b4e0): with a target, at the rate that reaches them by the end of
// the convergence time (pitch: pitch convergence, yaw: yaw convergence), clamped to the weapon's min/max
// vert/hor turn speeds; without one at 200°/s.
void World::turretTurn(Entity& t, bool tracking) {
    TurretState& ts = *t.turret;
    const float dPitch = angleDelta(ts.wantAngles[0], ts.pitch), dYaw = angleDelta(ts.wantAngles[1], ts.yaw);
    float rate[2] = {200.0f, 200.0f};
    if (tracking) {
        const float frameMs = 50.0f;
        rate[0] = std::fabs(dPitch) * 1000.0f / frameMs;
        rate[1] = std::fabs(dYaw) * 1000.0f / frameMs;
        const int64_t pitchLeft = ts.engageStartMs + int64_t(ts.pitchConvergenceTime * 1000.0f) - timeMs_;
        const int64_t yawLeft = ts.engageStartMs + int64_t(ts.yawConvergenceTime * 1000.0f) - timeMs_;
        if (yawLeft > 0 && frameMs < float(yawLeft)) rate[1] *= frameMs / float(yawLeft);
        if (pitchLeft > 0 && frameMs < float(pitchLeft)) rate[0] *= frameMs / float(pitchLeft);
        const auto* w = weapon(ts.weapon);
        rate[0] = std::clamp(rate[0], weaponNumber(w, "minvertturnspeed"), std::max(weaponNumber(w, "minvertturnspeed"), weaponNumber(w, "maxvertturnspeed")));
        rate[1] = std::clamp(rate[1], weaponNumber(w, "minhorturnspeed"), std::max(weaponNumber(w, "minhorturnspeed"), weaponNumber(w, "maxhorturnspeed")));
    }
    // 0x51b6e8: while the gunner's pitch bound is active (0x200) and the drop pitch is in use (0x100), pitch turns at 360°/s.
    if ((ts.flags & 0x200u) && (ts.flags & 0x100u)) rate[0] = 360.0f;
    const float d[2] = {dPitch, dYaw};
    float* cur[2] = {&ts.pitch, &ts.yaw};
    for (int i = 0; i < 2; ++i) {
        const float step = rate[i] * 0.05f;
        *cur[i] += std::clamp(d[i], -step, step);
    }
    // 0x51b779..0x51b7c5: the AI gunner's pitch bound (+0xa8, set by the pose step 0x549ba0). Beyond it (above with 0x400,
    // below without) the excess becomes the bone-control drop e+0x78 (0x51b7a3); within it the drop-pitch flag 0x100 is
    // cleared.
    if (ts.flags & 0x200u) {
        const bool beyond = (ts.flags & 0x400u) ? ts.pitch < ts.pitchLimit : ts.pitch > ts.pitchLimit;
        if (beyond) ts.drop = ts.pitch - ts.pitchLimit;
        else ts.flags &= ~0x100u;
    }
    t.obj->set("$turretangles", Value(Vec3{ts.pitch, ts.yaw, 0}));
}

int World::debugFireTurrets() {
    int n = 0;
    for (const auto& e : entities_)
        if (e->turret && !e->obj->removed) {
            shootTurret(*e);
            const Vec3 o = e->obj->get("origin").isVector() ? e->obj->get("origin").asVector() : Vec3{0, 0, 0};
            print("debug turret " + e->obj->get("model").toString() + " at " + std::to_string(o[0]) + "," + std::to_string(o[1]) + "," + std::to_string(o[2]) + " yaw " + std::to_string(e->turret->yaw));
            ++n;
        }
    return n;
}

// One shot (0x51b250 → 0x51ada0): countdown = fireTime · 3; from tag_flash towards the aim point with a random
// offset of up to 8 units on the two axes across the line of fire (0x5501e0) when engaging a target, else along the
// barrel; bullet spread aiSpread (AI or no gunner) or playerSpread, range 8192.
void World::shootTurret(Entity& t) {
    TurretState& ts = *t.turret;
    const auto* w = weapon(ts.weapon);
    ts.fireCountdownMs = int(weaponNumber(w, "firetime") * 1000.0f) * 3;
    Vec3 muzzle;
    if (!turretTag(t, "tag_flash", muzzle)) {
        print("turret " + t.obj->get("model").toString() + " has no tag_flash");
        return;
    }
    const Vec3 base = t.obj->get("angles").asVector();
    Vec3 dir = forwardOf({base[0] + ts.pitch, base[1] + ts.yaw, 0});
    Entity* target = ts.engaged ? entityOf(Value(ts.engaged)) : nullptr;
    if (target && (ts.flags & 0x40u)) {
        const Vec3 d = normalized(sub(ts.aimPoint, muzzle));
        const Vec3 up{0, 0, 1};
        const Vec3 across = normalized(cross(d, up));
        const Vec3 p = add(ts.aimPoint, add(scale(across, rand11() * 8.0f), scale(up, rand11() * 8.0f)));
        const Vec3 nd = normalized(sub(p, muzzle));
        if (dot(nd, dir) > 0.95f) dir = nd;
    }
    // Bullet spread in degrees (0x531210; the cone distribution is inferred).
    const bool player = ts.inUse && ts.owner && entityOf(Value(ts.owner)) && entityOf(Value(ts.owner))->isPlayer;
    const float spread = (player ? ts.playerSpread : ts.aiSpread) * float(M_PI) / 180.0f;
    const Vec3 a = toAngles(dir);
    const Vec3 sd = forwardOf({a[0] + rand11() * spread * 180.0f / float(M_PI), a[1] + rand11() * spread * 180.0f / float(M_PI), 0});
    // The damage attacker is the turret's user, or the world without one (0x51ada0 → 0x5312b0).
    Entity* user = ts.inUse && ts.owner ? entityOf(Value(ts.owner)) : nullptr;
    fireBullet(t, muzzle, add(muzzle, scale(sd, 8192.0f)), weaponDamage(ts.weapon), user, ts.weapon);
    // EV_FIRE_WEAPON_MG42 (0xaf, sent at 0x51afc1) → cgame 0x4b7800: for an entity that is not the local client
    // the muzzle flash 0x4b6660 plays the weapon's worldFlashEffect (0xf48e48) at the model's tag_flash; fireSound
    // (empty for the MG42s) plays too. The event also shakes the view (0x4bf5e0 with scale 0.05, 100 ms, radius 100 at the entity origin, ViewShake). The effect is bolted on tag_flash of the turret entity.
    shakeEvents_.push_back({t.obj->get("origin").asVector(), 0.05f, 100, 100.0f, timeMs_});
    // The same event (0x4bb069..0x4bb0b6) pings the compass for the entity at es +0xa4 (the turret user) when it is an actor (type 9):
    // 50 ms at the turret's origin (centity +0x148) through 0x4c0e20. The centity byte +0x140 it tests is "present in the
    // current snapshot": 0x4a4336 clears it on every entity of the old snapshot and sets it for the new snapshot's entities and the
    // own player. The ping is skipped when the event entity's number equals the snapshot ps word +0x570; ps +0x570 is not the turret
    // link (turret use writes ps +0x568, 0x51db8d) and is not identified, so the skip is not modelled.
    // TODO(spec): ps +0x570, and the snapshot membership of the user (the server's PVS / broadcast test; every actor counts as in it).
    if (user && !user->isPlayer && user->obj->kind == "actor")
        compassPing(*user, t.obj->get("origin").asVector(), 50);
    const std::string flash = weaponField(ts.weapon, "worldFlashEffect");
    if (!flash.empty()) fxEvents_.push_back({flash, muzzle, dir, t.number, "tag_flash"});
    // Shell eject: the same handler 0x4b7800 calls CG_EjectWeaponBrass (0x4b7730, 0x4b79dc) unless boltAction (+0x328) is set:
    // shellEjectEffect bolted on tag_brass. No shipped turret weapon (mg42_bipod_*, mg42_tank_*) defines shellEjectEffect, so
    // nothing ejects in the shipped data. The turret fire event is always 0xaf (0x51afc1), never the last-shot event 0xa0, and no shipped
    // weapon file of a turret (mg42_*) has a lastShotEjectEffect key (checked in all weapons/sp/mg42_*), so lastShotEjectEffect is moot.
    const std::string brass = weaponField(ts.weapon, "shellEjectEffect");
    Vec3 brassAt;
    if (!brass.empty() && brass != "none" && std::atoi(weaponField(ts.weapon, "boltAction").c_str()) == 0 && turretTag(t, "tag_brass", brassAt))
        fxEvents_.push_back({brass, brassAt, dir, t.number, "tag_brass"});
    const std::string fireSnd = weaponField(ts.weapon, "fireSound");
    if (!fireSnd.empty()) startSound(fireSnd, &muzzle);
}

// Fire sounds (0x51b320): the entity's loop sound is cleared; while the fire countdown (+0x6c, set to fireTime · 3 by
// a shot) runs it is loopFireSoundPlayer (info +0xae) when a client owns the turret and that is set, else
// loopFireSound (+0xac). The countdown drops by 50; when it ends and stopFireSound (+0xb0) is set the loop stops and
// stopFireSoundPlayer (+0xb2, client owner, 0x4d8360) or stopFireSound (event 0xb3) plays. Sound fields are set
// from the weapon at turret setup (0x51df70…0x51e04c).
// Every shipped turret weapon (mg42_bipod_*, mg42_tank_*) defines stopFireSound, so the case of a loop ending without a stop sound does not
// occur in the shipped data.
void World::turretFireSound(Entity& t, bool playerOwner) {
    TurretState& ts = *t.turret;
    std::string loop;
    if (ts.fireCountdownMs > 0) {
        const std::string playerLoop = weaponField(ts.weapon, "loopFireSoundPlayer");
        loop = playerOwner && !playerLoop.empty() ? playerLoop : weaponField(ts.weapon, "loopFireSound");
        ts.fireCountdownMs -= 50;
        const std::string stop = weaponField(ts.weapon, "stopFireSound");
        if (ts.fireCountdownMs < 1 && !stop.empty()) {
            loop.clear();
            const std::string playerStop = weaponField(ts.weapon, "stopFireSoundPlayer");
            const Vec3 at = t.obj->get("origin").asVector();
            startSound(playerOwner && !playerStop.empty() ? playerStop : stop, &at);
        }
    }
    if (loop != ts.loopSound) {
        if (ts.loopHandle > 0) stopSound(ts.loopHandle);
        ts.loopHandle = -1;
        ts.loopSound = loop;
        if (!loop.empty()) {
            const Vec3 at = t.obj->get("origin").asVector();
            ts.loopHandle = startSound(loop, &at);
        }
    }
}

// 0x51cf10 (first think 0x51ced0 and restoredefaultdroppitch): raises the stock point tag_butt about tag_aim in 3
// degree steps (pitch -3·i, i = 0..30, the muzzle going up) and at the first step where a point trace (mask 0x800811,
// the turret passed) from the tag_aim pivot to the stock hits something stores -3·i + the turret's pitch as the drop
// pitch (info +0x4c). No hit in 31 steps leaves the drop pitch as it was. Done in the turret's yaw frame.
// The tag poses are the dobj's (model space, including the bone controls of the turret's current pitch and yaw, which
// entityBones poses through entityModels); the frame carrying the points to the world is AnglesToAxis of the full entity
// angles (e+0xe8: pitch, yaw and roll) at the entity origin (e+0xdc, 0x423430), not the yaw alone.
void World::restoreDefaultDropPitch(Entity& t) {
    TurretState& ts = *t.turret;
    const auto bones = entityBones(t);
    const auto ia = bones.find("tag_aim"), ib = bones.find("tag_butt");
    if (ia == bones.end() || ib == bones.end()) return;
    const Value ov = t.obj->get("origin"), av = t.obj->get("angles");
    if (!ov.isVector()) return;
    const Vec3 origin = ov.asVector();
    const Vec3 angles = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
    // entityBones places the model by the yaw alone: back to model space (x forward, y left, z up).
    const Axis yawFrame = yawAxis(angles[1]);
    auto toModel = [&](const Vec3& w) {
        const Vec3 r = sub(w, origin);
        return Vec3{dot(r, yawFrame[0]), dot(r, yawFrame[1]), dot(r, yawFrame[2])};
    };
    const Vec3 pivot = toModel(ia->second.position), butt = toModel(ib->second.position);
    const Axis frame = anglesAxis(angles);
    const Vec3 start = axisApply(origin, frame, pivot);
    const float d0 = butt[0] - pivot[0], d1 = butt[1] - pivot[1], d2 = butt[2] - pivot[2];
    for (int i = 0; i <= 30; ++i) {
        const float p = float(i) * -3.0f * 3.14159265f / 180.0f, cp = std::cos(p), sp = std::sin(p);
        // d0 · forward − d1 · right + d2 · up of AngleVectors(pitch p) (right = (0,-1,0)).
        const float l0 = d0 * cp + d2 * sp, l1 = d1, l2 = -d0 * sp + d2 * cp;
        const Vec3 end = axisApply(origin, frame, {pivot[0] + l0, pivot[1] + l1, pivot[2] + l2});
        const TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, 0x800811, t.number);
        if (tr.fraction < 1.0f) {
            ts.dropPitch = float(i) * -3.0f + ts.pitch;
            return;
        }
    }
}

// Turret think for one turret (0x51d250).
void World::thinkTurret(Entity& t) {
    TurretState& ts = *t.turret;
    ts.drop = 0.0f; // 0x51d279: e+0x78, set again by the turn (0x51b7a3)
    if (!ts.dropPitchDone) { // the first think (0x51ced0) computes the drop pitch
        ts.dropPitchDone = true;
        restoreDefaultDropPitch(t);
    }
    Entity* owner = ts.inUse && ts.owner ? entityOf(Value(ts.owner)) : nullptr;
    if (owner && owner->isActor && !isAlive(*owner)) { // a dead gunner leaves the turret (state finish 0x549b20)
        owner->usingTurret.reset();
        releaseTurret(t);
        owner = nullptr;
    }
    if (owner && owner->isPlayer) return; // the player aims
    turretFireSound(t, false);
    const bool ai = ts.flags & 1u, automatic = ts.flags & 2u;
    // Enemy team of the turret team (table {0, 2, 1, 0, 0} in 0x51c400): axis ↔ allies, others none.
    const std::string enemyTeam = ts.team == 1 ? "allies" : ts.team == 2 ? "axis" : "";
    Entity* scriptTarget = ts.target ? entityOf(Value(ts.target)) : nullptr;
    auto inRange = [&](Entity& e) {
        const Vec3 d = sub(e.obj->get("origin").asVector(), t.obj->get("origin").asVector());
        return dot(d, d) < ts.maxRangeSq;
    };
    auto engageScript = [&]() -> bool {
        if (!scriptTarget || !inRange(*scriptTarget)) return false;
        if (isSentient(*scriptTarget)) return turretEngageSentient(t, *scriptTarget);
        return turretEngagePoint(t, scriptTarget, scriptTarget->obj->get("origin").asVector());
    };
    bool tracking = false;
    if (automatic && !ai) {
        // Automatic without AI (0x51c540 / 0x51c400): keep the engaged sentient for 4 s or while valid; scan one
        // more enemy sentient per frame and switch when it is valid and closer.
        Entity* cur = ts.engaged ? entityOf(Value(ts.engaged)) : nullptr;
        Entity* pick = nullptr;
        float curD = 0;
        if (cur && (ts.flags & 0x40u) && isSentient(*cur)) {
            if (timeMs_ <= ts.engageStartMs + 4000) pick = cur;
            else if (!cur->obj->get("ignoreme").truthy() && turretTargetValid(t, *cur, &curD)) pick = cur;
        }
        if (!(pick == cur && cur && timeMs_ <= ts.engageStartMs + 4000)) {
            std::vector<Entity*> enemies;
            for (const auto& ep : entities_)
                if (isSentient(*ep) && !enemyTeam.empty() && ep->obj->get("team").toString() == enemyTeam) enemies.push_back(ep.get());
            if (enemies.empty()) {
                ts.scanIndex = -1;
            } else {
                ts.scanIndex = (ts.scanIndex + 1) % int(enemies.size());
                Entity* c = enemies[size_t(ts.scanIndex)];
                float d = 0;
                if (!c->obj->get("ignoreme").truthy() && turretTargetValid(t, *c, &d) && (!pick || d < curD)) pick = c;
            }
        }
        if (pick && turretEngageSentient(t, *pick)) tracking = true;
        else if (engageScript()) tracking = true;
        else turretIdle(t);
    } else if (owner && owner->isActor) {
        // Gunner (0x51c630 automatic / 0x51cc80 manual), simplified: the gunner's enemy while it is known to the
        // gunner within the suppression time and in range, else the script target. TODO: the full 0x51c630 rules.
        Entity* enemy = entityOf(owner->obj->get("enemy"));
        bool engaged = false;
        if (enemy && isSentient(*enemy) && inRange(*enemy)) {
            auto it = owner->sentientInfo.find(enemy->obj.get());
            const bool known = it != owner->sentientInfo.end() &&
                               (it->second.visible || timeMs_ - it->second.lastKnownMs < int64_t(ts.suppressionTime * 1000.0f));
            if (known) engaged = turretEngageSentient(t, *enemy);
        }
        if (!engaged) engaged = engageScript();
        if (!engaged) turretIdle(t);
        tracking = engaged;
    } else if (ai) {
        // 0x51d2e2 → 0x51b8a0 with ebx = 0: the unmanned turret turns to (drop pitch, yaw 0).
        turretSetState(t, 0);
        ts.wantAngles = {ts.dropPitch, 0, 0};
    } else {
        if (engageScript()) tracking = true; // 0x51cc80 without a gunner
        else turretIdle(t);
    }
    turretTurn(t, tracking);
}


} // namespace iw2::game
