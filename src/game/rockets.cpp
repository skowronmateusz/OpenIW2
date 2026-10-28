// Rockets (projectile weapons: tank shells from fireturret, docs/spec/vehicles.md "Turret projectiles"): the missile
// entity (G_FireRocket 0x527810), its straight flight (G_RunMissile 0x526c30), the impact (0x526200) and the
// explosion events EV_GRENADE_EXPLODE / EV_ROCKET_EXPLODE / EV_CUSTOM_EXPLODE (cgame 0x4ba8d0 cases 0xbf…0xc3).
#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {

constexpr uint32_t kRocketClip = 0x280e891; // +0x12c

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

float vlen(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// projExplosionType (weapon +0x390): 0 grenade, 1 rocket, other values the custom event.
int explosionType(const std::string& v) {
    if (v.empty()) return 0;
    if (v == "grenade") return 0;
    if (v == "rocket") return 1;
    if (v[0] >= '0' && v[0] <= '9') return std::atoi(v.c_str());
    return 2;
}

} // namespace

// G_FireRocket (0x527810): classname "rocket", ET_MISSILE, think (explode) in 30 s, flags 0x400, the shooter's
// weapon, parent (+0xf4 / +0x134), direct damage = the weapon's damage (+0x1ec → +0x144), clip 0x280e891, a linear
// trajectory from `start` at projectileSpeed along `dir` starting 50 ms ago, angles of the direction, the weapon's
// projectileModel. TODO(spec): +0x168 (projectile time from weapon +0x5ac), the 0x20000 flag copy, 0x4d6110.
Entity& World::spawnRocket(Entity& parent, const std::string& weaponName, const Vec3& start, const Vec3& dir) {
    Entity& e = spawnEntity("rocket");
    e.rocket = std::make_unique<RocketState>();
    RocketState& r = *e.rocket;
    r.weapon = weaponName;
    r.parent = &parent;
    r.explodeMs = timeMs_ + 30000;
    r.trTime = timeMs_ - 50;
    r.trBase = start;
    const float speed = float(std::atoi(weaponField(weaponName, "projectileSpeed").c_str()));
    r.trDelta = {dir[0] * speed, dir[1] * speed, dir[2] * speed};
    e.obj->set("origin", Value(start));
    const float yaw = std::atan2(dir[1], dir[0]) * 180.0f / float(M_PI);
    const float pitch = -std::atan2(dir[2], std::sqrt(dir[0] * dir[0] + dir[1] * dir[1])) * 180.0f / float(M_PI);
    e.obj->set("angles", Value(Vec3{pitch, yaw, 0}));
    const std::string model = weaponField(weaponName, "projectileModel");
    if (!model.empty()) e.obj->set("model", Value(model));
    rockets_.push_back(&e);
    ++stats.rockets;
    // CG_Missile (0x4ab3af): the weapon's projTrailEffect (+0x458, handle weapon info +0x170) is played once (cent +0x143),
    // bolted (0x4853f0) on the missile's tag_origin (scr const 0x1c7f144, 0x47eec0). Queued here at the launch.
    // TODO(spec): projectileDLight (+0x45c -> info +0x14c, rgb +0x150..0x158) added each frame (0x4ab404 -> *0x617b1c).
    const std::string trail = weaponField(weaponName, "projTrailEffect");
    if (!trail.empty() && !model.empty()) fxEvents_.push_back({trail, start, dir, e.number, "tag_origin"});
    return e;
}

