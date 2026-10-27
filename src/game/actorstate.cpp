// The actor state machine (docs/spec/ai.md "AI states"): the state stack and its request queue, the per-frame
// actor think, the exposed / pain / death / animcustom states and the engine's choice of animscript.
#include "game/world.h"

#include <cctype>
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
    case AiDeath: // 0x548980: +0x740 / +0x741 cleared and the claimed node released at once (0x548995..0x54899a).
                  // TODO(spec): the head look-at hand-over (0x514970, 0x4fd9e0; 500 ms override-angle gate) and sentient +0x28 = 200
                  // (0x548a02) are not modelled.
        e.obj->set("keepclaimednode", Value(int32_t(0)));
        e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
        if (e.claimedNode >= 0) releaseNode(e);
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
    expireDangerPlanes(e); // 0x4ff030 → 0x54b4d0: a short-lived plane goes after 300 ms, the rest after suppressionwait
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
    // 0x54d340: 500 ms into the pain, with an attacker on the sentient (+0x28), the pain event (type 5) reveals
    // the attacker to the actors around; the attacker is then forgotten.
    if (e.lastAttacker && timeMs_ - ai.painStartMs > 499) {
        aiEventPoint(e, 5, e.obj->get("origin").isVector() ? e.obj->get("origin").asVector() : Vec3{0, 0, 0});
        e.lastAttacker = nullptr;
    }
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
        // 0x548a10: the death event (type 6) makes the actors around learn the killer (sentient +0x28).
        aiEventPoint(e, 6, e.obj->get("origin").isVector() ? e.obj->get("origin").asVector() : Vec3{0, 0, 0});
        clearActorPath(e);
        releaseNode(e); // the sentient release (0x514d50) frees the claimed node (0x5191a0)
        removeDangerPlanesFrom(e.number); // and the danger planes it added elsewhere (0x4fe170 → 0x54b5a0)
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
    // 0x54b240..0x54b26c: keepclaimednode (+0x740) / keepclaimednodeingoal (+0x741) cleared, claimed node released.
    e.obj->set("keepclaimednode", Value(int32_t(0)));
    e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
    if (e.claimedNode >= 0) releaseNode(e);
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
        // 0x549520 (0x549643..0x5496d8): holding the position (0x502850) at a claimed node flagged 0x8000: facing 6, else 3.
        // TODO(spec): 0x548be0 (keepclaimednode / stop timing) before the facing-3 stop choice is not read.
        ai.facing = 3;
        if (e.claimedNode >= 0 && (nodes_[size_t(e.claimedNode)].spawnflags & 0x8000) && holdsPosition(e)) ai.facing = 6;
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

