// The game side of the player's weapon events (docs/spec/controls.md): the melee blow (0x531da0 → 0x530f80), the
// offhand grenade throw (0x531d60 → 0x531990) and the offhand pick (0x4dc6d0).

#include "game/world.h"
#include "game/hitboxes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iw2::game {

using script::Value;

namespace {

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len(const Vec3& a) { return std::sqrt(dot(a, a)); }

// AngleVectors: forward, right, up of pitch / yaw / roll in degrees.
void angleVectors(const Vec3& a, Vec3& f, Vec3& r, Vec3& u) {
    const float k = float(M_PI) / 180.0f;
    const float sp = std::sin(a[0] * k), cp = std::cos(a[0] * k), sy = std::sin(a[1] * k), cy = std::cos(a[1] * k),
                sr = std::sin(a[2] * k), cr = std::cos(a[2] * k);
    f = {cp * cy, cp * sy, -sp};
    r = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    u = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
}

} // namespace

// 0x4dc6d0: the first owned weapon of the offhand class with rounds in its clip or stock. The original walks the
// weapon indices in load order; OpenIW2 walks the owned weapons by name (TODO(spec): the load order).
std::string World::playerOffhandFor(const std::string& offhandClass) {
    if (!player_) return {};
    Inventory& inv = inventory(*player_);
    for (const std::string& w : inv.owned) {
        if (lowerStr(weaponField(w, "offhandClass")) != lowerStr(offhandClass)) continue;
        if (inv.clip[weaponField(w, "clipName")] + inv.ammo[weaponField(w, "ammoName")] != 0) return w;
    }
    return {};
}

// The throw (pmove state 14 → 15 takes one round from the clip; the event 0xa6 runs 0x531d60): the view's forward
// (0x531b90) times projectileSpeed, plus projectileSpeedUp upwards, is the grenade's velocity from the eye with the
// weapon's fuseTime; the player's velocity along that direction is added (0x5319e5…0x531a2e).
void World::playerThrowGrenade(const std::string& weaponName, const Vec3& eye, const Vec3& viewAngles, const Vec3& playerVelocity,
                              int cookedFuseMs) {
    if (!player_ || !isAlive(*player_)) return;
    Inventory& inv = inventory(*player_);
    int& clip = inv.clip[weaponField(weaponName, "clipName")];
    if (clip > 0) --clip;
    Vec3 f, r, u;
    angleVectors(viewAngles, f, r, u);
    const float speed = float(weaponInt(weaponName, "projectileSpeed"));
    const Vec3 v{speed * f[0], speed * f[1], speed * f[2] + float(weaponInt(weaponName, "projectileSpeedUp"))};
    // 0x527605..0x52762c: a non-zero client fuse (ps+0x3c, the time cooked in the hand) replaces the weapon fuseTime.
    const int fuseMs = cookedFuseMs != 0 ? cookedFuseMs : int(std::lround(weaponFloat(weaponName, "fuseTime") * 1000.0f));
    Entity& g = spawnGrenade(*player_, weaponName, eye, v, fuseMs);
    const float l = len(v);
    if (l > 0) {
        const Vec3 n = scale(v, 1.0f / l);
        g.grenade->trDelta = add(g.grenade->trDelta, scale(n, dot(n, playerVelocity)));
    }
}

// The melee blow (0x530f80 → 0x530de0): from the eye, locational traces (mask 0x280e831, priority map 0x5a9584, skipping the player) to
// eye + forward·player_meleeRange (64) + right·player_meleeWidth (10)·a + up·player_meleeHeight (10)·b for
// (a, b) = (0, 0), (1, 1), (1, −1), (−1, 1), (−1, −1) (table 0x58f7c4; only the centre when width or height ≤ 0);
// the first trace that hits something other than sky damages the entity it hit: meleeDamage + rand() % 5,
// MOD_MELEE, along the view. TODO(spec): the impact effect of the centre trace (0x5295f0) and the hit event
// 0xad / 0xae.
void World::playerMelee(const Vec3& eye, const Vec3& viewAngles) {
    if (!player_ || !isAlive(*player_)) return;
    Vec3 f, r, u;
    angleVectors(viewAngles, f, r, u);
    const Value wn = player_->obj->get("$weapon");
    meleeBlow(*player_, eye, f, r, u, wn.isString() ? wn.asString() : std::string());
}

