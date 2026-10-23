// Grenade missiles and the throw built-ins (docs/spec/grenades.md): the grenade entity (0x5275f0), its flight,
// bounces and fuse (G_RunMissile 0x526c30, 0x525f30, 0x526600), the radius damage (0x5289d0), the landing prediction
// (0x527270), the throw solvers behind checkGrenadeThrowPos (0x51f490) and throwGrenade / MagicGrenade(Manual) /
// pickUpGrenade.
#include "game/world.h"

#include <cmath>

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

Vec3 World::grenadeVelocity(const GrenadeState& g, int64_t atMs) const {
    if (g.trType != 5) return {0, 0, 0};
    const float t = float(atMs - g.trTime) * 0.001f;
    return {g.trDelta[0], g.trDelta[1], g.trDelta[2] - 800.0f * t};
}

// The grenade entity (0x5275f0): classname "grenade", fuse now + fuseMs, thrown by `thrower` (+0x134, owner +0xf4),
// gravity trajectory from `start` with `velocity`, bouncing (flags 0x1000000), the weapon's projectile model.
// TODO(spec): the fuse a player cooked (client +0x3c), the angular trajectory (+0x38 … +0x58, random yaw and spin).
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
// TODO(spec): the angular trajectory and the bounce event's sound; 0x520230 (actors re-planning when the grenade
// hits a character).
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
            g.trType = 0;
            g.trBase = tr.endPos;
            g.trDelta = {0, 0, 0};
            g.onGround = true;
            e.obj->set("origin", Value(tr.endPos));
            return false;
        }
    }
    const Vec3 p = add(tr.endPos, {n[0] * 0.1f, n[1] * 0.1f, std::min(n[2] * 0.1f, 0.0f)});
    g.trType = 5;
    g.trBase = p;
    g.trTime = timeMs_;
    g.trDelta = nv;
    e.obj->set("origin", Value(p));
    return tr.entity < 0 && len(sub(nv, v)) > 100.0f;
}

// Radius damage (0x5289d0): every damageable entity but `ignore` within `radius` of `origin` (distance to its origin;
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
        if (&t == ignore || t.obj->removed || !isAlive(t)) continue;
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
    const float radius = weaponFloat(g.weapon, "explosionRadius");
    radiusDamage(at, g.parent, weaponFloat(g.weapon, "explosionInnerDamage"), weaponFloat(g.weapon, "explosionOuterDamage"),
                 radius, nullptr, "MOD_GRENADE_SPLASH");
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
// TODO(spec): water surfaces (0x526b40), the 1.5-unit ground settle, whizby (0x5297b0) and the in-flight rotation.
void World::runGrenade(Entity& e) {
    GrenadeState& g = *e.grenade;
    if (timeMs_ - g.pingMs > 499) {
        grenadePing(e);
        g.pingMs = timeMs_;
    }
    if (!g.inHand) {
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
            const TraceResult tr = collision_->trace(o, to, {0, 0, 0}, {0, 0, 0}, kGrenadeClip, pass);
            e.obj->set("origin", Value(tr.endPos));
            if (tr.fraction < 1.0f) {
                if (tr.surfaceFlags & 0x10) {
                    forgetGrenade(e);
                    e.obj->removed = true;
                    return;
                }
                const int64_t hitMs = timeMs_ - 50 + int64_t(50.0f * tr.fraction);
                if (bounceGrenade(e, tr, hitMs)) {
                    // TODO(spec): the bounce event (0xbd / 0xbe) and its surface sound.
                }
            } else if (len(g.trDelta) != 0.0f) {
                g.onGround = false;
            }
        }
    }
    if (timeMs_ >= g.explodeMs) explodeGrenade(e);
}

void World::runGrenades() {
    for (size_t i = 0; i < grenades_.size();) {
        Entity& e = *grenades_[i];
        if (!e.obj->removed) runGrenade(e);
        if (e.obj->removed) grenades_.erase(grenades_.begin() + long(i));
        else ++i;
    }
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

// 0x51f490: a throw from `from` + the arm offset (turned by the actor's yaw, 0x51eb00: forward·x − right·y + up·z) to
// the target: none while g ≤ 0, while the actor already handles a grenade (unless `anyway`) or with friends of the
// thrower within 1.1 × explosionRadius of the target (0x51f6e0). The aim point is moved back towards the start by the
// bounce off the target's surface (0x51ebe0: 1 − (parallel + (perpendicular − parallel) · 0.9397)).
// TODO(spec): the grenade hint nodes (info_grenade_hint, 0x51ece0) tried when the start is blocked.
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
    const TraceResult dn = collision_->trace({target[0], target[1], target[2] + 1.0f}, {target[0], target[1], target[2] - 1.0f},
                                             {0, 0, 0}, {0, 0, 0}, kGrenadeClip, actor.number);
    const int surf = dn.fraction == 1.0f ? 0 : int(dn.surfaceFlags >> 20 & 0x1f);
    const char* name = surf < 23 ? kSurfaces[surf] : "Default";
    const float par = weaponFloat(weapon, ("parallel" + std::string(name) + "Bounce").c_str());
    const float perp = weaponFloat(weapon, ("perpendicular" + std::string(name) + "Bounce").c_str());
    const float f = 1.0f - ((perp - par) * 0.9397f + par);
    const Vec3 aim = add(scale(sub(target, start), f), start);
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
    // MagicGrenade(origin, target [, fuse seconds]) (0x4ee3d0): aim adjusted for the bounce, then min energy, min
    // time, max time. TODO(spec): the last fallback 0x51f050 when all three fail.
    vm_.registerBuiltin("magicgrenade", [this, fuseArg, grenadeWeapon](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("MagicGrenade only supports actors.");
        if (c.argc() != 2 && c.argc() != 3) throw std::runtime_error("<actor> MagicGrenade <origin> <target position> [time to blow (seconds)].");
        const Vec3 from = c.arg(0).asVector(), target = c.arg(1).asVector();
        Vec3 start, vel;
        bool ok = false;
        for (const char* m : {"min energy", "min time", "max time"})
            if (!ok) ok = throwSolution(*e, from, {0, 0, 0}, m, target, true, start, vel);
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
