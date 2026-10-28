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
std::string World::vehicleCollmap(Entity& v) {
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
    return 0;
}

void World::syncEntityClips() {
    std::vector<EntityClip> clips;
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
        if ((e.isActor || e.isPlayer) && isAlive(e)) {
            const Value o = e.obj->get("origin");
            if (!o.isVector()) continue;
            EntityClip c;
            c.id = e.number;
            c.origin = o.asVector();
            c.mins = {-15, -15, 0};
            c.maxs = {15, 15, e.isActor ? 72.0f : 70.0f};
            c.contents = entityContents(e); // 0xc0: 0x4000 actor (0x4fc74f), 0x2000000 player (0x4e90e2), or what setcontents stored
            if (!c.contents) continue;      // 0x44b2d0 unlinks an entity without contents: no trace sees it
            c.hitBoxes = e.isActor;
            clips.push_back(c);
            continue;
        }
        // A vehicle takes the brush model of the script_vehicle_collmap whose targetname is its model name, contents
        // 0x800000 (0xa00000 with spawnflag 1) (0x50dc40 after level load 0x4d9c6f and on model changes 0x511a6f /
        // 0x511c5b). The collmap entities themselves have no contents (0x512fd0).
        if (e.vehicle && !e.obj->removed) {
            const std::string brush = vehicleCollmap(*ep);
            if (brush.empty()) continue;
            EntityClip c;
            c.id = e.number;
            c.model = std::atoi(brush.c_str() + 1);
            const Value o = e.obj->get("origin"), a = e.obj->get("angles");
            if (o.isVector()) c.origin = o.asVector();
            if (a.isVector()) c.angles = a.asVector();
            const Value sf = e.obj->get("spawnflags");
            c.contents = (sf.isNumber() && (sf.asInt() & 1)) ? 0xa00000u : 0x800000u;
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
