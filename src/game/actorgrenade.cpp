// The actor's grenade state (AI state 3 of the table at 0x56f720: start 0x5205b0, finish 0x520600, suspend 0x520650,
// resume 0x5206a0, think 0x520c30; docs/spec/grenades.md): noticing grenades (grenade_ping, 0x5200e0), fleeing,
// taking cover, cowering, and picking a grenade up to throw it back.
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

constexpr int kFlee = 300, kTakeCover = 0x12d, kCower = 0x12e, kCombat = 0x12f, kCoverAttack = 0x130, kAcquire = 0x131,
              kThrowBack = 0x132;

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

int enemyTeam(int team) { return team == 1 ? 2 : team == 2 ? 1 : 0; }

float dist2dSq(const Vec3& a, const Vec3& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }

} // namespace

// 1.1 × the grenade's explosionRadius (0x56fe28): the danger radius.
float World::grenadeDangerRadius(Entity& g) { return weaponFloat(g.grenade->weapon, "explosionRadius") * 1.1f; }

// 0x51f8b0: `pos` is safe from the grenade's predicted landing point (0x5285b0 fails).
bool World::safeFromGrenade(Entity& e, Entity& g, const Vec3& pos) {
    return !canDamage(e, g.grenade->landing, grenadeDangerRadius(g), &pos);
}

void World::setActorGrenade(Entity& e, Entity* g) {
    e.ai.grenade = g;
    e.obj->set("grenade", g ? Value(g->obj) : Value());
}

// grenade_ping (event 8, every 500 ms within 512 units of a grenade, 0x535770 → 0x5200e0): an actor with
// grenadeawareness that has no grenade (or one without a predicted landing) notices this one with that probability.
// A grenade of the enemy team makes it the enemy's (0x4f9f30 / 0x4fa070). The landing is predicted (0x51f7c0); the
// grenade is forgotten again when the actor's path leads away from it (0x51fe90) or its position is safe (0x51f8b0).
// TODO(spec): 0x4f9f30 / 0x4fa070 (an AI notification on enemy grenades) are not read; 0x51fe90's last test
// (0x420d90 > 16) is not read, so the path test never forgets the grenade.
void World::grenadePing(Entity& g) {
    const Vec3 at = originOf(g.obj);
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.isActor || e.obj->removed || !e.ai.alive || e.ai.corpse) continue;
        const Vec3 o = originOf(e.obj);
        const float dx = o[0] - at[0], dy = o[1] - at[1], dz = o[2] - at[2];
        if (dx * dx + dy * dy + dz * dz >= 262144.0f) continue; // 512² (event table 0x57031c)
        const Value aw = e.obj->get("grenadeawareness");
        const float awareness = aw.isNumber() ? aw.asFloat() : 0.0f;
        if (awareness == 0.0f) continue;
        Entity* cur = e.ai.grenade;
        const bool curLanded = cur && (cur->grenade->landing[0] != 0.0f || cur->grenade->landing[1] != 0.0f || cur->grenade->landing[2] != 0.0f);
        if (cur && curLanded) continue;
        if (float(rng_() % 32768) > awareness * 32767.0f) continue;
        setActorGrenade(e, &g);
        predictGrenadeLanding(g);
        if (safeFromGrenade(e, g, o)) setActorGrenade(e, nullptr);
    }
}

// Start (0x5205b0): nobody else going for the grenade and no throw pending: try to acquire it (0x51fb20); else decide
// between flee, cover and cower (0x51f900).
bool World::grenadeStateStart(Entity& e) {
    if (!e.ai.grenade) return false;
    GrenadeState& g = *e.ai.grenade->grenade;
    if (!g.holder && !e.ai.throwPending && grenadeAcquire(e)) return true;
    grenadeReact(e, false);
    return true;
}

