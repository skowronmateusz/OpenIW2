#include <cstdlib>
// Path following (docs/spec/ai.md "Path following"): the follower step 0x551960 — other sentients in the way
// (0x551100 / 0x551490), waiting for them, following their wait (0x504720) or detouring around them (0x5451b0) — and
// the move checks it uses (0x543c30 / 0x543d30 / 0x5440e0).
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

// 0x550f10: `other` is on the team `me` fights (team table {0, 2, 1, 0, 0}).
bool isEnemy(const Entity& me, const Entity& other) {
    static constexpr int kEnemy[5] = {0, 2, 1, 0, 0};
    return teamOf(other) == kEnemy[teamOf(me)];
}

float normalize2(float& x, float& y) {
    const float l = std::sqrt(x * x + y * y);
    if (l != 0.0f) {
        const float k = 1.0f / l;
        x *= k;
        y *= k;
    }
    return l;
}

float dist2d(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

constexpr int kNoEntity = 0x7ff;
constexpr bool kNoDodge = false; // ai_noDodge (dvar 0x1c13f2c, default 0)

// The follower's context (0x551960's frame, passed in EDI / EBX).
struct FollowCtx {
    float move[2]{};       // [1, 2] the actor's velocity, or 190 along the lookahead when it stands
    Vec3 origin{};         // [3 … 5]
    float dir[2]{};        // [6, 7] unit move direction
    float speed2 = 0;      // [8]
    float ahead = 0;       // [9] speed² / 100 while moving
    float near2 = 0;       // [10] interval² (at least 30²), else 30²
    float half2 = 0;       // [11] (interval / 2, at least 37.5)², else 37.5²
    float range2 = 0;      // [12] the larger of near2 and speed2
    bool tiny = false;     // [13] budget ≤ 0.001
    float best2 = 0;       // [14] distance² of the nearest blocking sentient
    std::vector<int> ents; // [15 …], count [48]: actors to test the detour moves against
    Entity* blocker = nullptr; // [49]
    bool waiting = false;  // [50] waiting for a blocker, or holding its position (0x502850) when running
    float yield2 = 0;      // [51] distance² of the nearest moving sentient that is not in the way
    Entity* yieldTo = nullptr; // [52]
    float yieldVel[2]{};   // [53, 54]
};

// One sentient (0x551100's ESI frame).
struct SentientCtx {
    Entity* s = nullptr;
    Vec3 origin{};   // [1 … 3]
    float vel[2]{};  // [4, 5]
    float off[2]{};  // [6, 7] from the actor
    float perp[2]{}; // [8, 9]
    float dist2 = 0; // [10]
    float ahead = 0; // [11]
};

} // namespace

// 0x543d30: a box (−15, −15, step)…(15, 15, 72) moved level from `start` towards `target`, then dropped by 72 and
// raised by the step. A hit that is not a sentient continues from where it stopped (and was dropped) when the box
// moved at least 15 units; a sentient lets it through (the mask loses actor contents) when it is a moving actor or
// waits for `passEnt`. The end must stand on ground no steeper than normal z 0.7 where the drop did not lower it,
// and within 72 units of the target's height. `out` gets where the box got to.
bool World::aiReach(const Vec3& start, const Vec3& target, int passEnt, uint32_t mask, Vec3& out, float step) {
    const Vec3 mins{-15, -15, step}, maxs{15, 15, 72};
    const int pass = passEnt == kNoEntity ? -1 : passEnt;
    Vec3 cur = start;
    auto drop = [&]() {
        const TraceResult tr = collision_->trace({out[0], out[1], cur[2]}, {out[0], out[1], cur[2] - 72.0f}, mins, maxs, mask, pass);
        out = tr.endPos;
        return !(out[2] >= cur[2] && tr.fraction != 1.0f && tr.normal[2] < 0.7f);
    };
    // TODO(spec): the exe loop has no found bound, but a sentient with contents bit 0x10 (a sidestepping actor, only 0x4000 is
    // dropped from the mask below) is hit again from the same place; a cap of 16 passes keeps that from spinning.
    for (int pass16 = 0;; ++pass16) {
        if (pass16 >= 16) return false;
        const TraceResult tr = collision_->trace(cur, {target[0], target[1], cur[2]}, mins, maxs, mask, pass);
        out = tr.endPos;
        if (!tr.startSolid && tr.fraction == 1.0f) break;
        Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
        if (static const bool dbg = std::getenv("IW2_REACH_LOG") != nullptr; dbg) std::fprintf(stderr, "[reach %lld] from %.0f %.0f %.0f to %.0f %.0f frac %.2f startsolid %d entity %d normal %.2f %.2f %.2f contents %x\n", (long long)timeMs_, cur[0], cur[1], cur[2], target[0], target[1], tr.fraction, int(tr.startSolid), tr.entity, tr.normal[0], tr.normal[1], tr.normal[2], unsigned(tr.contents));
        if (!hit || !isSentient(*hit)) {
            if (dist2d(cur, out) < 225.0f) return false;
        } else {
            if (!hit->isActor) return false;
            const bool moving = hit->ai.animMode == 1 && hit->ai.movemode != 0;
            if (!moving && !(hit->ai.blocker && hit->ai.blocker->number == passEnt)) return false;
            mask &= ~Contents::ActorBody;
        }
        if (!drop()) return false;
        cur = {out[0], out[1], out[2] + step};
    }
    if (!drop()) return false;
    out[2] += step;
    return std::fabs(out[2] - target[2]) < 72.0f;
}

