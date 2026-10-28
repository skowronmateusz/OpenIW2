// Actor spawn checks of the spawn core 0x532200 (dospawn / stalingradspawn) and the actor allocation 0x4fc640:
// the telefrag test 0x4e8500, the "player can see" probe 0x532100 and the force-spawn eviction 0x4fcaf0.
// docs/spec/ai.md "Spawn checks".
#include "game/world.h"

#include <cmath>

namespace iw2::game {

using namespace script;

// r.maxs[2] of the player: ClientThink (0x4e7d68..0x4e7da2) copies the pmove box (pm mins/maxs) into r.mins / r.maxs
// after every move, so the box follows the stance (pmove maxsZ: prone 30, crouch 50, standing 70; spawn 0x4e9100 sets
// (-15,-15,0)..(15,15,70)).
float World::playerBoxHeight() const {
    return playerCmd_.stance == 2 ? 30.0f : playerCmd_.stance == 1 ? 50.0f : 70.0f;
}

// 0x4e8500: the entities the area query 0x419ea0 returns for the spawn box origin + (-15,-15,0)..(15,15,70)
// (0x5a5bdc..0x5a5bf0) with contents mask 0x200c000. The query compares the box with each linked entity's absolute
// box, which SV_LinkEntity (0x44b555..0x44b5c8) widens by 1 on every side (0x591e20), touching counts (0x419f2b..).
// Any of them that is a client with ps pm_type < 4 (client +0xfc, +4; 4 and above is dead, pmove 0x50c733) or an actor
// (+0x100) with health > 0 (+0x13c) blocks the spawn.
bool World::spawnWouldTelefrag(const Entity& spawner) {
    const Value so = spawner.obj->get("origin");
    if (!so.isVector()) return false;
    const Vec3 sp = so.asVector();
    const Vec3 qmin{sp[0] - 15.0f, sp[1] - 15.0f, sp[2]}, qmax{sp[0] + 15.0f, sp[1] + 15.0f, sp[2] + 70.0f};
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
        if (&e == &spawner || e.obj->removed || e.isSpawner || !(e.isActor || e.isPlayer)) continue;
        if (!(entityContents(e) & 0x200c000u)) continue;
        if (e.isActor && e.ai.corpse) continue; // a corpse has given its actor slot back (0x4fdba0)
        const Value ov = e.obj->get("origin");
        if (!ov.isVector()) continue;
        const Vec3 o = ov.asVector();
        // actor box (-15,-15,0)..(15,15,72) (0x4fc7xx spawn, makefakeai uses the same); the player's from pmove
        const float h = e.isActor ? 72.0f : playerBoxHeight();
        const Vec3 amin{o[0] - 15.0f - 1.0f, o[1] - 15.0f - 1.0f, o[2] - 1.0f};
        const Vec3 amax{o[0] + 15.0f + 1.0f, o[1] + 15.0f + 1.0f, o[2] + h + 1.0f};
        bool overlap = true;
        for (size_t i = 0; i < 3; ++i)
            if (amin[i] > qmax[i] || amax[i] < qmin[i]) overlap = false;
        if (!overlap) continue;
        if (isAlive(e)) return true; // player: pm_type < 4 while alive; actor: health > 0
    }
    return false;
}

// 0x532100 (eye, pos, pass1, pass2): false when distance² * 0.685584 (0x592298) exceeds the fog sight limit
// (0x1c61038); else true when any of the 11 points pos + (p.x * 30 - 15, p.y * 30 - 15, p.z * 72) (table 0x5701e8)
// is not sight-blocked from the eye (0x44c3c0, point trace, mask 0x801, passing both entities).
bool World::sightToActorBox(const Vec3& eye, const Vec3& pos, int pass1, int pass2) const {
    const float dx = eye[0] - pos[0], dy = eye[1] - pos[1], dz = eye[2] - pos[2];
    if ((dx * dx + dy * dy + dz * dz) * 0.685584f > fogSightSq_) return false;
    static const float probe[11][3] = {{0.5f, 0.5f, 0.8f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.2f}, {0, 0, 1}, {0, 1, 1}, {1, 1, 1},
                                       {1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}};
    for (const auto& p : probe) {
        const Vec3 q{pos[0] + p[0] * 30.0f - 15.0f, pos[1] + p[1] * 30.0f - 15.0f, pos[2] + p[2] * 72.0f};
        if (!collision_->sightBlocked(eye, q, {0, 0, 0}, {0, 0, 0}, 0x801u, pass1, pass2)) return true;
    }
    return false;
}

