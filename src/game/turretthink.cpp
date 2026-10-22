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
    const float d[2] = {dPitch, dYaw};
    float* cur[2] = {&ts.pitch, &ts.yaw};
    for (int i = 0; i < 2; ++i) {
        const float step = rate[i] * 0.05f;
        *cur[i] += std::clamp(d[i], -step, step);
    }
    t.obj->set("$turretangles", Value(Vec3{ts.pitch, ts.yaw, 0}));
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
    fireBullet(t, muzzle, add(muzzle, scale(sd, 8192.0f)), weaponDamage(ts.weapon));
}

// Turret think for one turret (0x51d250).
void World::thinkTurret(Entity& t) {
    TurretState& ts = *t.turret;
    Entity* owner = ts.inUse && ts.owner ? entityOf(Value(ts.owner)) : nullptr;
    if (owner && owner->isActor && !isAlive(*owner)) { // a dead gunner leaves the turret (state finish 0x549b20)
        owner->usingTurret.reset();
        releaseTurret(t);
        owner = nullptr;
    }
    if (owner && owner->isPlayer) return; // the player aims
    if (ts.fireCountdownMs > 0) ts.fireCountdownMs -= 50; // 0x51b320
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
        turretSetState(t, 0); // 0x51b8a0
    } else {
        if (engageScript()) tracking = true; // 0x51cc80 without a gunner
        else turretIdle(t);
    }
    turretTurn(t, tracking);
}

void World::thinkTurrets() {
    for (const auto& ep : entities_)
        if (ep->turret && !ep->obj->removed) thinkTurret(*ep);
}

} // namespace iw2::game