// 0x543c30: first the listed actors alone (0x44c5b0: the box, step 10 when that actor is prone (+0x288), else 18, moved level
// towards the target), then the move check (0x543d30, step 18). A listed actor in the way fails with `out` = start.
bool World::aiReachPast(Vec3 start, const Vec3& target, const std::vector<int>& ents, int passEnt, uint32_t mask,
                        Vec3& out) {
    const Vec3 end{target[0], target[1], start[2]};
    for (int n : ents) {
        Entity* x = entityByNumber(n);
        // Actor +0x288 is the prone tilt byte (set by enterprone 0x514b8f, cleared by the think 0x4ff209).
        const float h = x && x->isActor && x->ai.prone.tilt ? 10.0f : 18.0f;
        for (const EntityClip& c : collision_->entityClips()) {
            if (c.id != n || c.model != 0) continue;
            const TraceResult tr = collision_->traceBox(c.origin, c.mins, c.maxs, c.contents, start, end, {-15, -15, h},
                                                        {15, 15, 72}, 0xffffffffu);
            if (tr.startSolid || tr.fraction < 1.0f) {
                out = start;
                return false;
            }
        }
    }
    return aiReach(start, target, passEnt, mask, out, 18.0f);
}

// 0x5440e0: the lookahead move check. While detouring around an actor only that actor is tested (0x543c30, mask
// 0x820011); around the player the mask also has player contents (0x2820011).
bool World::pathLookClear(const ActorPath& p, const Vec3& o, const Vec3& target) {
    uint32_t mask = 0x820011;
    Vec3 out;
    if (p.detour != 0 && p.avoidEnt != kNoEntity) {
        Entity* a = entityByNumber(p.avoidEnt);
        if (a && a->isActor) return aiReachPast(o, target, {p.avoidEnt}, kNoEntity, 0x820011, out);
        mask = 0x2820011;
    }
    return aiReach(o, target, kNoEntity, mask, out, 18.0f);
}

// 0x4fedc0, run at the start of every think (0x4fef20) for the actor's followed gentity (+0x2148): at 37.5 units (1406.25 squared,
// 0x591fd0) or more it is let go, and the followed actor's own link back to this one is cleared too; closer, it is let go
// when the followed actor is blocked by this one (its +0x72c). Between 30 and 37.5 units (900, 0x591fd4) a moving actor
// (anim mode 1 with a path, not itself blocked +0x728) is let go once it heads away from the followed one (dot of the
// offset to the followed with the lookahead direction +0x6c8 > 0).
// TODO(spec): the retarget to the player (0x4fec50, via the box test 0x447830, 0x504610 and 0x519050), the movement trail
// (0x505520 / 0x504f60: the follower is not path-walked in the exe, 0x5041a3) and the other consumers (0x4fe6ca, 0x4fe8eb,
// 0x50081d, 0x53ade0) are not ported.
void World::updateFollowed(Entity& e) {
    ActorAi& ai = e.ai;
    Entity* f = ai.followed;
    if (f && f->obj->removed) {
        ai.followed = nullptr;
        f = nullptr;
    }
    if (f) {
        const Vec3 mo = originOf(e.obj), fo = originOf(f->obj);
        const float dx = fo[0] - mo[0], dy = fo[1] - mo[1];
        const float d2 = dy * dy + dx * dx;
        const bool fActor = f->isActor;
        if (!(d2 < 1406.25f)) {
            if (fActor && f->ai.followed == &e) f->ai.followed = nullptr;
            ai.followed = nullptr;
        } else if (fActor && f->ai.blockerEnt == &e) {
            ai.followed = nullptr;
        }
        if (!ai.followed) ++stats.followReleases;
    }
    // 0x4fee45: unless it already follows the player (gentity 0), the retarget 0x4fec50 runs.
    if (player_ && ai.followed != player_) retargetFollowed(e, *player_);
    f = ai.followed;
    if (!f) return;
    const Vec3 mo = originOf(e.obj), fo = originOf(f->obj);
    const float d2 = (fo[1] - mo[1]) * (fo[1] - mo[1]) + (fo[0] - mo[0]) * (fo[0] - mo[0]);
    if (d2 < 900.0f) return;
    if (ai.blocker == &e || ai.animMode != 1 || e.path.count <= 0) return;
    if ((mo[0] - fo[0]) * e.path.lookDir[0] + (mo[1] - fo[1]) * e.path.lookDir[1] > 0.0f) ai.followed = nullptr, ++stats.followReleases;
}

// 0x4fec50 (actor, player gentity): an actor that avoids the player (dontavoidplayer +0x734 = 0, and the player contents bit
// +0x28c & 0x2000000), not a turret gunner (state 2), whose team is not the one the player's team fights (table {0, 2, 1, 0, 0}),
// and whose box overlaps the player's box shifted by its velocity * 0.1 (0x59202c; 0x447830, an AABB test for an actor), follows
// the player: +0x2148 = the player gentity. When it stands at its claimed node (0x504610: node set, 0x502660 close) it is skipped
// while the client byte +0x86b6 (the player moved 0.1 units or more since the last frame, 0x4e81e5) is set; otherwise its claimed node
// is taken over by the player (0x519050). TODO(spec): 0x519050 also evicts the owners of the linked nodes, which is not modelled;
// the player's mins/maxs (+0x540/+0x54c; the standing box is used).
void World::retargetFollowed(Entity& e, Entity& player) {
    ActorAi& ai = e.ai;
    const Value dap = e.obj->get("dontavoidplayer");
    if ((dap.isNumber() && dap.asInt() != 0) || !ai.noPush) return;
    if (ai.stack[size_t(ai.depth)] == AiTurret) return;
    if (isEnemy(player, e)) return;
    const Vec3 po = originOf(player.obj), ao = originOf(e.obj);
    const float px = po[0] + player.velocity[0] * 0.1f, py = po[1] + player.velocity[1] * 0.1f, pz = po[2] + player.velocity[2] * 0.1f;
    if (px + 15 < ao[0] - 15 || px - 15 > ao[0] + 15 || py + 15 < ao[1] - 15 || py - 15 > ao[1] + 15 || pz + 70 < ao[2] || pz > ao[2] + 72) return;
    // 0x4fed74: standing at its claimed node (0x504610) the actor is skipped while the player moves (+0x86b6); otherwise a claimed node
    // is handed over to the player's sentient (0x4fed9f: 0x519050, the player takes it over and the actor is evicted).
    if (nearClaimedNode(e)) {
        if (playerMoved_) return;
        if (e.claimedNode >= 0) claimNode(player, e.claimedNode, true);
    }
    ai.followed = &player;
    ++stats.followLinks;
}

