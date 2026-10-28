// Grenade missiles and the throw built-ins (docs/spec/grenades.md): the grenade entity (0x5275f0), its flight,
// bounces and fuse (G_RunMissile 0x526c30, 0x525f30, 0x526600), the radius damage (0x5289d0), the landing prediction
// (0x527270), the throw solvers behind checkGrenadeThrowPos (0x51f490) and throwGrenade / MagicGrenade(Manual) /
// pickUpGrenade.
#include "game/grenadeangles.h"
#include "game/itemslide.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace iw2::game {

using namespace script;

namespace {

constexpr uint32_t kGrenadeClip = 0x280e891; // clipmask set by 0x5275f0 (+0x12c)
constexpr float kMaxThrowSpeedSq = 810000.0f; // 0x56fe2c

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len(const Vec3& a) { return std::sqrt(dot(a, a)); }

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

// 0x4e7010: the enemy team of a team (table {0, 2, 1, 0, 0}).
int enemyTeam(int team) { return team == 1 ? 2 : team == 2 ? 1 : 0; }

// Surface type of a trace (surface flags >> 20 & 0x1f) → the weapon file's bounce key suffix (0x3a0 / 0x3fc tables).
const char* const kSurfaces[] = {"Default", "Bark", "Brick", "Carpet", "Cloth", "Concrete", "Dirt", "Flesh",
                                 "Foliage", "Glass", "Grass", "Gravel", "Ice", "Metal", "Mud", "Paper",
                                 "Plaster", "Rock", "Sand", "Snow", "Water", "Wood", "Asphalt"};

std::string lowerStr(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// projExplosionType (weapon +0x390) through the enum table 0x5a7e1c: "grenade" 0, "rocket" 1, "none" 2.
int explosionType(const std::string& v) {
    if (v.empty() || v == "grenade") return 0;
    if (v == "rocket") return 1;
    return 2;
}

} // namespace

float World::gravity() {
    auto it = cvars.find("g_gravity");
    return it != cvars.end() && !it->second.empty() ? float(std::atof(it->second.c_str())) : 800.0f;
}

float World::weaponFloat(const std::string& weaponName, const char* key) {
    return float(std::atof(weaponField(weaponName, key).c_str()));
}

// BG_EvaluateTrajectory (0x4fae80) for the two kinds a grenade uses: stationary, and gravity (z − 400·t²).
Vec3 World::grenadePosition(const GrenadeState& g, int64_t atMs) const {
    if (g.trType != 5) return g.trBase;
    const float t = float(atMs - g.trTime) * 0.001f;
    return {g.trBase[0] + g.trDelta[0] * t, g.trBase[1] + g.trDelta[1] * t, g.trBase[2] + g.trDelta[2] * t - 400.0f * t * t};
}

// BG_EvaluateTrajectory type 2 (0x4fae80) for the grenade's angular trajectory: base + delta · t.
Vec3 World::grenadeAngles(const GrenadeState& g, int64_t atMs) const {
    const float t = float(atMs - g.aTime) * 0.001f;
    return {g.aBase[0] + g.aDelta[0] * t, g.aBase[1] + g.aDelta[1] * t, g.aBase[2] + g.aDelta[2] * t};
}

Vec3 World::grenadeVelocity(const GrenadeState& g, int64_t atMs) const {
    if (g.trType != 5) return {0, 0, 0};
    const float t = float(atMs - g.trTime) * 0.001f;
    return {g.trDelta[0], g.trDelta[1], g.trDelta[2] - 800.0f * t};
}

// The grenade entity (0x5275f0): classname "grenade", fuse now + fuseMs, thrown by `thrower` (+0x134, owner +0xf4),
// gravity trajectory from `start` with `velocity`, bouncing (flags 0x1000000), the weapon's projectile model.
// The fuse a player cooked (client +0x3c) is passed in by playerThrowGrenade as fuseMs (0x527605).
Entity& World::spawnGrenade(Entity& thrower, const std::string& weaponName, const Vec3& start, const Vec3& velocity, int fuseMs) {
    Entity& e = spawnEntity("grenade");
    e.grenade = std::make_unique<GrenadeState>();
    GrenadeState& g = *e.grenade;
    g.weapon = weaponName;
    g.parent = &thrower;
    g.explodeMs = timeMs_ + fuseMs;
    g.trType = 5;
    g.trTime = timeMs_;
    g.trBase = start;
    g.trDelta = velocity;
    e.obj->set("origin", Value(start));
    // Angular trajectory (0x5275f0): base angles = vectoangles(velocity) with the pitch replaced by AngleMod(pitch − 120); linear
    // spin per second: pitch r1/32768·90 + 675, yaw 0, roll r2/32768·90 + 315 (r = the game's rand() draws, 0x5a56f4, pitch
    // first). TODO(spec): the AngleMod/vectoangles details beyond the current evidence.
    {
        const float hyp = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
        const float yaw = std::atan2(velocity[1], velocity[0]) * 57.29578f;
        const float pitch = -std::atan2(velocity[2], hyp) * 57.29578f;
        const float a = pitch - 120.0f;
        const float pm = (360.0f / 65536.0f) * float(int(a * (65536.0f / 360.0f)) & 65535);
        const float r1 = float(nextRand());
        const float r2 = float(nextRand());
        g.aBase = {pm, yaw, 0};
        g.aDelta = {r1 / 32768.0f * 90.0f + 675.0f, 0, r2 / 32768.0f * 90.0f + 315.0f};
        g.aTime = timeMs_;
        e.obj->set("angles", Value(g.aBase));
    }
    const std::string model = weaponField(weaponName, "projectileModel");
    if (!model.empty()) e.obj->set("model", Value(model));
    grenades_.push_back(&e);
    ++stats.grenades;
    return e;
}

// Bounce (0x525f30): reflect the velocity at the hit time off the surface, scaled for bouncing grenades by the
// surface's bounce factor, interpolated from the parallel to the perpendicular factor by the impact angle. Resting on
// ground (normal z > 0.7) slower than 20 units/s stops it; otherwise it moves 0.1 along the normal (never upwards)
// and flies on. Returns true for a bounce that changed the velocity by more than 100 (the bounce event).
// Angular trajectory: grenadeRebaseAngles (0x525cb0). A flying bounce off a character re-plans the actors handling the
// grenade (0x520230, grenadeHitCharacter).
bool World::bounceGrenade(Entity& e, const TraceResult& tr, int64_t hitMs) {
    GrenadeState& g = *e.grenade;
    const Vec3 v = grenadeVelocity(g, hitMs);
    const Vec3& n = tr.normal;
    const float d = dot(v, n);
    Vec3 nv = add(v, scale(n, -2.0f * d));
    if (g.bounce) {
        const float speed = len(v);
        const int surf = int(tr.surfaceFlags >> 20 & 0x1f);
        const char* name = surf < 23 ? kSurfaces[surf] : "Default";
        const float par = weaponFloat(g.weapon, ("parallel" + std::string(name) + "Bounce").c_str());
        const float perp = weaponFloat(g.weapon, ("perpendicular" + std::string(name) + "Bounce").c_str());
        if (speed > 0.0f && d <= 0.0f) nv = scale(nv, -1.0f / speed * d * (perp - par) + par);
        if (n[2] > 0.7f && len(nv) < 20.0f) {
            // Rest branch (0x5260d2): 0x525cb0 aligns the pitch with the slope; 0x4d83c0 then makes the angular trajectory
            // stationary (type/time/duration/delta 0) with base = currentAngles = those angles.
            const GrenadeRebase rb = grenadeRebaseAngles(grenadeAngles(g, hitMs), g.aDelta[0], n, true, [&] { return nextRand(); });
            g.trType = 0;
            g.trBase = tr.endPos;
            g.trDelta = {0, 0, 0};
            g.onGround = true;
            g.aBase = rb.angles;
            g.aDelta = {0, 0, 0};
            g.aTime = 0;
            e.obj->set("angles", Value(g.aBase));
            e.obj->set("origin", Value(tr.endPos));
            return false;
        }
    }
    // Flying branch (0x52615e): re-base the angular trajectory at the hit angles, stamped with the current level time.
    const GrenadeRebase rb = grenadeRebaseAngles(grenadeAngles(g, hitMs), g.aDelta[0], n, false, [&] { return nextRand(); });
    g.aBase = rb.angles;
    g.aDelta[0] = rb.pitchSpin;
    g.aTime = timeMs_;
    const Vec3 p = add(tr.endPos, {n[0] * 0.1f, n[1] * 0.1f, std::min(n[2] * 0.1f, 0.0f)});
    g.trType = 5;
    g.trBase = p;
    g.trTime = timeMs_;
    g.trDelta = nv;
    e.obj->set("origin", Value(p));
    grenadeHitCharacter(e, tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr); // 0x526198
    return tr.entity < 0 && len(sub(nv, v)) > 100.0f;
}

// Radius damage (0x5289d0): every entity that takes damage (ent+0x111) but `ignore` within `radius` of `origin` (distance to its origin;
// brush models: to their bounds) that the explosion can reach (0x5285b0) takes inner + (outer − inner)·d/r, pushed
// along (target − origin + (0, 0, 24)). Entities with a client (the player) are spared while the script radiusdamage
// runs with setplayerignoreradiusdamage(true) (0x528b8c: 0x1c61054). TODO(spec): the bounds of brush models and the
// knockback.
void World::radiusDamage(const Vec3& origin, Entity* attacker, float inner, float outer, float radius, Entity* ignore,
                         const std::string& mod) {
    if (radius < 1.0f) radius = 1.0f;
    std::vector<Entity*> hit;
    for (const auto& ep : entities_) {
        Entity& t = *ep;
        if (&t == ignore || !takesDamage(t)) continue;
        if (t.isPlayer && sparePlayerRadiusDamage_) continue;
        const float dist = len(sub(originOf(t.obj), origin));
        if (dist >= radius) continue;
        if (!canDamage(t, origin, radius)) continue;
        hit.push_back(&t);
    }
    for (Entity* t : hit) {
        const float dist = len(sub(originOf(t->obj), origin));
        const int amount = int((1.0f - dist / radius) * (inner - outer) + outer);
        const Vec3 dir = add(sub(originOf(t->obj), origin), {0, 0, 24.0f});
        damage(*t, attacker, amount, dir, originOf(t->obj), mod);
    }
}

// 0x5285b0: within `radius` of `from`, and a trace from `from` reaches the entity's eye (sentients) or origin, or one
// of the four points offset ±15 horizontally from it (five tries). The offsets follow the five-try loop; the tested
// points themselves are inferred (TODO(spec): 0x4db890's targets and mask).
bool World::canDamage(Entity& t, const Vec3& from, float radius, const Vec3* at) {
    const Vec3 o = at ? *at : originOf(t.obj);
    if (dot(sub(o, from), sub(o, from)) > radius * radius) return false;
    Vec3 base = o;
    if (t.isActor || t.isPlayer) base = add(o, sub(t.isActor ? sightEye(t, nullptr) : eyePosition(t), originOf(t.obj)));
    static const float offs[5][2] = {{0, 0}, {15, 15}, {15, -15}, {-15, 15}, {-15, -15}};
    for (const auto& of : offs) {
        const Vec3 p{base[0] + of[0], base[1] + of[1], base[2]};
        if (!collision_->sightBlocked(from, p, {0, 0, 0}, {0, 0, 0}, Contents::BulletMaskNoCharacters)) return true;
    }
    return false;
}

// Explode (think 0x526600 → 0x526200): a grenade held in an actor's hand goes off at its tag_weapon_right; a smoke
// grenade (offhandClass "Smoke Grenade") still in the air waits 50 ms. Radius damage from the thrower
// (explosionInnerDamage / OuterDamage / Radius, MOD_GRENADE_SPLASH), the explosion effect and sound, then actors
// forget it (0x520380) and the entity goes away.
void World::explodeGrenade(Entity& e) {
    GrenadeState& g = *e.grenade;
    if (lowerStr(weaponField(g.weapon, "offhandClass")) == "smoke grenade" && !g.onGround) {
        g.explodeMs = timeMs_ + 50;
        return;
    }
    Vec3 at = grenadePosition(g, timeMs_);
    if (g.holder && g.inHand) {
        const auto bones = entityBones(*g.holder);
        auto it = bones.find("tag_weapon_right");
        if (it != bones.end()) at = it->second.position;
        g.inHand = false;
    }
    e.obj->set("origin", Value(at));
    ++stats.grenadeExplosions;
    // 0x5267d3: radius damage only when explosionInnerDamage (+0x37c) is not 0 (the moscow potato has 0).
    const float radius = weaponFloat(g.weapon, "explosionRadius");
    if (weaponFloat(g.weapon, "explosionInnerDamage") != 0.0f)
        radiusDamage(at, g.parent, weaponFloat(g.weapon, "explosionInnerDamage"), weaponFloat(g.weapon, "explosionOuterDamage"),
                     radius, nullptr, "MOD_GRENADE_SPLASH");
    // The explode event (0x526700 → 0x4d8020, EV_GRENADE_EXPLODE 0xbf for projExplosionType grenade): a point trace
    // 16 down from the grenade (mask 0x811, skipping it) gives the direction (its normal, sent as a dir byte) and the
    // surface type (surface flags >> 20 & 0x1f; 20 = water when the point is in water, 0x44c840 mask 0x20). The client
    // (0x4ba8d0 case 0xbf) plays grenade_explode_<surface> (sounds 0xf5b7a4, built by 0x4a27b0 as "%s_%s") and the
    // impact table's grenade_explode effect for the surface along the direction, then the weapon's own
    // projExplosionEffect and projExplosionSound. A trace that hits nothing has a zero normal, which the dir byte
    // (0x420c30) turns into bytedirs[0] (0x59ab10). TODO(spec): rocket / custom explosion events (0xc0–0xc3).
    {
        const TraceResult tr = collision_->trace(at, {at[0], at[1], at[2] - 16.0f}, {0, 0, 0}, {0, 0, 0}, 0x811, e.number);
        const bool water = collision_->trace(at, at, {0, 0, 0}, {0, 0, 0}, 0x20).startSolid;
        const int surface = water ? 20 : int((tr.surfaceFlags >> 20) & 0x1f);
        Vec3 dir{tr.normal[0], tr.normal[1], tr.normal[2]};
        if (tr.fraction == 1.0f) dir = {-0.525731f, 0.0f, 0.850651f};
        static const char* const kSurfaceAlias[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                                    "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                                    "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
        // 0x526763: the event follows projExplosionType (+0x390, enum table 0x5a7e1c: grenade 0, rocket 1, none 2):
        // 0 → 0xbf (grenade_explode_<surface> + impact type 9), 1 → 0xc0 (rocket_explode_<surface> + impact type 10),
        // anything else → 0xc2 (no surface sound or impact; only projExplosionEffect / projExplosionSound below).
        const int type = explosionType(lowerStr(weaponField(g.weapon, "projExplosionType")));
        if (type < 2 && surface < 23) {
            startSound(std::string(type == 0 ? "grenade_explode_" : "rocket_explode_") + kSurfaceAlias[surface], &at);
            const std::string& impact = impactEffect(type == 0 ? 9 : 10, surface);
            if (!impact.empty()) fxEvents_.push_back({impact, at, dir});
            if (std::getenv("IW2_GRENADE_DEBUG"))
                std::printf("grenade %s explodes at (%g %g %g), surface %s, effect %s\n", g.weapon.c_str(), at[0], at[1], at[2],
                            kSurfaceAlias[surface], impact.c_str());
        }
    }
    const std::string fx = weaponField(g.weapon, "projExplosionEffect");
    if (!fx.empty()) fxEvents_.push_back({fx, at, Vec3{0, 0, 1}});
    const std::string snd = weaponField(g.weapon, "projExplosionSound");
    if (!snd.empty()) startSound(snd, &at);
    forgetGrenade(e);
    e.obj->removed = true;
}

// 0x520380: actors that know this grenade forget it; nobody goes for it any more.
void World::forgetGrenade(Entity& g) {
    for (const auto& ep : entities_)
        if (ep->isActor && ep->ai.grenade == &g) {
            ep->ai.grenade = nullptr;
            ep->obj->set("grenade", Value());
        }
    g.grenade->holder = nullptr;
}

// G_RunMissile (0x526c30) for grenades: the grenade_ping event every 500 ms (0x526bd0), a resting grenade that lost
// its ground starts falling, a flying one moves along its trajectory with a point trace (clipmask 0x280e891, the
// thrower passes) and bounces off what it hits, or is removed when it hits the sky (surface flag 0x10). Then the fuse.
// Water entry: missileTrace (0x526b40). TODO(spec): the 1.5-unit ground settle and the in-flight rotation. 0x5297b0 is not a whizby
// but the trigger_damage segment test (below).
void World::runGrenade(Entity& e) {
    GrenadeState& g = *e.grenade;
    if (timeMs_ - g.pingMs > 499) {
        grenadePing(e);
        g.pingMs = timeMs_;
    }
    if (!g.inHand) {
        if (g.trType != 0) e.obj->set("angles", Value(grenadeAngles(g, timeMs_)));
        Vec3 o = originOf(e.obj);
        const int pass = g.parent ? g.parent->number : -1;
        if (g.trType == 0) {
            const TraceResult tr = collision_->trace(o, {o[0], o[1], o[2] - 1.5f}, {0, 0, 0}, {0, 0, 0}, kGrenadeClip, pass);
            if (tr.fraction == 1.0f) {
                g.trType = 5;
                g.trTime = timeMs_;
                g.trBase = o;
                g.trDelta = {0, 0, 0};
                g.onGround = false;
            }
        }
        if (g.trType == 5) {
            const Vec3 to = grenadePosition(g, timeMs_);
            const TraceResult tr = missileTrace(o, to, kGrenadeClip, pass, g.trDelta);
            e.obj->set("origin", Value(tr.endPos));
            // 0x526f90..0x526fbb: missiles of handler type 3 (grenades, table 0x5a5754) pass the segment from the origin before
            // the move to the new origin to 0x5297b0, the missile twin of 0x5295f0: every trigger_damage on it gets "damage"
            // (explosionInnerDamage +0x37c, attacker = the grenade itself) and the action with MOD_GRENADE (3), before the
            // impact is handled. TODO(spec): the z adjustments of 0x526e94..0x526f84 to the end point and the +0x128 & 0x8000
            // hit-entity call 0x5268b0 (0x526e01..0x526e23).
            triggerDamageSegment(&e, o, {tr.endPos[0], tr.endPos[1], tr.endPos[2]}, weaponInt(g.weapon, "explosionInnerDamage"), 3);
            if (e.obj->removed) return;
            if (tr.fraction < 1.0f) {
                if (tr.surfaceFlags & 0x10) {
                    forgetGrenade(e);
                    e.obj->removed = true;
                    return;
                }
                const int64_t hitMs = timeMs_ - 50 + int64_t(50.0f * tr.fraction);
                if (bounceGrenade(e, tr, hitMs) && !tr.startSolid) {
                    // The bounce event (0x5262bf): EV_POTATO_BOUNCE 0xbd for the weapon "potato", else
                    // EV_GRENADE_BOUNCE 0xbe, with the surface type (surface flags >> 20 & 0x1f). The client
                    // (0x4ba8d0 case 0xbd/0xbe) plays <potato|grenade>_bounce_<surface> (sounds built at 0x4a28c5 /
                    // 0x4a28dc by 0x4a27b0) and the impact table's grenade_bounce effect (type 8) along the normal.
                    static const char* const kSurf[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                                        "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                                        "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
                    const int surface = int((tr.surfaceFlags >> 20) & 0x1f);
                    if (surface < 23) {
                        const Vec3 at = grenadePosition(g, hitMs);
                        startSound(std::string(lowerStr(g.weapon) == "potato" ? "potato_bounce_" : "grenade_bounce_") + kSurf[surface], &at);
                        const std::string& fx = impactEffect(8, surface);
                        if (!fx.empty()) fxEvents_.push_back({fx, at, Vec3{tr.normal[0], tr.normal[1], tr.normal[2]}});
                    }
                }
            } else if (len(g.trDelta) != 0.0f) {
                g.onGround = false;
            }
        }
    }
    if (timeMs_ >= g.explodeMs) explodeGrenade(e);
}

// Landing prediction (0x527270 via 0x51f7c0): from now in 50 ms steps until the fuse, the trajectory with the same
// bounces (0x527080) on a copy; the rest point and its time, or the position when the fuse runs out. Hitting the sky
// gives no prediction (the current origin is used).
void World::predictGrenadeLanding(Entity& e) {
    GrenadeState& g = *e.grenade;
    if (g.landing[0] != 0.0f || g.landing[1] != 0.0f || g.landing[2] != 0.0f) return;
    GrenadeState s = g;
    Vec3 o = originOf(e.obj);
    const int pass = g.parent ? g.parent->number : -1;
    int64_t t = timeMs_ + 50;
    g.landingMs = g.explodeMs;
    bool ok = true;
    for (; t < g.explodeMs && s.trType == 5; t += 50) {
        const Vec3 to = grenadePosition(s, t);
        const TraceResult tr = collision_->trace(o, to, {0, 0, 0}, {0, 0, 0}, kGrenadeClip, pass);
        o = tr.endPos;
        if (tr.fraction < 1.0f) {
            if (tr.surfaceFlags & 0x10) {
                ok = false;
                break;
            }
            const Vec3 v = grenadeVelocity(s, t - 50 + int64_t(50.0f * tr.fraction));
            const Vec3& n = tr.normal;
            const float d = dot(v, n);
            Vec3 nv = add(v, scale(n, -2.0f * d));
            if (s.bounce) {
                const float speed = len(v);
                const int surf = int(tr.surfaceFlags >> 20 & 0x1f);
                const char* name = surf < 23 ? kSurfaces[surf] : "Default";
                const float par = weaponFloat(s.weapon, ("parallel" + std::string(name) + "Bounce").c_str());
                const float perp = weaponFloat(s.weapon, ("perpendicular" + std::string(name) + "Bounce").c_str());
                if (speed > 0.0f && d <= 0.0f) nv = scale(nv, -1.0f / speed * d * (perp - par) + par);
                if (n[2] > 0.7f && len(nv) < 20.0f) {
                    s.trType = 0;
                    g.landingMs = t;
                    break;
                }
            }
            s.trBase = add(o, {n[0] * 0.1f, n[1] * 0.1f, std::min(n[2] * 0.1f, 0.0f)});
            s.trTime = t;
            s.trDelta = nv;
            o = s.trBase;
        }
    }
    g.landing = ok ? o : originOf(e.obj);
}

// The throw solvers (0x51efb0 min energy, 0x51ee30 min / max time, 0x51ef20 infinite energy): the launch velocity
// from `start` to `aim` under gravity g; min energy needs (dz + |d|)·g ≤ 810000 (the speed limit²), the time
// variants solve with that speed (the smaller or larger root), infinite energy flies |d| at ·g. The arc is then
// checked (0x51e710).
bool World::solveThrow(Entity& actor, const std::string& method, const Vec3& start, const Vec3& aim, Vec3& vel) {
    const float g = gravity();
    const Vec3 d = sub(aim, start);
    float t;
    if (method == "min energy") {
        const float l = len(d);
        if ((d[2] + l) * g > kMaxThrowSpeedSq) return false;
        t = std::sqrt(2.0f * (l / g));
    } else if (method == "min time" || method == "max time") {
        const float l2 = dot(d, d);
        if (l2 < 1.0f) return false;
        const float b = kMaxThrowSpeedSq - d[2] * g;
        const float disc = b * b - g * g * l2;
        if (disc < 0.0f) return false;
        const float root = method == "min time" ? b - std::sqrt(disc) : std::sqrt(disc) + b;
        t = std::sqrt(root + root) / g;
    } else if (method == "infinite energy") {
        const float l = len(d) * g;
        t = std::sqrt(l + l) / g;
    } else {
        throw std::runtime_error("checkGrenadeThrow: method must be 'min energy', 'min time', or 'max time' - value passed in was '" + method + "'");
    }
    if (!(t > 0.0f)) return false;
    vel = {d[0] / t, d[1] / t, t * g * 0.5f + d[2] / t};
    return throwArcClear(actor, start, vel, aim);
}

// 0x51e710: the arc from `start` with `vel` must reach `target` (within 0.1 horizontally at the computed time) and be
// clear (mask 0x280e891, the thrower passes): start → the point at half the rise time → the apex, then on to the point
// halfway down, each segment as a box as large as the arc's sag over it (g·t²/8 + 1); the last stretch to the target
// with a 2-unit box may end within 4 units of it on the world or on a sentient of the enemy team.
bool World::throwArcClear(Entity& actor, const Vec3& start, const Vec3& vel, const Vec3& target) {
    const float g = gravity();
    float rise = 0.0f, half = 0.0f, fall;
    if (vel[2] <= 0.0f) {
        const float disc = vel[2] * vel[2] + 2.0f * (start[2] - target[2]) * g;
        if (disc < 0.0f) return false;
        fall = (std::sqrt(disc) + vel[2]) / g;
    } else {
        rise = vel[2] / g;
        half = rise * 0.5f;
        const float disc = rise * rise + 2.0f * (start[2] - target[2]) / g;
        if (disc < 0.0f) return false;
        fall = std::sqrt(disc);
    }
    fall *= 0.5f;
    const float mid = fall + rise;
    if (std::fabs((mid + fall) * vel[0] + start[0] - target[0]) > 0.1f || std::fabs((mid + fall) * vel[1] + start[1] - target[1]) > 0.1f)
        return false;
    auto clear = [&](const Vec3& a, const Vec3& b, float ext) {
        return !collision_->sightBlocked(a, b, {-ext, -ext, -ext}, {ext, ext, ext}, kGrenadeClip, actor.number);
    };
    Vec3 top = start;
    if (rise > 0.0f) {
        const float s = half * half * g;
        const Vec3 a{half * vel[0] + start[0], half * vel[1] + start[1], s * 1.5f + start[2]};
        if (!clear(start, a, 0.125f * s + 1.0f)) return false;
        top = {rise * vel[0] + start[0], rise * vel[1] + start[1], rise * g * rise * 0.5f + start[2]};
        if (!clear(a, top, 0.125f * s + 1.0f)) return false;
    }
    const Vec3 down{mid * vel[0] + start[0], mid * vel[1] + start[1], (vel[2] - mid * g * 0.5f) * mid + start[2]};
    if (!clear(top, down, fall * fall * g * 0.125f + 1.0f)) return false;
    const TraceResult tr = collision_->trace(down, target, {-1, -1, -1}, {1, 1, 1}, kGrenadeClip, actor.number);
    if (tr.fraction == 1.0f) return true;
    if (tr.entity < 0) return len(sub(tr.endPos, target)) < 4.0f;
    Entity* hit = entityByNumber(tr.entity);
    return hit && (hit->isActor || hit->isPlayer) && teamOf(*hit) == enemyTeam(teamOf(actor));
}

// 0x51ebe0: the aim point moved back from the target towards the start by the bounce off the target's surface
// (1 − (parallel + (perpendicular − parallel) · 0.9397) of the way, the surface from a ±1 trace at the target).
Vec3 World::bounceAim(Entity& actor, const std::string& weapon, const Vec3& start, const Vec3& target) {
    const TraceResult dn = collision_->trace({target[0], target[1], target[2] + 1.0f}, {target[0], target[1], target[2] - 1.0f},
                                             {0, 0, 0}, {0, 0, 0}, kGrenadeClip, actor.number);
    const int surf = dn.fraction == 1.0f ? 0 : int(dn.surfaceFlags >> 20 & 0x1f);
    const char* name = surf < 23 ? kSurfaces[surf] : "Default";
    const float par = weaponFloat(weapon, ("parallel" + std::string(name) + "Bounce").c_str());
    const float perp = weaponFloat(weapon, ("perpendicular" + std::string(name) + "Bounce").c_str());
    const float f = 1.0f - ((perp - par) * 0.9397f + par);
    return add(scale(sub(target, start), f), start);
}

// flrand (0x4d8480): one draw of the game's rand() (0x5a56f4: seed·0x343fd + 0x269ec3, >> 17) scaled into [lo, hi).
float World::randomRange(float lo, float hi) {
    meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3;
    return float(meleeSeed_ >> 17) * (hi - lo) * 3.0517578125e-05f + lo;
}

// ai_debugGrenadeHintArc (registered at 0x4d9196: float, default 150, 0..180, flags 0x1080): the cone of 0x51ece0.
float World::grenadeHintArc() const {
    const auto it = cvars.find("ai_debugGrenadeHintArc");
    if (it == cvars.end() || it->second.empty()) return kGrenadeHintArcDefault;
    return std::clamp(float(std::atof(it->second.c_str())), 0.0f, 180.0f);
}

// 0x51f050 (MagicGrenade's last resort): the hint table in its current order; the first hint whose lob over it
// (grenadeHintLob) passes the arc check 0x51e710 to its landing point gives the velocity.
bool World::grenadeHintLobThrow(Entity& actor, const Vec3& start, const Vec3& target, Vec3& vel) {
    for (const Vec3& h : grenadeHints_) {
        Vec3 v, land;
        if (!grenadeHintLob(h, start, target, gravity(), v, land)) continue;
        if (throwArcClear(actor, start, v, land)) {
            vel = v;
            if (std::getenv("IW2_GRENADE_HINT_DEBUG"))
                std::printf("grenade hint: MagicGrenade lob over (%g %g %g) lands (%g %g %g)\n", h[0], h[1], h[2], land[0], land[1], land[2]);
            return true;
        }
    }
    return false;
}

// 0x51f320 (the throw-back's last resort, 0x51fd7c): the hint table is sorted in place by distance from the landing
// point (it stays sorted for later users); the first hint reachable from the landing point with a min time throw
// (0x51ee30 flag 0, no arm offset, no bounce adjustment) gives the velocity.
bool World::grenadeHintFromLanding(Entity& actor, const Vec3& landing, Vec3& vel) {
    if (grenadeHints_.empty()) return false;
    grenadeHintSortByDistance(grenadeHints_, landing);
    for (const Vec3& h : grenadeHints_) {
        if (solveThrow(actor, "min time", landing, h, vel)) {
            if (std::getenv("IW2_GRENADE_HINT_DEBUG"))
                std::printf("grenade hint: throw back from (%g %g %g) at hint (%g %g %g)\n", landing[0], landing[1], landing[2], h[0], h[1], h[2]);
            return true;
        }
    }
    return false;
}

// 0x51f490 (inlined as well in checkGrenadeThrow 0x53e720 and checkGrenadeThrowPos 0x53ead0): a throw from `from` +
// the arm offset (turned by the actor's yaw, 0x51eb00: forward·x − right·y + up·z) to the target: none while g ≤ 0,
// while the actor already handles a grenade (unless `anyway`) or with friends of the thrower within 1.1 ×
// explosionRadius of the target (0x51f6e0). A line trace start ↔ target (0x526840: mask 0x811, no pass entity) decides
// the aim: clear, or blocked with no hint (0x51ece0), the bounce-adjusted target (0x51ebe0); blocked with a hint, the
// hint moved by flrand(−15, 15) on x, y, z in that order. A zero aim distance fails; then the method's solver.
bool World::throwSolution(Entity& actor, const Vec3& from, const Vec3& armOffset, const std::string& method,
                          const Vec3& target, bool anyway, Vec3& start, Vec3& vel) {
    if (gravity() <= 0.0f) return false;
    if (actor.ai.grenade && !anyway) return false;
    const std::string weapon = actor.obj->get("grenadeweapon").toString();
    const float r = weaponFloat(weapon, "explosionRadius") * 1.1f;
    const int team = teamOf(actor);
    for (const auto& ep : entities_) {
        Entity& f = *ep;
        if (!(f.isActor || f.isPlayer) || !isAlive(f) || teamOf(f) != team) continue;
        if (dot(sub(target, originOf(f.obj)), sub(target, originOf(f.obj))) < r * r) return false;
    }
    const Value ang = actor.obj->get("angles");
    const float yaw = (ang.isVector() ? ang.asVector()[1] : 0.0f) * float(M_PI) / 180.0f;
    const Vec3 fwd{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
    start = add(from, add(scale(fwd, armOffset[0]), add(scale(right, -armOffset[1]), Vec3{0, 0, armOffset[2]})));
    Vec3 aim, hint;
    const TraceResult los = collision_->trace(start, target, {0, 0, 0}, {0, 0, 0}, 0x811, -1);
    if (los.fraction != 1.0f && !grenadeHints_.empty() && grenadeHintPick(grenadeHints_, start, target, grenadeHintArc(), hint)) {
        aim[0] = randomRange(hint[0] - kGrenadeHintJitter, hint[0] + kGrenadeHintJitter);
        aim[1] = randomRange(hint[1] - kGrenadeHintJitter, hint[1] + kGrenadeHintJitter);
        aim[2] = randomRange(hint[2] - kGrenadeHintJitter, hint[2] + kGrenadeHintJitter);
        if (std::getenv("IW2_GRENADE_HINT_DEBUG"))
            std::printf("grenade hint: %s blocked throw (%g %g %g) -> (%g %g %g) aims at hint (%g %g %g)\n", method.c_str(),
                        start[0], start[1], start[2], target[0], target[1], target[2], hint[0], hint[1], hint[2]);
    } else {
        if (los.fraction != 1.0f && std::getenv("IW2_GRENADE_HINT_DEBUG"))
            std::printf("grenade hint: %s blocked throw (%g %g %g) -> (%g %g %g), no hint of %zu\n", method.c_str(), start[0],
                        start[1], start[2], target[0], target[1], target[2], grenadeHints_.size());
        aim = bounceAim(actor, weapon, start, target);
    }
    if (dot(sub(aim, start), sub(aim, start)) == 0.0f) return false;
    return solveThrow(actor, method, start, aim, vel);
}

void World::registerGrenadeBuiltins() {
    // radiusdamage(origin, range, maxDamage, minDamage) (0x4f3e70): radius damage from the world entity (no attacker),
    // inner = maxDamage, outer = minDamage, nothing ignored, MOD_EXPLOSIVE (14); the player is spared while
    // setplayerignoreradiusdamage is on (0x1c61058 copied to 0x1c61054 for the call).
    vm_.registerBuiltin("radiusdamage", [this](CallContext& c) {
        const Vec3 origin = c.arg(0).asVector();
        const float range = c.arg(1).asFloat(), maxDamage = c.arg(2).asFloat(), minDamage = c.arg(3).asFloat();
        sparePlayerRadiusDamage_ = ignorePlayerRadiusDamage_;
        radiusDamage(origin, nullptr, maxDamage, minDamage, range, nullptr, "MOD_EXPLOSIVE");
        sparePlayerRadiusDamage_ = false;
        return Value();
    });
    // setplayerignoreradiusdamage(flag) (0x4f3ef0): the int argument is kept for radiusdamage (saved with the game).
    vm_.registerBuiltin("setplayerignoreradiusdamage", [this](CallContext& c) {
        ignorePlayerRadiusDamage_ = c.arg(0).asInt() != 0;
        return Value();
    });
    auto actorOf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        return *e;
    };
    auto fuseArg = [](CallContext& c, size_t i) { return c.argc() > i ? int(c.arg(i).asFloat() * 1000.0f) : 5000; };
    auto grenadeWeapon = [](Entity& e) {
        const std::string w = e.obj->get("grenadeweapon").toString();
        if (w.empty() || w == "undefined" || w == "none")
            throw std::runtime_error("Actor [" + e.classname + "] doesn't have a grenade weapon set.");
        return w;
    };
    // MagicGrenadeManual(origin, velocity [, fuse seconds]) (0x4ee5e0): a grenade of the actor's grenade weapon.
    vm_.registerBuiltin("magicgrenademanual", [this, fuseArg, grenadeWeapon](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("MagicGrenadeManual only supports actors.");
        if (c.argc() != 2 && c.argc() != 3) throw std::runtime_error("<actor> MagicGrenadeManual <origin> <velocity> [time To Blow (seconds)].");
        spawnGrenade(*e, grenadeWeapon(*e), c.arg(0).asVector(), c.arg(1).asVector(), fuseArg(c, 2));
        return Value();
    });
    // MagicGrenade(origin, target [, fuse seconds]) (0x4ee3d0): no gravity / held grenade / friendly / line-of-sight
    // checks (unlike 0x51f490): the bounce-adjusted aim (0x51ebe0) from the origin, then min energy, min time, max time,
    // then the hint lob 0x51f050 towards that aim; the grenade leaves from the origin itself.
    vm_.registerBuiltin("magicgrenade", [this, fuseArg, grenadeWeapon](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("MagicGrenade only supports actors.");
        if (c.argc() != 2 && c.argc() != 3) throw std::runtime_error("<actor> MagicGrenade <origin> <target position> [time to blow (seconds)].");
        const Vec3 from = c.arg(0).asVector(), target = c.arg(1).asVector();
        const std::string weapon = e->obj->get("grenadeweapon").toString();
        const Vec3 aim = bounceAim(*e, weapon, from, target);
        Vec3 vel;
        bool ok = false;
        for (const char* m : {"min energy", "min time", "max time"})
            if (!ok) ok = solveThrow(*e, m, from, aim, vel);
        if (!ok) ok = grenadeHintLobThrow(*e, from, aim, vel);
        if (!ok) {
            print("MagicGrenade: None of the methods worked (probably distance or blocked)...need a good fail safe");
            return Value();
        }
        spawnGrenade(*e, grenadeWeapon(*e), from, vel, fuseArg(c, 2));
        return Value();
    });
    // checkGrenadeThrowPos(armOffset, method, target) (0x53ead0): with grenade ammo, the solution from the actor's
    // origin; it is kept for throwGrenade (+0x20dc start, +0x20e8 target, +0x20f4 velocity, +0x20d0 pending) and the
    // velocity returned.
    vm_.registerBuiltin("checkgrenadethrowpos", [this, actorOf](CallContext& c) {
        Entity& e = actorOf(c);
        const Vec3 arm = c.arg(0).asVector();
        const std::string method = c.arg(1).toString();
        const Vec3 target = c.arg(2).asVector();
        const Value ammo = e.obj->get("grenadeammo");
        if (!ammo.isNumber() || ammo.asInt() < 1) return Value();
        ActorAi& ai = e.ai;
        ai.throwTarget = target;
        Vec3 start, vel;
        const bool ok = throwSolution(e, originOf(e.obj), arm, method, target, false, start, vel);
        ai.throwPending = ok;
        if (!ok) return Value();
        ai.throwStart = start;
        ai.throwVel = vel;
        ai.throwMethod = method;
        return Value(vel);
    });
    // checkGrenadeThrow(armOffset, method) (0x53e720): checkGrenadeThrowPos at the enemy's predicted position. Nothing
    // without an enemy sentient or without grenade ammo; with g ≤ 0 or a grenade already handled the pending throw is
    // dropped; otherwise the target is the enemy's origin plus its velocity (0x514ed0: actor +0x230, the player's
    // ps velocity), kept with the re-solve-from-the-hand flag (+0x20d4 = 1) and the method, and the 0x51f490 logic
    // (anyway = 0, from the actor's origin) decides; the velocity is returned.
    vm_.registerBuiltin("checkgrenadethrow", [this, actorOf](CallContext& c) {
        Entity& e = actorOf(c);
        const Vec3 arm = c.arg(0).asVector();
        const std::string method = c.arg(1).toString();
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy || !(enemy->isActor || enemy->isPlayer)) return Value();
        const Value ammo = e.obj->get("grenadeammo");
        if (!ammo.isNumber() || ammo.asInt() < 1) return Value();
        ActorAi& ai = e.ai;
        if (gravity() <= 0.0f || ai.grenade) {
            ai.throwPending = false;
            return Value();
        }
        const Vec3 target = add(originOf(enemy->obj), enemy->velocity);
        ai.throwTarget = target;
        ai.throwFromTag = true;
        ai.throwMethod = method;
        Vec3 start, vel;
        const bool ok = throwSolution(e, originOf(e.obj), arm, method, target, false, start, vel);
        ai.throwPending = ok;
        if (!ok) return Value();
        ai.throwStart = start;
        ai.throwVel = vel;
        return Value(vel);
    });
    // throwGrenade (0x53ee10): a pending throw goes off: the held grenade thrown back from the grenade state, else a
    // new grenade of the actor's grenade weapon (5 s fuse; grenadeammo − 1). Dead actors only drop the pending throw.
    vm_.registerBuiltin("throwgrenade", [this, actorOf, grenadeWeapon](CallContext& c) {
        Entity& e = actorOf(c);
        ActorAi& ai = e.ai;
        if (isAlive(e) && (ai.grenade || ai.throwPending)) {
            Vec3 start = ai.throwStart, vel = ai.throwVel;
            if (ai.throwFromTag) {
                const auto bones = entityBones(e);
                auto it = bones.find("tag_weapon_right");
                Vec3 s2, v2;
                if (it != bones.end() && throwSolution(e, it->second.position, {0, 0, 0}, ai.throwMethod, ai.throwTarget, true, s2, v2)) {
                    start = s2;
                    vel = v2;
                }
            }
            if (ai.grenade && ai.stack[size_t(ai.depth)] == AiGrenade) {
                Entity& g = *ai.grenade;
                releaseHeldGrenade(e);
                GrenadeState& gs = *g.grenade;
                gs.trType = 5;
                gs.trTime = timeMs_;
                gs.trBase = start;
                gs.trDelta = vel;
                gs.onGround = false;
                gs.parent = &e;
                g.obj->set("origin", Value(start));
            } else {
                spawnGrenade(e, grenadeWeapon(e), start, vel, 5000);
                const Value ammo = e.obj->get("grenadeammo");
                if (ammo.isNumber() && ammo.asInt() > 0) e.obj->set("grenadeammo", Value(int32_t(ammo.asInt() - 1)));
            }
        }
        ai.throwPending = false;
        ai.throwMethod.clear();
        return Value();
    });
    // pickUpGrenade (0x53f0a0 → 0x5203c0): the grenade's world model goes on tag_weapon_right, the actor turns to the
    // throw direction (facing 1), the grenade stops and counts as in hand, thrown by the actor.
    vm_.registerBuiltin("pickupgrenade", [actorOf](CallContext& c) {
        Entity& e = actorOf(c);
        if (!e.ai.grenade) return Value();
        GrenadeState& g = *e.ai.grenade->grenade;
        if (e.ai.throwVel[0] != 0.0f || e.ai.throwVel[1] != 0.0f) {
            const float yaw = std::atan2(e.ai.throwVel[1], e.ai.throwVel[0]) * 57.29578f;
            e.obj->set("angles", Value(Vec3{0, yaw, 0}));
        }
        e.ai.facing = 1;
        g.trType = 0;
        g.trBase = originOf(e.ai.grenade->obj);
        g.inHand = true;
        g.holder = &e;
        g.parent = &e;
        e.ai.grenade->obj->set("$hidden", Value(true));
        return Value();
    });
}

// 0x520470: the grenade leaves the hand where the hand is (tag_weapon_right), at rest, nobody holding it.
void World::releaseHeldGrenade(Entity& actor) {
    Entity& g = *actor.ai.grenade;
    GrenadeState& gs = *g.grenade;
    const auto bones = entityBones(actor);
    auto it = bones.find("tag_weapon_right");
    const Vec3 p = it != bones.end() ? it->second.position : originOf(actor.obj);
    g.obj->set("origin", Value(p));
    g.obj->set("$hidden", Value());
    gs.trType = 0;
    gs.trBase = p;
    gs.trDelta = {0, 0, 0};
    gs.trTime = timeMs_;
    gs.inHand = false;
    gs.holder = nullptr;
    gs.landing = {0, 0, 0};
}

} // namespace iw2::game

namespace iw2::game {

// 0x4b9090 → 0x4b8f00: every .csv in fx/, then in fx/maps/<map>/, in name order (qsort 0x4b8ee0); each row
// "impact type,surface,effect" sets that cell, so later files override earlier ones. Type names are the table at
// 0x56ea58, surface names the surfaceparm table (0x5a0344, "default" for 0).
const std::string& World::impactEffect(int type, int surface) {
    static const std::string kNone;
    if (impactEffects_.empty()) {
        impactEffects_.resize(11);
        static const char* const kTypes[] = {"bullet_small_normal", "bullet_small_reflect", "bullet_large_normal", "bullet_large_reflect",
                                             "shotgun_normal", "shotgun_reflect", "bullet_ap_normal", "bullet_ap_reflect",
                                             "grenade_bounce", "grenade_explode", "rocket_explode"};
        static const char* const kSurf[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                            "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                            "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
        for (const std::string dir : {std::string("fx/"), "fx/maps/" + mapName_ + "/"}) {
            std::vector<std::string> files;
            for (const auto& f : vfs_.list(dir)) {
                const std::string rest = f.substr(dir.size());
                if (rest.find('/') == std::string::npos && rest.size() > 4 && lowerStr(rest.substr(rest.size() - 4)) == ".csv") files.push_back(f);
            }
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                const auto data = vfs_.read(f);
                if (!data) continue;
                std::istringstream in(std::string(data->begin(), data->end()));
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::vector<std::string> col;
                    std::stringstream ls(line);
                    for (std::string c; std::getline(ls, c, ',');) col.push_back(c);
                    if (col.size() < 2) continue;
                    const auto t = std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char* n) { return lowerStr(col[0]) == n; });
                    const auto su = std::find_if(std::begin(kSurf), std::end(kSurf), [&](const char* n) { return lowerStr(col[1]) == n; });
                    if (t == std::end(kTypes) || su == std::end(kSurf)) continue;
                    impactEffects_[size_t(t - std::begin(kTypes))][size_t(su - std::begin(kSurf))] = col.size() > 2 ? col[2] : std::string();
                }
            }
        }
    }
    if (type < 0 || type >= 11 || surface < 0 || surface >= 23) return kNone;
    return impactEffects_[size_t(type)][size_t(surface)];
}

} // namespace iw2::game

