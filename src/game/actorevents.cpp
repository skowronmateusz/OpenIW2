// AI events (0x535770 point broadcast, 0x535860 line broadcast, dispatch 0x535d70; docs/spec/ai.md "AI events"):
// what one entity does (fires, is hurt, dies) is told to the actors around it — they learn positions
// (0x500c50), mark attackers (+0x1780) and, for the suppression line (type 16, from Bullet_Fire_Extended), add a
// danger plane (0x54b3b0) that the path planner routes around. Evidence: the objdump
// call sites listed in the spec.
#include "game/world.h"

#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

// The radius² defaults of 0x535770 / 0x535860 (table 0x57031c, float per type, stride 8).
float aiEventRadius2(int type) {
    switch (type) {
    case 3: return 65536.0f;      // 256²
    case 4: case 6: case 13: return 1048576.0f; // 1024²
    case 10: return 4194304.0f;   // 2048²
    case 11: return 16777216.0f;  // 4096²
    case 16: return 16384.0f;     // 128²
    case 20: return 65536.0f;     // 256²
    default: return 262144.0f;    // 512² (types 2, 5, 7, 8, 9, 12, 17 and anything else)
    }
}

// The 0x535770 / 0x535860 gate (when not forced, EAX flags 0): the source must be a sentient of a team that has an
// enemy (axis ↔ allies, table {0, 2, 1, 0, 0}).
bool aiEventSourceValid(const Entity& source, bool forced) {
    if (forced) return true;
    if (!(source.isActor || source.isPlayer)) return false;
    const Value t = source.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" || s == "allies";
}

// 0x535770 with EAX 0: the receivers are the actors of the source's enemy team (table {0, 2, 1, 0, 0}).
bool aiEventEnemyTeams(const Entity& source, const Entity& actor) {
    const Value t = source.obj->get("team"), u = actor.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string(), a = u.isString() ? u.asString() : std::string();
    return (s == "axis" && a == "allies") || (s == "allies" && a == "axis");
}

bool aiEventSameTeam(const Entity& source, const Entity& actor) {
    const Value t = source.obj->get("team"), u = actor.obj->get("team");
    return t.isString() && u.isString() && t.asString() == u.asString();
}

} // namespace

// 0x500c50: `target` learns `about`'s current position (last known time, position and where the target itself was
// then; the ignore-until copy of sentient +0x38 is not modelled — the field is not read elsewhere in the exe).
void World::actorLearns(Entity& target, Entity& about) {
    SentientInfo& info = target.sentientInfo[about.obj.get()];
    info.lastKnownMs = timeMs_;
    info.lastKnownPos = about.obj->get("origin").isVector() ? about.obj->get("origin").asVector() : Vec3{0, 0, 0};
    info.ownPosThen = target.obj->get("origin").isVector() ? target.obj->get("origin").asVector() : Vec3{0, 0, 0};
}

// 0x54ddc0: the target has known `about` within `ms` (0 counts as never) — the handlers only update stale knowledge.
bool World::actorKnowsRecently(const Entity& target, const Entity& about, int64_t ms) const {
    auto it = target.sentientInfo.find(about.obj.get());
    if (it == target.sentientInfo.end() || it->second.lastKnownMs <= 0) return false;
    return timeMs_ - it->second.lastKnownMs < ms;
}

// The 0x535d70 dispatch of one actor (`target`) receiving an event of `type` from `source`. Types 7 (grenade cook),
// 12 (door open) and 13 (door kick) only print "not yet handled" in the exe; 11 does nothing; 8/9 (grenade ping) are
// implemented as the per-actor loop of `grenadePing` (0x526c07 → 0x5200e0), not through this dispatch.
// TODO(spec): the turret's entity +0x10c field of the pain/death handler (only the "is on a turret" part is
// modelled), pain accumulator 0x5485a0 (field meanings of actor+0x224.. not decoded, TODO(spec)) and grenade flee fallback 0x5429e0 (node search radius 192 via 0x517f90, TODO(spec)). Player events 2/3 are ported (playerPmEvents).
void World::aiEventDispatch(Entity& source, Entity& target, int type) {
    switch (type) {
    case 2:
    case 3: // pain / death of `source` (its attacker is `source` itself here): the actors around learn the attacker
        if (actorKnowsRecently(target, source, 2000)) break;
        // 0x54afd0 + 0x535dab: an actor using a turret only updates while it knew the source within 3 s
        if (target.usingTurret && !actorKnowsRecently(target, source, 3000)) break;
        actorLearns(target, source);
        break;
    case 4: { // weapon fired (0x550a70 → 0x535ba0)
        if (!source.isActor) { // the player (entity +0x100 is 0): the plain position update
            actorLearns(target, source);
            break;
        }
        // The shooter's own knowledge of its enemy (per-sentient entries +0x1780…, copied with the attacked and
        // ignore-until flags cleared) is passed on when the target knows the enemy less recently.
        Entity* enemy = entityOf(source.obj->get("enemy"));
        if (!enemy || !(enemy->isActor || enemy->isPlayer)) break;
        SentientInfo& mine = source.sentientInfo[enemy->obj.get()];
        SentientInfo& theirs = target.sentientInfo[enemy->obj.get()];
        if (theirs.lastKnownMs >= mine.lastKnownMs) break;
        theirs.lastAttackedMs = 0;
        theirs.ignoreUntilMs = 0;
        theirs.lastKnownMs = mine.lastKnownMs;
        theirs.lastKnownPos = mine.lastKnownPos;
        theirs.ownPosThen = mine.ownPosThen;
        break;
    }
    case 5: // in pain for 500 ms (0x54d340): the source's attacker is revealed
        if (Entity* killer = source.lastAttacker; killer && (killer->isActor || killer->isPlayer) &&
                                                 !actorKnowsRecently(target, *killer, 2000)) {
            target.sentientInfo[killer->obj.get()].lastAttackedMs = timeMs_;
            actorLearns(target, *killer);
        }
        break;
    case 6: // death (0x548a10): the actors around learn the killer
        if (Entity* killer = source.lastAttacker; killer && (killer->isActor || killer->isPlayer) &&
                                                 !actorKnowsRecently(target, *killer, 2000))
            actorLearns(target, *killer);
        break;
    case 10: // bullet fired (Bullet_Fire_Extended 0x531443)
        if (!actorKnowsRecently(target, source, 2000)) actorLearns(target, source);
        break;
    default:
        break;
    }
}

