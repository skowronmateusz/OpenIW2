// The actor state machine (docs/spec/ai.md "AI states"): the state stack and its request queue, the per-frame
// actor think, the exposed / pain / death / animcustom states and the engine's choice of animscript.
#include "game/world.h"

#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

// 0x56f838: merging the last two queued requests, indexed [previous type][new type] (1 replace, 2 push, 3 pop):
// -1 keep both, 0 drop both, otherwise the previous one becomes that type with the new one's state.
constexpr int kMerge[4][4] = {{-1, -1, -1, -1}, {-1, 1, -1, 3}, {-1, 1, -1, 0}, {-1, -1, 1, -1}};
// 0x56f878: a push of state 6…9 is refused while one of these is on the predicted stack (levels 1…depth).
constexpr int kPushBlockedBy[4][4] = {{7, 9, 0, 0}, {0, 0, 0, 0}, {6, 9, 0, 0}, {7, 0, 0, 0}};
// 0x56f8b8: a push of state 6…9 first pops the predicted stack down to below the first of these.
constexpr int kPushReplaces[4][4] = {{6, 0, 0, 0}, {6, 7, 8, 9}, {6, 7, 8, 9}, {6, 7, 8, 9}};

constexpr int kSubCombat = 100, kSubNoncombat = 0x65, kSubReacquire = 0x66; // exposed substates (+0x28)
constexpr int kSubDeath = 200, kSubDeathDone = 201;

// movemode values (byte +0x214) as the script field shows them (getter 0x536960, strings 0x1c7f112…0x1c7f118).
const char* const kMovemodes[] = {"stop", "stop_soon", "walk", "run"};

bool contains(const int (&row)[4], int v) {
    for (int x : row)
        if (x == v) return true;
    return false;
}

} // namespace

// Queues a request and merges it with the previous one (0x4fe980). A dead actor (+0x25c = 0) keeps only the
// first request (the death state set by the die callback).
void World::aiRequest(Entity& e, int type, int state) {
    ActorAi& ai = e.ai;
    if (ai.requests.size() >= 11) return; // the queue holds 11 requests (+0x30 … +0x88); not handled by the original
    ai.requests.push_back({type, state});
    while (ai.requests.size() >= 2) {
        if (!ai.alive) {
            ai.requests.resize(1);
            return;
        }
        auto& prev = ai.requests[ai.requests.size() - 2];
        const auto& last = ai.requests.back();
        const int m = kMerge[prev.type][last.type];
        if (m == -1) return;
        if (m == 0) {
            ai.requests.resize(ai.requests.size() - 2);
        } else {
            prev.type = m;
            prev.state = last.state;
            ai.requests.pop_back();
        }
    }
}

// Replace the current state (0x4feb00).
void World::aiSetState(Entity& e, int state) {
    aiRequest(e, 1, state);
    e.ai.predicted[size_t(e.ai.predictedDepth)] = state;
}

// Push a state (0x4feb40): refused while a blocking state is predicted (0x4fea00); states it replaces are popped
// first (0x4fea60).
bool World::aiPushState(Entity& e, int state) {
    ActorAi& ai = e.ai;
    if (state >= AiPain) {
        const auto& blocked = kPushBlockedBy[state - AiPain];
        for (int level = 1; level <= ai.predictedDepth; ++level)
            if (contains(blocked, ai.predicted[size_t(level)])) return false;
        const auto& replaces = kPushReplaces[state - AiPain];
        for (int level = 1; level <= ai.predictedDepth; ++level) {
            if (!contains(replaces, ai.predicted[size_t(level)])) continue;
            do {
                aiRequest(e, 3, 0);
                --ai.predictedDepth;
            } while (level <= ai.predictedDepth);
            break;
        }
    }
    if (ai.predictedDepth + 1 >= int(ai.predicted.size())) return false; // stack of 5 (+0x8 … +0x18); unchecked in the original
    aiRequest(e, 2, state);
    ++ai.predictedDepth;
    ai.predicted[size_t(ai.predictedDepth)] = state;
    return true;
}

// Pop the current state (0x4feba0).
void World::aiPopState(Entity& e) {
    aiRequest(e, 3, 0);
    --e.ai.predictedDepth;
}