// The explosion event the impact sends (0x526200 → 0x4d8020) and what the client does with it (0x4ba8d0):
// grenade type 0xbf: grenade_explode_<surface> and impact effect type 9; rocket 0xc0 / 0xc1 (a sentient was hit):
// rocket_explode_<surface> (sounds 0xf5b800 from "rocket_explode", 0x4a28f5) and impact type 10 (+0x398); other
// types 0xc2 / 0xc3: no surface sound or impact. Then the weapon's projExplosionEffect (0xf48ee8) along the
// direction and projExplosionSound (0xf48eec). TODO(spec): the flag 0xf79778 the 0xc1 / 0xc3 events set.
void World::missileExplosionEvent(const std::string& weaponName, const Vec3& at, const Vec3& dir, int surface) {
    static const char* const kSurfaceAlias[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                                "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                                "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
    const int type = explosionType(lowerStr(weaponField(weaponName, "projExplosionType")));
    if (type < 2 && surface >= 0 && surface < 23) {
        startSound(std::string(type == 0 ? "grenade_explode_" : "rocket_explode_") + kSurfaceAlias[surface], &at);
        const std::string& impact = impactEffect(type == 0 ? 9 : 10, surface);
        if (!impact.empty()) fxEvents_.push_back({impact, at, dir});
    }
    const std::string fx = weaponField(weaponName, "projExplosionEffect");
    if (!fx.empty()) fxEvents_.push_back({fx, at, dir});
    const std::string snd = weaponField(weaponName, "projExplosionSound");
    if (!snd.empty()) startSound(snd, &at);
}

// Impact (0x526200) of a missile without the bounce flag: the hit entity takes the direct damage (MOD_PROJECTILE,
// direction of flight, (0, 0, 1) when it has none); with projImpactExplode the explosion event goes out with the
// surface and the hit normal, the missile stops there and the radius damage (explosionInnerDamage /
// OuterDamage / Radius from the parent, the hit entity skipped, MOD_PROJECTILE_SPLASH) follows.
// TODO(spec): projImpactExplode 0 (the missile bounces, 0x525f30; every shipped projectile weapon has 1), the AI event 0x535860 (type 0x11) for the
// parent, and that the exploded missile stays as an ET_GENERAL entity until freed.
void World::rocketImpact(Entity& e, const TraceResult& tr) {
    RocketState& r = *e.rocket;
    Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
    const int damage = std::atoi(weaponField(r.weapon, "damage").c_str());
    if (hit && damage != 0) {
        Vec3 dir = r.trDelta;
        const float l = vlen(dir);
        dir = l != 0.0f ? Vec3{dir[0] / l, dir[1] / l, dir[2] / l} : Vec3{0, 0, 1};
        World::damage(*hit, r.parent, damage, dir, tr.endPos, "MOD_PROJECTILE", "none");
    }
    // 0x526484: with a direct damage the impact also runs the trigger_damage segment test (0x5295f0) from the missile
    // origin (the trace end point was stored there at 0x526e7b), MOD_PROJECTILE, for the parent.
    // The segment's end argument is the G_RunMissile local vec3 (frame +0x24) that is stored to the origin (0xdc) at 0x526e7b, so
    // both ends are the origin (0x526e38..0x526e80, call at 0x526ff6).
    if (damage != 0) triggerDamageSegment(r.parent, tr.endPos, tr.endPos, damage, 5);
    if (std::atoi(weaponField(r.weapon, "projImpactExplode").c_str()) == 0) {
        e.obj->removed = true;
        return;
    }
    const Vec3 at = tr.endPos;
    e.obj->set("origin", Value(at));
    if (std::getenv("IW2_ROCKET_DEBUG"))
        print("rocket " + r.weapon + " hits " + (hit ? hit->classname : std::string("world")) + " at " + std::to_string(at[0]) + "," +
              std::to_string(at[1]) + "," + std::to_string(at[2]));
    missileExplosionEvent(r.weapon, at, {tr.normal[0], tr.normal[1], tr.normal[2]}, int((tr.surfaceFlags >> 20) & 0x1f));
    const float inner = weaponFloat(r.weapon, "explosionInnerDamage");
    if (inner != 0.0f)
        radiusDamage(at, r.parent, inner, weaponFloat(r.weapon, "explosionOuterDamage"), weaponFloat(r.weapon, "explosionRadius"),
                     hit, "MOD_PROJECTILE_SPLASH");
    e.obj->removed = true;
}

// G_RunMissile (0x526c30) for a linear missile: a point trace from the origin to the trajectory's position now
// (clip 0x280e891, the parent passes); the sky (surface flag 0x10) frees it, anything else is the impact. The think
// at 30 s explodes it in the air. TODO(spec): whizby (0x5297b0), water (0x526b40), the 30 s think's event.
void World::runRocket(Entity& e) {
    {
        {
            RocketState& r = *e.rocket;
            const Value ov = e.obj->get("origin");
            const Vec3 from = ov.isVector() ? ov.asVector() : r.trBase;
            const float t = float(timeMs_ - r.trTime) * 0.001f;
            const Vec3 to{r.trBase[0] + r.trDelta[0] * t, r.trBase[1] + r.trDelta[1] * t, r.trBase[2] + r.trDelta[2] * t};
            const int pass = r.parent ? r.parent->number : -1;
            const TraceResult tr = collision_->trace(from, to, {0, 0, 0}, {0, 0, 0}, kRocketClip, pass);
            e.obj->set("origin", Value(tr.endPos));
            if (tr.fraction < 1.0f) {
                if (tr.surfaceFlags & 0x10) e.obj->removed = true;
                else rocketImpact(e, tr);
            } else if (timeMs_ >= r.explodeMs) {
                missileExplosionEvent(r.weapon, tr.endPos, {0, 0, 1}, 0);
                const float inner = weaponFloat(r.weapon, "explosionInnerDamage");
                if (inner != 0.0f)
                    radiusDamage(tr.endPos, r.parent, inner, weaponFloat(r.weapon, "explosionOuterDamage"),
                                 weaponFloat(r.weapon, "explosionRadius"), nullptr, "MOD_PROJECTILE_SPLASH");
                e.obj->removed = true;
            }
        }
    }
}

void World::pruneMissiles() {
    auto prune = [](std::vector<Entity*>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [](const Entity* e) { return e->obj->removed; }), v.end());
    };
    prune(grenades_);
    prune(rockets_);
}

} // namespace iw2::game
