// The game side of the player's weapon events (docs/spec/controls.md): the melee blow (0x531da0 → 0x530f80), the
// offhand grenade throw (0x531d60 → 0x531990) and the offhand pick (0x4dc6d0).

#include "game/world.h"
#include "game/hitboxes.h"

#include <algorithm>
#include <cmath>

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
void World::playerThrowGrenade(const std::string& weaponName, const Vec3& eye, const Vec3& viewAngles, const Vec3& playerVelocity) {
    if (!player_ || !isAlive(*player_)) return;
    Inventory& inv = inventory(*player_);
    int& clip = inv.clip[weaponField(weaponName, "clipName")];
    if (clip > 0) --clip;
    Vec3 f, r, u;
    angleVectors(viewAngles, f, r, u);
    const float speed = float(weaponInt(weaponName, "projectileSpeed"));
    const Vec3 v{speed * f[0], speed * f[1], speed * f[2] + float(weaponInt(weaponName, "projectileSpeedUp"))};
    const int fuseMs = int(std::lround(weaponFloat(weaponName, "fuseTime") * 1000.0f));
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
    const Value wn = player_->obj->get("$weapon");
    const std::string w = wn.isString() ? wn.asString() : std::string();
    const int base = weaponInt(w, "meleeDamage");
    Vec3 f, r, u;
    angleVectors(viewAngles, f, r, u);
    const float range = 64.0f, width = 10.0f, height = 10.0f;
    static const float offs[5][2] = {{0, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int n = width > 0 && height > 0 ? 5 : 1;
    for (int i = 0; i < n; ++i) {
        const float a = width * offs[i][0], b = height * offs[i][1];
        const Vec3 end = add(add(add(eye, scale(f, range)), scale(r, a)), scale(u, b));
        int location = 0;
        const TraceResult tr = locationalTrace(eye, end, Contents::BulletMask, player_, kHitPriorityDefault, location);
        if ((tr.surfaceFlags & 0x10) || tr.fraction == 1.0f) continue;
        Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
        if (hit && isAlive(*hit)) {
            meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3; // the Q3 rand() at 0x5a56f4
            damage(*hit, player_, int((meleeSeed_ >> 0x11) % 5) + base, f, tr.endPos, "MOD_MELEE", hitLocationName(location));
        }
        return;
    }
}

} // namespace iw2::game