// Finish every state above the bottom one and queue a replace with `state` (0x4febd0, used by the die callback).
void World::aiKillState(Entity& e, int state) {
    ActorAi& ai = e.ai;
    while (ai.depth > 0) {
        aiFinish(e, ai.stack[size_t(ai.depth)]);
        --ai.depth;
    }
    ai.requests.clear();
    ai.predictedDepth = 0;
    aiRequest(e, 1, state);
    ai.predicted[0] = state;
}

// After a pop or a failed start: resume the states below until one resumes; with none left the bottom state
// becomes exposed (0x4fe460).
void World::aiResumeBelow(Entity& e) {
    ActorAi& ai = e.ai;
    for (;;) {
        if (ai.depth < 1) {
            if (ai.depth == 0) ai.baseStateMs = timeMs_;
            ai.stack[size_t(ai.depth)] = AiExposed;
            aiStart(e, AiExposed);
            return;
        }
        --ai.depth;
        if (aiResume(e, ai.stack[size_t(ai.depth)])) return;
        aiFinish(e, ai.stack[size_t(ai.depth)]);
    }
}

// Applies the queued requests (0x4fe4e0). A state whose start fails is replaced by exposed (replace) or dropped
// (push).
void World::aiApplyRequests(Entity& e) {
    ActorAi& ai = e.ai;
    const auto requests = ai.requests;
    for (const auto& r : requests) {
        if (r.type == 1) {
            aiFinish(e, ai.stack[size_t(ai.depth)]);
            if (ai.depth == 0) ai.baseStateMs = timeMs_;
            ai.stack[size_t(ai.depth)] = r.state;
            if (!aiStart(e, r.state)) {
                if (ai.depth == 0) ai.baseStateMs = timeMs_;
                ai.stack[size_t(ai.depth)] = AiExposed;
                aiStart(e, AiExposed);
            }
        } else if (r.type == 2) {
            if (ai.depth + 1 >= int(ai.stack.size())) continue; // see aiPushState
            aiSuspend(e, ai.stack[size_t(ai.depth)]);
            ++ai.depth;
            ai.stack[size_t(ai.depth)] = r.state;
            if (!aiStart(e, r.state)) aiResumeBelow(e);
        } else if (r.type == 3) {
            aiFinish(e, ai.stack[size_t(ai.depth)]);
            aiResumeBelow(e);
        }
    }
    ai.requests.clear();
    ai.predicted = ai.stack;
    ai.predictedDepth = ai.depth;
}

// State table 0x56f720 (7 entries of 4 bytes per state): start, finish, suspend, resume, think, touch, damaged.
// Suspend is "finish the current state" (0x5498a0) and resume "start the current state" (0x5498b0) for every
// state but exposed (resume 0x548d40) and turret (suspend 0x549b70).
bool World::aiStart(Entity& e, int state) {
    switch (state) {
    case AiExposed: // 0x548d10
        e.ai.substate = kSubCombat;
        return true;
    case AiTurret:
        return turretStateStart(e); // 0x549960
    case AiDeath: // 0x548980. TODO(spec): the claimed node release (0x5191a0) and the head look-at hand-over
                  // (0x514970, 0x4fd9e0) are not modelled.
        e.ai.substate = kSubDeath;
        return true;
    case AiPain: // 0x54d2a0
        e.ai.painStartMs = timeMs_;
        clearActorPath(e);
        setActorAnimScript(e, "pain", 0, 4);
        return true;
    case AiCustom: // 0x54b230
        return true;
    case AiScripted: // 0x54b060
        return true;
    case AiGrenade: // 0x5205b0
        return grenadeStateStart(e);
    case AiBadplace: // 0x524030: the flag keeps whether a flee target was found
        e.ai.badPlace = badPlaceFlee(e);
        return true;
    default:
        // TODO(spec): negotiation (0x54b720) needs traversals, which are not implemented; its start fails, so the
        // actor stays exposed.
        return false;
    }
}

void World::aiFinish(Entity& e, int state) {
    switch (state) {
    case AiTurret:
        turretStateFinish(e); // 0x549b20
        break;
    case AiGrenade:
        grenadeStateFinish(e); // 0x520600
        break;
    case AiScripted: // 0x54b070: the scripted block is freed
        e.scripted.reset();
        break;
    default:
        // exposed 0x548d20, death / custom 0x549890: nothing. TODO(spec): pain 0x54d300 broadcasts the pending
        // pain event to nearby AI (0x535770).
        break;
    }
}

void World::aiSuspend(Entity& e, int state) {
    if (state == AiTurret) {
        turretStateFinish(e); // TODO(spec): the turret's own suspend 0x549b70 is not read; finish is used
        return;
    }
    aiFinish(e, state); // 0x5498a0
}