// 0x504630: an actor that follows nobody takes the first other living actor (slot order) standing within 30 units and 80 vertically
// (0x5025f0, the same test as nearnode) as its followed entity. Unless it plays grenade_cower (handle 0x1c7e120) or has a claimed
// node it stands at (0x502660), the other must itself be blocked by a sentient (its +0x318 hit entity has a sentient, +0x104).
void World::scanFollowed(Entity& e) {
    static const bool enabled = [] { const char* v = std::getenv("IW2_FOLLOW_SCAN"); return !(v && v[0] == '0'); }();
    if (!enabled || e.ai.followed) return;
    const Vec3 mo = originOf(e.obj);
    bool needBlocked = true;
    if (e.animScript != "grenade_cower") {
        needBlocked = false;
        if (e.claimedNode >= 0) {
            const Vec3 no = originOf(nodes_[size_t(e.claimedNode)].obj);
            const float dz = mo[2] - no[2];
            needBlocked = dz * dz < 6400.0f && dist2d(no, mo) < 900.0f;
        }
    }
    for (auto& ep : entities_) {
        Entity& o = *ep;
        if (!o.isActor || &o == &e || o.obj->removed || !isAlive(o)) continue;
        if (needBlocked) {
            Entity* b = o.ai.bumpedEnt < 0x7fe ? entityByNumber(o.ai.bumpedEnt) : nullptr;
            if (!b || !(b->isActor || b->isPlayer)) continue;
        }
        const Vec3 oo = originOf(o.obj);
        const float dz = oo[2] - mo[2];
        if (dz * dz < 6400.0f && dist2d(mo, oo) < 900.0f) {
            e.ai.followed = &o;
            ++stats.followLinks;
            return;
        }
    }
}

// 0x53ae40 / 0x5523d2 / 0x5206c0: the scan, then without a followed entity `script` (movemode 0, anim mode 1); with one 0x53ade0: a
// moving actor (horizontal speed squared > 1, 0x591e20) plays "move" at the walk movemode, a standing one `script` with stop_soon.
void World::standOrFollow(Entity& e, const std::string& script) {
    scanFollowed(e);
    if (!e.ai.followed) {
        setActorAnimScript(e, script, 0, 1);
        return;
    }
    const float v2 = e.velocity[0] * e.velocity[0] + e.velocity[1] * e.velocity[1];
    if (v2 > 1.0f) setActorAnimScript(e, "move", 2, 1);
    else setActorAnimScript(e, script, 1, 1);
}

// 0x500d80 (this = actor, arg = the bumped entity number +0x318): the lookahead shrink flag of the followed branch. A bumped
// gentity with a sentient (+0x104: an actor or the player): level time < +0x724. Otherwise 0 when its contents hold 0x400 (an
// obstacle, as gethitenttype), else actor +0x6ec > gentity +0x1ec as signed integers. TODO(spec): neither field has a writer
// that was found (+0x6ec is only read, +0x1ec is the pain point of an actor struct and zero for other gentities), so the last
// branch is taken as false.
bool World::followShrink(Entity& e, int bumped) {
    if (bumped < 0 || bumped >= 0x7fe) return false;
    Entity* o = entityByNumber(bumped);
    if (!o) return false;
    if (o->isActor || o->isPlayer) return timeMs_ < e.ai.detourMs;
    return false;
}

// Bookkeeping after the actor move 0x5487c0: +0x318 is the block's hit entity (+0xf4). The followed link itself is written by
// the yield step (0x501c6f..0x501c8b, yieldFollowed) from the followed entity, not from the hit entity.
void World::linkBumpedActor(Entity& e, int hit) { e.ai.bumpedEnt = hit; }

// 0x504610: the actor's sentient has a claimed node (+0x2c) and stands at it (0x502660: height within 80, 2D distance within 30).
bool World::nearClaimedNode(const Entity& a) const {
    if (a.claimedNode < 0) return false;
    const Vec3 mo = originOf(a.obj), no = originOf(nodes_[size_t(a.claimedNode)].obj);
    const float dz = mo[2] - no[2];
    return dz * dz <= 6400.0f && dist2d(no, mo) <= 900.0f;
}

// 0x501380 (this = actor, run after a yield move with the block at the new position): whether the way is clear of other actors.
// Every other actor (not the one followed) that the move brought within 30 units (2D, 900) of the block position although it was 30
// or more from the actor's own origin, within 80 vertically, blocks it; one that follows nobody and does not stand at its claimed
// node (0x504610) starts following this actor.
bool World::followClear(Entity& e, const Vec3& newPos) {
    static const bool enabled = [] { const char* v = std::getenv("IW2_ACTOR_FOLLOW"); return !(v && v[0] == '0'); }();
    bool clear = true;
    const Vec3 me = originOf(e.obj);
    for (auto& ep : entities_) {
        Entity& o = *ep;
        if (!o.isActor || &o == &e || o.obj->removed || !isAlive(o) || &o == e.ai.followed) continue;
        const Vec3 oo = originOf(o.obj);
        if (!(dist2d(oo, newPos) < 900.0f)) continue;
        if (!(dist2d(oo, me) >= 900.0f)) continue;
        if (!(std::fabs(me[2] - oo[2]) < 80.0f)) continue;
        if (enabled && !o.ai.followed && !nearClaimedNode(o)) o.ai.followed = &e, ++stats.followLinks;
        clear = false;
    }
    return clear;
}

