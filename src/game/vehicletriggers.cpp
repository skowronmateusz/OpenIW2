// G_TouchTriggers (0x4e7320) for script vehicles. The vehicle think (0x512b80) calls it right after the solid touch
// 0x50ff00 (call at 0x512c50). For an entity with a vehicle pointer at +0x108 the area mask is 8 (0x4e7329..0x4e733c);
// trigger_multiple / trigger_once / trigger_radius carry that contents bit exactly when spawnflags & 16 is set
// (0x528dc0: `testb $0x10` -> `orl $0x8, contents`). A contact (SV_EntityContact 0x447830 of the box origin + r.mins ..
// origin + r.maxs against the trigger) runs the trigger's touch (0x528cb0) every frame it lasts, which sends the script
// notify "trigger" with the vehicle as the toucher; `toujane` uses it: the vehicle trigger "killtank" starts the
// opening's flak shot at the lead tank.
// The vehicle's r.mins / r.maxs are those of its collmap brush model (SV_SetBrushModel, docs/spec/vehicles.md "Collmap");
// a vehicle without a collmap has a zero box.
// TODO(spec): trigger_hurt (contents include 8 but its touch 0x5291d0 acts on a damageable toucher), trigger_friendlychain
// and the rotated bounds of the vehicle box (unrotated model bounds used, as the original adds r.mins to the origin).
#include "world.h"

namespace iw2::game {

void World::vehicleTouchTriggers(Entity& v) {
    const Value ov = v.obj->get("origin");
    if (!ov.isVector()) return;
    const Vec3 o = ov.asVector();
    Vec3 mins{0, 0, 0}, maxs{0, 0, 0};
    const std::string brush = vehicleCollmap(v);
    if (!brush.empty()) submodelBounds(brush, mins, maxs);
    const Vec3 amin{o[0] + mins[0], o[1] + mins[1], o[2] + mins[2]}, amax{o[0] + maxs[0], o[1] + maxs[1], o[2] + maxs[2]};
    std::vector<Entity*> fired;
    for (const auto& e : entities_) {
        if (e->obj->removed || e->isSpawner) continue;
        if (e->classname != "trigger_multiple" && e->classname != "trigger_once" && e->classname != "trigger_radius") continue;
        if (e->obj->get("$disabled").truthy()) continue;
        const Value sf = e->obj->get("spawnflags");
        if (!sf.isNumber() || !(sf.asInt() & 16)) continue;
        if (triggerContact(*e, amin, amax)) fired.push_back(e.get());
    }
    for (Entity* e : fired) {
        if (e->obj->removed || v.obj->removed) continue;
        vm_.notify(e->obj, "trigger", {Value(v.obj)});
        if (e->classname == "trigger_once") e->obj->removed = true; // as for the player (inferred)
    }
}

} // namespace iw2::game
