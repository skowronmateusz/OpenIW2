// The actor's turret state (AI state 2 of the state table at 0x56f720: start 0x549960, finish 0x549b20,
// think 0x54aba0; docs/spec/ai.md "AI states").
#include "game/world.h"

namespace iw2::game {

using namespace script;

// Releases the turret (0x549a60): "turret_deactivate" while it was in use, owner cleared, "turretownerchange".
void World::releaseTurret(Entity& turret) {
    TurretState& ts = *turret.turret;
    if (ts.inUse) vm_.notify(turret.obj, "turret_deactivate");
    ts.inUse = false;
    ts.owner.reset();
    ts.flags &= ~0x200u;
    vm_.notify(turret.obj, "turretownerchange");
}

// Inside the goal widened by `extra` (0x502760): height within goalheight, horizontal distance within
// goalradius + extra. Without a goal the actor counts as inside.
bool World::actorInGoal(Entity& e, float extra) {
    const Value g = e.obj->get("$goalpos");
    if (!g.isVector()) return true;
    const Vec3 gp = g.asVector(), o = e.obj->get("origin").asVector();
    const Value rv = e.obj->get("goalradius"), hv = e.obj->get("goalheight");
    const float r = rv.isNumber() ? rv.asFloat() : 0.0f, h = hv.isNumber() ? hv.asFloat() : 0.0f;
    const float dz = gp[2] - o[2];
    if (h * h < dz * dz) return false;
    const float dx = gp[0] - o[0], dy = gp[1] - o[1];
    return dx * dx + dy * dy <= (extra + r) * (extra + r);
}

// Returns true while the actor is in its turret state (the rest of the actor think is skipped).
bool World::actorTurretThink(Entity& e) {
    if (!e.usingTurret) return false;
    Entity* t = entityOf(Value(e.usingTurret));
    if (!t || !t->turret || t->obj->removed) {
        e.usingTurret.reset();
        return false;
    }
    TurretState& ts = *t->turret;
    if (!(ts.inUse && ts.owner == e.obj)) {
        if (ts.inUse) { // someone else has it: the state cannot start
            e.usingTurret.reset();
            return false;
        }
        // Start (0x549960). TODO: the actor is linked to the turret there (0x4d6540).
        ts.inUse = true;
        ts.owner = e.obj;
        vm_.notify(t->obj, "turretownerchange");
        turretIdle(*t); // 0x51b850
        ts.flags = (ts.flags & ~0xe8u) | 0x100u;
        ts.rightArc = ts.setupRightArc;
        ts.leftArc = ts.setupLeftArc;
        ts.flags &= ~0x10u;
    }
    // Think (0x54aba0): leave unless inside the goal + 92 (or ignoring goals); then run the turret weapon's
    // animscript (weapon key "script", e.g. "mg42/stand"; turret script table 0x1c7e128).
    if (!(ts.flags & 0x2000u) && !actorInGoal(e, 92.0f)) {
        releaseTurret(*t);
        e.usingTurret.reset();
        return false;
    }
    const auto* w = weapon(ts.weapon);
    auto it = w ? w->find("script") : decltype(w->end()){};
    if (!w || it == w->end() || it->second.empty()) {
        print("turret weapon " + ts.weapon + " has no script");
        releaseTurret(*t);
        e.usingTurret.reset();
        return false;
    }
    if (e.animScript != it->second) startAnimScript(e, it->second); // 0x53ac90 keeps a running script
    return true;
}

} // namespace iw2::game
