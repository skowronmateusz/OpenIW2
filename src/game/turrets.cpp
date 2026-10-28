// Turrets: misc_turret / misc_mg42 entities, spawnturret and the turret and gunner script methods
// (docs/spec/turrets.md). The turret think that picks targets and fires for AI gunners is not implemented.
#include "game/world.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

float weaponFloat(const std::map<std::string, std::string>* w, const char* key) {
    if (!w) return 0.0f;
    auto it = w->find(key);
    return it == w->end() ? 0.0f : float(std::atof(it->second.c_str()));
}

// A map key overrides the weapon file's value (0x51dd80 reads the spawn key, falling back to the weapon).
float spawnOrWeapon(const ObjectPtr& o, const char* key, const std::map<std::string, std::string>* w, const char* wkey) {
    const Value v = o->get(key);
    if (v.isNumber()) return v.asFloat();
    if (v.isString()) return float(std::atof(v.asString().c_str()));
    return weaponFloat(w, wkey);
}

} // namespace

// Bone controls of a turret model, the same on the server (0x51d740, entity type callback +0x10) and in cgame
// (0x4ab0d0): tag_aim and tag_aim_animated turn by (pitch − e+0x78, yaw, 0), tag_flash by (e+0x78, 0, 0). e+0x78 is
// cleared by every think (0x51d279) and only set by the turn (0x51b7a3) with turret flag 0x200, which nothing in
// OpenIW2 sets yet, so it is 0 here.
std::vector<std::pair<int, assets::Quat>> World::turretBoneControls(const assets::XModelParts& parts, float pitch, float yaw) {
    std::vector<std::pair<int, assets::Quat>> out;
    const float drop = 0.0f;
    for (size_t i = 0; i < parts.bones.size(); ++i) {
        std::string n = parts.bones[i].name;
        for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (n == "tag_aim" || n == "tag_aim_animated") out.push_back({int(i), assets::anglesQuat(pitch - drop, yaw, 0)});
        else if (n == "tag_flash") out.push_back({int(i), assets::anglesQuat(drop, 0, 0)});
    }
    return out;
}

// Turret setup (0x51dd80): weapon from `weaponinfo`; arcs from the rightarc/leftarc/toparc/bottomarc keys or the
// weapon's rightArc/leftArc/topArc/bottomArc. Right and top arcs are negated and kept ≤ 0, left and bottom ≥ 0.
void World::initTurret(Entity& e, const std::string& weaponInfo) {
    const auto* w = weapon(weaponInfo);
    if (!w) throw std::runtime_error("Couldn't find weapon [" + weaponInfo + "]");
    auto t = std::make_unique<TurretState>();
    t->weapon = weaponInfo;
    t->rightArc = -spawnOrWeapon(e.obj, "rightarc", w, "rightarc");
    if (t->rightArc > 0.0f) t->rightArc = 0.0f;
    t->leftArc = spawnOrWeapon(e.obj, "leftarc", w, "leftarc");
    if (t->leftArc < 0.0f) t->leftArc = 0.0f;
    t->topArc = -spawnOrWeapon(e.obj, "toparc", w, "toparc");
    if (t->topArc > 0.0f) t->topArc = 0.0f;
    t->bottomArc = spawnOrWeapon(e.obj, "bottomarc", w, "bottomarc");
    if (t->bottomArc < 0.0f) t->bottomArc = 0.0f;
    t->setupRightArc = t->rightArc;
    t->setupLeftArc = t->leftArc;
    // yawconvergencetime, falling back to convergencetime, then the weapon (0x51dd80).
    const Value yc = e.obj->get("yawconvergencetime");
    t->yawConvergenceTime = yc.isDefined() ? spawnOrWeapon(e.obj, "yawconvergencetime", w, "yawconvergencetime")
                                           : spawnOrWeapon(e.obj, "convergencetime", w, "yawconvergencetime");
    t->pitchConvergenceTime = spawnOrWeapon(e.obj, "pitchconvergencetime", w, "pitchconvergencetime");
    t->suppressionTime = spawnOrWeapon(e.obj, "suppressiontime", w, "suppressiontime");
    t->aiSpread = spawnOrWeapon(e.obj, "aispread", w, "aispread");
    const float range = spawnOrWeapon(e.obj, "maxrange", w, "maxrange");
    t->maxRangeSq = range <= 0.0f ? 3.4028235e38f : range * range;
    t->playerSpread = spawnOrWeapon(e.obj, "playerspread", w, "playerspread");
    // stance (0x51df5d: weapondef +0x8c, field table 0x5a8e40 "stance", enum 0x5a7e64 stand / duck / prone).
    if (auto it = w->find("stance"); it != w->end()) t->stance = it->second == "prone" ? 2 : it->second == "duck" ? 1 : 0;
    e.turret = std::move(t);
}

