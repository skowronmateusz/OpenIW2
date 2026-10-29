// trigger_damage: a brush entity that fires when it is hit. Spawn 0x529540; hit by a shot or a melee blow along the
// segment (0x5295f0, called from Bullet_Fire 0x5314ac and the melee trace 0x530f37); hit by G_Damage through the
// entity type 8 callbacks (pain 0x5294c0, die 0x529500); the shared action is 0x5293c0.
#include "game/triggerdamage.h"
#include "game/world.h"

#include <cstdlib>

namespace iw2::game {

int modNumber(const std::string& mod) {
    static const char* names[] = {"MOD_UNKNOWN",    "MOD_PISTOL_BULLET",     "MOD_RIFLE_BULLET", "MOD_GRENADE",  "MOD_GRENADE_SPLASH",
                                  "MOD_PROJECTILE", "MOD_PROJECTILE_SPLASH", "MOD_MELEE",        "MOD_HEAD_SHOT", "MOD_CRUSH",
                                  "MOD_TELEFRAG",   "MOD_FALLING",           "MOD_SUICIDE",      "MOD_TRIGGER_HURT", "MOD_EXPLOSIVE"};
    for (int i = 0; i < 15; ++i)
        if (mod == names[i]) return i;
    return 0;
}

bool triggerDamageAccepts(int f, int mod) {
    if ((f & 0x1) && mod == 1) return false;
    if ((f & 0x2) && mod == 2) return false;
    if ((f & 0x4) && mod >= 3 && mod <= 6) return false;
    if ((f & 0x8) && mod == 14) return false;
    if ((f & 0x10) && (mod == 4 || mod == 6)) return false;
    if ((f & 0x20) && mod == 7) return false;
    if ((f & 0x100) && (mod == 0 || (mod > 8 && mod <= 13))) return false;
    return true;
}

bool triggerDamageFires(int threshold, int accumulate, int spawnflags, int damage, int mod, int health) {
    if (threshold > 0 && damage < threshold) return false;
    if (!triggerDamageAccepts(spawnflags, mod)) return false;
    if (accumulate != 0 && kTriggerDamageFullHealth - health < accumulate) return false;
    return true;
}

namespace {

int intOf(const Value& v) { return v.isNumber() ? v.asInt() : v.isString() ? std::atoi(v.asString().c_str()) : 0; }

} // namespace

// Spawn (0x529540): threshold (+0x154) and accumulate (+0x158) are integer keys (default 0); health 32000 (+0x13c);
// takedamage 1 (+0x111); entity type 8; the "wait" key (0x528d60) with a value of 0 or less sets flag 0x200, which
// removes the entity after it fired (0x52941b, 0x464ea0).
void World::spawnTriggerDamage(Entity& e) {
    e.obj->set("$tdThreshold", Value(int32_t(intOf(e.obj->get("threshold")))));
    e.obj->set("$tdAccumulate", Value(int32_t(intOf(e.obj->get("accumulate")))));
    e.obj->set("health", Value(int32_t(kTriggerDamageFullHealth)));
    e.obj->set("$takedamage", Value(true));
    const Value w = e.obj->get("wait");
    if (w.isDefined()) {
        const float v = w.isNumber() ? w.asFloat() : float(std::atof(w.toString().c_str()));
        if (!(v > 0.0f)) e.obj->set("$tdRemove", Value(true));
    }
}

// 0x5293c0: below the threshold nothing happens; the means of death must be accepted; with an accumulate value the
// damage taken since the last firing (32000 - health) must reach it. Then the trigger fires ("trigger" with the
// attacker as activator, the record 0x528cb0), health returns to 32000 and, with flag 0x200, the entity is removed.
// Without an accumulate value the callers reset the health after every hit (0x52977b, 0x5294e6).
void World::triggerDamageApply(Entity& e, Entity* attacker, int damage, int mod) {
    if (!triggerDamageFires(intOf(e.obj->get("$tdThreshold")), intOf(e.obj->get("$tdAccumulate")), intOf(e.obj->get("spawnflags")), damage,
                            mod, intOf(e.obj->get("health"))))
        return;
    vm_.notify(e.obj, "trigger", {attackerValue(attacker)});
    e.obj->set("health", Value(int32_t(kTriggerDamageFullHealth)));
    if (e.obj->get("$tdRemove").truthy()) e.obj->removed = true;
}

// G_Damage on a trigger_damage after the "damage" notify: the die callback (health < 1, 0x529500) and the pain
// callback (0x5294c0) both run the action, then without an accumulate value the health is reset.
void World::triggerDamageHit(Entity& e, Entity* attacker, int amount, const std::string& mod) {
    triggerDamageApply(e, attacker, amount, modNumber(mod));
    if (!e.obj->removed && intOf(e.obj->get("$tdAccumulate")) == 0) e.obj->set("health", Value(int32_t(kTriggerDamageFullHealth)));
}

// 0x5295f0: every trigger_damage whose brush the segment start -> end crosses gets a "damage" notify (damage,
// attacker), then the action (0x5293c0, 0x529769).
// grenadeTouchOnly (the missile twin 0x5297b0, 0x529874..0x52987d): only triggers whose +0x128 has 0x4000, which
// enablegrenadetouchdamage (0x4f8000) sets; the bullet / melee test 0x5295f0 has no such gate.
void World::triggerDamageSegment(Entity* attacker, const Vec3& start, const Vec3& end, int damage, int mod, bool grenadeTouchOnly) {
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (e.obj->removed || e.isSpawner || e.classname != "trigger_damage") continue;
        if (grenadeTouchOnly && !e.obj->get("$grenadetouch").truthy()) continue;
        const Value m = e.obj->get("model");
        if (!m.isString() || m.asString().size() < 2 || m.asString()[0] != '*') continue;
        const Value ov = e.obj->get("origin"), av = e.obj->get("angles");
        const TraceResult tr = collision_->traceModel(std::atoi(m.asString().c_str() + 1), ov.isVector() ? ov.asVector() : Vec3{0, 0, 0},
                                                      av.isVector() ? av.asVector() : Vec3{0, 0, 0}, start, end, {0, 0, 0}, {0, 0, 0},
                                                      0xffffffffu);
        if (!tr.startSolid && tr.fraction >= 1.0f) continue;
        vm_.notify(e.obj, "damage", {Value(int32_t(damage)), attackerValue(attacker)});
        triggerDamageApply(e, attacker, damage, mod);
        if (!e.obj->removed && intOf(e.obj->get("$tdAccumulate")) == 0) e.obj->set("health", Value(int32_t(kTriggerDamageFullHealth)));
    }
}

