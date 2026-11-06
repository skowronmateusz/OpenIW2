// G_TouchTriggers (0x4e7320) for actors (docs/spec/entities.md "Trigger touch"). The actor frame calls it for every actor that
// is alive (0x4ff260, actor +0x25c), moving or not, after the move and prone steps. The area mask comes from the sentient team:
// axis 0x40000, allies 0x80000, neutral 0x100000; any other team touches nothing. Trigger contents (0x528dc0) hold those bits
// for spawnflags 1 / 2 / 4. trigger_hurt has the fixed contents 0x405c0008, which match every team, and skips actors with
// spawnflags 2 (0x5291ec). The touch only records the pair (0x528cb0); the notifies go out in the next frame's stage 3.
#include "world.h"

#include <algorithm>

namespace iw2::game {

namespace {
int teamTriggerBit(const Value& team) {
    if (!team.isString()) return 0;
    const std::string& t = team.asString();
    return t == "axis" ? 1 : t == "allies" ? 2 : t == "neutral" ? 4 : 0;
}
} // namespace

void World::actorTouchTriggers(Entity& a) {
    const int bit = teamTriggerBit(a.obj->get("team"));
    const Value ov = a.obj->get("origin");
    if (!bit || !ov.isVector()) return;
    if (actorTriggersFrame_ != frameNumber_) { // the triggers any team can touch, once per frame
        actorTriggersFrame_ = frameNumber_;
        actorTriggers_.clear();
        for (const auto& e : entities_) {
            if (e->obj->removed || e->isSpawner) continue;
            const Value sf = e->obj->get("spawnflags");
            const int flags = sf.isNumber() ? sf.asInt() : 0;
            const bool hurt = e->classname == "trigger_hurt" && !(flags & 2);
            const bool plain = (e->classname == "trigger_multiple" || e->classname == "trigger_once" || e->classname == "trigger_radius" ||
                                e->classname == "trigger_disk" || e->classname == "trigger_friendlychain") &&
                               (flags & 7);
            if (hurt || plain) actorTriggers_.push_back(e.get());
        }
    }
    const Vec3 o = ov.asVector();
    const Vec3 amin{o[0] - 15, o[1] - 15, o[2]}, amax{o[0] + 15, o[1] + 15, o[2] + 72}; // the actor box (0x4fc640)
    for (size_t i = 0; i < actorTriggers_.size(); ++i) {
        Entity& t = *actorTriggers_[i];
        if (t.obj->removed || t.obj->get("$disabled").truthy()) continue;
        const Value sf = t.obj->get("spawnflags");
        const int flags = sf.isNumber() ? sf.asInt() : 0;
        if (t.classname == "trigger_hurt") {
            // Touch 0x5291d0, as for the player and for vehicles.
            if (t.obj->get("$hurtOff").truthy() || !takesDamage(a) || !triggerContact(t, amin, amax)) continue;
            const Value nt = t.obj->get("$hurtNext");
            if (nt.isNumber() && nt.asInt() > timeMs_) continue;
            t.obj->set("$hurtNext", Value(int32_t(timeMs_ + ((flags & 0x10) ? 1000 : 50))));
            actorTouches_.push_back({t.obj, a.obj});
            const Value dv = t.obj->get("dmg");
            damage(a, &t, dv.isNumber() ? dv.asInt() : 5, Vec3{0, 0, 0}, Vec3{0, 0, 0}, "MOD_TRIGGER_HURT");
            if (flags & 0x20) t.obj->set("$hurtOff", Value(true));
            if (a.obj->removed || a.ai.corpse) return;
            continue;
        }
        if (!(flags & bit) || !triggerContact(t, amin, amax)) continue;
        if (t.classname == "trigger_friendlychain") touchFriendlyChain(t, a); // touch 0x529140: at once, then the record
        actorTouches_.push_back({t.obj, a.obj});
    }
}

// Stage 3 (0x4dab00): a record whose trigger or toucher was freed is dropped; a trigger gets one notify per pass (the pass mark
// 0x1c628bc), a further record of it waits for the next pass, and the woken threads run between passes.
void World::deliverActorTouches() {
    while (!actorTouches_.empty()) {
        std::vector<ActorTouch> later;
        std::vector<const script::Object*> done;
        const std::vector<ActorTouch> pass = std::exchange(actorTouches_, {});
        for (const ActorTouch& r : pass) {
            if (r.trigger->removed || r.toucher->removed) continue;
            if (std::find(done.begin(), done.end(), r.trigger.get()) != done.end()) {
                later.push_back(r);
                continue;
            }
            done.push_back(r.trigger.get());
            vm_.notify(r.trigger, "trigger", {Value(r.toucher)});
            // trigger_once (spawnflags 0x20 set at spawn 0x529300): removed by its touch 0x528e50, as for the player.
            if (r.trigger->kind == "trigger_once")
                if (Entity* t = entityOf(Value(r.trigger))) freeEntity(*t);
        }
        vm_.drainReady();
        actorTouches_ = std::move(later);
    }
}

} // namespace iw2::game