// 0x501620 (this = the actor, which has a followed entity and anim mode 1 or 7; called from the physics step 0x501d00): the actor
// steps out of the way of the entity it follows, in up to three moves of the actor block (0x5487c0), the block starting at the
// actor's origin each time with the velocity, ground entity and contact it had on entry.
//  1. Away from the followed entity: for the player, sideways to its direction of movement (0x5014b0: the perpendicular of the
//     player's horizontal velocity, or of its view direction when it stands still, on the actor's side; at least 60 * 0.05 = 3
//     units a frame, the player's speed * 0.05 above that); for an actor, straight away from it (a random direction when they
//     share the position) by the animation's frame distance (0x503e40, at least 1.5). The move is accepted when it did not end
//     closer than the actor started, went more than half of the intended distance and 0x501380 finds nobody in the way.
//  2./3. When the move hit something (and the ground entity is not "none"): the unit hit normal n (block +0x100) decides the side
//     ((x * n.y) against (y * n.x) of the first direction); the delta is the perpendicular of n at 0.75 (then 0.5625) of the
//     distance. The side memory is the gentity contents bits 0x10 / 8: when the other bit was set the new delta is added to
//     0.33 of the perpendicular instead of replacing the old one. Accepted at more than 0.375 (then 0.281) of the distance moved;
//     the bit of the side taken (0x10 for the first branch, 8 for the second) is stored.
//  When nothing worked, +0x318 = the followed entity, an actor followed that follows nobody follows this one, the velocity is zeroed
//  and the actor stays. Returns 0 when a move was impossible (0x5487c0 returned 0; the contents bits 0x18 are cleared).
bool World::yieldFollowed(Entity& e, ActorSlide& pm, const SlideTraceFn& fn, const Vec3& before) {
    static const bool enabled = [] { const char* v = std::getenv("IW2_ACTOR_FOLLOW"); return !(v && v[0] == '0'); }();
    ActorAi& ai = e.ai;
    Entity& f = *ai.followed;
    const Vec3 fo = originOf(f.obj);
    const float sv[3] = {pm.velocity[0], pm.velocity[1], pm.velocity[2]};
    const uint16_t sGround = pm.groundEnt;
    const bool sContact = pm.contact;
    const float sCnz = pm.contactNormalZ;
    float dir[2], speed, len2;
    float s14;
    if (f.isPlayer) {
        // 0x5014b0
        const float dx = fo[0] - before[0], dy = fo[1] - before[1];
        len2 = dx * dx + dy * dy;
        const float vx = f.velocity[0], vy = f.velocity[1];
        const float ps = std::sqrt(vx * vx + vy * vy);
        float ux = 0, uy = 0;
        if (ps > 0.0f) {
            ux = vx / ps;
            uy = vy / ps;
        } else {
            const Value av = f.obj->get("angles");
            const Vec3 a = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
            const float pitch = a[0] * 0.017453292f, yaw = a[1] * 0.017453292f;
            const float fx = std::cos(pitch) * std::cos(yaw), fy = std::cos(pitch) * std::sin(yaw);
            const float fl = std::sqrt(fx * fx + fy * fy);
            if (fl > 0.0f) ux = fx / fl, uy = fy / fl;
        }
        dir[0] = uy;
        dir[1] = -ux;
        if (dir[0] * (before[0] - fo[0]) + dir[1] * (before[1] - fo[1]) < 0.0f) dir[0] = -uy, dir[1] = ux;
        speed = (ps < 60.0f ? 60.0f : ps) * 0.05f;
        s14 = speed * speed;
    } else {
        dir[0] = before[0] - fo[0];
        dir[1] = before[1] - fo[1];
        const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
        len2 = l * l;
        if (l > 0.0f) {
            dir[0] /= l;
            dir[1] /= l;
        } else {
            meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3;
            const float r1 = float(meleeSeed_ >> 0x11 & 0x7fff);
            meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3;
            const float r2 = float(meleeSeed_ >> 0x11 & 0x7fff);
            dir[0] = r1 / 32768.0f * 2.0f - 1.0f;
            dir[1] = r2 / 32768.0f * 2.0f - 1.0f;
            const float rl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
            if (rl > 0.0f) dir[0] /= rl, dir[1] /= rl;
            len2 = 0.0f;
        }
        float d2 = 0.0f;
        if (e.anim) {
            const AnimDelta d = e.anim->delta(ai.animDeltaGoalWeights);
            d2 = d.trans[0] * d.trans[0] + d.trans[1] * d.trans[1];
        }
        if (d2 < 2.25f) d2 = 2.25f; // 0x592334
        s14 = d2;
        speed = std::sqrt(d2);
    }
    float delta[2] = {speed * dir[0], speed * dir[1]};
    auto restore = [&] {
        for (int a = 0; a < 3; ++a) pm.origin[a] = before[a];
        for (int a = 0; a < 3; ++a) pm.velocity[a] = sv[a];
        pm.groundEnt = sGround;
        pm.contact = sContact;
        pm.contactNormalZ = sCnz;
    };
    auto move = [&] {
        pm.moveDelta[0] = delta[0];
        pm.moveDelta[1] = delta[1];
        return moveBlock(pm, fn) != 0;
    };
    auto d2f = [&](const ActorSlide& b) {
        const float x = fo[0] - b.origin[0], y = fo[1] - b.origin[1];
        return x * x + y * y;
    };
    auto moved2 = [&](const ActorSlide& b) {
        const float x = before[0] - b.origin[0], y = before[1] - b.origin[1];
        return x * x + y * y;
    };
    auto fail0 = [&] { // 0x501be3
        ai.sideBits &= ~0x18;
        return false;
    };
    // First move.
    if (!move()) return fail0();
    if (d2f(pm) > len2 && moved2(pm) > s14 * 0.25f && followClear(e, {pm.origin[0], pm.origin[1], pm.origin[2]})) {
        ai.sideBits &= ~0x18;
        return true;
    }
    if (pm.groundEnt == 0x7ff) return true; // 0x5018c9: no ground entity
    int flag = 0;
    if (pm.hitEnt != 0x7ff) {
        static const float kMoved[2] = {0.140625f, 0.0791015625f};
        for (int attempt = 0; attempt < 2; ++attempt) {
            // 0x5018e7 / 0x501a9d: the unit hit normal, a shorter step, the side of the hit.
            float hx = pm.hitNormal[0], hy = pm.hitNormal[1];
            const float hl = std::sqrt(hx * hx + hy * hy);
            if (hl > 0.0f) hx /= hl, hy /= hl;
            pm.hitNormal[0] = hx;
            pm.hitNormal[1] = hy;
            speed *= 0.75f;
            if (dir[0] * hy <= dir[1] * hx) {
                if (!(ai.sideBits & 8)) {
                    delta[0] = -(speed * hy);
                    delta[1] = speed * hx;
                } else {
                    delta[0] = delta[0] - speed * hy * 0.33f;
                    delta[1] = delta[1] + speed * hx * 0.33f;
                }
                flag = 0x10;
            } else {
                if (!(ai.sideBits & 0x10)) {
                    delta[0] = speed * hy;
                    delta[1] = -(speed * hx);
                } else {
                    delta[0] = delta[0] + speed * hy * 0.33f;
                    delta[1] = delta[1] - speed * hx * 0.33f;
                }
                flag = 8;
            }
            restore();
            if (!move()) return fail0();
            if (d2f(pm) > len2 && moved2(pm) > s14 * kMoved[attempt] && followClear(e, {pm.origin[0], pm.origin[1], pm.origin[2]})) {
                ai.sideBits = (ai.sideBits & ~0x18) | flag;
                return true;
            }
            if (pm.hitEnt == 0x7ff) break;
        }
    }
    // 0x501c67: nothing worked. The followed entity becomes the bumped one, and an actor followed that follows nobody follows this one.
    ai.bumpedEnt = f.number;
    pm.hitEnt = uint16_t(f.number);
    if (enabled && f.isActor && !f.ai.followed) f.ai.followed = &e, ++stats.followLinks;
    ai.sideBits &= ~0x18;
    restore();
    for (int a = 0; a < 3; ++a) pm.velocity[a] = 0.0f;
    return true;
}

