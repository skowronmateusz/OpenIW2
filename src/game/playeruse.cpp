// The player's use key and what it uses (docs/spec/controls.md "Use"): the use scan (0x534490) and the pick of the
// usable entity (0x534d40), the press (0x5343d0 → 0x5342b0 → 0x5341a0), and turrets used by the player (use
// 0x51db50, think 0x51b5c0, release 0x51b3d0).
#include "game/world.h"
#include "game/pmove.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

float normalize(Vec3& v) {
    const float l = std::sqrt(dot(v, v));
    if (l > 0) for (float& c : v) c /= l;
    return l;
}

// AngleDelta (0x423f50): a − b wrapped to [−180, 180].
float angleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// AngleNormalize360 (0x4240a0).
float angleNormalize360(float a) {
    while (a < 0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

Vec3 viewForward(const Vec3& angles) {
    const float rad = 0.0174532924f; // 0x591f50
    const float yaw = angles[1] * rad, pitch = angles[0] * rad;
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

// The turret's pitch and yaw towards view angles (0x51db50 / 0x51b140): AngleDelta from the turret's angles, then
// held inside the arcs (a value at or past an arc takes the arc).
void turretAnglesFromView(TurretState& ts, const Vec3& base, const Vec3& view) {
    float p = angleDelta(view[0], base[0]);
    const float p0 = p;
    if (p0 - ts.bottomArc >= 0.0f) p = ts.bottomArc;
    if (ts.topArc - p0 >= 0.0f) p = ts.topArc;
    ts.pitch = p;
    float y = angleDelta(view[1], base[1]);
    const float y0 = y;
    if (y0 - ts.leftArc >= 0.0f) y = ts.leftArc;
    if (ts.rightArc - y0 >= 0.0f) y = ts.rightArc;
    ts.yaw = y;
}

} // namespace

// The eye (0x4e8b70): on a turret its tag_player, else the origin raised by the view height and moved by the lean
// (0x440380 with roll factor 16 and distance 20, Vec leanOffset).
Vec3 World::playerEye() {
    const Vec3 o = player_->obj->get("origin").asVector();
    if (playerOnTurret() && playerTurret_) {
        Vec3 eye;
        if (playerTurretEye(eye)) return eye;
    }
    const game::Vec lo = leanOffset(playerCmd_.viewAngles[1], playerCmd_.lean);
    return {o[0] + lo[0], o[1] + lo[1], o[2] + playerCmd_.viewHeight + lo[2]};
}

bool World::playerTurretEye(Vec3& eye) {
    if (!playerTurret_ || playerTurret_->obj->removed) return false;
    return turretTag(*playerTurret_, "tag_player", eye); // 0x4d7730 (prints "no tag_player" when missing)
}

// The use scan (0x534490): entities in the box eye ± (108, 108, 96) that are items (type 2), usable (contents
// 0x200000: turrets, vehicles, trigger_use) or actors with +0x211b. A trigger_use_touch counts while it touches the
// player's box (score −144); any other within 128 (vehicles) or 72 of the eye, measured to its absbox centre, scores
// distance + (1 − (dir · forward + 1) / 2) · 144, −144 more for a trigger_use. Sorted; then each one but a
// trigger_use_touch gets +10000 and drops off the end when the eye → absbox centre line (a turret: its tag_player)
// is blocked (mask 0x11); sorted again.
// TODO(spec): items (type 2, pickup check 0x4fad50) and actors (+0x211b) are not candidates yet; trigger absboxes
// ignore the entity angles.
std::vector<World::UseCandidate> World::scanUsables(const Vec3& eye, const Vec3& forward) {
    const Vec3 po = player_->obj->get("origin").asVector();
    const Vec3 pmin{po[0] - 15, po[1] - 15, po[2]}, pmax{po[0] + 15, po[1] + 15, po[2] + 70}; // TODO(spec): stance maxs
    std::vector<UseCandidate> list;
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (e.obj->removed || e.isSpawner || &e == player_) continue;
        Vec3 mn, mx;
        const bool trigUse = e.classname == "trigger_use", trigTouch = e.classname == "trigger_use_touch";
        if (e.turret) {
            const Vec3 o = e.obj->get("origin").asVector(); // turret bounds (−32, −32, 0)…(32, 32, 56) (0x51e650)
            mn = {o[0] - 32, o[1] - 32, o[2]};
            mx = {o[0] + 32, o[1] + 32, o[2] + 56};
        } else if (e.classname == "script_vehicle") {
            if (!vehicleState(Value(e.obj)).usable || !vehicleUseBounds(e, mn, mx)) continue;
        } else if (trigUse || trigTouch) {
            const Value m = e.obj->get("model");
            if (!m.isString() || !submodelBounds(m.asString(), mn, mx)) continue;
            const Value ov = e.obj->get("origin");
            const Vec3 o = ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
            for (int i = 0; i < 3; ++i) { mn[size_t(i)] += o[size_t(i)]; mx[size_t(i)] += o[size_t(i)]; }
        } else {
            continue;
        }
        if (mn[0] > eye[0] + 108 || mx[0] < eye[0] - 108 || mn[1] > eye[1] + 108 || mx[1] < eye[1] - 108 ||
            mn[2] > eye[2] + 96 || mx[2] < eye[2] - 96)
            continue;
        if (trigTouch) {
            if (mn[0] <= pmax[0] && pmin[0] <= mx[0] && mn[1] <= pmax[1] && pmin[1] <= mx[1] && mn[2] <= pmax[2] &&
                pmin[2] <= mx[2] && triggerContact(e, pmin, pmax))
                list.push_back({&e, -144.0f});
            continue;
        }
        const float limit = e.vehicle || e.classname == "script_vehicle" ? 128.0f : 72.0f;
        const Vec3 centre{(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        Vec3 d = sub(centre, eye);
        const float dist = normalize(d);
        if (dist > limit) continue;
        float score = (1.0f - (dot(d, forward) + 1.0f) * 0.5f) * 144.0f;
        if (trigUse) score -= 144.0f;
        list.push_back({&e, dist + score});
    }
    auto byScore = [](const UseCandidate& a, const UseCandidate& b) { return a.score < b.score; };
    std::sort(list.begin(), list.end(), byScore);
    size_t blocked = 0;
    for (UseCandidate& c : list) {
        if (c.e->classname == "trigger_use_touch") continue;
        Vec3 point;
        if (c.e->turret) {
            if (!turretTag(*c.e, "tag_player", point)) point = c.e->obj->get("origin").asVector();
        } else {
            Vec3 mn, mx;
            if (c.e->classname == "script_vehicle") vehicleUseBounds(*c.e, mn, mx);
            else {
                submodelBounds(c.e->obj->get("model").asString(), mn, mx);
                const Value ov = c.e->obj->get("origin");
                const Vec3 o = ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
                for (int i = 0; i < 3; ++i) { mn[size_t(i)] += o[size_t(i)]; mx[size_t(i)] += o[size_t(i)]; }
            }
            point = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        }
        if (collision_->trace(eye, point, {0, 0, 0}, {0, 0, 0}, 0x11, player_->number).fraction < 1.0f) {
            c.score += 10000.0f;
            ++blocked;
        }
    }
    std::sort(list.begin(), list.end(), byScore);
    list.resize(list.size() - blocked);
    return list;
}

// A turret the player may use (0x51dac0): turret flag 0x1000, not in use, the player within its half arc
// (0x51d8a0: the angle between the flat direction of the arc centre, yaw + right arc + (|right| + |left|) / 2, and
// the flat turret → player direction at most (|right| + |left|) / 2), no grenade being cooked (ps+0x3c) and on
// the ground.
bool World::turretUsableByPlayer(Entity& t) {
    const TurretState& ts = *t.turret;
    if (!(ts.flags & 0x1000u) || ts.inUse) return false;
    const Vec3 to = t.obj->get("origin").asVector(), po = player_->obj->get("origin").asVector();
    const Vec3 ta = t.obj->get("angles").isVector() ? t.obj->get("angles").asVector() : Vec3{0, 0, 0};
    // Arcs as the use sets them (+0x38 right, +0x40 left; the setup values are copied over at use).
    const float half = (std::fabs(ts.rightArc) + std::fabs(ts.leftArc)) * 0.5f;
    float centre = ta[1] + ts.rightArc + half;
    centre = angleNormalize360(centre + 180.0f) - 180.0f; // 0x424040 (AngleNormalize180)
    const float rad = 0.0174532924f;
    Vec3 dir{std::cos(centre * rad), std::sin(centre * rad), 0};
    normalize(dir);
    Vec3 d{po[0] - to[0], po[1] - to[1], 0};
    normalize(d);
    float a = std::acos(std::clamp(dot(d, dir), -1.0f, 1.0f)); // 0x565d70
    if (a > 3.1415927f || a < -3.1415927f) a = 3.1415927f;
    if (!(a * 57.2957763671875f <= half)) return false;
    if (playerCmd_.grenadeFuseMs != 0) return false;
    return playerCmd_.onGround;
}

// What the use key would use now (0x534d40): nothing while dead, in the binocular weapon states 0x11 … 0x16, in a
// vehicle, on a turret or mantling. Otherwise the first scan candidate that is usable: a trigger_use(_touch) with a
// cursor hint (+0x98, 2 at spawn 0x52cde0 unless the cursorhint key changes it; never 0 there) ends the search
// either way, a turret (0x51dac0) or a vehicle (0x512200) only when usable, anything else is skipped.
void World::findPlayerUseEntity() {
    playerUseEntity_ = -1;
    if (!player_ || !isAlive(*player_)) return;
    if (playerCmd_.weaponState >= 0x11 && playerCmd_.weaponState <= 0x16) return;
    if ((pmFlags_ & 0x20000) || player_->usingVehicle >= 0) return;
    if (playerUseState_ != 0 || playerCmd_.mantling) return;
    const Vec3 eye = playerEye();
    for (const UseCandidate& c : scanUsables(eye, viewForward(playerCmd_.viewAngles))) {
        Entity& e = *c.e;
        if (e.classname == "trigger_use" || e.classname == "trigger_use_touch") {
            playerUseEntity_ = e.number;
            return;
        }
        if (e.turret) {
            if (!turretUsableByPlayer(e)) continue;
        } else if (e.classname == "script_vehicle") {
            const VehicleState& v = *e.vehicle;
            const Value h = e.obj->get("health");
            if (v.owner >= 0 || std::fabs(v.forwardSpeed) > 1.0f || !h.isNumber() || h.asInt() < 1 || !v.usable) continue;
        } else {
            continue;
        }
        playerUseEntity_ = e.number;
        return;
    }
}

// A use (0x5341a0): "trigger" with the user, then the entity type's use function: a vehicle's 0x512f00 (in or
// out), a turret's 0x51db50; trigger_use's (0x52cdd0) does nothing more.
void World::usePlayerEntity(Entity& e) {
    if (e.classname == "script_vehicle") {
        vehicleState(Value(e.obj));
        useVehicle(e, *player_);
        return;
    }
    vm_.notify(e.obj, "trigger", {Value(player_->obj)});
    if (e.obj->removed) return;
    if (e.turret) playerUseTurret(e);
}

// The player gets on a turret (0x51db50).
void World::playerUseTurret(Entity& t) {
    TurretState& ts = *t.turret;
    playerUseState_ = 1;
    ts.inUse = true;
    ts.owner = player_->obj;
    playerTurret_ = &t;
    ts.flags |= 0x800u; // first frame
    ts.rightArc = ts.setupRightArc;
    ts.leftArc = ts.setupLeftArc;
    ts.savedOrigin = player_->obj->get("origin").asVector();
    ts.savedStance = playerCmd_.stance;
    playerEFlags_ = ts.stance == 2 ? 0x100u : ts.stance == 1 ? 0x200u : 0x300u;
    const Vec3 base = t.obj->get("angles").isVector() ? t.obj->get("angles").asVector() : Vec3{0, 0, 0};
    turretAnglesFromView(ts, base, playerCmd_.viewAngles);
    viewClampRange_[0] = angleDelta(ts.bottomArc, ts.topArc) * 0.5f;
    viewClampBase_[0] = angleNormalize360(base[0] + ts.bottomArc - viewClampRange_[0]);
    viewClampRange_[1] = angleDelta(ts.leftArc, ts.rightArc) * 0.5f;
    viewClampBase_[1] = angleNormalize360(base[1] + ts.leftArc - viewClampRange_[1]);
    vm_.notify(t.obj, "turretownerchange");
}

void World::clampPlayerView(float& pitch, float& yaw) const {
    if (!playerOnTurret()) return;
    float* v[2] = {&pitch, &yaw};
    for (int i = 0; i < 2; ++i) {
        const float d = angleDelta(*v[i], viewClampBase_[i]);
        if (d > viewClampRange_[i]) *v[i] = viewClampBase_[i] + viewClampRange_[i];
        else if (d < -viewClampRange_[i]) *v[i] = viewClampBase_[i] - viewClampRange_[i];
        else continue;
        *v[i] = angleNormalize360(*v[i]);
        if (*v[i] > 180.0f) *v[i] -= 360.0f;
    }
}

// Getting off (0x51b3d0): the fire countdown stops, the stance the player had comes back, the player returns to
// where it got on (0x4e8890: + 1 up), keeps its view angles, and the turret is free again: "turret_deactivate"
// while it was in use, then "turretownerchange".
void World::releasePlayerTurret(Entity& t) {
    TurretState& ts = *t.turret;
    ts.fireCountdownMs = 0;
    TurretRelease r;
    r.stance = ts.savedStance;
    ts.savedStance = -1;
    r.origin = {ts.savedOrigin[0], ts.savedOrigin[1], ts.savedOrigin[2] + 1.0f};
    player_->obj->set("origin", Value(r.origin));
    turretRelease_ = r;
    playerEFlags_ &= ~0x300u;
    playerTurret_ = nullptr;
    playerUseState_ = 0;
    if (ts.inUse) vm_.notify(t.obj, "turret_deactivate");
    ts.inUse = false;
    ts.owner.reset();
    ts.flags &= ~0x800u;
    vm_.notify(t.obj, "turretownerchange");
}

// Each frame on a turret (0x51b5c0): while the player is on it (+0x112 == 1) and alive, the turret follows the view
// (0x51b140), the player stands 34 units behind the turret origin along the barrel at its own height (0x51afe0),
// and attack fires every fireTime (0x51b2b0: timer −50 per frame; at ≤ 0 with attack the timer restarts at
// fireTime and 0x51b250 shoots); the fire countdown runs down (0x51b320). Otherwise the player gets off.
// TODO(spec): the fire loop sound and the stop sound of 0x51b320.
void World::thinkPlayerTurret() {
    if (!playerOnTurret() || !playerTurret_) return;
    Entity& t = *playerTurret_;
    if (t.obj->removed || !t.turret) { // 0x51da40: a freed turret releases its user
        playerEFlags_ &= ~0x300u;
        playerTurret_ = nullptr;
        playerUseState_ = 0;
        return;
    }
    TurretState& ts = *t.turret;
    if (playerUseState_ != 1 || !isAlive(*player_)) {
        releasePlayerTurret(t);
        return;
    }
    const Vec3 base = t.obj->get("angles").isVector() ? t.obj->get("angles").asVector() : Vec3{0, 0, 0};
    turretAnglesFromView(ts, base, playerCmd_.viewAngles);
    ts.flags &= ~0x800u;
    t.obj->set("$turretangles", Value(Vec3{ts.pitch, ts.yaw, 0}));
    const Vec3 f = viewForward({base[0] + ts.pitch, base[1] + ts.yaw, 0});
    const Vec3 to = t.obj->get("origin").asVector(), po = player_->obj->get("origin").asVector();
    player_->obj->set("origin", Value(Vec3{to[0] - f[0] * 34.0f, to[1] - f[1] * 34.0f, po[2]}));
    ts.fireTimerMs -= 50;
    if (ts.fireTimerMs < 1) {
        ts.fireTimerMs = 0;
        if (playerCmd_.buttons & 1) {
            // fireTime (weapondef +0x204, in ms)
            const auto* w = weapon(ts.weapon);
            const auto it = w ? w->find("firetime") : std::map<std::string, std::string>::const_iterator{};
            ts.fireTimerMs = w && it != w->end() ? int(std::atof(it->second.c_str()) * 1000.0f) : 0;
            shootTurret(t);
        }
    }
    if (ts.fireCountdownMs > 0) ts.fireCountdownMs -= 50;
}

// The client think's use part (0x4e7f90): the use key pressed this frame (+activate 8 or +usereload 0x20; 0x5343d0,
// g_useholdtime 0 so the use happens at once, 0x534350) gets out of the vehicle, off the turret (+0x112 = 2, the
// turret think releases), or uses what the last pick found; then the turret think and the pick for the next frame.
// TODO(spec): the order of these inside 0x4e7f90 and relative to the script frame is not read; the global
// 0x9bef24 that gates the press is treated as set.
void World::runPlayerUse() {
    if (!player_) return;
    const uint32_t pressed = playerCmd_.buttons & ~playerOldButtons_;
    playerOldButtons_ = playerCmd_.buttons;
    const bool binoculars = playerCmd_.weaponState >= 0x11 && playerCmd_.weaponState <= 0x16;
    if ((pressed & 0x28) && !binoculars) {
        if ((pmFlags_ & 0x20000) && player_->usingVehicle >= 0) {
            Entity* v = entityByNumber(player_->usingVehicle);
            if (v && v->vehicle && v->vehicle->usable) useVehicle(*v, *player_);
        } else if (playerUseState_ != 0) {
            playerUseState_ = playerOnTurret() ? 2 : 0;
        } else if (!playerCmd_.mantling && playerUseEntity_ >= 0) {
            if (Entity* e = entityByNumber(playerUseEntity_); e && !e->obj->removed) usePlayerEntity(*e);
        }
    }
    thinkPlayerTurret();
    findPlayerUseEntity();
}

} // namespace iw2::game
