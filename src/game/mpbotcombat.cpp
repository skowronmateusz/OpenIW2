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
    if (c == "smg") return dist >= 450.0f; // from the hip an smg's spread is wider than a body beyond that
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

BotWeaponAction botWeaponAction(const BotWeaponIn& in) {
    const int heldRounds = in.heldClip + in.heldReserve, otherRounds = in.otherClip + in.otherReserve;
    const bool hasOther = !in.otherClass.empty();
    auto longRange = [](const std::string& c) { return c == "rifle" || c == "bolt" || c == "sniper" || c == "mg"; };
    auto closeRange = [](const std::string& c) { return c == "smg" || c == "shotgun" || c == "pistol"; };
    // Nothing left in the held weapon: the other one when it has a round.
    if (heldRounds == 0) return hasOther && otherRounds > 0 ? BotWeaponAction::Switch : BotWeaponAction::None;
    if (in.fighting) {
        if (in.heldClip == 0) {
            // An enemy close by: a loaded second weapon is faster than a reload of more than a second.
            if (hasOther && in.otherClip > 0 && in.enemyDist < 500.0f && in.heldReloadTime > 1.0f) return BotWeaponAction::Switch;
            return BotWeaponAction::Reload;
        }
        // Two loaded weapons: the one that fits the distance, at most every 3 s.
        if (hasOther && in.otherClip > 0 && in.sinceSwitchMs > 3000) {
            if (in.enemyDist < 300.0f && (in.heldClass == "bolt" || in.heldClass == "sniper") && closeRange(in.otherClass)) return BotWeaponAction::Switch;
            if (in.enemyDist > 1000.0f && closeRange(in.heldClass) && longRange(in.otherClass)) return BotWeaponAction::Switch;
        }
        return BotWeaponAction::None;
    }
    if (in.heldClip == 0) return BotWeaponAction::Reload;
    // Quiet for 2 s: back to the weapon the bot prefers.
    if (hasOther && !in.heldPreferred && otherRounds > 0 && in.quietMs > 2000 && in.sinceSwitchMs > 2000) return BotWeaponAction::Switch;
    // Tactical reload: under half a clip once the enemy is out of sight for 1.5 s, under 70% when nothing is known for 3 s.
    if (in.heldReserve > 0 && in.heldClip < in.heldClipSize) {
        if (in.enemyKnown ? (in.quietMs > 1500 && in.heldClip * 2 < in.heldClipSize) : (in.quietMs > 3000 && in.heldClip * 10 < in.heldClipSize * 7))
            return BotWeaponAction::Reload;
    }
    // Nothing known for 6 s and the held weapon full (or without reserve): the second weapon's empty clip is filled too
    // (the change, its automatic reload, and the change back by the rule above).
    if (hasOther && !in.enemyKnown && in.quietMs > 6000 && in.otherClip == 0 && in.otherReserve > 0 &&
        (in.heldClip >= in.heldClipSize || in.heldReserve == 0) && in.sinceSwitchMs > 2000)
        return BotWeaponAction::Switch;
    return BotWeaponAction::None;
}

bool botWantMelee(float eyeDist, float aimErrDeg, const std::string& c, int clip, bool busy) {
    if (eyeDist > 58.0f || aimErrDeg > 20.0f) return false;
    // Within reach a blow is surer than a shot from an empty, busy or slow weapon; with a loaded automatic the bot shoots.
    return clip == 0 || busy || c == "bolt" || c == "sniper" || c == "rifle" || c == "pistol" || eyeDist < 40.0f;
}

BotItemWant botItemWant(const BotItemIn& in) {
    BotItemWant w;
    if (in.itemClass.empty() || in.itemClass == "grenade") return w;
    if (in.owned) { // ammunition for a weapon the bot has
        if (in.ownedRounds == 0) w.desire = 0.9f;
        else if (in.ownedReserve * 2 < in.ownedMaxAmmo) w.desire = 0.6f;
        return w;
    }
    w.use = true;
    if (in.freeSlot) {
        w.desire = 0.8f;
        return w;
    }
    auto rank = [](const std::string& c) { return c == "mg" ? 4 : c == "smg" || c == "rifle" ? 3 : c == "pistol" ? 0 : c.empty() ? -1 : 2; };
    auto longRange = [](const std::string& c) { return c == "rifle" || c == "bolt" || c == "sniper" || c == "mg"; };
    // A slot weapon without a round is replaced first (with nothing left at all the bot takes anything).
    for (int i = 0; i < 2; ++i)
        if (in.slotRounds[i] == 0) {
            w.desire = in.slotRounds[1 - i] == 0 ? 1.0f : 0.9f;
            w.replace = i;
            return w;
        }
    // A pistol gives way to a second main weapon, sooner to one that covers the other distance.
    for (int i = 0; i < 2; ++i)
        if (in.slotClass[i] == "pistol" && in.itemClass != "pistol") {
            const std::string& main = in.slotClass[1 - i];
            w.desire = main != "pistol" && longRange(main) != longRange(in.itemClass) ? 0.7f : 0.5f;
            w.replace = i;
            return w;
        }
    // A main weapon down to less than a clip, or two classes worse than the item.
    int worst = rank(in.slotClass[0]) <= rank(in.slotClass[1]) ? 0 : 1;
    for (int i = 0; i < 2; ++i)
        if (in.slotRounds[i] < in.slotClipSize[i] && in.itemClass != "pistol") {
            w.desire = 0.6f;
            w.replace = i;
            return w;
        }
    if (rank(in.itemClass) >= rank(in.slotClass[worst]) + 2) {
        w.desire = 0.4f;
        w.replace = worst;
    }
    return w;
}

