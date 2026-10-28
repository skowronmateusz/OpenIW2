// G_TouchTriggers (0x4e7320) for script vehicles. The vehicle think (0x512b80) calls it right after the solid touch
// 0x50ff00 (call at 0x512c50). For an entity with a vehicle pointer at +0x108 the area mask is 8 (0x4e7329..0x4e733c);
// trigger_multiple / trigger_once / trigger_radius carry that contents bit exactly when spawnflags & 16 is set
// (0x528dc0: `testb $0x10` -> `orl $0x8, contents`). A contact (SV_EntityContact 0x447830 of the box origin + r.mins ..
// origin + r.maxs against the trigger) runs the trigger's touch (0x528cb0) every frame it lasts, which sends the script
// notify "trigger" with the vehicle as the toucher; `toujane` uses it: the vehicle trigger "killtank" starts the
// opening's flak shot at the lead tank.
// The vehicle's r.mins / r.maxs are those of its collmap brush model (SV_SetBrushModel, docs/spec/vehicles.md "Collmap");
// a vehicle without a collmap has a zero box.
// trigger_hurt (spawn 0x5292a0: contents 0x405c0008, so it always matches mask 8): touch 0x5291d0 needs the toucher's
// take-damage (+0x111); spawnflags 2 skips only a toucher with +0x100 set (a vehicle has none); it waits for +0x15c
// (spawnflags 0x10: +1000 ms, else +50 ms), records the touch (0x528cb0), then G_Damage(vehicle, trigger, trigger, dmg,
// flags (spawnflags & 8) << 2, MOD_TRIGGER_HURT) and spawnflags 0x20 switches it off: the same as for the player.
// The box is the unrotated r.mins / r.maxs added to the origin (0x4e7440..0x4e7472 read ent+0xa8..0xbc, no rotation);
// that these are the unrotated model bounds is inferred.
// TODO(spec): trigger_friendlychain: its touch 0x529140 passes the toucher's sentient (+0x104) to 0x518bf0, which
// dereferences it (+0x30) when a squad mode flag is on, so it cannot be meant for a vehicle (null); not run for vehicles.
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
        if (e->classname == "trigger_hurt") {
            if (e->obj->get("$hurtOff").truthy() || !takesDamage(v) || !triggerContact(*e, amin, amax)) continue;
            const Value nt = e->obj->get("$hurtNext");
            if (nt.isNumber() && nt.asInt() > timeMs_) continue;
            const int flags = e->obj->get("spawnflags").isNumber() ? e->obj->get("spawnflags").asInt() : 0;
            e->obj->set("$hurtNext", Value(int32_t(timeMs_ + ((flags & 0x10) ? 1000 : 50))));
            vm_.notify(e->obj, "trigger", {Value(v.obj)});
            const Value dv = e->obj->get("dmg");
            damage(v, e.get(), dv.isNumber() ? dv.asInt() : 5, Vec3{0, 0, 0}, Vec3{0, 0, 0}, "MOD_TRIGGER_HURT");
            if (flags & 0x20) e->obj->set("$hurtOff", Value(true));
            if (v.obj->removed) return;
            continue;
        }
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