bool World::aiResume(Entity& e, int state) {
    if (state == AiExposed) { // 0x548d40
        e.ai.substate = kSubCombat;
        return true;
    }
    if (state == AiGrenade) return e.ai.grenade && e.ai.throwPending; // 0x5206a0
    return aiStart(e, state); // 0x5498b0
}

int World::aiThink(Entity& e, int state) {
    switch (state) {
    case AiExposed: return exposedThink(e);   // 0x5496f0
    case AiTurret: return turretStateThink(e); // 0x54aba0
    case AiDeath: return deathThink(e);       // 0x548ab0
    case AiPain: return painThink(e);         // 0x54d340
    case AiCustom: return customThink(e);     // 0x54b240
    case AiScripted: return scriptedThink(e); // 0x54b0a0
    case AiBadplace: return badPlaceThink(e); // 0x524060
    case AiGrenade: return grenadeStateThink(e); // 0x520c30
    default:
        aiSetState(e, AiExposed);
        return 1;
    }
}

// Perception once per frame (0x4fe890: frame time at +0x24).
void World::actorPerceive(Entity& e) {
    if (e.ai.perceptionFrameMs == timeMs_) return;
    e.ai.perceptionFrameMs = timeMs_;
    updatePerception(e);
}

// The actor's animscript (0x53ac90). The same script keeps running: only the anim mode changes, and a new
// movemode notifies "movemode". Another script kills the current one ("killanimscript", 0x53ac40) and starts.
void World::setActorAnimScript(Entity& e, const std::string& name, int movemode, int animMode) {
    ActorAi& ai = e.ai;
    ai.animMode = ai.animModeOverride ? ai.animModeOverride : animMode;
    if (e.animScript == name) {
        if (ai.movemode != movemode) {
            ai.movemode = movemode;
            e.obj->set("movemode", Value(kMovemodes[movemode]));
            vm_.notify(e.obj, "movemode");
        }
        return;
    }
    killActorAnimScript(e);
    ai.movemode = movemode;
    e.obj->set("movemode", Value(kMovemodes[movemode]));
    e.animScript = name;
    e.animScriptStartMs = timeMs_;
    try {
        if (name == "<custom>") {
            if (!std::holds_alternative<FunctionRef>(ai.customScript.v)) throw std::runtime_error("animcustom without a function");
            e.animScriptThread = vm_.startFunction(std::get<FunctionRef>(ai.customScript.v), {}, Value(e.obj));
        } else {
            e.animScriptThread = vm_.start("animscripts/" + name + ".gsc", "main", {}, Value(e.obj));
        }
    } catch (const std::exception& ex) {
        print("animscripts/" + name + ": " + ex.what());
        e.animScriptThread = 0;
    }
}

// 0x53ac40: no current script; a running one is notified "killanimscript".
void World::killActorAnimScript(Entity& e) {
    if (e.animScriptThread) {
        vm_.notify(e.obj, "killanimscript");
        e.animScriptThread = 0;
    }
    e.animScript.clear();
}

// The script thread is gone or finished (thread type 0x17 in the script variable table).
bool World::actorAnimScriptDone(Entity& e) {
    return !e.animScriptThread || !vm_.isThreadAlive(e.animScriptThread);
}

// The actor's think, every frame (0x4ff030): apply the queued requests and run the current state's think until it
// stops asking to think again (1). Result 2 turns the actor into a corpse; otherwise an actor at its goal is
// notified "goal" (0x4fef50).
void World::actorThink(Entity& e) {
    ActorAi& ai = e.ai;
    if (ai.corpse) return;
    int r = 0;
    for (int guard = 0; guard < 16; ++guard) { // the original loops without a limit
        if (!ai.requests.empty()) aiApplyRequests(e);
        r = aiThink(e, ai.stack[size_t(ai.depth)]);
        if (r != 1) break;
    }
    if (r == 2) {
        actorBecomeCorpse(e);
        return;
    }
    // 0x4fef50: inside the goal and the path (if any) ends inside it. TODO(spec): facing mode 6 at a cover node
    // also needs the actor's yaw within range of the node's angle.
    if (actorInGoal(e, 0.0f)) {
        bool pathInGoal = true;
        if (actorHasPath(e)) {
            const Value saved = e.obj->get("origin");
            const Vec3 end = pathEnd(e);
            e.obj->set("origin", Value(Vec3{end[0], end[1], end[2]}));
            pathInGoal = actorInGoal(e, 0.0f);
            e.obj->set("origin", saved);
        }
        if (pathInGoal) vm_.notify(e.obj, "goal");
    }
}

