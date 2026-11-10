// Multiplayer bot combat (docs/spec/mp-bots.md "Combat behaviour"): stance, aiming down the sights, bursts, grenades and
// the damage reaction. An OpenIW2 extension, implemented from the spec; the original's test clients never fight.
#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iw2::game {

using script::Value;

namespace {
std::string lowerOf(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

Stance botChooseStance(const BotStanceIn& in) {
    if (!in.fighting || in.moving) return Stance::Stand;
    const std::string& c = in.weaponClass;
    if (c == "sniper" && in.dist >= 1000.0f) return Stance::Prone;
    if (in.coverCrouch) return in.holdFire ? Stance::Crouch : Stance::Stand;
    if (in.dist >= 800.0f && (c == "rifle" || c == "bolt" || c == "sniper" || c == "mg")) return Stance::Crouch;
    return Stance::Stand;
}

bool botWantAds(const std::string& c, float dist) {
    if (c == "sniper") return dist >= 400.0f;
    if (c == "rifle" || c == "bolt" || c == "mg") return dist >= 500.0f;
    if (c == "smg") return dist >= 900.0f;
    return false; // shotgun, pistol
}

BotBurst botBurst(const std::string& c, float dist, int skillBurst, int skillPauseMs) {
    if (c == "smg" || c == "mg") {
        if (dist < 600.0f) return {std::max(1, skillBurst), skillPauseMs};
        return {std::max(2, skillBurst / 2), skillPauseMs * 3 / 2};
    }
    if (c == "pistol") return {3, skillPauseMs};
    return {1, 0}; // rifle, sniper, shotgun: single shots at the weapon's fire time
}

bool botGrenadeVelocity(const std::array<float, 3>& from, const std::array<float, 3>& to, float speed, float gravity, bool high,
                        std::array<float, 3>& vel) {
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const float x = std::sqrt(dx * dx + dy * dy);
    if (speed <= 0 || gravity <= 0 || x < 1.0f) return false;
    const float v2 = speed * speed;
    const float disc = v2 * v2 - gravity * (gravity * x * x + 2.0f * dz * v2);
    if (disc < 0) return false;
    const float r = std::sqrt(disc);
    const float tanA = (v2 + (high ? r : -r)) / (gravity * x);
    const float a = std::atan(tanA);
    const float h = std::cos(a) * speed;
    vel = {dx / x * h, dy / x * h, std::sin(a) * speed};
    return true;
}

// Damage to a client (from mpPlayerDamage / mpPlayerDie): the bots' counters and the damaged bot's reaction.
void World::mpBotOnDamage(Entity& target, Entity* attacker, const std::string& mod, bool died) {
    if (!bots_) return;
    const int tc = mpClientOf(target), ac = attacker ? mpClientOf(*attacker) : -1;
    for (MpBot& b : bots_->bots) {
        if (b.client == ac && ac != tc) {
            const bool grenade = mod.rfind("MOD_GRENADE", 0) == 0;
            if (died) ++b.stats.kills, ++b.kills;
            else if (grenade) ++b.stats.grenadeHits;
            else if (mod.find("BULLET") != std::string::npos) ++b.stats.hits;
        }
        if (b.client == tc) {
            if (died) {
                ++b.stats.deaths;
                continue;
            }
            // The attacker becomes known (its position is the last seen one) and the bot dodges: the strafe flips.
            const bool teammate = mp_->gametype != "dm" && attacker &&
                                  attacker->obj->get("sessionteam").toString() == target.obj->get("sessionteam").toString();
            if (attacker && ac >= 0 && ac != tc && b.combat && !teammate) {
                b.enemy = attacker->number;
                b.enemyLastPos = attacker->obj->get("origin").isVector() ? attacker->obj->get("origin").asVector() : b.enemyLastPos;
                b.enemySeenMs = timeMs_;
                b.strafe = -b.strafe;
                b.strafeFlipMs = timeMs_ + 600 + nextRand() % 900;
            }
        }
    }
}

// The frag or smoke grenade the bot owns with ammunition left ("" none).
std::string World::mpBotGrenadeWeapon(Entity& self, bool smoke) {
    Inventory& inv = inventory(self);
    for (const std::string& w : inv.owned) {
        const std::string oc = lowerOf(weaponField(w, "offhandClass"));
        if (oc.empty() || oc == "none") continue;
        if ((oc == "smoke grenade") != smoke) continue;
        if (inv.clip[weaponField(w, "clipName")] + inv.ammo[weaponField(w, "ammoName")] > 0) return w;
    }
    return {};
}

// Throws the bot's frag (or smoke) grenade at `target` when an arc reaches it (docs/spec/mp-bots.md "Grenades").
bool World::mpBotGrenade(MpBot& b, Entity& self, const Vec3& eye, bool smoke, const Vec3& target) {
    const std::string w = mpBotGrenadeWeapon(self, smoke);
    if (w.empty()) return false;
    const float speed = float(weaponInt(w, "projectileSpeed"));
    const float g = gravity();
    constexpr uint32_t kGrenadeClipMask = 0x280e891; // the grenade's clip mask (docs/spec/grenades.md)
    for (int high = 0; high < 2; ++high) {
        std::array<float, 3> v{};
        if (!botGrenadeVelocity(eye, target, speed, g, high != 0, v)) continue;
        // Trace the arc in 50 ms steps: nothing may be hit more than 120 units before the target.
        Vec3 p = eye;
        bool ok = false;
        for (int i = 1; i <= 100; ++i) {
            const float t = float(i) * 0.05f;
            const Vec3 q{eye[0] + v[0] * t, eye[1] + v[1] * t, eye[2] + v[2] * t - 0.5f * g * t * t};
            const TraceResult tr = collision_->trace(p, q, {-2, -2, -2}, {2, 2, 2}, kGrenadeClipMask, self.number);
            const Vec3 at = tr.fraction < 1.0f ? Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]} : q;
            const float dx = at[0] - target[0], dy = at[1] - target[1], dz = at[2] - target[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (tr.fraction < 1.0f || tr.startSolid) {
                ok = d < 120.0f;
                break;
            }
            if (d < 64.0f) {
                ok = true;
                break;
            }
            if (q[2] < target[2] - 256.0f) break;
            p = q;
        }
        if (!ok) continue;
        // The throw goes through the weapon state machine's offhand states (the pull-back and throw animations of the
        // killcam view): the solved velocity waits for the throw event (mpBotThrowPending).
        b.throwPending = {true, smoke, w, v};
        if (smoke) b.smokeUsed = true;
        b.yaw = botTurn(b.yaw, std::atan2(v[1], v[0]) * 57.29578f, botSkill(b.skill).turnRate * 0.05f); // the bot turns to face the throw
        return true;
    }
    return false;
}

// The weapon state machine's throw event: the pending grenade leaves the bot's eye with the solved velocity; the fuse is
// the one the hand cooked (the machine's fuse field, as a player's throw takes it), else the weapon's.
void World::mpBotThrowPending(MpBot& b, Entity& self, const Vec3& eye, int cookedFuseMs) {
    if (!b.throwPending.valid) return;
    const MpBot::PendingThrow t = std::exchange(b.throwPending, {});
    Inventory& inv = inventory(self);
    int& clip = inv.clip[weaponField(t.weapon, "clipName")];
    if (clip > 0) --clip;
    else if (inv.ammo[weaponField(t.weapon, "ammoName")] > 0) --inv.ammo[weaponField(t.weapon, "ammoName")];
    const int fuseMs = cookedFuseMs > 0 ? cookedFuseMs : int(std::lround(weaponFloat(t.weapon, "fuseTime") * 1000.0f));
    spawnGrenade(self, t.weapon, eye, Vec3{t.velocity[0], t.velocity[1], t.velocity[2]}, fuseMs);
    if (t.smoke) ++b.stats.smokes;
    else ++b.stats.grenades;
}

} // namespace iw2::game