namespace iw2::game {

// LaunchItem (0x521f70): from the dropper's origin raised by half its height, velocity (cos yaw · 150, sin yaw · 150,
// (2·rand − 1) · 50 + 200) with the dropper yaw (ent +0xec); height = r.maxs.z − r.mins.z (0x5220a0..0x5220c2); TR_GRAVITY (0x521e00). TODO(spec): the
// callers that pass no velocity (ecx ≠ 0) and the yaw offset argument.
void World::launchItem(Entity& item, const Entity& dropper) {
    const Vec3 o = dropper.obj->get("origin").asVector();
    const Value av = dropper.obj->get("angles");
    const float yaw = (av.isVector() ? av.asVector()[1] : 0.0f) * float(M_PI) / 180.0f;
    const float height = dropper.isPlayer ? 70.0f : dropper.isActor ? 72.0f : 0.0f; // TODO(spec): the entity's maxs.z − mins.z
    const float r = float(std::rand() % 32768) / 32768.0f;
    item.obj->set("origin", Value(Vec3{o[0], o[1], o[2] + height * 0.5f}));
    item.obj->set("$trbase", item.obj->get("origin"));
    item.obj->set("$trdelta", Value(Vec3{std::cos(yaw) * 150.0f, std::sin(yaw) * 150.0f, (r + r - 1.0f) * 50.0f + 200.0f}));
    item.obj->set("$trtime", Value(float(timeMs_)));
    item.obj->set("$trtype", Value(int32_t(5))); // TR_GRAVITY (0x521f2d)
    item.obj->set("$groundent", Value(int32_t(kItemNoGround)));
}

// G_RunItem (0x523040), the per-frame item state machine. Item state on the entity: $trtype (0 stationary, 3 linear stop,
// 5 gravity), $trtime / $trduration (ms), $trbase / $trdelta, $groundent (kItemNoGround = 0x7ff, -1 = the world, else the
// entity number of a clip).
//  1. 0x523049..0x5230b2: unless the item rests on a stationary ground (groundEntityNum +0xc set and that entity's trType
//     +0x14 == 0), an item whose trajectory is not already gravity and that is not suspended (spawnflags +0x124 & 1; eType 0xb
//     ignores the flag) is re-based: TR_GRAVITY from its current origin at the level time with a zero velocity.
//  2. 0x5230b5..0x5230d2: trType 0 or 6, or linked (tagInfo +0x1c8), runs only the think (0x5233b0).
//  3. 0x5230d8..0x523127: the position 50 ms ahead (0x4fae80 at levelTime + 0x32); when its squared distance from the origin
//     (0x420d90) is below 0.1 (0x59202c) the end z drops by 1 (0x591e20).
//  4. 0x52312b..0x523163: trace of the entity box from the origin to that end, passEnt = owner (+0xf4), mask = clipmask
//     (+0x12c) or 0x491.
//  5. Hit (fraction < 1): endpos = lerp; the slide (not start-solid, fraction < 0.01, normal z < 0.5, 0x5231b2..0x523278)
//     replaces the trace with the second one. Then trType 3, duration 50 ms, base = old origin, delta = (endpos - origin) * 20,
//     origin = endpos (0x52327b..0x5232fd). No hit: origin = end (0x5232f1).
//  6. Link, G_RunThink (0x4da090); a freed item stops here (+0xa4 in-use byte, 0x523315).
//  7. Settle (0x523323..0x5233a9) only when this frame's (possibly slide) trace has fraction < 0.01 (0x591ea0): normal z <= 0
//     frees the item (0x4d7c30), so do no-drop contents 0x80000000 at the origin (0x44c840); else 0x522ea0 lays it on the surface
//     (not for eType 0xb: 0x4fd9e0, TODO(spec), no weapon item has it), G_SetOrigin(endpos) (0x4d8380: trType 0, delta 0) and
//     groundEntityNum = the trace's entity (0x523397).
// After the first floor hit the item sits 0.125 (the trace epsilon) above the floor with trType 3; the next frame's re-base
// probes 1 unit down from there, which ends at once (fraction 0): it settles one frame after the hit. A wall hit (fraction
// >= 0.01) does not settle; the item drops straight down from the wall afterwards (zero velocity after the re-base).
// A map-placed item (FinishSpawningItem 0x5225e0, unless spawnflags 1 = suspended) is dropped 4096 down once, retried from 15
// down when the first trace starts solid; still solid -> freed; it rests there (trType 0) with the trace's entity as ground
// (0x52283f).
void World::runItems() {
    const Vec3 mins{-1, -1, -1}, maxs{1, 1, 1}; // weapon items (item type 1, LaunchItem 0x521e5b..0x521eaa). TODO(spec): other item types use z 0..2
    for (size_t ei = 0; ei < entities_.size(); ++ei) {
        Entity& e = *entities_[ei];
        if (e.obj->removed || e.classname.rfind("weapon_", 0) != 0) continue;
        const Value sf = e.obj->get("spawnflags");
        const bool suspended = sf.isNumber() && (sf.asInt() & 1);
        if (!e.obj->get("$trtype").isNumber()) {
            if (e.obj->get("$itemsettled").truthy()) continue;
            e.obj->set("$itemsettled", Value(true));
            e.obj->set("$trtype", Value(int32_t(0)));
            if (suspended) continue; // groundEntityNum stays unset; the suspended flag blocks the re-base (step 1)
            Vec3 o = e.obj->get("origin").asVector();
            TraceResult tr = collision_->trace(o, {o[0], o[1], o[2] - 4096.0f}, mins, maxs, 0x491, e.number);
            if (tr.startSolid) {
                o[2] -= 15.0f;
                tr = collision_->trace(o, {o[0], o[1], o[2] - 4096.0f}, mins, maxs, 0x491, e.number);
                if (tr.startSolid) { e.obj->removed = true; continue; }
            }
            e.obj->set("origin", Value(Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]}));
            e.obj->set("$groundent", Value(int32_t(tr.fraction < 1.0f ? tr.entity : kItemNoGround)));
            continue;
        }
        Trajectory trj;
        trj.type = e.obj->get("$trtype").asInt();
        trj.startMs = e.obj->get("$trtime").isNumber() ? int64_t(e.obj->get("$trtime").asFloat()) : 0;
        trj.durationMs = e.obj->get("$trduration").isNumber() ? e.obj->get("$trduration").asInt() : 0;
        {
            const Value b = e.obj->get("$trbase"), d = e.obj->get("$trdelta");
            if (b.isVector()) { const Vec3 v = b.asVector(); trj.base = {v[0], v[1], v[2]}; }
            if (d.isVector()) { const Vec3 v = d.asVector(); trj.delta = {v[0], v[1], v[2]}; }
        }
        Vec3 cur = e.obj->get("origin").asVector();
        // 1. Re-base. A ground entity counts as moving while a mover channel of it is active (its trType is not 0).
        const int ground = e.obj->get("$groundent").isNumber() ? e.obj->get("$groundent").asInt() : kItemNoGround;
        bool groundStill = false;
        if (ground == -1) groundStill = true;
        else if (ground != kItemNoGround) {
            const auto m = movers_.find(ground);
            groundStill = m == movers_.end() || !m->second.pos.active;
        }
        if (!groundStill && trj.type != 5 && !suspended) {
            trj.type = 5;
            trj.startMs = timeMs_;
            trj.durationMs = 0;
            trj.base = {cur[0], cur[1], cur[2]};
            trj.delta = {0, 0, 0};
        }
        // 2. Stationary or linked: think only.
        if (trj.type == 0 || trj.type == 6 || links_.count(e.number)) {
            e.obj->set("$trtype", Value(int32_t(trj.type)));
            continue;
        }
        // 3. The position 50 ms ahead.
        const MVec ahead = trj.evaluate(timeMs_ + 50);
        Vec3 next{ahead[0], ahead[1], ahead[2]};
        {
            const float dx = next[0] - cur[0], dy = next[1] - cur[1], dz = next[2] - cur[2];
            if (dx * dx + dy * dy + dz * dz < 0.1f) next[2] -= 1.0f;
        }
        // 4. The trace. TODO(spec): passEnt is the owner (+0xf4); LaunchItem does not set it, the port passes the item itself.
        TraceResult tr = collision_->trace(cur, next, mins, maxs, 0x491, e.number);
        Vec3 endpos = next;
        if (tr.fraction < 1.0f) {
            endpos = {cur[0] + (next[0] - cur[0]) * tr.fraction, cur[1] + (next[1] - cur[1]) * tr.fraction, cur[2] + (next[2] - cur[2]) * tr.fraction};
            // 5. Slide: next' = next + n * (1 - (next - cur) . n), traced from the hit point; the second trace replaces the first.
            if (!tr.startSolid && tr.fraction < 0.01f && tr.normal[2] < 0.5f) {
                const Vec3 n{tr.normal[0], tr.normal[1], tr.normal[2]};
                const Vec3 end2 = itemSlideEnd(cur, next, n);
                const Vec3 from = endpos;
                tr = collision_->trace(from, end2, mins, maxs, 0x491, e.number);
                const float f2 = tr.fraction;
                endpos = {from[0] + (end2[0] - from[0]) * f2, from[1] + (end2[1] - from[1]) * f2, from[2] + (end2[2] - from[2]) * f2};
            }
            trj.type = 3;
            trj.startMs = timeMs_;
            trj.durationMs = 50;
            trj.base = {cur[0], cur[1], cur[2]};
            trj.delta = {(endpos[0] - cur[0]) * 20.0f, (endpos[1] - cur[1]) * 20.0f, (endpos[2] - cur[2]) * 20.0f};
        }
        cur = endpos;
        e.obj->set("origin", Value(cur));
        e.obj->set("$trtype", Value(int32_t(trj.type)));
        e.obj->set("$trtime", Value(float(trj.startMs)));
        e.obj->set("$trduration", Value(int32_t(trj.durationMs)));
        e.obj->set("$trbase", Value(Vec3{trj.base[0], trj.base[1], trj.base[2]}));
        e.obj->set("$trdelta", Value(Vec3{trj.delta[0], trj.delta[1], trj.delta[2]}));
        // 7. Settle.
        if (!(tr.fraction < 0.01f)) continue;
        if (tr.normal[2] <= 0.0f) { e.obj->removed = true; continue; }
        if (collision_->trace(cur, cur, {0, 0, 0}, {0, 0, 0}, 0x80000000u).startSolid) { e.obj->removed = true; continue; }
        // 0x522ea0: the item's forward vector (its pitch and yaw) is projected onto the plane of the hit normal, (n x f) x n, and
        // the angles become its vectoangles with roll 0, +90 for a weapon item (item type 1: it lies on its side); the angular
        // trajectory is cleared and currentAngles (+0xe8..) set.
        {
            const Value av = e.obj->get("angles");
            const Vec3 a = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
            const float rad = 0.01745329238474369f;
            const Vec3 f{std::cos(a[0] * rad) * std::cos(a[1] * rad), std::cos(a[0] * rad) * std::sin(a[1] * rad), -std::sin(a[0] * rad)};
            const Vec3 n{tr.normal[0], tr.normal[1], tr.normal[2]};
            const Vec3 t{n[1] * f[2] - n[2] * f[1], n[2] * f[0] - n[0] * f[2], n[0] * f[1] - n[1] * f[0]};
            const Vec3 g{t[1] * n[2] - t[2] * n[1], t[2] * n[0] - t[0] * n[2], t[0] * n[1] - t[1] * n[0]};
            float pitch = 90.0f, yaw = 0.0f;
            if (g[0] == 0.0f && g[1] == 0.0f) {
                pitch = g[2] > 0.0f ? 270.0f : 90.0f; // vectoangles of a vertical vector
            } else {
                yaw = std::atan2(g[1], g[0]) / rad;
                if (yaw < 0) yaw += 360.0f;
                pitch = -std::atan2(g[2], std::sqrt(g[0] * g[0] + g[1] * g[1])) / rad;
                if (pitch < 0) pitch += 360.0f;
            }
            e.obj->set("angles", Value(Vec3{pitch, yaw, 90.0f}));
        }
        // G_SetOrigin(endpos) (0x4d8380) and groundEntityNum = tr.entityNum.
        e.obj->set("$trtype", Value(int32_t(0)));
        e.obj->set("$trbase", Value(cur));
        e.obj->set("$trdelta", Value(Vec3{0, 0, 0}));
        e.obj->set("$trduration", Value(int32_t(0)));
        e.obj->set("$groundent", Value(int32_t(tr.entity)));
        e.obj->set("$itemsettled", Value(true));
    }
}