// 0x5451b0: a detour past a blocker: from `o` to the corner `a`, on to `b`, and back onto the path at point `k` (or
// the one after it). Failing that, with `split` the second corner is pulled back 15 units towards the first and the
// path is split at the lookahead point when that is reachable from it. The corners become the path's last points
// (+0x6b4 counts them) and the lookahead is re-aimed (0x541a70).
bool World::pathDetour(Entity& e, const Vec3& o, const Vec3& a, const Vec3& b, int k, const std::vector<int>& ents,
                       uint32_t mask, bool split) {
    ActorPath& p = e.path;
    const int pass = e.number;
    Vec3 ra, rb, tmp;
    aiReachPast(o, a, ents, pass, mask, ra);
    aiReachPast(ra, b, ents, pass, mask, rb);
    bool joined = false;
    if (k >= p.traversal) {
        joined = aiReachPast(rb, p.pts[size_t(k)].pos, ents, pass, mask, tmp);
        if (!joined) {
            int lim = p.count - 2;
            if (p.detour > 0) lim -= p.detour;
            if (k < lim) {
                ++k;
                joined = aiReachPast(rb, p.pts[size_t(k)].pos, ents, pass, mask, tmp);
            }
        }
    }
    if (joined) {
        const int n = k - 0x1d;
        if (n > 0) {
            dropPathGoalPoints(p, n);
            k -= n;
        }
    } else {
        if (!split) return false;
        float vx = rb[0] - ra[0], vy = rb[1] - ra[1];
        if (normalize2(vx, vy) > 15.0f) {
            rb[0] -= vx * 15.0f;
            rb[1] -= vy * 15.0f;
        } else {
            rb = ra;
        }
        k = p.lookIndex;
        const Vec3 look{p.lookDist * p.lookDir[0] + o[0], p.lookDist * p.lookDir[1] + o[1], p.pts[size_t(k)].pos[2]};
        if (!aiReachPast(rb, look, ents, pass, mask, tmp)) return false;
        const int n = k - 0x1c;
        if (n > 0) {
            dropPathGoalPoints(p, n);
            k -= n;
        }
        PathPoint& q = p.pts[size_t(k)];
        PathPoint& nx = p.pts[size_t(k + 1)];
        const float s = p.lookSegLen;
        nx.pos = {q.pos[0] - s * q.dir[0], q.pos[1] - s * q.dir[1], q.pos[2] - (q.pos[2] - nx.pos[2]) / q.len * s};
        q.len = s;
        ++k;
        p.lookIndex = k;
        p.lookSegLen = 0;
        p.pts[size_t(k)].node = -1;
    }
    p.detour = 0;
    auto append = [&](const Vec3& v) {
        PathPoint& q = p.pts[size_t(k)];
        if (q.pos[0] == v[0] && q.pos[1] == v[1]) return;
        float dx = q.pos[0] - v[0], dy = q.pos[1] - v[1];
        q.len = normalize2(dx, dy);
        q.dir = {dx, dy};
        ++p.detour;
        ++k;
        p.pts[size_t(k)].pos = v;
        p.pts[size_t(k)].node = -1;
    };
    append(rb);
    append(ra);
    p.startLen = k > 0 ? p.pts[size_t(k - 1)].len : 0.0f;
    p.start = ra;
    p.pts[size_t(k)].len = 0;
    p.pts[size_t(k)].dir = {0, 0};
    p.flags &= ~0x143u;
    p.count = p.planned = k + 1;
    pathLookNew(e, o);
    return true;
}