// Finish / suspend (0x520600 / 0x520650): a grenade this actor holds drops from its hand.
void World::grenadeStateFinish(Entity& e) {
    if (!e.ai.grenade) return;
    GrenadeState& g = *e.ai.grenade->grenade;
    if (g.holder == &e) {
        if (g.inHand) releaseHeldGrenade(e);
        g.holder = nullptr;
    }
}

// 0x51f900: react to the grenade. A grenade this actor holds is dropped. Without an enemy, a thrower of another sentient
// becomes the enemy. A path that does not pass near the landing point (0x5437f0) is kept (flee). For its own grenade the
// usual path update runs and a path that ends safe is fled along; else the best safe cover node within 768 units
// (0x54c110) is walked to and claimed (take cover); else a path away from the landing point to beyond the danger radius
// (0x503ca0 → 0x545f30) (flee), or cowering when there is none.
void World::grenadeReact(Entity& e, bool keepPath) {
    ActorAi& ai = e.ai;
    Entity& ge = *ai.grenade;
    GrenadeState& g = *ge.grenade;
    if (g.holder == &e) {
        if (g.inHand) releaseHeldGrenade(e);
        g.holder = nullptr;
    }
    if (!entityOf(e.obj->get("enemy")) && g.parent && g.parent != &e && (g.parent->isActor || g.parent->isPlayer) && isAlive(*g.parent))
        setEnemy(e, g.parent, true);
    const float r = grenadeDangerRadius(ge);
    if (!keepPath && actorHasPath(e) && !pathNearPoint(e, g.landing, r * r)) {
        ai.substate = kFlee;
        return;
    }
    if (g.parent == &e) {
        updateActorPath(e);
        if (actorHasPath(e) && safeFromGrenade(e, ge, pathEnd(e))) {
            ai.substate = kFlee;
            return;
        }
    } else {
        const int node = grenadeCoverNode(e, r);
        if (node >= 0 && fleePathToNode(e, node, true)) { // 0x51fa49: 0x503890(…, 1) — with the danger planes
            e.obj->set("keepclaimednode", Value(int32_t(0)));
            e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
            claimNode(e, node);
            ai.substate = kTakeCover;
            return;
        }
    }
    // 0x51fa7b: 0x503ca0(…, planes = 1); with no path and a standing (not short-lived) danger plane (0x54b610) the plain
    // search 0x5429e0 runs once more (0x51faa0…0x51fb05) before cowering.
    pathAwayFrom(e, g.landing, r, true);
    if (!actorHasPath(e) && hasFreeDangerPlane(e)) {
        ++stats.grenadeFleeRetry;
        pathAwayFrom(e, g.landing, r, false);
    }
    ai.substate = actorHasPath(e) ? kFlee : kCower;
    ++(ai.substate == kFlee ? stats.grenadeFlee : stats.grenadeCower);
}

// 0x5437f0 (simplified to the remaining path): a segment of it comes within √r2 of `p` (2D).
// TODO(spec): the lookahead point (+0x398 / +0x3ac) and the extra segment (+0x3b4) it also tests.
bool World::pathNearPoint(Entity& e, const Vec3& p, float r2) {
    Vec3 a = originOf(e.obj);
    for (int i = e.path.count - 2; i >= 0; --i) {
        const Vec3& b = e.path.pts[size_t(i)].pos;
        const float ex = b[0] - a[0], ey = b[1] - a[1], l2 = ex * ex + ey * ey;
        float t = l2 > 0.0f ? ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float qx = a[0] + ex * t - p[0], qy = a[1] + ey * t - p[1];
        if (qx * qx + qy * qy < r2) return true;
        a = b;
    }
    return false;
}