// The pain callback (0x4ff3b0) after damage that leaves the actor alive: the attacker counts as having attacked
// (+0x1780 of its sentient entry), then the pain state is pushed; when it is, the current animscript is killed.
// TODO(spec): the states' damaged handlers (0x5498c0 node release, turret 0x54af30) are not modelled.
void World::actorPain(Entity& e, Entity* attacker) {
    if (attacker && isSentient(*attacker)) e.sentientInfo[attacker->obj.get()].lastAttackedMs = timeMs_;
    if (aiPushState(e, AiPain)) killActorAnimScript(e);
}

// The die callback (0x4ff530): unless already dying, every state is finished and replaced by death, the actor
// stops counting as alive for requests (+0x25c), and the current animscript is killed.
void World::actorDie(Entity& e, Entity* attacker) {
    (void)attacker;
    if (e.ai.stack[size_t(e.ai.depth)] == AiDeath) return;
    aiKillState(e, AiDeath);
    e.ai.alive = false;
    killActorAnimScript(e);
}

// 0x4fdba0: the actor becomes a corpse (entity type 0xb, contents 0x4000000) and is notified "death".
// TODO(spec): corpse contents and the head look-at hand-over to the corpse slot are not modelled.
void World::actorBecomeCorpse(Entity& e) {
    e.ai.corpse = true;
    clearActorPath(e);
    vm_.notify(e.obj, "death");
}

// Pain think (0x54d340): once the pain animscript has finished (after the start frame) the state is popped.
// Meanwhile the actor is notified "pain" every frame and keeps the pain script (anim mode 4).
int World::painThink(Entity& e) {
    ActorAi& ai = e.ai;
    if (ai.painStartMs < timeMs_ && actorAnimScriptDone(e)) {
        aiPopState(e);
        return 1;
    }
    vm_.notify(e.obj, "pain");
    // TODO(spec): the pending pain event is broadcast to nearby AI 500 ms after the pain started (0x535770).
    actorPerceive(e);
    ai.facing = 1;
    setActorAnimScript(e, "pain", 0, 4);
    return 0;
}

// Death think (0x548ab0): 800 ms after the death state was set the actor stops being a target (0x548a10); when
// the death animscript has finished the actor becomes a corpse (2). Until then the death script runs (anim mode
// override or 4).
int World::deathThink(Entity& e) {
    ActorAi& ai = e.ai;
    // TODO(spec): 0x548a10 also broadcasts the death event (0x535770), drops the grenade and releases the sentient
    // (0x514d50); only the path is cleared here.
    if (ai.substate == kSubDeath && timeMs_ - ai.baseStateMs > 799) {
        clearActorPath(e);
        releaseNode(e); // the sentient release (0x514d50) frees the claimed node (0x5191a0)
        ai.substate = kSubDeathDone;
    }
    if (e.animScript == "death" && actorAnimScriptDone(e)) {
        if (ai.substate == kSubDeath) clearActorPath(e);
        return 2;
    }
    ai.facing = 1;
    setActorAnimScript(e, "death", 0, 4);
    // TODO(spec): 500 ms after death the entity's contents become 0x4002000 (corpse); collision does not model it.
    return 0;
}

// animcustom think (0x54b240): the custom function runs as the animscript (anim mode override or 4); when it has
// finished the state is popped.
int World::customThink(Entity& e) {
    clearActorPath(e);
    setActorAnimScript(e, "<custom>", 0, 4);
    if (!actorAnimScriptDone(e)) {
        actorPerceive(e);
        return 0;
    }
    aiPopState(e);
    return 1;
}

// Sight of the current enemy, from the last check when it is at most `withinMs` old, else a new check (0x54ce10).
bool World::seesEnemyRecently(Entity& e, int64_t withinMs) {
    Entity* enemy = entityOf(e.obj->get("enemy"));
    if (!enemy) return false;
    auto it = e.sentientInfo.find(enemy->obj.get());
    if (it != e.sentientInfo.end() && it->second.lastCheckMs != 0 && timeMs_ <= it->second.lastCheckMs + withinMs)
        return it->second.visible;
    return sightCheck(e, *enemy);
}