// The same blow for an AI (builtin melee: 0x53c690 -> 0x550c60 -> 0x530f80, range 64): from the actor's eye towards its enemy.
// TODO(spec): the actor aim origin and direction (0x54fee0, 0x500910; here the eye position and the line to the enemy's eye);
// the per-frame guard (actor +0xcc = level time) is the caller's.
void World::meleeBlow(Entity& attacker, const Vec3& eye, const Vec3& f, const Vec3& r, const Vec3& u, const std::string& w) {
    const int base = weaponInt(w, "meleeDamage");
    const float range = 64.0f, width = 10.0f, height = 10.0f;
    static const float offs[5][2] = {{0, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int n = width > 0 && height > 0 ? 5 : 1;
    for (int i = 0; i < n; ++i) {
        const float a = width * offs[i][0], b = height * offs[i][1];
        const Vec3 end = add(add(add(eye, scale(f, range)), scale(r, a)), scale(u, b));
        int location = 0;
        const TraceResult tr = locationalTrace(eye, end, Contents::BulletMask, &attacker, kHitPriorityDefault, location);
        // 0x530f37: the first ray (index 0) always hits the trigger_damage brushes along it with the base damage, MOD_MELEE,
        // whether or not it struck anything.
        if (i == 0) triggerDamageSegment(&attacker, eye, tr.endPos, base, 7);
        static const bool log = std::getenv("IW2_MELEE_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "[melee] trace %d from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f: fraction %.3f entity %d (%s) surf 0x%x\n", i, eye[0], eye[1],
                         eye[2], end[0], end[1], end[2], tr.fraction, tr.entity,
                         tr.entity >= 0 && entityByNumber(tr.entity) ? entityByNumber(tr.entity)->classname.c_str() : "-", tr.surfaceFlags);
        if ((tr.surfaceFlags & 0x10) || tr.fraction == 1.0f) continue;
        Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
        // The hit event of 0x530f80 (0x530fcf…0x531031), sent for every counted hit, the world included: 0xad when the hit
        // entity has a client or an actor (+0xfc / +0x100), else 0xae. The cgame plays "melee_hit" on the entity (0xad,
        // 0x4a34a0) or "melee_hit_other" at the position (0xae, 0x4a35c0).
        if (hit && (hit->isPlayer || hit->isActor)) startSound("melee_hit", hit->isPlayer ? nullptr : &tr.endPos);
        else startSound("melee_hit_other", &tr.endPos);
        if (hit) { // damage() applies the G_Damage (0x528050) takedamage check (ent+0x111)
            meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3; // the Q3 rand() at 0x5a56f4
            damage(*hit, &attacker, int((meleeSeed_ >> 0x11) % 5) + base, f, tr.endPos, "MOD_MELEE", hitLocationName(location));
        }
        return;
    }
}

// The AI melee builtin body: aim from the eye along the line to the enemy's eye (or the facing when there is none).
void World::aiMelee(Entity& a) {
    Entity* enemy = entityOf(a.obj->get("enemy"));
    const Vec3 eye = eyePosition(a);
    const Value ang = a.obj->get("angles");
    Vec3 f, r, u;
    angleVectors(ang.isVector() ? ang.asVector() : Vec3{0, 0, 0}, f, r, u);
    if (enemy) {
        const Vec3 d = Vec3{eyePosition(*enemy)[0] - eye[0], eyePosition(*enemy)[1] - eye[1], eyePosition(*enemy)[2] - eye[2]};
        const float l = len(d);
        if (l > 0) {
            f = scale(d, 1.0f / l);
            const float rl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
            r = rl > 0 ? Vec3{f[1] / rl, -f[0] / rl, 0} : Vec3{0, -1, 0};
            u = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        }
    }
    const Value wn = a.obj->get("weapon");
    meleeBlow(a, eye, f, r, u, wn.isString() ? wn.asString() : std::string());
}

// Event 200 (0x4e775d): a cooked grenade went off in the player's hand: 100000 damage to the player, MOD_SUICIDE.
// TODO(spec): it goes through the radius damage 0x527d80 (others nearby are hurt too).
void World::playerGrenadeInHand(const Vec3& eye) {
    // Skipped with god / demigod (flags & 3, 0x4e776b); otherwise health and ps+0x108 become 0 and player_die runs (0x4e778b..0x4e7791).
    if (player_ && isAlive(*player_) && !player_->obj->get("$godmode").truthy() && !player_->obj->get("$demigod").truthy())
        damage(*player_, player_, 100000, {0, 0, 0}, eye, "MOD_SUICIDE");
}

void World::notifyPlayer(const std::string& name) {
    if (player_ && player_->obj) vm_.notify(player_->obj, name);
}

const std::map<std::string, std::string>* World::binocularWeapon() {
    // The exe walks its weapon table in registration order; the shipped SP data has one binocular weapon, so the first loaded
    // definition with weaponType "binoculars" is the same one.
    for (const auto& [name, def] : weapons_) {
        if (!def) continue;
        const auto it = def->find("weapontype");
        if (it != def->end() && it->second == "binoculars") return &*def;
    }
    return nullptr;
}

} // namespace iw2::game