// 0x54c110: cover nodes (types 2…12, 18) within 768 units and 80 of height of the actor that are outside the danger
// radius of the landing point or that the explosion cannot reach, usable, facing the enemy and covering; the highest
// cover score wins (0x54bf10).
int World::grenadeCoverNode(Entity& e, float r) {
    const Vec3 landing = e.ai.grenade->grenade->landing;
    int best = -1;
    float bestScore = 0.0f;
    std::string script;
    for (const auto& [node, d2] : nodesInRange(originOf(e.obj), 768.0f, 80.0f, 256, 0x41ffc)) {
        const Vec3 p = originOf(nodes_[size_t(node)].obj);
        if (!(dist2dSq(landing, p) > r * r || !canDamage(e, landing, r, &p))) continue;
        if (!nodeUsable(e, node) || !nodeFacesEnemy(e, node)) continue;
        if (coverScriptAt(e, node, true, script) && script.empty()) continue;
        const float s = coverScore(e, node);
        if (s > bestScore) {
            bestScore = s;
            best = node;
        }
    }
    return best;
}

// 0x503ca0 → 0x5429e0 / 0x542a60 / 0x545f30: from the sentient's node, a best-first search over the graph (team-blocked
// links skipped) for the first node at least `dist` (2D) from `p`, preferring nodes far from it (h = dist − distance).
// The search keeps the farthest node seen in its result block (+0x14 best distance², starting at -1.0, +0x18 the node,
// 0x542abd / 0x542ac5). The fallback 0x542a60 (0x542ae3..0x542b18): when the search finds no node far enough, no path is
// built if that farthest node is the start node itself; otherwise the path is built to it with 0x541da0 (goal point 0,
// traversals = the wrapper's last argument, 1 from both callers 0x503ca0 / 0x51faf9). The path ends at that node. The plane
// variant (0x542b70 -> 0x542bf0 -> 0x5461a0, goal point 0, traversals 1) has no such fallback.
bool World::pathAwayFrom(Entity& e, const Vec3& p, float dist, bool usePlanes) {
    clearActorPath(e);
    e.nodeCached = false;
    // 0x503ca0 with its planes flag: when the sentient has danger planes (0x54b670) the plane variant runs instead of
    // 0x5429e0: the node query takes the planes (0x542b70), the start node must not be beyond one (0x542b30 via
    // 0x542bf0) and the search (0x5461a0, the same best-first search) skips the neighbours beyond a plane.
    const std::vector<PathPlane> planes = usePlanes ? dangerPlanesOf(e) : std::vector<PathPlane>{};
    const int start = queryNode(originOf(e.obj), 192.0f, true, 0xfffffffe, planes, nullptr);
    if (start < 0) return false;
    if (!planes.empty() && nodeBeyondPlanes(start, planes)) return false;
    const size_t n = nodes_.size();
    if (search_.size() != n) search_.assign(n, {});
    auto& s = search_;
    static constexpr int kHead = -2;
    const int id = ++searchId_;
    const int team = teamOf(e);
    int head = start, found = -1, far = -1;
    float farD = -1.0f; // 0x542abd: best distance² starts at -1.0
    s[size_t(start)] = {id, -1, kHead, -1, 0.0f, 0.0f, true};
    auto setNext = [&](int node, int next) { (node == kHead ? head : s[size_t(node)].next) = next; };
    while (head >= 0) {
        const int cur = head;
        const Vec3& cp = paths_.position(size_t(cur));
        const float d2 = dist2dSq(cp, p);
        if (d2 >= dist * dist) {
            found = cur;
            break;
        }
        if (farD < d2) {
            farD = d2;
            far = cur;
        }
        head = s[size_t(cur)].next;
        if (head >= 0) s[size_t(head)].prev = kHead;
        const auto& links = paths_.links(size_t(cur));
        for (size_t li = 0; li < links.size(); ++li) {
            if (linkBlocked(size_t(cur), li, team)) continue;
            const int nb = links[li].target;
            if (!planes.empty() && nodeBeyondPlanes(nb, planes)) continue;
            auto& sn = s[size_t(nb)];
            float g;
            if (sn.id == id) {
                g = s[size_t(cur)].g + links[li].distance;
                if (sn.g <= g) continue;
                if (sn.queued) {
                    setNext(sn.prev, sn.next);
                    if (sn.next >= 0) s[size_t(sn.next)].prev = sn.prev;
                }
            } else {
                sn.id = id;
                sn.h = dist - std::sqrt(dist2dSq(paths_.position(size_t(nb)), p));
                g = links[li].distance + s[size_t(cur)].g;
            }
            sn.g = g;
            sn.parent = cur;
            int after = kHead;
            for (int it = head; it >= 0 && s[size_t(it)].h + s[size_t(it)].g < g + sn.h; it = s[size_t(it)].next) after = it;
            sn.prev = after;
            sn.next = after == kHead ? head : s[size_t(after)].next;
            setNext(after, nb);
            if (sn.next >= 0) s[size_t(sn.next)].prev = nb;
            sn.queued = true;
        }
        s[size_t(cur)].queued = false;
    }
    const int goal = found >= 0 ? found : far;
    // The plane variant (0x542b70 -> 0x542bf0 -> 0x5461a0) has no fallback: 0x542bf0 returns the search result, 0 when it ran
    // out of nodes. The plain one (0x542a60) falls back to the farthest node unless it is the start node (0x542ae7).
    if (found < 0 && !planes.empty()) return false;
    if (goal < 0 || (found < 0 && goal == start)) return false;
    std::vector<int> chain;
    for (int c = goal; c >= 0; c = c == start ? -1 : s[size_t(c)].parent) chain.push_back(c);
    std::reverse(chain.begin(), chain.end());
    return buildActorPath(e, chain, paths_.position(size_t(goal)), false, true);
}