// Exposed think (0x5496f0): a grenade (+0x20c4) → grenade state, a recorded free turret (+0x2114) → turret state;
// else perception, the substate (0x548d50) and its behaviour.
int World::exposedThink(Entity& e) {
    ActorAi& ai = e.ai;
    if (e.ai.grenade) {
        aiSetState(e, AiGrenade);
        return 1;
    }
    if (ai.badPlace) {
        aiSetState(e, AiBadplace);
        return 1;
    }
    if (e.usingTurret) {
        Entity* t = entityOf(Value(e.usingTurret));
        if (t && t->turret && !t->turret->inUse) {
            aiSetState(e, AiTurret);
            return 1;
        }
        e.usingTurret.reset();
    }
    actorPerceive(e);
    exposedSubstate(e);
    if (ai.substate == kSubCombat) {
        exposedCombat(e);
    } else if (ai.substate == kSubNoncombat) {
        exposedNoncombat(e);
    } else if (ai.substate == kSubReacquire) { // 0x5494a0
        ai.facing = 5;
        followActorPath(e, true);
    }
    updateClaimedNode(e); // 0x4fe8d0 → 0x4fe740. TODO(spec): 0x504450 before it, the followed-actor check after.
    return 0;
}

// 0x548d50: no enemy → noncombat; noncombat with the enemy seen within 250 ms → combat; reacquire without a path
// → combat.
void World::exposedSubstate(Entity& e) {
    ActorAi& ai = e.ai;
    if (!entityOf(e.obj->get("enemy"))) {
        ai.substate = kSubNoncombat;
        return;
    }
    if (ai.substate == kSubNoncombat) {
        if (seesEnemyRecently(e, 250)) ai.substate = kSubCombat;
    } else if (ai.substate == kSubReacquire) {
        // TODO(spec): with a path, combat also when the enemy is seen and 0x54d010 (eye trace + 0x54cf40) passes.
        if (!actorHasPath(e)) ai.substate = kSubCombat;
    }
}

// Combat (substate 100): with a path and the enemy in sight, stop inside the goal; without a path fight in place
// (0x548c70), else run along the path (0x5494c0).
void World::exposedCombat(Entity& e) {
    ActorAi& ai = e.ai;
    updateActorPath(e);
    // TODO(spec): +0x728 / +0x2148 (blocking actor / followed actor) also clear the path here.
    if (actorHasPath(e) && seesEnemyRecently(e, 250) && actorInGoal(e, 0.0f)) clearActorPath(e);
    if (!actorHasPath(e)) {
        // 0x548c70: inside the goal, the enemy not seen for 5 s and a claimed node flagged 0x8000 that covers
        // from it: facing 6, else 3. TODO(spec): 0x548be0 (moving back inside a shrunken goal) and the path end
        // conditions of 0x502850.
        combatScript(e);
        ai.facing = 3;
        if (actorInGoal(e, 0.0f) && e.claimedNode >= 0 && (nodes_[size_t(e.claimedNode)].spawnflags & 0x8000)) {
            Entity* enemy = entityOf(e.obj->get("enemy"));
            const auto it = enemy ? e.sentientInfo.find(enemy->obj.get()) : e.sentientInfo.end();
            const bool seen = it != e.sentientInfo.end() && it->second.lastSeenMs != 0 && timeMs_ - it->second.lastSeenMs < 5000;
            std::string script;
            if (!seen && (!coverScriptAt(e, e.claimedNode, true, script) || !script.empty())) ai.facing = 6;
        }
        return;
    }
    ai.facing = 4;
    followActorPath(e, true, true); // 0x5494c0 (cl = 1)
}

// Noncombat (substate 0x65, 0x549520): without a path stop (a known enemy → the combat script); with one move,
// running when the path end is farther than walkdist.
void World::exposedNoncombat(Entity& e) {
    ActorAi& ai = e.ai;
    updateActorPath(e);
    if (!actorHasPath(e)) {
        // TODO(spec): at a claimed node with spawnflag 0x8000 the stop/cover script runs with facing 6; 0x548be0
        // (keepclaimednode / stop timing) is not read.
        ai.facing = 3;
        if (entityOf(e.obj->get("enemy"))) {
            combatScript(e);
            return;
        }
        setActorAnimScript(e, stopScript(e), 0, 1); // 0x53ae40
        return;
    }
    const Vec3 o = e.obj->get("origin").asVector();
    const float dx = o[0] - e.path.goal[0], dy = o[1] - e.path.goal[1], dz = o[2] - e.path.goal[2];
    const Value wd = e.obj->get("walkdist");
    const float walkdist = wd.isNumber() ? wd.asFloat() : 0.0f;
    // TODO(spec): facing 6 near claimed nodes and the 0x1c148a8 override.
    ai.facing = 2;
    followActorPath(e, walkdist * walkdist < dx * dx + dy * dy + dz * dz);
    if (actorInGoal(e, 0.0f)) ai.facing = 4;
}

