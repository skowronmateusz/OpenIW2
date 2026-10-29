// Solid brush-model entities as collision clips for every trace (docs/spec/collision.md, "Entities").
#include "game/world.h"

#include <cctype>
#include <cstdlib>

namespace iw2::game {

// Brush models of script_brushmodel and script_vehicle_collmap entities, at their current origin and angles, with
// their brushes' contents; `notsolid` removes one. Living actors and the player as boxes (model 0x3ff) with their
// spawn bounds and contents (FUN_004fc640, 0x4e90e2); actors also have hit boxes (ent+0xa2 |= 2).
// TODO: the entity contents set at spawn and by solid() for brush models are not read; the player's r.maxs follow
// the stance in the original (pmove), OpenIW2 keeps the standing box; corpses are not clips.
// 0x50d270: the in-use script_vehicle_collmap whose targetname equals the vehicle's model name (case-insensitive);
// its brush model ("*N"), or empty (an empty brush model warns "Cannot use empty vehicle collmap for [%s]").
// The lookup (0x50dc40) runs when a vehicle is spawned or the level loaded, never on setmodel (its only callers are 0x511a6f
// and 0x511c5b): the first result is kept, so a destroyed vehicle's wreck model (setmodel to the _d model, which has no
// collmap of its own) keeps the brush model and contents of the intact vehicle. A model without a collmap leaves the
// vehicle's brush model unchanged (0x50dc6f: nothing found -> return).
std::string World::vehicleCollmap(Entity& v) {
    if (v.vehicle && v.vehicle->collmapSet) return v.vehicle->collmap;
    const std::string brush = lookupVehicleCollmap(v);
    if (v.vehicle) {
        v.vehicle->collmap = brush;
        v.vehicle->collmapSet = true;
    }
    return brush;
}

std::string World::lookupVehicleCollmap(Entity& v) {
    const Value mv = v.obj->get("model");
    if (!mv.isString()) return {};
    const std::string model = mv.asString();
    auto cached = collmapByModel_.find(model);
    if (cached != collmapByModel_.end()) return cached->second;
    std::string brush;
    for (const auto& ep : entities_) {
        const Entity& c = *ep;
        if (c.obj->removed || c.classname != "script_vehicle_collmap") continue;
        const Value tn = c.obj->get("targetname");
        if (!tn.isString() || tn.asString().size() != model.size()) continue;
        bool same = true;
        for (size_t i = 0; i < model.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(model[i])) == std::tolower(static_cast<unsigned char>(tn.asString()[i]));
        if (!same) continue;
        const Value bm = c.obj->get("model");
        if (bm.isString() && bm.asString().size() > 1 && bm.asString()[0] == '*') brush = bm.asString();
        else print("WARNING: Cannot use empty vehicle collmap for [" + model + "]");
        break;
    }
    collmapByModel_[model] = brush;
    return brush;
}

// freevehicle (0x513850) turns the vehicle into a script_vehicle_corpse: 0x511c80 clears the vehicle info (+0x108), health, takedamage
// and the trajectories but writes neither the contents (+0xc0) nor the brush model, and the entity stays linked, so a wreck keeps
// blocking with the collmap of the intact vehicle (see "Collision after destruction", docs/spec/vehicles.md). The brush model is
// kept on the object by freevehicle.
std::string World::corpseCollmap(const Entity& e) {
    if (e.vehicle || e.classname != "script_vehicle_corpse") return {};
    const Value v = e.obj->get("$collmap");
    return v.isString() ? v.asString() : std::string();
}

bool World::isScriptMover(const Entity& e) {
    return e.classname == "script_model" || e.classname == "script_brushmodel" || e.classname == "script_origin";
}

// The gentity's contents word (+0xc0) as the exe has it: what setcontents stored (0x4efd70), else the spawn value: actors 0x4000
// (0x4fc74f), the player 0x2000000 (0x4e90e2), script_brushmodel 1 (0x533440), script_model 0x2080 (0x533480), script_origin 0
// (0x5334d0); `solid` restores the model / brush value and `notsolid` clears it (0x533f70 / 0x534050).
// TODO(spec): other entity classes (their spawn contents are not examined; 0 here).
uint32_t World::entityContents(const Entity& e) const {
    const Value c = e.obj->get("$contents");
    if (c.isNumber()) return uint32_t(c.asInt());
    if (e.isActor) return Contents::ActorBody;
    if (e.isPlayer) return Contents::PlayerBody;
    if (e.obj->get("$notsolid").truthy()) return 0;
    if (e.classname == "script_brushmodel") return 1;
    if (e.classname == "script_model") return 0x2080;
    // script_vehicle init 0x50d820: contents 0x2080 (0x202080 with spawnflag 1), then the collmap (0x50dc40) gives 0x800000 (0xa00000).
    if (e.vehicle) {
        const Value sf = e.obj->get("spawnflags");
        const bool one = sf.isNumber() && (sf.asInt() & 1);
        if (e.vehicle->collmapSet && !e.vehicle->collmap.empty()) return one ? 0xa00000u : 0x800000u;
        return one ? 0x202080u : 0x2080u;
    }
    // freevehicle (0x513850 -> 0x511c80) keeps the contents word and the brush model.
    if (corpseCollmap(e).size() > 1) {
        const Value sf = e.obj->get("spawnflags");
        return (sf.isNumber() && (sf.asInt() & 1)) ? 0xa00000u : 0x800000u;
    }
    return 0;
}

void World::syncEntityClips() {
    std::vector<EntityClip> clips;
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
        // A dying actor (death state, not yet a corpse) keeps its box: the living contents until the death think writes
        // 0x4002000 (actorstate.cpp deathThink, 0x548bc0).
        const bool dying = e.isActor && !e.isSpawner && !e.obj->removed && !e.ai.corpse && !isAlive(e) && e.ai.stack[size_t(e.ai.depth)] == AiDeath;
        if (((e.isActor || e.isPlayer) && isAlive(e)) || dying) {
            const Value o = e.obj->get("origin");
            if (!o.isVector()) continue;
            EntityClip c;
            c.id = e.number;
            c.origin = o.asVector();
            c.mins = {-15, -15, 0};
            // the player's r.maxs is the pmove box of its stance (ClientThink 0x4e7d68..0x4e7da2, linked at 0x4e7dae)
            c.maxs = {15, 15, e.isActor ? 72.0f : playerBoxHeight()};
            c.contents = e.isActor && e.ai.dyingContents ? e.ai.dyingContents : entityContents(e); // 0xc0: 0x4000 actor (0x4fc74f), 0x2000000 player (0x4e90e2), what setcontents stored, or the death think's 0x4002000 (0x548bc0)
            if (!c.contents) continue;      // 0x44b2d0 unlinks an entity without contents: no trace sees it
            c.hitBoxes = e.isActor;
            clips.push_back(c);
            continue;
        }
        // A vehicle takes the brush model of the script_vehicle_collmap whose targetname is its model name, contents
        // 0x800000 (0xa00000 with spawnflag 1) (0x50dc40 after level load 0x4d9c6f / 0x511a6f and at vehicle spawn
        // 0x511c5b, never on setmodel). The collmap entities themselves have no contents (0x512fd0).
        const std::string corpseBrush = e.vehicle ? std::string() : corpseCollmap(e);
        if ((e.vehicle || corpseBrush.size() > 1) && !e.obj->removed) {
            const std::string brush = e.vehicle ? vehicleCollmap(*ep) : corpseBrush;
            if (brush.empty()) continue;
            EntityClip c;
            c.id = e.number;
            c.model = std::atoi(brush.c_str() + 1);
            const Value o = e.obj->get("origin"), a = e.obj->get("angles");
            if (o.isVector()) c.origin = o.asVector();
            if (a.isVector()) c.angles = a.asVector();
            const Value sf = e.obj->get("spawnflags");
            c.contents = (sf.isNumber() && (sf.asInt() & 1)) ? 0xa00000u : 0x800000u;
            c.collSurfs = true; // ent+0xa2 = 4 (0x50d844)
            clips.push_back(c);
            continue;
        }
        // trigger_lookat (spawn 0x529930): the brush model with contents 0x20000000, hit by the look trace of
        // updateCrosshairTarget (0x535070, mask 0x2280e801).
        if (e.classname == "trigger_lookat" && !e.obj->removed) {
            const Value m = e.obj->get("model");
            if (!m.isString() || m.asString().size() < 2 || m.asString()[0] != '*') continue;
            EntityClip c;
            c.id = e.number;
            c.model = std::atoi(m.asString().c_str() + 1);
            const Value o = e.obj->get("origin"), a = e.obj->get("angles");
            if (o.isVector()) c.origin = o.asVector();
            if (a.isVector()) c.angles = a.asVector();
            c.contents = 0x20000000u;
            clips.push_back(c);
            continue;
        }
        if (e.obj->removed || e.classname != "script_brushmodel") continue;
        if (e.obj->get("$notsolid").truthy()) continue;
        const Value m = e.obj->get("model");
        if (!m.isString() || m.asString().size() < 2 || m.asString()[0] != '*') continue;
        EntityClip c;
        c.id = e.number;
        c.model = std::atoi(m.asString().c_str() + 1);
        const Value o = e.obj->get("origin"), a = e.obj->get("angles");
        if (o.isVector()) c.origin = o.asVector();
        if (a.isVector()) c.angles = a.asVector();
        c.contents = e.obj->get("$contents").isNumber() ? entityContents(e) : collision_->modelContents(c.model);
        if (c.contents) clips.push_back(c);
    }
    collision_->setEntityClips(std::move(clips));
}

} // namespace iw2::game