// 0x4fcaf0: called by the actor spawn 0x4fc640 when no actor slot is free and the new entity has spawnflags 2. While
// the level is still being set up (0x1c61048 set) it is Com_Error "too many actors in bsp file" (0x589e50). Otherwise
// the player (G_Find classname "player") gives the eye (actor eye +0x174 or 0x4e8b70) and the view forward from ps
// viewangles (+0xc4 pitch, +0xc8 yaw, AngleVectors). Up to four passes over the actors in use (0x4fe2c0 / 0x4fe310,
// every team), each skipping actors with spawnflags 4 (undeletable) and the entity G_RunFrame is running (0x1c61064);
// the first pass with a candidate wins:
//   1. the farthest (distance² strictly greater) the player cannot see (0x532100 false);
//   2. the farthest behind the view (forward · delta < 0);
//   3. the farthest outside the view cone: (forward · delta)² < 0.5 · distance² (0x591e10);
//   4. the farthest of all (greater or equal, so the last of equal ones).
// With none: "Tried to force spawning of an AI when all AI slots are used by undeletable AI" (0x589e00; the exe then
// reads the empty candidate). The victim gets notify "death" (0x46d0f0, const 0x1c7f05c) and G_FreeEntity (0x4d7c30),
// the same as the script delete (0x4ef570).
// TODO(spec): the exe iterates actor slots (pool 0x1c6051c); the port uses entity order, which differs only for ties.
bool World::freeActorForForceSpawn() {
    if (levelInitializing_) {
        print("too many actors in bsp file"); // Com_Error (0x42c7f0 code 1) in the exe
        return false;
    }
    if (!player_) return false; // the exe dereferences the player unconditionally
    const Vec3 eye = eyePosition(*player_);
    const Vec3 va = playerCmd_.viewAngles;
    const float kDeg = 0.017453292f; // 0x591f50
    const float cp = std::cos(va[0] * kDeg), sp = std::sin(va[0] * kDeg);
    const float cy = std::cos(va[1] * kDeg), sy = std::sin(va[1] * kDeg);
    const Vec3 fwd{cp * cy, cp * sy, -sp};
    std::vector<Entity*> actors;
    for (const auto& e : entities_) {
        if (!e->isActor || e->obj->removed || e->ai.corpse || e->isSpawner) continue;
        const Value f = e->obj->get("$spawnflags");
        if (f.isInt() && (f.asInt() & 4)) continue;
        if (e->number == runEntityNumber_) continue;
        actors.push_back(e.get());
    }
    auto originVec = [](Entity& a) {
        const Value ov = a.obj->get("origin");
        return ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
    };
    Entity* best = nullptr;
    for (int pass = 1; pass <= 4 && !best; ++pass) {
        float bestDist = 0.0f;
        for (Entity* a : actors) {
            const Vec3 o = originVec(*a);
            const Vec3 d{o[0] - eye[0], o[1] - eye[1], o[2] - eye[2]};
            const float dist = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const float dot = fwd[0] * d[0] + fwd[1] * d[1] + fwd[2] * d[2];
            bool take = false;
            switch (pass) {
            case 1: take = dist > bestDist && !sightToActorBox(eye, o, player_->number, a->number); break;
            case 2: take = dot < 0.0f && dist > bestDist; break;
            case 3: take = dist > bestDist && dist * 0.5f > dot * dot; break;
            default: take = dist >= bestDist; break;
            }
            if (take) {
                bestDist = dist;
                best = a;
            }
        }
    }
    if (!best) {
        print("Tried to force spawning of an AI when all AI slots are used by undeletable AI");
        return false;
    }
    deleteEntityScripted(*best);
    return true;
}

} // namespace iw2::game
