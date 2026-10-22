// Solid brush-model entities as collision clips for every trace (docs/spec/collision.md, "Entities").
#include "game/world.h"

#include <cstdlib>

namespace iw2::game {

// Brush models of script_brushmodel and script_vehicle_collmap entities, at their current origin and angles, with
// their brushes' contents; `notsolid` removes one. TODO: the entity contents set at spawn and by solid() in the
// original are not read; other clip models (actor/player boxes, model 0x3ff) are not included.
void World::syncEntityClips() {
    std::vector<EntityClip> clips;
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
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