// 0x51fb20: going for a grenade of the enemy team to throw it back: it must land within 96 units (2D) of the actor;
// the throw from the landing point (arm offset (0, 0, 80)) at the enemy (or the thrower) is solved with min energy,
// min time, max time (re-solved from the hand when thrown); then a path to the landing point (stopping 32 units short
// towards the actor unless that is the same point). The thrower becomes the enemy; a grenade nobody went for yet gets
// one more second of fuse. When all three fail, the hint fallback 0x51f320 (grenadeHintFromLanding).
bool World::grenadeAcquire(Entity& e) {
    ActorAi& ai = e.ai;
    Entity& ge = *ai.grenade;
    GrenadeState& g = *ge.grenade;
    Entity* thrower = g.parent;
    if (!thrower || !(thrower->isActor || thrower->isPlayer) || teamOf(*thrower) != enemyTeam(teamOf(e))) return false;
    const Vec3 landing = g.landing, o = originOf(e.obj);
    Entity* enemy = entityOf(e.obj->get("enemy"));
    const Vec3 target = originOf((enemy ? enemy : thrower)->obj);
    const float dx = o[0] - landing[0], dy = o[1] - landing[1];
    if (std::sqrt(dx * dx + dy * dy) > 96.0f) return false;
    // The approach point: 32 units from the landing point, away from the enemy (towards the actor without one).
    // TODO(spec): the arguments of the 0x5027e0 test that keeps the landing point itself, and the normalisation of
    // the direction, are inferred.
    Vec3 aim = landing;
    float ax = enemy ? landing[0] - target[0] : dx, ay = enemy ? landing[1] - target[1] : dy;
    const float al = std::sqrt(ax * ax + ay * ay);
    const float dz = o[2] - landing[2];
    if (al > 0.0f && !(dz * dz <= 6400.0f && dx * dx + dy * dy <= 4.0f))
        aim = {landing[0] + ax / al * 32.0f, landing[1] + ay / al * 32.0f, landing[2]};
    Vec3 start, vel;
    ai.throwFromTag = true;
    bool ok = false;
    for (const char* m : {"min energy", "min time", "max time"}) {
        if (ok) break;
        ai.throwMethod = m;
        ok = throwSolution(e, landing, {0, 0, 80.0f}, m, target, true, start, vel);
    }
    if (!ok) {
        // 0x51fd6c: no re-solve from the hand, then the hint fallback 0x51f320 from the landing point.
        ai.throwFromTag = false;
        if (!grenadeHintFromLanding(e, landing, vel)) return false;
    }
    pathToPoint(e, aim, false); // 0x51fd89: 0x503650(…, 0), no traversals
    if (!actorHasPath(e)) return false;
    if (!enemy && thrower != &e) setEnemy(e, thrower, true);
    // 0x51fded..0x51fe45: the kept start is the landing point itself (not the arm-offset start of 0x51f490); the
    // target is only stored when the throw is re-solved from the hand (+0x20d4).
    ai.throwPending = true;
    ai.throwStart = landing;
    ai.throwVel = vel;
    if (ai.throwFromTag) ai.throwTarget = target;
    if (g.holder != &e) {
        g.explodeMs += 1000;
        g.holder = &e;
        ai.substate = kAcquire;
    }
    return true;
}