// Goal facing (FUN_00505d80, run by the movement frame FUN_00504160 right before FUN_00504ca0).
// The facing mode (+0x104) selects the turn target: 6 -> the claimed node's yaw (node +0x20) when it is flagged 0x8000
// (0x505dcc); 3 without an enemy -> the same node yaw when the actor stands at the node (0x502660: height within 80,
// 2D distance within 30), after the hostile check 0x505790 (0x50596c..0x5059af). The target is stored by 0x504950 as a
// 16-bit binary angle (trunc(deg * 65536/360) & 0xffff, * 360/65536) and 0x504ca0 turns the entity towards it, at most
// 0.4 * 50 = 20 degrees per frame, re-quantising the yaw (pitch and roll 0), and refreshes desiredangle.
// TODO(spec): modes 2, 4, 5 and 3 with an enemy / hostile target (0x505630, 0x505b80, 0x505cb0, 0x505790, 0x505a80), the
// pair update by the path move (0x504aa0), orientmode (+0x114), the override angle (+0x159..0x160, 1.8 degrees/frame) and
// the target while a path is followed (moveActor writes the yaw there).
static Vec3 vecField(const ObjectPtr& o, const char* key) {
    const Value v = o->get(key);
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

static std::string lowerStr(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Yaw of a direction vector (0x421b30, the same as vectortoangles: 0..360).
static float yawOfVector(const Vec3& v) {
    float yaw = v[0] != 0.0f ? std::atan2(v[1], v[0]) * 57.2957795f : (v[1] > 0 ? 90.0f : 270.0f);
    if (yaw < 0) yaw += 360.0f;
    return yaw;
}

static float quantYaw(float deg) { return float(int(deg * 182.04444885253906f) & 0xffff) * 0.0054931640625f; } // 0x591e68, 0x591f10

void World::actorFacing(Entity& e) {
    const ActorAi& ai = e.ai;
    const int m = ai.orient ? ai.orient : ai.facing; // 0x505d80: the orientmode override, else the facing mode
    float target = 0;
    // Mode 3 with an enemy (0x505960 -> 0x5059c2): the enemy's sight record decides. Visible now (record byte 0): turn
    // towards the enemy's origin (0x505a80; ignored when the 2D distance² < 1.0, 0x591e20). Not visible: with no path and
    // the enemy seen (record +8) less than 5000 ms ago the target is left alone (0x505a08); otherwise (never seen, or
    // seen long ago) the node facing below runs (0x505a16 -> 0x505920).
    // TODO(spec): the path branch (0x505a22: +0x6ec time gate, 0x5432f0 position when not seen for > 500 ms); while a path
    // is followed moveActor writes the yaw.
    bool enemyTargeted = false;
    if (ai.orient != 1 && m >= 3 && m <= 5) {
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (enemy) {
            if (e.path.count > 0) return;
            const auto it = e.sentientInfo.find(enemy->obj.get());
            if (it != e.sentientInfo.end() && it->second.visible) {
                const Vec3 o = vecField(e.obj, "origin"), eo = vecField(enemy->obj, "origin");
                const float dx = eo[0] - o[0], dy = eo[1] - o[1];
                if (dx * dx + dy * dy < 1.0f) return;
                target = quantYaw(yawOfVector({dx, dy, 0}));
                enemyTargeted = true;
            } else if (it != e.sentientInfo.end() && it->second.lastSeenMs != 0 && timeMs_ - it->second.lastSeenMs < 5000) {
                return;
            }
        }
    }
    if (ai.orient == 1) {
        // orientmode angle / current / direction / point (0x53f510): 0x504ca0 turns to the override target +0x120
        // every frame (no selector runs for mode 1). While a path is followed moveActor writes the yaw instead.
        if (e.path.count > 0) return;
        target = ai.orientTarget;
    } else if (enemyTargeted) {
        // target set from the enemy (0x505a80 .. 0x505aec)
    } else {
        if (e.path.count > 0 || e.claimedNode < 0) return;
        const PathNode& n = nodes_[size_t(e.claimedNode)];
        if (!(n.spawnflags & 0x8000)) return;
        const Vec3 o = vecField(e.obj, "origin");
        if (m >= 3 && m <= 5) {
            const Vec3 np = vecField(n.obj, "origin");
            const float dz = o[2] - np[2], dx = np[0] - o[0], dy = np[1] - o[1];
            if (dz * dz > 6400.0f || dx * dx + dy * dy > 900.0f) return; // 0x502660 (0x591fdc, 0x591fd4)
        } else if (m != 6) {
            return;
        }
        target = quantYaw(vecField(n.obj, "angles")[1]);
    }
    const Vec3 a = vecField(e.obj, "angles");
    float d = target - a[1]; // FUN_00423f50 (AngleNormalize180)
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    d = std::max(-20.0f, std::min(20.0f, d)); // 0.4 * +-50 (0x592030 / 0x592034)
    e.obj->set("angles", Value(Vec3{0, quantYaw(a[1] + d), 0}));
    e.obj->set("desiredangle", Value(target)); // 0x504ca0
}

void World::registerActorStateBuiltins() {
    registerCoverBuiltins();
    registerBadPlaceBuiltins();
    registerGrenadeBuiltins();
    // orientmode(mode[, angle | vector]) (0x53f510): sets the override facing +0x114 and, for the explicit modes, the
    // override target +0x120 (16-bit quantised yaw). "face enemy" 3, "face enemy or motion" 4, "face goal" 6, "face
    // motion" 2 only set the override. "face default" (0x5063f0) copies the override pair into the base pair and
    // clears the override. TODO(spec): "face current" takes desiredangle (+0x124), which only follows the turn target here
    // when this turn code ran, so the current yaw is used; 0x504aa0 (animation yaw delta into the pairs) is not modelled.
    vm_.registerBuiltin("melee", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        if (!a || !a->obj || !isAlive(*a)) return Value();
        if (a->ai.meleeFrameMs == timeMs_) return Value(); // 0x550c7a: once per frame
        a->ai.meleeFrameMs = timeMs_;
        aiMelee(*a);
        return Value();
    });
    vm_.registerBuiltin("orientmode", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("orientmode on a non-actor");
        ActorAi& ai = e->ai;
        const std::string mode = lowerStr(c.arg(0).toString());
        if (mode == "face angle") {
            ai.orient = 1;
            ai.orientTarget = quantYaw(c.arg(1).asFloat());
        } else if (mode == "face current") {
            ai.orient = 1;
            ai.orientTarget = quantYaw(vecField(e->obj, "angles")[1]);
        } else if (mode == "face direction") {
            const Vec3 v = c.arg(1).isVector() ? c.arg(1).asVector() : Vec3{0, 0, 0};
            if (v[0] == 0.0f && v[1] == 0.0f) throw std::runtime_error("cannot face (0, 0, *)");
            ai.orient = 1;
            ai.orientTarget = quantYaw(yawOfVector(v));
        } else if (mode == "face enemy") {
            ai.orient = 3;
        } else if (mode == "face enemy or motion") {
            ai.orient = 4;
        } else if (mode == "face goal") {
            ai.orient = 6;
        } else if (mode == "face motion") {
            ai.orient = 2;
        } else if (mode == "face point") {
            const Vec3 p = c.arg(1).isVector() ? c.arg(1).asVector() : Vec3{0, 0, 0};
            const Vec3 o = vecField(e->obj, "origin");
            const Vec3 d{p[0] - o[0], p[1] - o[1], p[2] - o[2]};
            ai.orient = 1;
            if (d[0] * d[0] + d[1] * d[1] >= 1.0f) ai.orientTarget = quantYaw(yawOfVector(d)); // 0x591e20 = 1.0
        } else if (mode == "face default") {
            ai.orient = 0; // the base pair keeps the override target; the base target is not modelled
        } else {
            throw std::runtime_error("orientMode must be 'face angle', 'face current', 'face direction', 'face enemy', 'face enemy or motion', 'face goal', 'face motion', 'face point', or 'face default'");
        }
        return Value();
    });
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

// 0x5485a0 (every movement frame, 0x4ff1b3, while actor +0x25c is set; not a pain handler): the movement struct
// actor +0x224 keeps a kind (+0x40, set by FUN_00504160 from the anim mode +0x218: modes 1, 3 -> 1; 2, 4 -> 2; 5 -> 3;
// 6 -> 4; 7 -> 5; mode 0 leaves it) and a step (+0x48, 50, 0x500cff). While the kind is 1, 2 or 5 (and the ground entity
// +0x18 is not 0x7ff, i.e. the actor stands on something) every frame adds the step to the carried sum (+0x1c, zeroed
// otherwise); at 500 an AI event 2 is sent at the actor (0x535770, origin = the struct's position, enemy-team receivers)
// and the sum keeps its remainder.
void World::actorMoveNoise(Entity& e) {
    ActorAi& ai = e.ai;
    const int m = ai.animMode;
    const bool kind125 = m == 1 || m == 2 || m == 3 || m == 4 || m == 7;
    const int old = ai.moveNoiseMs;
    ai.moveNoiseMs = 0;
    if (!ai.alive || !kind125) return;
    // The ground entity (+0x18, 0x548290): a trace of the entity box (r.mins/r.maxs) with mask 0x2820011 (actor +0x28c)
    // from the origin 0.25 down (0x591fe0; the step height 10/18 instead when rising on a ground entity, not modelled
    // as the vertical velocity +0x14 is 0 here); a miss leaves 0x7ff (airborne) and the accumulator zeroed.
    {
        const Value ov = e.obj->get("origin");
        const Vec3 o = ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
        const TraceResult tr = collision_->trace(o, {o[0], o[1], o[2] - 0.25f}, {-15, -15, 0}, {15, 15, 72}, 0x2820011u, e.number);
        static const bool dbg = std::getenv("IW2_GROUND_DEBUG") != nullptr;
        if (dbg) ++(tr.fraction < 1.0f || tr.startSolid ? stats.groundHit : stats.groundMiss);
        if (!(tr.fraction < 1.0f || tr.startSolid)) return;
    }
    ai.moveNoiseMs = old + 50;
    if (ai.moveNoiseMs < 500) return;
    const Value o = e.obj->get("origin");
    ++stats.actorNoise;
    aiEventPoint(e, 2, o.isVector() ? o.asVector() : Vec3{0, 0, 0}, 0.0f, false, AiHear::Enemy);
    ai.moveNoiseMs %= 500;
}

} // namespace iw2::game