bool botWantRetreat(const BotRetreatIn& in) {
    if (in.skill <= 0 || !in.enemyKnown) return false; // a recruit stands and fights
    // Hurt in the last 3 s and down to 30 (regular), 40 (hardened) or 45 (veteran) health; a bold bot goes lower.
    const float limit = (in.skill == 1 ? 30.0f : in.skill == 2 ? 40.0f : 45.0f) * (2.0f - in.aggression);
    if (in.hurtMs < 3000 && float(in.health) <= limit) return true;
    // An empty clip with the enemy within 900 units: a hardened bot or a veteran reloads behind cover.
    return in.skill >= 2 && in.emptyClip && in.enemyDist < 900.0f;
}

bool botGrenadeAngles(const std::array<float, 3>& from, const std::array<float, 3>& to, float speed, float speedUp, float gravity, bool high,
                      float& yawDeg, float& pitchDeg, std::array<float, 3>& vel) {
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const float x = std::sqrt(dx * dx + dy * dy);
    if (speed <= 0 || gravity <= 0 || x < 1.0f) return false;
    // The height of the arc over the target for a view raised by e degrees: above it (> 0) or short of it (< 0).
    auto over = [&](float e) {
        const float r = e * 0.0174532925f, vh = speed * std::cos(r), vz = speed * std::sin(r) + speedUp;
        const float t = x / vh;
        return vz * t - 0.5f * gravity * t * t - dz;
    };
    // The low arc is the first elevation from below at which the arc clears the target, the high arc the last.
    float lo = 0, hi = 0;
    bool found = false;
    if (!high) {
        for (float e = -45.0f; e < 80.0f && !found; e += 1.0f)
            if (over(e) < 0 && over(e + 1.0f) >= 0) lo = e, hi = e + 1.0f, found = true;
    } else {
        for (float e = 85.0f; e > -45.0f && !found; e -= 1.0f) // a view looks up 85 degrees at most
            if (over(e) < 0 && over(e - 1.0f) >= 0) lo = e, hi = e - 1.0f, found = true;
    }
    if (!found) return false;
    for (int i = 0; i < 14; ++i) {
        const float mid = (lo + hi) * 0.5f;
        (over(mid) < 0 ? lo : hi) = mid;
    }
    const float e = (lo + hi) * 0.5f, r = e * 0.0174532925f;
    yawDeg = std::atan2(dy, dx) * 57.29578f;
    pitchDeg = -e;
    vel = {dx / x * speed * std::cos(r), dy / x * speed * std::cos(r), speed * std::sin(r) + speedUp};
    return true;
}

BotView botViewAngles(float fov43) {
    const float t = std::tan(std::clamp(fov43, 1.0f, 160.0f) * 0.5f * 0.0174532925f);
    return {std::atan(t * 4.0f / 3.0f) * 57.29578f, std::atan(t * 0.75f) * 57.29578f};
}

std::array<float, 2> botViewOffset(const std::array<float, 3>& d, float viewYawDeg, float viewPitchDeg) {
    const float yaw = std::atan2(d[1], d[0]) * 57.29578f;
    const float pitch = -std::atan2(d[2], std::sqrt(d[0] * d[0] + d[1] * d[1])) * 57.29578f;
    float dy = yaw - viewYawDeg;
    while (dy > 180.0f) dy -= 360.0f;
    while (dy < -180.0f) dy += 360.0f;
    return {dy, pitch - viewPitchDeg};
}

bool botInView(const BotView& v, const std::array<float, 3>& d, float viewYawDeg, float viewPitchDeg) {
    const auto o = botViewOffset(d, viewYawDeg, viewPitchDeg);
    return std::fabs(o[0]) <= v.halfYawDeg && std::fabs(o[1]) <= v.halfPitchDeg;
}

int botClassIndex(const std::string& c) {
    static const char* kNames[7] = {"rifle", "bolt", "sniper", "smg", "mg", "shotgun", "pistol"};
    for (int i = 0; i < 7; ++i)
        if (c == kNames[i]) return i;
    return -1;
}

// Damage to a client (from mpPlayerDamage / mpPlayerDie): the bots' counters and the damaged bot's reaction.