// misc_turret and misc_mg42 need a weaponinfo key (spawn function 0x51e650).
void World::initMapTurrets() {
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (e.classname != "misc_turret" && e.classname != "misc_mg42") continue;
        const Value wi = e.obj->get("weaponinfo");
        if (!wi.isString()) {
            print("misc_turret without a weaponinfo");
            continue;
        }
        try {
            initTurret(e, wi.asString());
        } catch (const std::exception& ex) {
            print(ex.what());
        }
    }
}

TurretState& World::turretOf(const Value& self) {
    Entity* e = entityOf(self);
    if (!e) throw std::runtime_error("not an entity");
    if (!e->turret) throw std::runtime_error("entity type [" + e->classname + "] is not a turret");
    return *e->turret;
}

void World::registerTurretBuiltins() {
    // spawnturret(classname, origin, weaponinfo) (0x4ed0c0).
    vm_.registerBuiltin("spawnturret", [this](CallContext& c) {
        Entity& e = spawnEntity(c.arg(0).toString());
        if (c.arg(1).isVector()) e.obj->set("origin", c.arg(1));
        initTurret(e, c.arg(2).toString());
        return Value(e.obj);
    });
    // setmode(mode) (0x4f01f0): "auto_ai" sets flags 1|2, "manual" clears both, "manual_ai" sets 1, clears 2,
    // "auto_nonai" sets 2, clears 1.
    vm_.registerBuiltin("setmode", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        const std::string m = c.arg(0).toString();
        if (m == "auto_ai") t.flags |= 3u;
        else if (m == "manual") t.flags &= ~3u;
        else if (m == "manual_ai") t.flags = (t.flags & ~2u) | 1u;
        else if (m == "auto_nonai") t.flags = (t.flags & ~1u) | 2u;
        else throw std::runtime_error("unknown turret mode " + m);
        return Value();
    });
    vm_.registerBuiltin("getturretowner", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        return t.inUse && t.owner ? Value(t.owner) : Value();
    });
    vm_.registerBuiltin("isturretactive", [this](CallContext& c) {
        Entity* e = entityOf(c.arg(0));
        if (!e || !e->turret) throw std::runtime_error("entity is not a turret");
        return Value(e->turret->inUse);
    });
    vm_.registerBuiltin("startfiring", [this](CallContext& c) { turretOf(c.self).flags |= 4u; return Value(); });
    vm_.registerBuiltin("stopfiring", [this](CallContext& c) { turretOf(c.self).flags &= ~4u; return Value(); });
    // Whether the turret think has it aiming or firing (turret +0x4 ≠ 0, 0x4eea60).
    vm_.registerBuiltin("isfiringturret", [this](CallContext& c) { return Value(turretOf(c.self).state != 0); });
    // settargetentity(ent) stores the script target (+0x10); cleartargetentity clears it.
    vm_.registerBuiltin("settargetentity", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        if (!entityOf(c.arg(0))) throw std::runtime_error("not an entity");
        t.target = c.arg(0).asObject();
        return Value();
    });
    vm_.registerBuiltin("cleartargetentity", [this](CallContext& c) { turretOf(c.self).target.reset(); return Value(); });
    // getturrettarget: the target being engaged (+0x14 with flag 0x40), chosen by the turret think.
    vm_.registerBuiltin("getturrettarget", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        return (t.flags & 0x40u) && t.engaged ? Value(t.engaged) : Value();
    });
    vm_.registerBuiltin("setaispread", [this](CallContext& c) { turretOf(c.self).aiSpread = c.arg(0).asFloat(); return Value(); });
    // setconvergencetime(time, ["yaw" | "pitch"]) (0x4f05f0; default yaw).
    vm_.registerBuiltin("setconvergencetime", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        const std::string axis = c.argc() > 1 ? c.arg(1).toString() : "yaw";
        if (axis == "pitch") t.pitchConvergenceTime = c.arg(0).asFloat();
        else if (axis == "yaw") t.yawConvergenceTime = c.arg(0).asFloat();
        else throw std::runtime_error("setconvergencetime: axis must be \"yaw\" or \"pitch\"");
        return Value();
    });
    vm_.registerBuiltin("setsuppressiontime", [this](CallContext& c) { turretOf(c.self).suppressionTime = c.arg(0).asFloat(); return Value(); });
    vm_.registerBuiltin("setturretteam", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        const std::string team = c.arg(0).toString();
        if (team == "axis") t.team = 1;
        else if (team == "allies") t.team = 2;
        else throw std::runtime_error("bad team " + team);
        return Value();
    });
    vm_.registerBuiltin("setturretignoregoals", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        if (c.arg(0).truthy()) t.flags |= 0x2000u;
        else t.flags &= ~0x2000u;
        return Value();
    });
    vm_.registerBuiltin("maketurretusable", [this](CallContext& c) { turretOf(c.self).flags |= 0x1000u; return Value(); });
    // setcursorhint (0x4f0c50): HINT_NONE 1, HINT_ACTIVATE 2, HINT_HEALTH 3, HINT_FRIENDLY 4 (table 0x5a965c) into
    // +0x98. TODO(spec): HINT_INHERIT (trigger_use only) and the weapon-name hints.
    vm_.registerBuiltin("setcursorhint", [](CallContext& c) {
        static const char* const kHints[] = {"HINT_NONE", "HINT_ACTIVATE", "HINT_HEALTH", "HINT_FRIENDLY"};
        const std::string h = c.arg(0).toString();
        for (int i = 0; i < 4; ++i)
            if (h == kHints[i]) c.self.asObject()->set("$cursorhint", Value(int32_t(i + 1)));
        return Value();
    });
    // sethintstring (0x4f0ec0): trigger_use / trigger_use_touch (+3) or actors (+0x2124); "" clears it.
    // TODO(spec): the extra parameters appended to the string (0x4eb9b0 "Hint String").
    vm_.registerBuiltin("sethintstring", [](CallContext& c) {
        const Value v = c.arg(0);
        std::string key;
        if (std::holds_alternative<script::LocalizedString>(v.v)) key = std::get<script::LocalizedString>(v.v).key;
        else key = v.toString();
        c.self.asObject()->set("$hintstring", key.empty() ? Value() : Value(key));
        return Value();
    });
    vm_.registerBuiltin("maketurretunusable", [this](CallContext& c) { turretOf(c.self).flags &= ~0x1000u; return Value(); });
    // Arcs (0x4f8440…0x4f85f0): right and top are stored negated and clamped to ≤ 0, left and bottom to ≥ 0.
    vm_.registerBuiltin("setrightarc", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        t.rightArc = -c.arg(0).asFloat();
        if (t.rightArc > 0.0f) t.rightArc = 0.0f;
        return Value();
    });
    vm_.registerBuiltin("setleftarc", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        t.leftArc = c.arg(0).asFloat();
        if (t.leftArc < 0.0f) t.leftArc = 0.0f;
        return Value();
    });
    vm_.registerBuiltin("settoparc", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        t.topArc = -c.arg(0).asFloat();
        if (t.topArc > 0.0f) t.topArc = 0.0f;
        return Value();
    });
    vm_.registerBuiltin("setbottomarc", [this](CallContext& c) {
        TurretState& t = turretOf(c.self);
        t.bottomArc = c.arg(0).asFloat();
        if (t.bottomArc < 0.0f) t.bottomArc = 0.0f;
        return Value();
    });
    vm_.registerBuiltin("setdefaultdroppitch", [this](CallContext& c) { turretOf(c.self).dropPitch = c.arg(0).asFloat(); return Value(); });
    // restoredefaultdroppitch is a jump to 0x51cf10 (0x4f8744).
    vm_.registerBuiltin("restoredefaultdroppitch", [this](CallContext& c) {
        turretOf(c.self);
        restoreDefaultDropPitch(*entityOf(c.self));
        return Value();
    });
    // shootturret fires once (0x51b250 → 0x51ada0, src/game/turretthink.cpp).
    vm_.registerBuiltin("shootturret", [this](CallContext& c) {
        turretOf(c.self);
        shootTurret(*entityOf(c.self));
        return Value();
    });

    // Gunner methods on actors (0x53f100…0x53f8e0).
    auto actorOf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        return *e;
    };
    auto turretArg = [this](CallContext& c) -> Entity& {
        Entity* t = entityOf(c.arg(0));
        if (!t) throw std::runtime_error("not an entity");
        if (!t->turret) throw std::runtime_error("entity is not a turret");
        return *t;
    };
    // Whether the actor may use the turret (0x51d6d0): yes if it already owns it, no if someone else is using it.
    // TODO: with an enemy, 0x51d3c0 / 0x51d640 also check the enemy's distance and visibility against the turret;
    // here an actor may always take a free turret.
    auto canUse = [](Entity& actor, Entity& turret) {
        if (actor.usingTurret == turret.obj && turret.turret->owner == actor.obj && turret.turret->inUse) return true;
        return !turret.turret->inUse;
    };
    vm_.registerBuiltin("canuseturret", [actorOf, turretArg, canUse](CallContext& c) {
        Entity& a = actorOf(c);
        return Value(canUse(a, turretArg(c)));
    });
    vm_.registerBuiltin("useturret", [actorOf, turretArg, canUse](CallContext& c) {
        Entity& a = actorOf(c);
        Entity& t = turretArg(c);
        if (!canUse(a, t)) {
            a.usingTurret.reset();
            return Value(false); // returns the 0x51d6d0 result (0x53f1c1…0x53f1ec)
        }
        // Only records the turret (actor +0x2114); the actor's turret state takes it on its next think
        // (0x5496f0 → state 2, 0x549960).
        a.usingTurret = t.obj;
        return Value(true);
    });
    // stopuseturret (0x53f200 → 0x549a60): frees the turret and notifies "turretownerchange".
    vm_.registerBuiltin("stopuseturret", [this, actorOf](CallContext& c) {
        Entity& a = actorOf(c);
        Entity* t = a.usingTurret ? entityOf(Value(a.usingTurret)) : nullptr;
        if (t && t->turret && t->turret->owner == a.obj) releaseTurret(*t);
        a.usingTurret.reset();
        return Value();
    });
    vm_.registerBuiltin("getturret", [this, actorOf](CallContext& c) {
        Entity& a = actorOf(c);
        Entity* t = a.usingTurret ? entityOf(Value(a.usingTurret)) : nullptr;
        if (t && t->turret && t->turret->owner == a.obj) return Value(t->obj);
        return Value();
    });
    // setturretanim (0x53f860): the animation's node in the actor's tree (0x46e530) at actor +0x2118, flag +0x211a = 1.
    vm_.registerBuiltin("setturretanim", [actorOf](CallContext& c) {
        Entity& a = actorOf(c);
        if (!std::holds_alternative<AnimRef>(c.arg(0).v)) throw std::runtime_error("expected an animation, got " + c.arg(0).typeName());
        const std::string& name = std::get<AnimRef>(c.arg(0).v).name;
        const int n = a.anim ? a.anim->tree().find(name) : -1;
        if (n < 0) throw std::runtime_error("animation " + name + " is not in animtree " + a.animTree);
        a.turretAnim = n;
        a.turretAnimSet = true;
        return Value();
    });
}

} // namespace iw2::game