// 0x504720: an actor moving along a path that waits for a chain of blockers waits for the last one (the actor that
// is blocked itself) while that one's plan time (plus a frame when it has not thought yet) is ahead; otherwise, or
// when it is not moving, it waits for nobody.
void World::resolveBlocker(Entity& a) {
    ActorAi& ai = a.ai;
    if (ai.animMode == 1) {
        if (ai.blocker == &a) return;
        Entity* x = &a;
        // The original follows the chain without a limit.
        for (int guard = 0; guard < 64; ++guard) {
            x = x->ai.blocker;
            if (!x) break;
            if (x->ai.blocker == x) {
                const int64_t t = x->ai.planTimeMs + (x->ai.thoughtMs == timeMs_ ? 0 : 50);
                if (timeMs_ < t && &a != x->ai.blockerEnt) {
                    ai.blocker = x;
                    ai.blockerEnt = x->ai.blockerEnt;
                    return;
                }
                break;
            }
        }
    }
    ai.blocker = nullptr;
    ai.blockerEnt = nullptr;
}

// The follower step (0x551960). Returns 0 to move on, 1 to stand this frame, 2 to walk behind a moving sentient.
// It waits while the plan time (+0x720) is ahead (50 ms more while following an actor). The actor's move (its
// velocity, or 190 along the lookahead when it stands) is checked against the other sentients (0x551100: within 80
// units of height and of the larger of the interval radius and the move; 0x551490: within 30 units of the move line
// up to the lookahead point or its 37.5-unit circle): the nearest in the way is the blocker. Moving sentients ahead
// that are not in the way are walked behind (0x551740). No blocker: the avoided sentient (+0x6ba) is let go once
// 60 units away (0x551800). An enemy blocker closer than 37.5 stops the actor (dropping the path when it is the
// enemy). Otherwise the actor may wait out the blocker's own wait (0x504720), detour on either side (0x5451b0;
// +0x724 = now + 1000), or stand blocked (+0x728 = itself, +0x72c the blocker, 500 ms), dropping the path every
// other second when a detour was already tried.
int World::stepActorPath(Entity& e, bool wide, bool run) {
    ActorAi& ai = e.ai;
    ActorPath& p = e.path;
    int result = 0;
    if (timeMs_ < ai.planTimeMs) return 1;
    if (ai.followed) {
        ai.planTimeMs = timeMs_ + 50;
        return 1;
    }
    FollowCtx c;
    c.origin = originOf(e.obj);
    // 0x550fd0.
    c.move[0] = e.velocity[0];
    c.move[1] = e.velocity[1];
    c.dir[0] = c.move[0];
    c.dir[1] = c.move[1];
    float len = normalize2(c.dir[0], c.dir[1]);
    if (len < 1.0f) {
        c.dir[0] = p.lookDir[0];
        c.dir[1] = p.lookDir[1];
        if (c.dir[0] == 0.0f && c.dir[1] == 0.0f) {
            if (p.traversal < 1) {
                clearActorPath(e);
                return 1;
            }
            return 0;
        }
        c.move[0] = c.dir[0] * 190.0f;
        c.move[1] = c.dir[1] * 190.0f;
        len = 190.0f;
    }
    c.speed2 = len * len;
    // 0x550f60.
    if (wide) {
        const Value iv = e.obj->get("interval");
        float f = iv.isNumber() ? iv.asFloat() : 0.0f;
        float h = 0.5f * f;
        if (h < 37.5f) {
            h = 37.5f;
            if (f < 30.0f) f = 30.0f;
        }
        c.near2 = f * f;
        c.half2 = h * h;
    } else {
        c.near2 = 900.0f;
        c.half2 = 1406.25f;
    }
    c.range2 = c.near2 - c.speed2 < 0.0f ? c.speed2 : c.near2;
    c.waiting = ai.blocker != nullptr || (run && holdsPosition(e));
    c.best2 = c.range2;
    c.yield2 = c.near2;
    c.tiny = p.budget <= 0.001f;
    c.ahead = ai.movemode != 0 ? c.speed2 * 0.01f : 0.0f;

    // 0x551100: whether the sentient counts at all (true: it blocks outright; false after recording a yield).
    auto consider = [&](SentientCtx& s) {
        Entity& S = *s.s;
        Entity* other = S.isActor ? &S : nullptr;
        if (other) {
            if (isEnemy(e, S) || &S == &e || other->ai.blockerEnt == &e) return false;
        }
        s.origin = originOf(S.obj);
        if (std::fabs(c.origin[2] - s.origin[2]) >= 80.0f) return false;
        s.off[0] = s.origin[0] - c.origin[0];
        s.off[1] = s.origin[1] - c.origin[1];
        s.dist2 = s.off[1] * s.off[1] + s.off[0] * s.off[0];
        if (!(s.dist2 < c.range2)) return false;
        s.ahead = c.move[0] * s.off[0] + c.move[1] * s.off[1] - c.ahead;
        if (s.ahead <= 0.0f && p.farDir[0] * s.off[0] + p.farDir[1] * s.off[1] <= 0.0f && !c.tiny) return false;
        const bool close = s.dist2 < 900.0f;
        const bool meMoving = e.velocity[1] * e.velocity[1] + e.velocity[0] * e.velocity[0] >= 1.0f;
        if (!other) {
            if (!(s.dist2 < c.best2)) return false;
            // The actor's clip mask (+0x28c) must hold player contents; TODO(spec): taken as the actor clip mask.
            if (!(Contents::ActorClipMask & Contents::PlayerBody)) return false;
            if (isEnemy(e, S)) return true;
            s.vel[0] = S.velocity[0];
            s.vel[1] = S.velocity[1];
        } else {
            // TODO(spec): the move delta +0x250 (here the last frame's velocity).
            if (!other->ai.blocker && !other->ai.followed) {
                s.vel[0] = other->velocity[0];
                s.vel[1] = other->velocity[1];
            }
            // TODO(spec): following (+0x2148): the exe sets the bumped actor's +0x2148 at 0x501c8b; the lifecycle is not ported (docs/spec/ai.md "Actor trace box, followed actor").
        }
        const bool still = s.vel[1] * s.vel[1] + s.vel[0] * s.vel[0] < 1.0f;
        const bool selfBlocked = ai.blocker == &e;
        bool yield = true;
        if (!selfBlocked && !still) {
            if (!(s.dist2 < c.near2)) return false;
            if (c.move[1] * s.vel[1] + c.move[0] * s.vel[0] < 0.0f) {
                if (!c.waiting && (meMoving || !close)) {
                    if (e.number <= S.number) return false;
                    yield = false;
                }
            } else {
                if (c.ahead + s.ahead <= 0.0f) return false;
                if (other && e.number > S.number && s.vel[1] * s.off[1] + s.off[0] * s.vel[0] < c.ahead) return false;
            }
        }
        if (other) c.ents.push_back(S.number);
        if (!(s.dist2 < c.best2)) return false;
        if (selfBlocked || still) return true;
        if (!(s.dist2 < c.yield2)) return false;
        if (!yield) return true;
        c.yield2 = s.dist2;
        c.yieldTo = &S;
        c.yieldVel[0] = s.vel[0];
        c.yieldVel[1] = s.vel[1];
        return false;
    };
    // 0x551490: whether the sentient is in the way.
    auto inWay = [&](SentientCtx& s) {
        Entity& S = *s.s;
        const float cross = s.off[1] * c.dir[0] - c.dir[1] * s.off[0];
        const float v = 900.0f - cross * cross;
        if (v <= 0.0f) return false;
        const float t = s.off[0] * c.dir[0] + s.off[1] * c.dir[1] - p.lookDist;
        if (t > 0.0f && !(v + 1406.25f - 900.0f > t * t)) return false;
        if (c.tiny) return true;
        s.ahead /= c.speed2;
        s.perp[0] = -s.ahead * c.move[0] + s.off[0];
        s.perp[1] = -s.ahead * c.move[1] + s.off[1];
        // 0x5449a0: the lookahead point, at most 60 along.
        auto lookPoint = [&](float q[2]) {
            if (p.count <= 1 || (p.flags & 1)) return false;
            const float l = p.lookDist > 60.0f ? 60.0f : p.lookDist;
            q[0] = p.lookDir[0] * l + c.origin[0];
            q[1] = p.lookDir[1] * l + c.origin[1];
            return true;
        };
        float q[2];
        auto farTest = [&] { // 0x5516cc
            s.ahead = p.farDir[0] * s.off[0] + p.farDir[1] * s.off[1];
            if (s.ahead <= 0.0f) return false;
            s.perp[0] = -s.ahead * p.farDir[0] + s.off[0];
            s.perp[1] = -s.ahead * p.farDir[1] + s.off[1];
            return s.perp[1] * s.perp[1] + s.perp[0] * s.perp[0] < 900.0f;
        };
        auto fromLook = [&] { // 0x551689
            if (!lookPoint(q)) return true;
            s.off[0] = s.origin[0] - q[0];
            s.off[1] = s.origin[1] - q[1];
            if (c.move[1] * s.off[1] + c.move[0] * s.off[0] <= 0.0f) return true;
            return farTest();
        };
        if (S.number == p.avoidEnt) {
            if (c.waiting) {
                if ((p.detour != 0 || (p.flags & 2)) && !(p.flags & 1) && std::sqrt(s.dist2) + 37.5f > p.lookDist) return true;
            } else if (ai.movemode != 0 && timeMs_ < ai.detourMs) {
                if (s.ahead <= 0.0f) return false;
                if (!(s.perp[1] * s.perp[1] + s.perp[0] * s.perp[0] < 900.0f)) return false;
                return fromLook();
            }
        }
        const float perp2 = s.perp[1] * s.perp[1] + s.perp[0] * s.perp[0];
        if (perp2 < 1406.25f) {
            if (s.ahead > 0.0f) return fromLook();
            if (!(s.dist2 < 3600.0f)) return false;
            if (!lookPoint(q)) return true;
        } else {
            if (!(s.dist2 < 3600.0f)) return false;
            if (!lookPoint(q)) return false;
        }
        s.off[0] = s.origin[0] - q[0];
        s.off[1] = s.origin[1] - q[1];
        return farTest();
    };

    for (const auto& ep : entities_) {
        Entity& S = *ep;
        if (!isSentient(S) || S.ai.corpse) continue;
        SentientCtx s;
        s.s = &S;
        if (consider(s) && inWay(s)) {
            c.best2 = s.dist2;
            c.blocker = &S;
        }
    }
    // 0x551740: a nearer moving sentient that is not in the way: walk behind one running (or walking, not too
    // close) along a path, else it becomes the blocker; one coming towards the actor makes it wait 500 ms.
    if (c.yieldTo && c.yield2 < c.best2) {
        Entity* other = c.yieldTo->isActor ? c.yieldTo : nullptr;
        if (!other || c.move[0] * c.yieldVel[0] + c.move[1] * c.yieldVel[1] < 0.0f) {
            ai.planTimeMs = timeMs_ + 500;
            return 1;
        }
        const bool running = other->ai.movemode == 3;
        const bool walking = other->ai.movemode == 2 && !(c.yield2 < c.half2);
        if (other->ai.animMode == 1 && (running || walking)) {
            result = 2;
        } else {
            c.blocker = c.yieldTo;
            c.best2 = c.yield2;
        }
    }
    Entity* S = c.blocker;
    ai.blocker = nullptr;
    ai.blockerEnt = nullptr;
    if (!S || kNoDodge) {
        // 0x551800.
        if (!c.tiny && c.yieldTo && c.yieldVel[1] * c.yieldVel[1] + c.yieldVel[0] * c.yieldVel[0] < c.speed2) {
            if (p.detour == 0) p.detour = -1;
            p.avoidEnt = c.yieldTo->number;
            return 0;
        }
        const int d = p.detour;
        if (d != 0) {
            if (p.avoidEnt != kNoEntity && (d >= 0 || p.lookDist >= 37.5f)) {
                Entity* a = entityByNumber(p.avoidEnt);
                if (a && dist2d(c.origin, originOf(a->obj)) < 3600.0f) return result;
            }
            if (d < 0) p.detour = 0;
        }
        if (p.avoidEnt != kNoEntity) {
            p.avoidEnt = kNoEntity;
            if (ai.movemode == 0 && timeMs_ >= ai.detourMs) return 1;
        }
        ai.detourMs = 0;
        return result;
    }
    if (S->number != p.avoidEnt) {
        ai.detourMs = 0;
        if (p.detour < 0) p.detour = 0;
        p.avoidEnt = S->number;
    }
    if (isEnemy(e, *S)) {
        if (!(c.best2 < 1406.25f)) return result;
        if (entityOf(e.obj->get("enemy")) == S) clearActorPath(e); // sentient +0x1c taken as the enemy
        ai.planTimeMs = timeMs_ + 500;
        return 1;
    }
    Entity* other = S->isActor ? S : nullptr;
    const Vec3 spos = originOf(S->obj);
    // 0x5521d9: wait out the other actor's wait.
    auto adoptBlocker = [&] {
        if (!other) return false;
        resolveBlocker(*other);
        Entity* b = other->ai.blocker;
        if (!b || b == &e || other->ai.blockerEnt == &e || ai.stack[size_t(ai.depth)] == AiGrenade) return false;
        if (p.detour < 0) p.detour = 0;
        ai.blocker = b;
        ai.blockerEnt = other->ai.blockerEnt;
        ai.planTimeMs = timeMs_ + 500;
        ai.detourMs = 0;
        return true;
    };
    auto lookShort = [&] { return !(p.lookDist >= 60.0f) && !(p.flags & 1); };
    const bool tryFirst = ai.movemode == 0 ? (!(p.flags & 2) || lookShort()) : (!(p.flags & 2) && lookShort());
    if (tryFirst && adoptBlocker()) return 1;

    if (timeMs_ < ai.detourMs) return result;
    // The point to rejoin: the first one (from the start end) at least √best2 away, not past the lookahead or a detour.
    const int last = p.count - 1;
    int k = last;
    if (p.detour > 0) k = last - p.detour;
    if (p.lookIndex < k) k = p.lookIndex;
    bool scan = true;
    if (k == last) {
        if (dist2d(c.origin, p.start) < c.best2) --k;
        else scan = false;
    }
    if (scan)
        while (k >= p.traversal && dist2d(c.origin, p.pts[size_t(k)].pos) < c.best2) --k;
    const bool split = p.lookSegLen != 0.0f && !(std::sqrt(c.best2) + 37.5f > p.lookDist);
    if (split || k >= p.traversal) {
        const float px = c.dir[1] * 37.5f, py = c.dir[0] * -37.5f;
        Vec3 a{spos[0] - c.dir[0] * 37.5f + px, spos[1] - c.dir[1] * 37.5f + py, spos[2]};
        Vec3 b{c.dir[0] * 75.0f + a[0], c.dir[1] * 75.0f + a[1], spos[2]};
        // TODO(spec): the actor's clip mask +0x28c (taken as the actor clip mask).
        const uint32_t mask = Contents::ActorClipMask | (other ? 4u : 0x4004u);
        Vec3 from = c.origin;
        if (c.best2 < 1406.25f) {
            float tx = spos[0] - from[0], ty = spos[1] - from[1];
            if (normalize2(tx, ty) < 37.5f)
                aiReachPast(from, {spos[0] - tx * 37.5f, spos[1] - ty * 37.5f, spos[2]}, c.ents, e.number, mask, from);
        }
        bool ok = pathDetour(e, from, a, b, k, c.ents, mask, split);
        if (!ok) {
            a[0] -= 2 * px;
            a[1] -= 2 * py;
            b[0] -= 2 * px;
            b[1] -= 2 * py;
            ok = pathDetour(e, from, a, b, k, c.ents, mask, split);
        }
        if (ok) {
            ai.detourMs = timeMs_ + 1000;
            return ai.movemode != 0 ? result : 1;
        }
    }
    // No detour.
    auto scripted = [](const Entity& a) {
        const int st = a.ai.stack[size_t(a.ai.depth)];
        return st == AiScripted || st == AiCustom;
    };
    bool waitLong = false, blocked = false;
    if (other && ai.substate == 100 && !(other->ai.substate == 100 && other->path.count > 0)) {
        waitLong = true;
    } else {
        if (!(c.best2 < 1406.25f)) {
            if (ai.movemode != 0) return result;
            if (!(c.best2 < 3600.0f) && (p.lookDist >= 60.0f || (p.flags & 1))) waitLong = true;
        }
        if (!waitLong) {
            bool timed = false, lastTry = false;
            if (p.detour < 0) {
                if (p.lookDist >= 60.0f || (p.flags & 1)) timed = true;
                else lastTry = true;
            } else if (p.detour > 0 || !other) {
                timed = true;
            } else if ((other->ai.animMode == 1 && other->ai.movemode == 0 && holdsPosition(*other)) || scripted(*other)) {
                waitLong = true;
            } else {
                timed = true;
            }
            if (timed) {
                if ((timeMs_ / 500) % 4 != 0) {
                    blocked = true;
                } else if (p.detour >= 0) {
                    if (p.detour != 0 || c.tiny) clearActorPath(e);
                    else p.detour = -1;
                    blocked = true;
                } else {
                    lastTry = true;
                }
            }
            if (lastTry) {
                if (other && (other->ai.animMode == 1 || scripted(*other))) {
                    waitLong = true;
                } else {
                    p.detour = 0;
                    blocked = true;
                }
            }
        }
    }
    if (blocked) {
        if (!other || other->ai.blockerEnt != &e) {
            ai.blocker = &e;
            ai.blockerEnt = S;
        }
        ai.planTimeMs = timeMs_ + 500;
        return 1;
    }
    // Wait long: no detour for a second.
    ai.detourMs = timeMs_ + 1000;
    if (ai.movemode != 0) return result;
    if (!tryFirst) adoptBlocker();
    return 1;
}

} // namespace iw2::game
