// Solid brush-model entities as collision clips for every trace (docs/spec/collision.md, "Entities").
#include "game/world.h"

#include <cstdlib>

namespace iw2::game {

// Brush models of script_brushmodel and script_vehicle_collmap entities, at their current origin and angles, with
// their brushes' contents; `notsolid` removes one. Living actors and the player as boxes (model 0x3ff) with their
// spawn bounds and contents (FUN_004fc640, 0x4e90e2); actors also have hit boxes (ent+0xa2 |= 2).
// TODO: the entity contents set at spawn and by solid() for brush models are not read; the player's r.maxs follow
// the stance in the original (pmove), OpenIW2 keeps the standing box; corpses are not clips.
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
            c.contents = e.isActor ? Contents::ActorBody : Contents::PlayerBody;
            c.hitBoxes = e.isActor;
            clips.push_back(c);
            continue;
        }
        if (e.obj->removed || (e.classname != "script_brushmodel" && e.classname != "script_vehicle_collmap")) continue;
        if (e.obj->get("$notsolid").truthy()) continue;
        const Value m = e.obj->get("model");
        if (!m.isString() || m.asString().size() < 2 || m.asString()[0] != '*') continue;
        EntityClip c;
        c.id = e.number;
        c.model = std::atoi(m.asString().c_str() + 1);
        const Value o = e.obj->get("origin"), a = e.obj->get("angles");
        if (o.isVector()) c.origin = o.asVector();
        if (a.isVector()) c.angles = a.asVector();
        c.contents = collision_->modelContents(c.model);
        if (c.contents) clips.push_back(c);
    }
    collision_->setEntityClips(std::move(clips));
}

} // namespace iw2::game