Entity& World::debugDropItem(const std::string& weaponName, const Vec3& origin, const Vec3& velocity) {
    Entity& item = spawnEntity("weapon_" + lowerStr(weaponName));
    item.obj->set("origin", Value(origin));
    item.obj->set("angles", Value(Vec3{0, 0, 0}));
    if (const auto* def = weapon(weaponName))
        if (auto it = def->find("worldmodel"); it != def->end() && !it->second.empty()) item.obj->set("model", Value(it->second));
    item.obj->set("$trtype", Value(int32_t(5)));
    item.obj->set("$trtime", Value(float(timeMs_)));
    item.obj->set("$trbase", Value(origin));
    item.obj->set("$trdelta", Value(velocity));
    item.obj->set("$groundent", Value(int32_t(kItemNoGround)));
    return item;
}

// G_RunMissile's move trace (0x526d6b..0x526df7), for every missile: when |trDelta.z| > 30 (0x591e08) and the origin is not in
// water (point contents 0x20, 0x44c840), the trace (0x526840) also stops at water (clipmask | 0x20). When the trace's surface is
// water ((surfaceFlags & 0x1f00000) == 0x1400000), 0x526b40 sends the splash and the move is traced again with the clipmask alone.
// 0x526b40: temp entity event 0xb8 (0x4d7f40) at the missile's origin before the move, with +4 the trace normal and +3 the
// normalised trDelta whose z is negated when negative (0x591e48 = -1), both as direction bytes (0x420c30), +1 the surface type,
// +0xa the missile's number. The client handles 0xb8 as a small bullet impact (jump table 0x4bb74c, case 0x4bb260 ->
// 0x4b81d0, the same case as Bullet_Fire's event 0xb8): bullet_small_water and the impact table's bullet_small normal (type 0)
// along +4 and reflect (type 1) along +3. TODO(spec): the direction-byte quantisation.
TraceResult World::missileTrace(const Vec3& from, const Vec3& to, uint32_t clip, int pass, const Vec3& trDelta) {
    uint32_t mask = clip;
    if (std::fabs(trDelta[2]) > 30.0f && !collision_->trace(from, from, {0, 0, 0}, {0, 0, 0}, 0x20).startSolid) mask |= 0x20;
    TraceResult tr = collision_->trace(from, to, {0, 0, 0}, {0, 0, 0}, mask, pass);
    if ((tr.surfaceFlags & 0x1f00000u) == 0x1400000u) {
        Vec3 d = trDelta;
        const float l = len(d);
        if (l != 0.0f) d = scale(d, 1.0f / l);
        if (d[2] < 0.0f) d[2] = -d[2];
        startSound("bullet_small_water", &from);
        const Vec3 n{tr.normal[0], tr.normal[1], tr.normal[2]};
        const std::string& normalFx = impactEffect(0, 20);
        const std::string& reflectFx = impactEffect(1, 20);
        if (!normalFx.empty()) fxEvents_.push_back({normalFx, from, n});
        if (!reflectFx.empty()) fxEvents_.push_back({reflectFx, from, d});
        ++stats.waterSplashes;
        tr = collision_->trace(from, to, {0, 0, 0}, {0, 0, 0}, clip, pass);
    }
    return tr;
}

} // namespace iw2::game