void World::mpBotOnDamage(Entity& target, Entity* attacker, const std::string& mod, bool died) {
    if (!bots_) return;
    const int tc = mpClientOf(target), ac = attacker ? mpClientOf(*attacker) : -1;
    for (MpBot& b : bots_->bots) {
        if (b.client == ac && ac != tc) {
            const bool grenade = mod.rfind("MOD_GRENADE", 0) == 0;
            // A team mate hit (team gametypes) is no hit on an enemy.
            const bool mate = mp_->gametype != "dm" && attacker && attacker->obj->get("sessionteam").toString() == target.obj->get("sessionteam").toString();
            if (died) {
                ++b.stats.kills, ++b.kills;
                // Where the kill fell: its weapon lies there (docs/spec/mp-bots.md "Weapons from the ground").
                if (target.obj->get("origin").isVector()) b.killPos = target.obj->get("origin").asVector(), b.killMs = timeMs_;
                if (mpClientOnTurret(b.client)) ++b.stats.turretKills;
            } else if (!mate) {
                // The damage of every hit comes before the death (mpPlayerDamage): a killing hit is counted once.
                if (grenade) ++b.stats.grenadeHits;
                else if (mod.find("BULLET") != std::string::npos) {
                    if (mpClientOnTurret(b.client)) ++b.stats.turretHits;
                    else if (b.firingShot) b.shotHit = true; // the bot's shot hit an enemy (mpBotFireShot counts it)
                } else if (mod == "MOD_MELEE") ++b.stats.meleeHits;
            }
        }
        if (b.client == tc) {
            if (died) {
                ++b.stats.deaths;
                continue;
            }
            // A hit (docs/spec/mp-bots.md "Hearing"): the bot dodges at once (the strafe flips); the hit shows where it came
            // from, so after a moment the attacker is known at a believed place (the direction within 8..15 degrees, the
            // distance within a fifth) and the bot turns to it. A bot that fights another enemy in sight turns to the attacker
            // on every second hit.
            const bool teammate = mp_->gametype != "dm" && attacker &&
                                  attacker->obj->get("sessionteam").toString() == target.obj->get("sessionteam").toString();
            if (attacker && ac >= 0 && ac != tc && !teammate && attacker->obj->get("origin").isVector())
                mpBotTurretDamaged(b, attacker->obj->get("origin").asVector());
            if (attacker && ac >= 0 && ac != tc && b.combat && !teammate && attacker->obj->get("origin").isVector()) {
                b.strafe = -b.strafe;
                b.strafeFlipMs = timeMs_ + 600 + nextRand() % 900;
                b.hurtMs = timeMs_;
                const bool seesIt = b.enemyVisible && b.enemy == attacker->number;
                const bool fightsOther = b.enemyVisible && b.enemy != attacker->number;
                if (!seesIt && !(fightsOther && (nextRand() & 1))) {
                    const float maxErr = 8.0f + float(3 - b.skill) * 2.3f;
                    const float err = (float(nextRand() % 2001) / 1000.0f - 1.0f) * maxErr;
                    const float scale = 0.8f + float(nextRand() % 401) / 1000.0f;
                    const int delay = 120 + int(float(botSkill(b.skill).reactionMs) * b.trait.reaction * 0.5f);
                    mpBotAlert(b, attacker->number, botHeardPosition(b.ps.origin, attacker->obj->get("origin").asVector(), err, scale), delay, BotCue::Hit);
                }
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
    const float speed = float(weaponInt(w, "projectileSpeed")), speedUp = float(weaponInt(w, "projectileSpeedUp"));
    const float g = gravity();
    constexpr uint32_t kGrenadeClipMask = 0x280e891; // the grenade's clip mask (docs/spec/grenades.md)
    for (int high = 0; high < 2; ++high) {
        std::array<float, 3> v{};
        float throwYaw = 0, throwPitch = 0;
        // The view angles of a player's throw that lands there (the grenade leaves along the view, docs/spec/grenades.md).
        if (!botGrenadeAngles(eye, target, speed, speedUp, g, high != 0, throwYaw, throwPitch, v)) continue;
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
        // killcam view): the bot turns its view to the throw's angles meanwhile, and at the throw event the grenade leaves
        // along its view as a player's (mpBotThrowPending).
        b.throwPending = {true, smoke, w, v, throwYaw, throwPitch, timeMs_};
        if (smoke) b.smokeUsed = true;
        return true;
    }
    return false;
}

// The weapon state machine's throw event: the grenade leaves the bot's eye along its view, as a player's throw (the
// client's throw of the weapon event, as for a network client); the fuse is the one the hand cooked, else the weapon's.
void World::mpBotThrowPending(MpBot& b, Entity& self, const Vec3& eye, int cookedFuseMs) {
    if (!b.throwPending.valid) return;
    const MpBot::PendingThrow t = std::exchange(b.throwPending, {});
    clientThrowGrenade(self, t.weapon, eye, Vec3{b.pitch, b.yaw, 0}, Vec3{b.ps.velocity[0], b.ps.velocity[1], b.ps.velocity[2]}, cookedFuseMs);
    if (t.smoke) ++b.stats.smokes;
    else ++b.stats.grenades;
}

} // namespace iw2::game
