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
// The goal volume (+0x778, setgoalvolume) contains the point (0x447830 with the point as a zero-size box); true without one.
bool World::goalVolumeHolds(Entity& e, const Vec3& p) {
    Entity* vol = entityOf(e.obj->get("$goalvolume"));
    return !vol || triggerContact(*vol, p, p);
}

// The working goal's volume (+0x75c, 0x505f70): the script volume while the working goal is the script's goal (mode 0), none
// for a goal entity or the path to the enemy.
bool World::workGoalVolumeHolds(Entity& e, const Vec3& p) {
    if (e.workGoal.set && !e.workGoal.scriptVolume) return true;
    return goalVolumeHolds(e, p);
}

// 0x5026d0 (extra 0): the height and the radius around the goal position, then the goal volume (0x447830) when set.
// With an extra radius (0x502760) there is no volume test.
bool World::actorInGoal(Entity& e, float extra) {
    Vec3 gp{};
    float r = 0, h = 0;
    if (!actorGoal(e, gp, r, h)) return true;
    const Vec3 o = e.obj->get("origin").asVector();
    const float dz = gp[2] - o[2];
    if (h * h < dz * dz) return false;
    const float dx = gp[0] - o[0], dy = gp[1] - o[1];
    if (!(dx * dx + dy * dy <= (extra + r) * (extra + r))) return false;
    return extra != 0.0f || workGoalVolumeHolds(e, o);
}

// Start (0x549960): take the recorded turret unless someone else uses it. TODO(spec): the actor is linked to the
// turret there (0x4d6540).
bool World::turretStateStart(Entity& e) {
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    if (!t || !t->turret || t->obj->removed) {
        e.usingTurret.reset();
        return false;
    }
    TurretState& ts = *t->turret;
    if (ts.inUse && ts.owner != e.obj) {
        e.usingTurret.reset();
        return false;
    }
    ts.inUse = true;
    ts.owner = e.obj;
    vm_.notify(t->obj, "turretownerchange");
    turretIdle(*t); // 0x51b850
    ts.flags = (ts.flags & ~0xe8u) | 0x100u;
    ts.rightArc = ts.setupRightArc;
    ts.leftArc = ts.setupLeftArc;
    ts.flags &= ~0x10u;
    return true;
}

// Finish (0x549b20): release the turret this actor owns.
void World::turretStateFinish(Entity& e) {
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    if (t && t->turret && t->turret->inUse && t->turret->owner == e.obj) releaseTurret(*t);
    e.usingTurret.reset();
}

// Think (0x54aba0). Leaving (the turret is gone or not ours, the actor is outside its goal + 92 unless the turret
// ignores goals): release it and replace the state with exposed, returning 1 (think again). Otherwise run the
// turret weapon's animscript (weapon key "script", e.g. "mg42/stand"; turret script table 0x1c7e128).
int World::turretStateThink(Entity& e) {
    clearActorPath(e);
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    auto leave = [&] {
        turretStateFinish(e);
        aiSetState(e, AiExposed);
        return 1;
    };
    if (!t || !t->turret || t->obj->removed || !(t->turret->inUse && t->turret->owner == e.obj)) return leave();
    TurretState& ts = *t->turret;
    // 0x54acc5..0x54ad0c: unless the claimed node is kept (0x4fe6f0), the friendly chain and the working goal are updated
    // (0x505ed0, 0x505f70). TODO(spec): the pathToEnemy reset before it (0x502fc0, an enemy known within 5 s).
    if (!keepClaimedNode(e)) {
        followFriendlyChain(e);
        updateWorkingGoal(e);
    }
    if (!(ts.flags & 0x2000u) && !actorInGoal(e, 92.0f)) return leave();
    actorPerceive(e);
    const auto* w = weapon(ts.weapon);
    auto it = w ? w->find("script") : decltype(w->end()){};
    if (!w || it == w->end() || it->second.empty()) {
        print("turret weapon " + ts.weapon + " has no script");
        return leave();
    }
    // TODO(spec): the movemode/anim mode arguments of this script start are not read yet.
    if (e.animScript != it->second) setActorAnimScript(e, it->second, 0, e.ai.animMode);
    return 0;
}

} // namespace iw2::game