// The AI part of G_ClientEvents (0x4e7620–0x4e7890): events since the last call, at most the newest 4 (the loop starts
// at max(old, sequence − 4)). The crouched player (pm_flags 2) makes no noise; the event goes to the actors of the
// enemy team (0x4e780a: team table {0,2,1,0,0}, EAX of 0x535770) at the player's origin with the type's radius.
// TODO(spec): the client flag test `+0x85c8 & 0x1000` (0x4e77b7) skips the event too; its meaning is not identified.
// The jump events 0x46..0x5c are processed like the run steps but nothing in pmove adds them yet.
void World::playerPmEvents(int eventSeq, const int (&events)[4], bool crouched) {
    int from = playerEventSeq_;
    playerEventSeq_ = eventSeq;
    if (from < eventSeq - 4) from = eventSeq - 4;
    Entity* p = player();
    if (!p || crouched) return;
    for (int i = from; i < eventSeq; ++i) {
        const int ev = events[i & 3];
        const bool run = (ev >= 1 && ev < 0x18) || (ev >= 0x46 && ev < 0x74);
        const bool walk = ev >= 0x18 && ev < 0x2f;
        if (!run && !walk) continue;
        const Value o = p->obj->get("origin");
        ++stats.playerNoise;
        aiEventPoint(*p, walk ? 3 : 2, o.isVector() ? o.asVector() : Vec3{0, 0, 0}, 0.0f, false, AiHear::Enemy);
    }
}

// 0x535770: an event of `type` at `pos`, radius² `radius2` (0: the type's default from the table 0x57031c), from
// `source` to every actor within the radius. `forced` is the broadcast's EAX flag (bit 0 skips the team gate; the
// producers pass 1, or 1 | 1 << team, except the weapon-fire line 0x526272 which passes 0).
void World::aiEventPoint(Entity& source, int type, const Vec3& pos, float radius2, bool forced, AiHear hear) {
    if (!forced && hear == AiHear::All) hear = AiHear::Enemy; // EAX 0: the table's enemy team
    if (!aiEventSourceValid(source, forced)) return;
    if (radius2 == 0.0f) radius2 = aiEventRadius2(type);
    for (const auto& ep : entities_) {
        Entity& a = *ep;
        if (!a.isActor || a.obj->removed || !a.ai.alive || a.ai.corpse) continue;
        if (hear == AiHear::Enemy && !aiEventEnemyTeams(source, a)) continue;
        if (hear == AiHear::Own && !aiEventSameTeam(source, a)) continue; // 1 << the source's team
        const Vec3 o = a.obj->get("origin").isVector() ? a.obj->get("origin").asVector() : Vec3{0, 0, 0};
        const float dx = o[0] - pos[0], dy = o[1] - pos[1], dz = o[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz <= radius2) aiEventDispatch(source, a, type);
    }
}

// 0x535860 with its handler 0x535f20: an event of `type` along the segment a–b. Every actor within `radius2` (0: the
// type's default) of the segment — measured at the closest point of it, behind `a` does not count — is told: the
// attacker mark (+0x1780 = now) and the source's position, and for type 16 (the suppression line of
// Bullet_Fire_Extended) a danger plane on the far side of the segment (0x54b3b0). `source` must be a sentient.
void World::aiEventLine(Entity& source, int type, const Vec3& a, const Vec3& b, float radius2, bool forced) {
    if (!(source.isActor || source.isPlayer)) return;
    if (!aiEventSourceValid(source, forced)) return;
    if (radius2 == 0.0f) radius2 = aiEventRadius2(type);
    const Vec3 d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (const auto& ep : entities_) {
        Entity& t = *ep;
        if (!t.isActor || t.obj->removed || !t.ai.alive || t.ai.corpse) continue;
        if (!forced && !aiEventEnemyTeams(source, t)) continue;
        const Vec3 o = t.obj->get("origin").isVector() ? t.obj->get("origin").asVector() : Vec3{0, 0, 0};
        const float along = (o[0] - a[0]) * d[0] + (o[1] - a[1]) * d[1] + (o[2] - a[2]) * d[2];
        if (along < 0.0f) continue; // behind the start: no event
        Vec3 p = b;
        if (along < len2) {
            const float f = len2 != 0.0f ? along / len2 : 0.0f;
            p = {a[0] + d[0] * f, a[1] + d[1] * f, a[2] + d[2] * f};
        }
        const float dx = o[0] - p[0], dy = o[1] - p[1], dz = o[2] - p[2];
        if (dx * dx + dy * dy + dz * dz > radius2) continue;
        // 0x535f20: attacker mark and position for both types, the danger plane for 16.
        t.sentientInfo[source.obj.get()].lastAttackedMs = timeMs_;
        actorLearns(t, source);
        if (type == 16) addDangerPlane(t, source, a, b);
    }
}

} // namespace iw2::game