// 0x53b050: at the claimed node (0x502910) its cover script (0, anim mode 1); a node that no longer covers is given
// up (0x4fe670); otherwise "combat" (anim mode 4, 1 when following an actor). TODO(spec): 0x504630 (crowding) and
// clearing +0x728.
void World::combatScript(Entity& e) {
    if (e.claimedNode >= 0 && atClaimedNode(e)) {
        std::string script;
        if (coverScriptAt(e, e.claimedNode, false, script)) {
            if (!script.empty()) {
                setActorAnimScript(e, script, 0, 1);
                return;
            }
            giveUpClaimedNode(e);
        }
    }
    setActorAnimScript(e, "combat", 0, 4);
}

// 0x53aed0: at the claimed node its cover script; a node that no longer covers is given up; else "stop".
std::string World::stopScript(Entity& e) {
    if (e.claimedNode >= 0 && atClaimedNode(e)) {
        std::string script;
        if (coverScriptAt(e, e.claimedNode, false, script)) {
            if (!script.empty()) return script;
            giveUpClaimedNode(e);
        }
    }
    return "stop";
}

bool World::actorHasPath(const Entity& e) const { return e.path.count > 0; }

// 0x503e10: the old path's area goes to the budget (0x541780), then no points and no detour wait (+0x724).
void World::clearActorPath(Entity& e) {
    pathClearBudget(e.path, e.obj->get("origin").isVector() ? e.obj->get("origin").asVector() : Vec3{0, 0, 0});
    e.path.count = 0;
    e.path.planned = 0;
    e.ai.detourMs = 0;
    e.ai.stopSoonMs = 0;
}

// Path following (0x552350): entering movement sets anim mode 1 / movemode stop. The step (0x551960,
// src/game/actorfollow.cpp; `wide`: the interval radius) returns 1 to stand — the "stop" animscript with movemode
// stop, anim mode 1 (0x53ac90(0, 1)) — 2 to walk (0x53af20), 0 to go on: run → 0x53afe0, else 0x53af20, i.e. "move"
// (run: movemode run, else walk) until the stop-soon time (+0x704) is within 200 ms, when the stop script starts
// with movemode stop_soon. TODO(spec): 0x504630 (crowding) and 0x53ade0 (a followed actor) when standing.
void World::followActorPath(Entity& e, bool run, bool wide) {
    ActorAi& ai = e.ai;
    const bool moving = ai.animMode == 1 && ai.movemode != 0;
    if (!moving) {
        ai.animMode = 1;
        ai.movemode = 0;
    }
    const int r = stepActorPath(e, wide, run);
    if (r == 1) {
        setActorAnimScript(e, "stop", 0, 1); // script handle 0x1c7e118 ("stop", registered at 0x4eb33f)
        return;
    }
    const bool runNow = r == 0 && run;
    const int64_t left = ai.stopSoonMs - timeMs_;
    if (ai.stopSoonMs != 0 && (runNow ? left <= 200 : left < 200))
        setActorAnimScript(e, stopScript(e), 1, 1);
    else
        setActorAnimScript(e, "move", runNow ? 3 : 2, 1);
    if (!moving && ai.animMode == 1 && ai.movemode != 0) ai.stopSoonMs = 0;
    moveActor(e, 0.05f);
}

void World::registerActorStateBuiltins() {
    registerCoverBuiltins();
    registerBadPlaceBuiltins();
    registerGrenadeBuiltins();
    // animcustom(function) (0x540450): push the custom state; when pushed, kill the current animscript and keep the
    // function for the state's think.
    vm_.registerBuiltin("animcustom", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("animcustom on a non-actor");
        if (!std::holds_alternative<FunctionRef>(c.arg(0).v)) throw std::runtime_error("animcustom needs a function");
        if (aiPushState(*e, AiCustom)) {
            killActorAnimScript(*e);
            e->ai.customScript = c.arg(0);
        }
        return Value();
    });
}

} // namespace iw2::game