// 0x51fff0 (eax = skipAcquire): give up the throw back. With skipAcquire 0, a grenade within 96 units (2D, d^2 <= 9216
// 0x59239c; `jp` at 0x520041 skips the acquire when d^2 > 9216) is acquired again when possible (0x51fb20, then false);
// holding it in a safe place, the actor forgets it and returns to exposed (true); otherwise it reacts (0x51f900 with
// keepPath, false). The acquire think's two give-ups (0x520b1e / 0x520b5b) pass skipAcquire 1.
bool World::grenadeGiveUp(Entity& e, bool skipAcquire) {
    ActorAi& ai = e.ai;
    GrenadeState& g = *ai.grenade->grenade;
    if (!skipAcquire && dist2dSq(originOf(ai.grenade->obj), originOf(e.obj)) <= 9216.0f && grenadeAcquire(e)) return false;
    if (g.holder == &e && safeFromGrenade(e, *ai.grenade, originOf(e.obj))) {
        g.holder = nullptr;
        setActorGrenade(e, nullptr);
        aiSetState(e, AiExposed);
        return true;
    }
    grenadeReact(e, true);
    return false;
}

// Acquire (0x520a50): walking to the grenade (facing 4, the move script); once there (within 16 units, 32 of height,
// else give up 0x51fff0), facing 1 towards it; when the grenade has landed, throw it back.
int World::grenadeAcquireThink(Entity& e) {
    ActorAi& ai = e.ai;
    if (actorHasPath(e)) {
        ai.facing = 4;
        followActorPath(e, true); // 0x53af90
        updateClaimedNode(e);
        return 0;
    }
    const Vec3 o = originOf(e.obj);
    if (dist2dSq(e.path.goal, o) > 256.0f && grenadeGiveUp(e, true)) return 1;
    if (std::fabs(o[2] - e.path.goal[2]) > 32.0f && grenadeGiveUp(e, true)) return 1;
    if (ai.substate != kAcquire) return 0;
    ai.facing = 1;
    if (ai.grenade->grenade->landingMs <= timeMs_) {
        ai.substate = kThrowBack;
        return grenadeThrowBackThink(e);
    }
    updateClaimedNode(e);
    return 0;
}

// Throw back (0x520930): grenade_return_throw (anim mode 1) until its thread ends, then exposed; a held grenade then
// still in the hand is an error.
int World::grenadeThrowBackThink(Entity& e) {
    setActorAnimScript(e, "grenade_return_throw", 0, 1);
    if (!actorAnimScriptDone(e)) {
        updateClaimedNode(e);
        return 0;
    }
    aiSetState(e, AiExposed);
    if (e.ai.grenade && e.ai.grenade->grenade->holder == &e && e.ai.grenade->grenade->inHand) {
        const Vec3 o = originOf(e.obj);
        throw std::runtime_error("ai classname '" + e.classname + "' at (" + std::to_string(o[0]) + " " + std::to_string(o[1]) +
                                 " " + std::to_string(o[2]) + ") finished the grenade return script without throwing a grenade");
    }
    return 1;
}