// Use function of entity type 8 (0x529450, via useby after its "trigger" notify): with a positive +0x154 and
// (+0x158 + 1) below it nothing happens; with a nonzero +0x158 the damage taken (32000 - health) must reach it; then the
// health returns to 32000 and, with flag 0x200 (+0x124), the entity is freed (0x464ea0).
void World::triggerDamageUse(Entity& e) {
    const int threshold = intOf(e.obj->get("$tdThreshold")), accumulate = intOf(e.obj->get("$tdAccumulate"));
    if (threshold > 0 && accumulate + 1 < threshold) return;
    if (accumulate != 0 && kTriggerDamageFullHealth - intOf(e.obj->get("health")) < accumulate) return;
    e.obj->set("health", Value(int32_t(kTriggerDamageFullHealth)));
    if (e.obj->get("$tdRemove").truthy()) e.obj->removed = true;
}

bool triggerDiskContact(float absminX, float absmaxX, float absminY, float absmaxY, float originX, float originY, float radius) {
    const float cx = (absminX + absmaxX) * 0.5f, cy = (absminY + absmaxY) * 0.5f;
    const float rr = (absmaxX - cx) + (radius + 64.0f) - 64.0f; // 0x4478b6 / 0x447950
    const float dx = cx - originX, dy = cy - originY;
    return !(dx * dx + dy * dy < rr * rr);
}

} // namespace iw2::game
