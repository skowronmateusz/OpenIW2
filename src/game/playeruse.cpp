#include <cstdio>
#include <cstdlib>
// The player's use key and what it uses (docs/spec/controls.md "Use"): the use scan (0x534490) and the pick of the
// usable entity (0x534d40), the press (0x5343d0 → 0x5342b0 → 0x5341a0), and turrets used by the player (use
// 0x51db50, think 0x51b5c0, release 0x51b3d0).
#include "game/world.h"
#include "game/pmove.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace iw2::game {
namespace {
std::string lowerStr(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
} // namespace

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
void turretAnglesFromView(const TurretState& ts, const Vec3& base, const Vec3& view, float& pitch, float& yaw) {
    float p = angleDelta(view[0], base[0]);
    const float p0 = p;
    if (p0 - ts.bottomArc >= 0.0f) p = ts.bottomArc;
    if (ts.topArc - p0 >= 0.0f) p = ts.topArc;
    pitch = p;
    float y = angleDelta(view[1], base[1]);
    const float y0 = y;
    if (y0 - ts.leftArc >= 0.0f) y = ts.leftArc;
    if (ts.rightArc - y0 >= 0.0f) y = ts.rightArc;
    yaw = y;
}

void turretAnglesFromView(TurretState& ts, const Vec3& base, const Vec3& view) {
    turretAnglesFromView(ts, base, view, ts.pitch, ts.yaw);
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

// The turret's pitch and yaw for the given view angles (the same rule as the 20 Hz think), evaluated for any view so
// the client can pose the gun and the eye from one pair per rendered frame.
bool World::playerTurretAnglesFor(const Vec3& view, float& pitch, float& yaw) {
    if (!playerOnTurret() || !playerTurret_ || !playerTurret_->turret) return false;
    const Value a = playerTurret_->obj->get("angles");
    turretAnglesFromView(*playerTurret_->turret, a.isVector() ? a.asVector() : Vec3{0, 0, 0}, view, pitch, yaw);
    return true;
}

// tag_player with the turret bone controls (0x4ab0d0) at the given pitch and yaw instead of the stored think angles.
// TODO(spec): whether the original cgame poses the eye per client frame (0x4e8b70 is the server side; the cgame view
// origin path was not read).
bool World::playerTurretEyeAt(float pitch, float yaw, Vec3& eye) {
    if (!playerTurret_ || playerTurret_->obj->removed || !playerTurret_->turret) return false;
    TurretState& ts = *playerTurret_->turret;
    const float sp = ts.pitch, sy = ts.yaw;
    ts.pitch = pitch;
    ts.yaw = yaw;
    const bool ok = turretTag(*playerTurret_, "tag_player", eye);
    ts.pitch = sp;
    ts.yaw = sy;
    return ok;
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
// Weapon items are candidates (pick: 0x534c80). TODO(spec): other items and actors (+0x211b); trigger absboxes
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
        } else if (e.classname.rfind("weapon_", 0) == 0) {
            // A weapon item (type 2): LaunchItem (0x521e00) gives weapons the bounds (−1, −1, −1)…(1, 1, 1).
            const Vec3 o = e.obj->get("origin").asVector();
            mn = {o[0] - 1, o[1] - 1, o[2] - 1};
            mx = {o[0] + 1, o[1] + 1, o[2] + 1};
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
            else if (c.e->classname.rfind("weapon_", 0) == 0) {
                const Vec3 o = c.e->obj->get("origin").asVector();
                mn = mx = o;
            } else {
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
// the flat player → turret direction at most (|right| + |left|) / 2, so the player stands behind the gun), no
// grenade being cooked (ps+0x3c) and on the ground.
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
    Vec3 d{to[0] - po[0], to[1] - po[1], 0}; // player → turret (0x51d962: turret +0xdc − player +0xdc)
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
// The pickup prompt for a weapon item (cgame 0x4a99b0): PICKUPNEWWEAPON when the weapon is of class
// item, has an offhandClass, or a primary slot is free; otherwise SWAPWEAPONS unless the hinted weapon is the one in its slot.
// TODO(spec): def+0x84 (assumed offhandClass), the slot table for slots above 2.
std::string World::pickupHintText(const std::string& w) {
    const Inventory& inv = inventory(*player_);
    const std::string cls = lowerStr(weaponField(w, "weaponClass")), off = lowerStr(weaponField(w, "offhandClass"));
    const std::string slotName = lowerStr(weaponField(w, "weaponSlot"));
    if (cls == "item" || (!off.empty() && off != "none")) return "PLATFORM_PICKUPNEWWEAPON";
    if ((slotName == "primary" || slotName == "primaryb") && (inv.slot[1].empty() || inv.slot[2].empty())) return "PLATFORM_PICKUPNEWWEAPON";
    const std::string cur = lowerStr(player_->obj->get("$weapon").toString());
    if (!cur.empty() && lowerStr(weaponField(cur, "weaponSlot")) == slotName) return cur == w ? std::string() : "PLATFORM_SWAPWEAPONS";
    const int idx = slotName == "primaryb" ? 2 : 1;
    return inv.slot[idx] == w ? std::string() : "PLATFORM_SWAPWEAPONS";
}

void World::findPlayerUseEntity() {
    playerUseEntity_ = -1;
    playerUseHint_ = {};
    if (!player_ || !isAlive(*player_)) return;
    // On a turret (0x534ce0): its weapon's hint with dropHintString when the weapon has one.
    if (playerOnTurret() && playerTurret_ && playerTurret_->turret) {
        const std::string& w = playerTurret_->turret->weapon;
        const std::string drop = weaponField(w, "dropHintString");
        if (!drop.empty()) playerUseHint_ = {0, w, drop};
        return;
    }
    if (playerCmd_.weaponState >= 0x11 && playerCmd_.weaponState <= 0x16) return;
    if ((pmFlags_ & 0x20000) || player_->usingVehicle >= 0) return;
    if (playerUseState_ != 0 || playerCmd_.mantling) return;
    const Vec3 eye = playerEye();
    for (const UseCandidate& c : scanUsables(eye, viewForward(playerCmd_.viewAngles))) {
        Entity& e = *c.e;
        if (e.classname == "trigger_use" || e.classname == "trigger_use_touch") {
            // Hint +0x98 (setcursorhint, 2 from spawn), string +3 (sethintstring).
            playerUseEntity_ = e.number;
            const Value h = e.obj->get("$cursorhint"), str = e.obj->get("$hintstring");
            playerUseHint_.hint = h.isNumber() ? h.asInt() : 2;
            if (str.isString()) playerUseHint_.text = str.asString();
            return;
        }
        if (e.classname.rfind("weapon_", 0) == 0) {
            // 0x534c80: a weapon item is usable when the weapon is not a grenade (weaponType, def +0x78 ≠ 1) and not
            // owned; the hint is the weapon's (index + 4). TODO(spec): health items (giType 3, hint 3).
            const std::string w = lowerStr(e.classname.substr(7));
            if (!weapon(w) || lowerStr(weaponField(w, "weaponType")) == "grenade" || inventory(*player_).owned.count(w)) continue;
            playerUseEntity_ = e.number;
            playerUseHint_.weapon = w;
            playerUseHint_.text = pickupHintText(w);
            return;
        }
        if (e.turret) {
            if (!turretUsableByPlayer(e)) continue;
            // Weapon hint (weapon index + 4) with useHintString when the weapon has one (+0x56c / +0x574).
            playerUseHint_.weapon = e.turret->weapon;
            playerUseHint_.text = weaponField(e.turret->weapon, "useHintString");
        } else if (e.classname == "script_vehicle") {
            const VehicleState& v = *e.vehicle;
            const Value h = e.obj->get("health");
            if (v.owner >= 0 || std::fabs(v.forwardSpeed) > 1.0f || !h.isNumber() || h.asInt() < 1 || !v.usable) continue;
            playerUseHint_.hint = 2; // TODO(spec): the vehicle weapon's useHintString (0x534d40 case 0xc)
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
    if (e.classname.rfind("weapon_", 0) == 0) {
        pickUpWeaponItem(e);
        return;
    }
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
    turretFireSound(t, true);
}

// The client think's use part (0x4e7f90): the use key pressed this frame (+activate 8 or +usereload 0x20; 0x5343d0,
// g_useholdtime 0 so the use happens at once, 0x534350) gets out of the vehicle, off the turret (+0x112 = 2, the
// turret think releases), or uses what the last pick found; then the turret think and the pick for the next frame.
// TODO(spec): the order of these inside 0x4e7f90 and relative to the script frame is not read; the global
// 0x9bef24 that gates the press is treated as set.
void World::runPlayerUse() {
    if (!player_) return;
    damageFeedback(); // P_DamageFeedback (0x4e7040) in the client think
    updateCrosshairTarget(); // 0x535070
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

namespace iw2::game {

// Using a weapon item (0x5341a0 type 2 → Touch_Item 0x521a90 → 0x521ad0 → Pickup_Weapon 0x521170): "touch" on the
// item with the player; a weapon the player does not own is taken: with no free primary slot the current weapon
// is dropped as its item where the player stands (0x5220f0) and its slot gets the new one; the item's ammo is
// given (item +0x148 stock / +0x154 clip) and the player switches to it (0x532060); the item goes away (0x4d7c30).
// TODO(spec): the exact slot test (weaponSlot class +0x80 and the per-slot ammo loop of 0x521170), the item ammo
// rules for map-placed items (0x370/0x374 random ranges), "menu_show_notify" and the pickup events 0x90/0x91.
void World::pickUpWeaponItem(Entity& item) {
    const std::string w = lowerStr(item.classname.substr(7));
    if (!player_ || !weapon(w)) return;
    vm_.notify(item.obj, "touch", {Value(player_->obj)});
    if (item.obj->removed) return;
    Inventory& inv = inventory(*player_);
    if (inv.owned.count(w)) return;
    const std::string slotName = lowerStr(weaponField(w, "weaponSlot"));
    const bool primary = slotName == "primary" || slotName == "primaryb";
    if (primary && !inv.slot[1].empty() && !inv.slot[2].empty()) {
        const std::string cur = lowerStr(player_->obj->get("$weapon").toString());
        if (!cur.empty() && cur != "none" && inv.owned.count(cur)) {
            Entity& dropped = spawnEntity("weapon_" + cur);
            dropped.obj->set("origin", player_->obj->get("origin"));
            dropped.obj->set("angles", player_->obj->get("angles"));
            const std::string wm = weaponField(cur, "worldModel");
            if (!wm.empty()) dropped.obj->set("model", Value(wm));
            launchItem(dropped, *player_); // 0x5220f0 → 0x521f70
            const int slot = inv.slot[1] == cur ? 1 : 2;
            takeWeapon(inv, cur);
            // 0x4dc8a0: taking the held weapon clears ps weapon at once, so the swap has no putaway.
            player_->obj->set("$weapontaken", Value(true));
            giveWeapon(inv, w);
            if (inv.slot[slot] != w) {
                for (auto& sname : inv.slot)
                    if (sname == w) sname.clear();
                inv.slot[slot] = w;
            }
        }
    } else {
        giveWeapon(inv, w);
    }
    addAmmo(inv, w, weaponInt(w, "startAmmo"), true);
    player_->obj->set("$weapon", Value(w)); // switch to it (0x532060)
    item.obj->removed = true;
}

} // namespace iw2::game

namespace iw2::game {

std::vector<World::CompassMarker> World::compassFriendlies() const {
    std::vector<CompassMarker> out;
    if (!drawCompassFriendlies_) return out;
    for (const auto& e : entities_) {
        if (e->obj->removed || e->obj->kind != "actor" || !isAlive(*e)) continue;
        const Value team = e->obj->get("team");
        if (!team.isString() || team.asString() != "allies") continue;
        if (!e->obj->get("drawoncompass").truthy()) continue;
        const Value org = e->obj->get("origin"), ang = e->obj->get("angles");
        if (!org.isVector()) continue;
        CompassMarker m;
        m.pos = org.asVector();
        if (ang.isVector()) m.yaw = ang.asVector()[1];
        out.push_back(m);
    }
    return out;
}

void World::debugSpawnItem(const std::string& weapon) {
    Entity& e = spawnEntity("weapon_" + weapon);
    e.obj->set("origin", player_->obj->get("origin"));
}

void World::debugSetClip(const std::string& w, int rounds) { inventory(*player_).clip[weaponField(w, "clipName")] = rounds; }

std::pair<int, int> World::debugAmmo(const std::string& w) {
    Inventory& inv = inventory(*player_);
    return {inv.ammo[weaponField(w, "ammoName")], inv.clip[weaponField(w, "clipName")]};
}

void World::debugDamage(Entity& e, int amount) { damage(e, player_, amount, {0, 0, 1}, e.obj->get("origin").asVector(), "MOD_EXPLOSIVE"); }

void World::debugVehicleTouch(Entity& v, Entity& t) {
    if (!v.vehicle) return;
    const Vec3 to = t.obj->get("origin").asVector(), saved = v.obj->get("origin").asVector();
    v.obj->set("origin", Value(Vec3{to[0] - 100.0f, to[1], to[2]}));
    v.vehicle->velocity = {400, 0, 0};
    v.vehicle->forwardSpeed = 400;
    vehicleTouchDamage(v, t);
    v.vehicle->velocity = {0, 0, 0};
    v.vehicle->forwardSpeed = 0;
    v.obj->set("origin", Value(saved));
}

void World::debugVehicleRocket(Entity& v, Entity& t) {
    if (!v.vehicle || !v.vehicle->info || v.vehicle->info->turretWeapon.empty()) return;
    const Vec3 to = t.obj->get("origin").asVector();
    const Vec3 start{to[0] - 300.0f, to[1], to[2] + 40.0f};
    spawnRocket(v, v.vehicle->info->turretWeapon, start, {1, 0, 0});
}

std::vector<World::CompassMarker> World::compassVehicles() const {
    std::vector<CompassMarker> out;
    for (const auto& e : entities_) {
        if (e->obj->removed || !e->vehicle || !e->vehicle->onCompass) continue;
        const Value org = e->obj->get("origin"), ang = e->obj->get("angles");
        if (!org.isVector()) continue;
        CompassMarker m;
        m.pos = org.asVector();
        if (ang.isVector()) m.yaw = ang.asVector()[1];
        out.push_back(m);
    }
    return out;
}

// 0x535070: a trace (0x534fd0 → G_Trace, mask 0x2280e801, skipping the player) 15000 units along the view from the
// eye. An actor hit (contents 0x4000) of the player's enemy team within the weapon's enemyCrosshairRange (+0x260;
// the turret's weapon on a turret) sets 0x200000; any other actor within g_friendlyfireDist (128, 0x1c13f30) sets
// 0x100000. TODO(spec): the name target (ps+0x8678, g_friendlyNameDist), the 0x8000000 flag, non-actor hits
// (turrets/vehicles, +0x1b4), the trace surface-flag 0x10 exception.
void World::updateCrosshairTarget() {
    crosshairFlags_ = 0;
    crosshairName_.clear();
    crosshairWeaponLine_.clear();
    if (!player_ || !isAlive(*player_)) return;
    const Vec3 eye = playerEye(), f = viewForward(playerCmd_.viewAngles);
    int loc = 0;
    const Vec3 end{eye[0] + f[0] * 15000.0f, eye[1] + f[1] * 15000.0f, eye[2] + f[2] * 15000.0f};
    TraceResult tr = locationalTrace(eye, end, 0x2280e801u, player_, nullptr, loc);
    // A first hit whose classname is trigger_lookat (0x535201, string 0x5883f4) is touched by the player (0x528cb0 queues
    // {hit, player}; the frame flush 0x4daaa8..0x4dabe9 skips stale serials and entities already touched this frame, then
    // notifies the hit entity "trigger" (0x58a278) with the player as activator, 0x4dab7c..0x4daba6): every frame while
    // looked at, no one-shot, no spawnflags or distance test beyond the 15000-unit trace. The trace is then repeated with
    // mask 0x280e801 (without 0x20000000) for the actor logic below. TODO(spec): the notify is deferred to the flush
    // here immediate; ps+0x8678 (= the hit entity) is not kept.
    if (Entity* lk = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr; lk && lk->classname == "trigger_lookat") {
        if (!lk->obj->removed) {
            ++stats.lookatTouches;
            vm_.notify(lk->obj, "trigger", {Value(player_->obj)});
        }
        tr = locationalTrace(eye, end, 0x280e801u, player_, nullptr, loc);
    }
    Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
    if (std::getenv("IW2_CROSSHAIR_DEBUG")) std::fprintf(stderr, "crosshair eye %g %g %g yaw %g ent %d\n", eye[0], eye[1], eye[2], playerCmd_.viewAngles[1], tr.entity);
    // A vehicle (entity type 0xc, 0x5353ce): inside g_friendlyNameDist (15000) it becomes the name target (0x535455) and the
    // HUD lines are its look-at strings (+0x194 / +0x196, 0x534b61): nothing when it has none. The enemy crosshair flag
    // 0x200000 (0x535471..0x5354cb): the player is in a vehicle (pm_flags 0x20000), the distance is inside the current
    // weapon's enemyCrosshairRange (weapon def +0x260) and the vehicle's team (+0x178, setvehicleteam) is the player's enemy
    // team (0x4e7010).
    if (hit && hit->vehicle && hit->classname == "script_vehicle") {
        const Value org = hit->obj->get("origin");
        if (org.isVector()) {
            const Vec3 o = org.asVector();
            const float dx = o[0] - eye[0], dy = o[1] - eye[1], dz = o[2] - eye[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 15000.0f * 15000.0f && !hit->vehicle->lookAtText.empty()) {
                crosshairName_ = hit->vehicle->lookAtText;
                crosshairWeaponLine_ = hit->vehicle->lookAtText2;
            }
            if (pmFlags_ & 0x20000) {
                const std::string wn = playerOnTurret() ? playerTurretWeapon() : lowerStr(player_->obj->get("$weapon").toString());
                const std::string ts = player_->obj->get("team").isString() ? player_->obj->get("team").asString() : std::string();
                const int pt = ts == "axis" ? 1 : 2;
                const float range = weaponFloat(wn, "enemyCrosshairRange");
                if (d2 < range * range && hit->vehicle->team == (pt == 1 ? 2 : 1)) crosshairFlags_ |= 0x200000u;
            }
        }
        return;
    }
    if (!hit || !hit->isActor || !isAlive(*hit)) return;
    const Vec3 d = sub(hit->obj->get("origin").asVector(), eye);
    const float dist2 = dot(d, d);
    const std::string w = playerOnTurret() ? playerTurretWeapon() : lowerStr(player_->obj->get("$weapon").toString());
    const auto team = [](const Value& v) {
        const std::string s = v.isString() ? v.asString() : std::string();
        return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
    };
    const int pt = team(player_->obj->get("team")) ? team(player_->obj->get("team")) : 2;
    const int enemy = pt == 1 ? 2 : pt == 2 ? 1 : 0; // 0x4e7010
    bool target = false; // ps+0x8678 is set when still empty (every branch of 0x535070 sets it only once per frame)
    if (team(hit->obj->get("team")) == enemy) {
        const float range = weaponFloat(w, "enemyCrosshairRange");
        if (dist2 < range * range) {
            crosshairFlags_ |= 0x200000u;
            target = true; // 0x535386..0x5353b9
        }
    } else {
        constexpr float nameDist = 15000.0f; // g_friendlyNameDist default (0x4d8a1a), 0x5352e3 / 0x535455
        if (dist2 < nameDist * nameDist) target = true;
        if (dist2 < 128.0f * 128.0f) crosshairFlags_ |= 0x100000u;
    }
    // 0x534aa0: configstring 0xb = "\x15" + the actor's name (+0xd0, script `.name`), 0xc = the display name of its weapon
    // (+0xd2, `.weapon`; weapon def +8). Nothing for an unnamed actor.
    if (std::getenv("IW2_CROSSHAIR_DEBUG")) std::fprintf(stderr, "crosshair hit %s target %d name '%s'\n", hit->classname.c_str(), int(target), hit->obj->get("name").isString() ? hit->obj->get("name").asString().c_str() : "?");
    if (target) {
        const Value n = hit->obj->get("name");
        if (n.isString() && !n.asString().empty()) {
            crosshairName_ = n.asString();
            const Value wv = hit->obj->get("weapon");
            if (wv.isString() && !wv.asString().empty() && wv.asString() != "none")
                crosshairWeaponLine_ = weaponField(lowerStr(wv.asString()), "displayName");
        }
    }
}

} // namespace iw2::game