// Cower (0x5206c0): facing 1, grenade_cower (anim mode 1). TODO(spec): 0x504630 / the followed actor (0x53ade0).
void World::grenadeCower(Entity& e) {
    e.ai.facing = 1;
    standOrFollow(e, "grenade_cower"); // 0x5206c0..0x520708
}

// Think (0x520c30): once per frame perception; for take cover / cover attack without a claimed node, react again
// (0x51f900 with keepPath); then the substate. Without a grenade: back to exposed.
int World::grenadeStateThink(Entity& e) {
    ActorAi& ai = e.ai;
    if (!ai.grenade || ai.grenade->obj->removed) {
        setActorGrenade(e, nullptr);
        aiSetState(e, AiExposed);
        return 1;
    }
    actorPerceive(e);
    if ((ai.substate == kTakeCover || ai.substate == kCoverAttack) && e.claimedNode < 0) grenadeReact(e, true);
    Entity& ge = *ai.grenade;
    auto toCombat = [&] { // 0x520710 / 0x520740
        if (!entityOf(e.obj->get("enemy"))) {
            ai.substate = kCower;
            grenadeCower(e);
            return;
        }
        ai.facing = 3;
        if (ai.substate == kCoverAttack && e.claimedNode >= 0) {
            std::string script;
            if (coverScriptAt(e, e.claimedNode, true, script) && !script.empty()) {
                setActorAnimScript(e, script, 0, ai.animModeOverride ? ai.animModeOverride : 4);
                return;
            }
        }
        combatScript(e);
    };
    auto flee = [&] { // 0x5207a0
        if (actorHasPath(e)) {
            ai.facing = 4;
            ai.planTimeMs = 0;
            followActorPath(e, true);
            return;
        }
        if (safeFromGrenade(e, ge, originOf(e.obj))) {
            ai.substate = kCombat;
            toCombat();
        } else {
            ai.substate = kCower;
            grenadeCower(e);
        }
    };
    switch (ai.substate) {
    case kFlee:
        flee();
        break;
    case kTakeCover: // 0x520860
        if (actorHasPath(e)) {
            flee();
            break;
        }
        if (e.claimedNode >= 0) {
            std::string script;
            if (coverScriptAt(e, e.claimedNode, true, script) && !script.empty() && safeFromGrenade(e, ge, originOf(e.obj))) {
                ai.substate = kCoverAttack;
                toCombat();
                break;
            }
        }
        ai.substate = kFlee;
        flee();
        break;
    case kCower:
        grenadeCower(e);
        break;
    case kCombat:
    case kCoverAttack:
        toCombat();
        break;
    case kAcquire:
        return grenadeAcquireThink(e);
    case kThrowBack:
        return grenadeThrowBackThink(e);
    default:
        break;
    }
    updateClaimedNode(e);
    return 0;
}

// 0x520230 (from the flying bounce 0x526198, eax = g_entities[trace entity], ebx = the grenade): a grenade bouncing off an
// entity whose contents (+0xc0) have a bit of 0x200c000 (a character) loses its predicted landing (+0x154..+0x15c = 0) and
// predicts it again (0x51f7c0); then every actor (0x4fe2c0 / 0x4fe310) in the grenade state (+0x8c == 3) handling this
// grenade (+0x20c4) runs the inlined give-up 0x51fff0 with skipAcquire 0.
void World::grenadeHitCharacter(Entity& ge, Entity* hit) {
    if (!hit) return;
    const uint32_t contents = hit->isActor && hit->ai.dyingContents ? hit->ai.dyingContents : entityContents(*hit);
    if (!(contents & 0x200c000u)) return;
    ge.grenade->landing = {0, 0, 0};
    predictGrenadeLanding(ge);
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& a = *entities_[i];
        if (!a.isActor || a.obj->removed || a.ai.grenade != &ge || a.ai.stack[size_t(a.ai.depth)] != AiGrenade) continue;
        grenadeGiveUp(a, false);
    }
}

} // namespace iw2::game
