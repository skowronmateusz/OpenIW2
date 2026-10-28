// Actor facing and turning (docs/spec/ai.md "Facing and desiredangle"). Evidence: the move frame
// 0x504450 -> 0x504160 and its tail 0x504aa0 (anim yaw into the blocks), 0x505d80 (facing selector), 0x504ca0 (body turn and
// desiredangle), 0x504be0 (look turn); the trail ring 0x504de0 / 0x504e70 / 0x504f60 / 0x505520 fed by the path walk 0x505230.
//
// An actor has two facing blocks: the base (+0x104 mode, +0x108 look pitch, +0x10c look yaw, +0x110 body yaw) and the
// orientmode override (+0x114 .. +0x120); the override is active while its mode is not 0. Angles stored in a block are
// quantised to the 16-bit binary angle grid (x * 65536/360 truncated, & 0xffff, * 360/65536: 0x591e68 / 0x591f10).
#include "game/world.h"

#include <cfloat>
#include <cmath>

namespace iw2::game {

namespace {

float quant(float deg) { return float(int(deg * 182.04444885253906f) & 0xffff) * 0.0054931640625f; }

// 0x423f50: a - b brought into [-180, 180] (two loops: subtract 360 while > 180, add 360 while < -180).
float angleSub(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

float clampf(float v, float lo, float hi) { return v > hi ? hi : v < lo ? lo : v; }

Vec3 originOfObj(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

Vec3 anglesOf(const ObjectPtr& o) {
    const Value v = o->get("angles");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

// 0x421b30 (vectoangles): pitch = -atan2(z, |xy|), yaw = atan2(y, x), both in degrees and brought to [0, 360).
void vecToAngles(const Vec3& v, float& pitch, float& yaw) {
    if (v[1] == 0.0f && v[0] == 0.0f) {
        yaw = 0.0f;
        pitch = v[2] > 0.0f ? 270.0f : 90.0f;
        return;
    }
    yaw = std::atan2(v[1], v[0]) * 180.0f / 3.14159265358979323846f;
    if (yaw < 0.0f) yaw += 360.0f;
    pitch = std::atan2(v[2], std::sqrt(v[0] * v[0] + v[1] * v[1])) * -180.0f / 3.14159265358979323846f;
    if (pitch < 0.0f) pitch += 360.0f;
}

int teamIndex(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

} // namespace

World::FaceBlock World::activeFaceBlock(Entity& e) {
    ActorAi& ai = e.ai;
    if (ai.orient != 0) return {ai.orient, ai.orientLook, ai.orientTarget};
    return {ai.facing, ai.faceLook, ai.faceYaw};
}

// 0x504950 / 0x5048b0: look pitch, look yaw and body yaw of a block (the yaw twice).
void World::setFaceBlock(FaceBlock b, float pitch, float yaw) {
    b.look[0] = quant(pitch);
    b.look[1] = quant(yaw);
    b.yaw = quant(yaw);
}

// 0x5055d0: the block looks along a direction (pitch from its z).
void World::setFaceBlockDir(FaceBlock b, const Vec3& dir) {
    float pitch = 0, yaw = 0;
    vecToAngles(dir, pitch, yaw);
    setFaceBlock(b, pitch, yaw);
}

// 0x5049a0: the actor's look angles (+0xd8 pitch, +0xdc yaw) and the look axes from them (0x421da0; the forward axis +0xe0
// is the script field lookforward, field table 0x570418: offset 0xe0, vector, read only).
void World::setLookAngles(Entity& e, float pitch, float yaw) {
    ActorAi& ai = e.ai;
    ai.lookAngles[0] = quant(pitch);
    ai.lookAngles[1] = quant(yaw);
    const float p = ai.lookAngles[0] * 0.01745329238474369f, y = ai.lookAngles[1] * 0.01745329238474369f;
    e.obj->set("lookforward", Value(Vec3{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)}));
}

// Spawn (0x4fc7c9..0x4fc806): the base block from the entity angles (0x504950), the look angles too (0x5049a0), the trail
// cleared (0x504de0).
void World::initActorFacing(Entity& e) {
    ActorAi& ai = e.ai;
    const Vec3 a = anglesOf(e.obj);
    setFaceBlock({ai.facing, ai.faceLook, ai.faceYaw}, a[0], a[1]);
    setLookAngles(e, a[0], a[1]);
    ai.trailIdx = 0;
    for (auto& t : ai.trail) t = {0.0f, 0.0f};
    ai.facingInit = true;
}

// 0x53e237 (teleport with angles), 0x54a829 (turret user), 0x54b1f3 (animscripted, every frame): the base block from the entity
// angles (0x504950); the first two also set the look angles (0x5049a0).
void World::faceBlockToAngles(Entity& e, bool look) {
    ActorAi& ai = e.ai;
    if (!ai.facingInit) initActorFacing(e);
    const Vec3 a = anglesOf(e.obj);
    setFaceBlock({ai.facing, ai.faceLook, ai.faceYaw}, a[0], a[1]);
    if (look) setLookAngles(e, a[0], a[1]);
}

// 0x504aa0 (pitch delta 0, yaw delta from the animation): both blocks' look pitch += pitch delta, look yaw and body yaw =
// body yaw + yaw delta; the entity yaw += yaw delta with pitch and roll zeroed; the look angles += the deltas.
void World::turnActorByDelta(Entity& e, float yawDelta) {
    ActorAi& ai = e.ai;
    ai.faceLook[0] = quant(ai.faceLook[0]);
    ai.faceLook[1] = quant(yawDelta + ai.faceYaw);
    ai.faceYaw = quant(yawDelta + ai.faceYaw);
    ai.orientLook[0] = quant(ai.orientLook[0]);
    ai.orientLook[1] = quant(yawDelta + ai.orientTarget);
    ai.orientTarget = quant(yawDelta + ai.orientTarget);
    const Vec3 a = anglesOf(e.obj);
    e.obj->set("angles", Value(Vec3{0, quant(yawDelta + a[1]), 0}));
    setLookAngles(e, ai.lookAngles[0], ai.lookAngles[1] + yawDelta);
}

// 0x504ca0: desiredangle (+0x124) = the active block's body yaw; the entity yaw turns towards it by at most rate * 50 degrees
// (0x592034 / 0x592030), re-quantised, pitch and roll zeroed. The rate is 0.4 (20 degrees a frame), or 0.036 (1.8 degrees)
// while the actor is prone (+0x159) and not a corpse blend (+0x158): settled (+0x160 == 0), entering (+0x160 > 0; once
// +0x15c + +0x160 has passed the length is cleared, still slow this frame) or leaving (+0x160 < 0) until +0x15c - +0x160 has
// passed, when the prone byte is cleared and the normal rate applies.
void World::actorBodyTurn(Entity& e) {
    ActorAi& ai = e.ai;
    const float desired = ai.orient != 0 ? ai.orientTarget : ai.faceYaw;
    e.obj->set("desiredangle", Value(desired));
    const Vec3 a = anglesOf(e.obj);
    const float d = angleSub(desired, a[1]);
    float rate = 0.4f;
    ActorProne& p = ai.prone;
    if (p.prone && !p.corpseBlend) {
        if (p.durMs == 0) {
            rate = 0.036f;
        } else if (p.durMs > 0) {
            if (p.startMs + p.durMs < timeMs_) p.durMs = 0;
            rate = 0.036f;
        } else if (p.startMs - p.durMs >= timeMs_) {
            rate = 0.036f;
        } else {
            p.prone = false;
        }
    }
    const float step = clampf(d, rate * -50.0f, rate * 50.0f);
    e.obj->set("angles", Value(Vec3{0, quant(a[1] + step), 0}));
}

// 0x504be0: the look angles turn towards the active block's look pitch / yaw by at most 40 degrees each (0x591f78 / 0x592110).
void World::actorLookTurn(Entity& e) {
    ActorAi& ai = e.ai;
    const FaceBlock b = activeFaceBlock(e);
    const float dp = clampf(angleSub(b.look[0], ai.lookAngles[0]), -40.0f, 40.0f);
    const float pitch = dp + ai.lookAngles[0];
    const float dy = clampf(angleSub(b.look[1], ai.lookAngles[1]), -40.0f, 40.0f);
    setLookAngles(e, pitch, dy + ai.lookAngles[1]);
}

// 0x504e70: the sum of the ten trail entries (x, y).
std::array<float, 2> World::actorTrailSum(const Entity& e) const {
    std::array<float, 2> s{0.0f, 0.0f};
    for (const auto& t : e.ai.trail) {
        s[0] += t[0];
        s[1] += t[1];
    }
    return s;
}

// 0x504f60: with C the current entry (+0x214c), P the one before and Q the one before that (the ring wraps at 10): when the
// turn Q -> P and the turn P -> C are not of opposite sign (cross(P, Q) * cross(P, C) <= 0 in the exe's operand order) and
// P points along both C and Q (dot >= 0), Q is replaced by C. Then the other entries are scaled, oldest first (C + 1 ..
// 9, then 0 .. C - 1), by k / (k + 1) with k = 1, 2, 3, ..., and the current index advances by one modulo 10.
void World::actorTrailAdvance(Entity& e) {
    ActorAi& ai = e.ai;
    auto& r = ai.trail;
    const int c = ai.trailIdx;
    const int p = (c == 0 ? 10 : c) - 1;
    const int q = (p == 0 ? 10 : p) - 1;
    const float py = r[size_t(p)][1], npx = -r[size_t(p)][0];
    const float a = py * r[size_t(q)][0] + npx * r[size_t(q)][1];
    const float b = py * r[size_t(c)][0] + npx * r[size_t(c)][1];
    if (!(a * b > 0.0f) && r[size_t(p)][0] * r[size_t(c)][0] + r[size_t(p)][1] * r[size_t(c)][1] >= 0.0f &&
        r[size_t(p)][0] * r[size_t(q)][0] + r[size_t(p)][1] * r[size_t(q)][1] >= 0.0f)
        r[size_t(q)] = r[size_t(c)];
    float k = 1.0f;
    for (int i = c + 1; i < 10; ++i, k += 1.0f) {
        const float f = k / (k + 1.0f);
        r[size_t(i)][0] *= f;
        r[size_t(i)][1] *= f;
    }
    for (int i = 0; i < c; ++i, k += 1.0f) {
        const float f = k / (k + 1.0f);
        r[size_t(i)][0] *= f;
        r[size_t(i)][1] *= f;
    }
    ai.trailIdx = (c + 1) % 10;
}

// 0x505230 (the path walk's trail entry): with k = (dist * 20 - 95) / 95 (0x591e18, 0x592364, 0x592360) from the frame's anim
// distance, the entry is the look direction (k <= 0), the far direction (k >= 1) or their normalised blend
// (look * (1 - k) + far * k, 0x421160); it is stored at the current index and the ring advances (0x504f60).
void World::actorTrailFromPath(Entity& e, float dist) {
    const ActorPath& p = e.path;
    const float k = (dist * 20.0f - 95.0f) * 0.010526316240429878f;
    std::array<float, 2> f;
    if (!(k > 0.0f)) {
        f = {p.lookDir[0], p.lookDir[1]};
    } else if (!(k < 1.0f)) {
        f = {p.farDir[0], p.farDir[1]};
    } else {
        f = {p.lookDir[0] * (1.0f - k) + p.farDir[0] * k, p.lookDir[1] * (1.0f - k) + p.farDir[1] * k};
        const float l = std::sqrt(f[0] * f[0] + f[1] * f[1]);
        if (l != 0.0f) {
            f[0] /= l;
            f[1] /= l;
        }
    }
    e.ai.trail[size_t(e.ai.trailIdx)] = f;
    actorTrailAdvance(e);
}

// 0x505520 (followed actor, no path walk): unless the level time / 100 is a multiple of 10 the look direction * 0.1 is added to
// the current entry; on such a tick the ring advances first and the entry becomes look * 0.1.
void World::actorTrailFollow(Entity& e) {
    const ActorPath& p = e.path;
    ActorAi& ai = e.ai;
    if ((timeMs_ / 100) % 10 != 0) {
        ai.trail[size_t(ai.trailIdx)][0] += p.lookDir[0] * 0.1f;
        ai.trail[size_t(ai.trailIdx)][1] += p.lookDir[1] * 0.1f;
        return;
    }
    actorTrailAdvance(e);
    ai.trail[size_t(ai.trailIdx)] = {p.lookDir[0] * 0.1f, p.lookDir[1] * 0.1f};
}

// 0x505630 (face motion), run only with a path and anim mode 1. Holding means the block is set to the current entity angles.
// With movemode set, flag 1 (lookahead at the end) and a lookahead distance under 60 (0x591e4c), a look-forward axis that points
// away from the look direction holds. A zero trail sum holds. Otherwise with movemode set the block faces the trail sum; without
// (stopping) it faces the normalised sum only when that is more than about 25 degrees off the block's yaw (cos < 0.9, 0x59200c),
// else it holds.
void World::faceMotion(Entity& e, FaceBlock b) {
    ActorAi& ai = e.ai;
    const ActorPath& p = e.path;
    if (p.count <= 0 || ai.animMode != 1) return;
    const bool moving = ai.movemode != 0;
    auto hold = [&] {
        const Vec3 a = anglesOf(e.obj);
        setFaceBlock(b, a[0], a[1]);
    };
    if (moving && (p.flags & 1) && p.lookDist < 60.0f) {
        const Value lf = e.obj->get("lookforward");
        const Vec3 f = lf.isVector() ? lf.asVector() : Vec3{0, 0, 0};
        if (f[1] * p.lookDir[1] + p.lookDir[0] * f[0] < 0.0f) {
            hold();
            return;
        }
    }
    std::array<float, 2> s = actorTrailSum(e);
    if (s[0] == 0.0f && s[1] == 0.0f) {
        hold();
        return;
    }
    if (!moving) {
        const float l = std::sqrt(s[0] * s[0] + s[1] * s[1]);
        if (l != 0.0f) {
            s[0] /= l;
            s[1] /= l;
        }
        const float y = b.yaw * 0.01745329238474369f;
        if (!(std::sin(y) * s[1] + std::cos(y) * s[0] < 0.9f)) {
            hold();
            return;
        }
    }
    setFaceBlockDir(b, {s[0], s[1], 0.0f});
}

// 0x517e40: nodes a and b see each other (the same node, or the bit min * count + max of the node visibility table).
bool World::nodesSeeEachOther(int a, int b) const {
    if (a == b) return true;
    const auto& bits = paths_.visibilityBits();
    if (bits.empty()) return false;
    const size_t lo = size_t(std::min(a, b)), hi = size_t(std::max(a, b));
    const size_t bit = paths_.size() * lo + hi;
    return bit / 8 < bits.size() && (bits[bit / 8] >> (bit % 8) & 1);
}

// 0x5432f0 (`rec` = the actor's record of the target, or none): the actor's node (0x515040, none -> no result); the target's node is
// the one remembered in the record (+0x24, the target's node when it was last known, 0x500ca2) or else its current node
// (0x515040); none -> no result. If the two nodes see each other (0x517e40) that node is the result. Otherwise 0x547330 searches
// from the target's node over the links (the target team's blocked links skipped, cost = link length, open list sorted without a
// heuristic) for the first node that sees the actor's node; that node, or none.
int World::enemyPositionNode(Entity& e, Entity& target, const SentientInfo* rec) {
    const int mine = sentientNode(e);
    if (mine < 0) return -1;
    int theirs = rec ? rec->lastKnownNode : -1;
    if (theirs < 0) theirs = sentientNode(target);
    if (theirs < 0) return -1;
    if (nodesSeeEachOther(theirs, mine)) return theirs;
    const size_t n = nodes_.size();
    if (search_.size() != n) search_.assign(n, {});
    auto& s = search_;
    static constexpr int kHead = -2;
    const int id = ++searchId_;
    const int team = teamIndex(target);
    int head = theirs;
    s[size_t(theirs)] = {id, -1, kHead, -1, 0.0f, 0.0f, true};
    auto setNext = [&](int node, int next) { (node == kHead ? head : s[size_t(node)].next) = next; };
    while (head >= 0) {
        const int cur = head;
        if (nodesSeeEachOther(cur, mine)) return cur;
        head = s[size_t(cur)].next;
        if (head >= 0) s[size_t(head)].prev = kHead;
        const auto& links = paths_.links(size_t(cur));
        for (size_t li = 0; li < links.size(); ++li) {
            if (linkBlocked(size_t(cur), li, team)) continue;
            const int nb = links[li].target;
            auto& sn = s[size_t(nb)];
            const float g = s[size_t(cur)].g + links[li].distance;
            if (sn.id == id) {
                if (sn.g <= g) continue;
                if (sn.queued) {
                    setNext(sn.prev, sn.next);
                    if (sn.next >= 0) s[size_t(sn.next)].prev = sn.prev;
                }
            } else {
                sn.id = id;
                sn.h = 0.0f;
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
    return -1;
}

// 0x505790 (no enemy, modes 3..5): returns whether it handled the facing. Without a team to fight ({0, 2, 1, 0, 0}) false.
// Until the timer +0x204c passes, true (the block stays). Then the nearest living sentient of the fought team (3D distance²,
// a player's (client) taken * 0.25, the first of equals) is picked; none -> false. The timer becomes now + 3000, the target's
// position node (0x5432f0 without a record) is looked up (none -> false) and the block faces it when it is 1 unit or more away (2D).
bool World::faceThreat(Entity& e, FaceBlock b) {
    static constexpr int kEnemy[5] = {0, 2, 1, 0, 0};
    const int fought = kEnemy[teamIndex(e)];
    if (fought == 0) return false;
    if (e.ai.threatCheckMs - timeMs_ > 0) return true;
    const Vec3 o = originOfObj(e.obj);
    Entity* best = nullptr;
    float bestD = FLT_MAX;
    for (const auto& ep : entities_) {
        Entity& s = *ep;
        if (&s == &e || !isSentient(s) || teamIndex(s) != fought) continue;
        const Vec3 so = originOfObj(s.obj);
        const float dx = so[0] - o[0], dy = so[1] - o[1], dz = so[2] - o[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (s.isPlayer) d *= 0.25f;
        if (d < bestD) {
            bestD = d;
            best = &s;
        }
    }
    if (!best) return false;
    e.ai.threatCheckMs = timeMs_ + 3000;
    const int node = enemyPositionNode(e, *best, nullptr);
    if (node < 0) return false;
    const Vec3 np = originOfObj(nodes_[size_t(node)].obj);
    const Vec3 d{np[0] - o[0], np[1] - o[1], np[2] - o[2]};
    if (d[0] * d[0] + d[1] * d[1] >= 1.0f) setFaceBlockDir(b, d);
    return true;
}

// 0x505920: the threat (0x505790) first; otherwise the claimed node's yaw (node +0x20) when it has spawnflag 0x8000 and the actor
// stands at it (0x502660: height within 80, 2D distance within 30).
void World::faceClaimedNode(Entity& e, FaceBlock b, bool threatFirst) {
    if (threatFirst && faceThreat(e, b)) return;
    if (e.claimedNode < 0) return;
    const PathNode& n = nodes_[size_t(e.claimedNode)];
    if (!(n.spawnflags & 0x8000)) return;
    const Vec3 o = originOfObj(e.obj), np = originOfObj(n.obj);
    const float dz = o[2] - np[2], dx = np[0] - o[0], dy = np[1] - o[1];
    if (dz * dz > 6400.0f || dx * dx + dy * dy > 900.0f) return;
    setFaceBlock(b, 0.0f, anglesOf(n.obj)[1]);
}

// 0x505960 (face enemy). Without an enemy: the threat, then the claimed node. With one, its sight record decides: visible now ->
// face the enemy's origin. Not visible: never seen -> 0x505920; no path -> nothing while it was seen less than 5000 ms ago, else
// 0x505920; with a path, when the path is older than 300 ms (+0x6ec + 300, 0x591f4c), the enemy was not seen for more than
// 500 ms and 0x5432f0 (with the record) gives a node, face that node; otherwise face the enemy's origin. A direction shorter than
// 1 unit (2D) leaves the block alone.
void World::faceEnemy(Entity& e, FaceBlock b) {
    Entity* enemy = entityOf(e.obj->get("enemy"));
    const Vec3 o = originOfObj(e.obj);
    if (!enemy) {
        if (faceThreat(e, b)) return;
        faceClaimedNode(e, b, false);
        return;
    }
    SentientInfo& rec = e.sentientInfo[enemy->obj.get()];
    Vec3 target = originOfObj(enemy->obj);
    if (!rec.visible) {
        if (rec.lastSeenMs == 0) {
            faceClaimedNode(e, b, true);
            return;
        }
        if (e.path.count <= 0) {
            if (timeMs_ - rec.lastSeenMs < 5000) return;
            faceClaimedNode(e, b, true);
            return;
        }
        if (float(e.path.planMs) + 300.0f < float(timeMs_) && timeMs_ - rec.lastSeenMs > 500) {
            const int node = enemyPositionNode(e, *enemy, &rec);
            if (node >= 0) target = originOfObj(nodes_[size_t(node)].obj);
        }
    }
    const Vec3 d{target[0] - o[0], target[1] - o[1], target[2] - o[2]};
    if (d[0] * d[0] + d[1] * d[1] >= 1.0f) setFaceBlockDir(b, d);
}

// 0x543ae0: the path left ahead of the actor is longer than `dist`: the lookahead segment +0x3b4 plus the lookahead distance
// (+ 128, 0x591e3c, with a traversal index), then the start length +0x3c4 when the lookahead index - 1 is count - 2, then the
// lengths of the points below it.
bool World::pathLongerThan(const Entity& e, float dist) const {
    const ActorPath& p = e.path;
    float len = p.lookSegLen + p.lookDist;
    if (p.traversal != 0) len += 128.0f;
    if (dist < len) return true;
    if (p.lookIndex == 0) return false;
    int i = p.lookIndex - 1;
    if (i == p.count - 2) {
        len += p.startLen;
        if (dist < len) return true;
        --i;
    }
    for (; i >= 0; --i) {
        len += p.pts[size_t(i)].len;
        if (dist < len) return true;
    }
    return false;
}

// 0x545580 (dir: unit direction to the enemy): false (keep facing the motion) when the path is older than 299 ms, more than 128
// units of it are left (0x543ae0) and either the lookahead distance is under 72 - 48 * k or the path age exceeds 3500 * k + 1500,
// with k = (dir . lookDir + 1) * 0.5 (0x591e10, 0x591f3c, 0x591f40, 0x591f38, 0x591f34); otherwise true.
bool World::mayFaceEnemyWhileMoving(const Entity& e, const Vec3& dir) const {
    const ActorPath& p = e.path;
    const int64_t age = timeMs_ - p.planMs;
    if (age > 299 && pathLongerThan(e, 128.0f)) {
        const float k = (dir[0] * p.lookDir[0] + p.lookDir[1] * dir[1] + 1.0f) * 0.5f;
        if (p.lookDist < 72.0f - 48.0f * k) return false;
        if (k * 3500.0f + 1500.0f < float(age)) return false;
    }
    return true;
}

// 0x54ce10: the record's sight result when its last check (+4) is within `ms`, else a new sight check (0x54ca60).
bool World::canSeeEnemyRecent(Entity& e, Entity& enemy, int ms) {
    SentientInfo& rec = e.sentientInfo[enemy.obj.get()];
    if (rec.lastCheckMs != 0 && rec.lastCheckMs + ms >= timeMs_) return rec.visible;
    sightCheck(e, enemy);
    return e.sentientInfo[enemy.obj.get()].visible;
}

// 0x505b80 (face enemy or motion) and 0x505cb0 (mode 5). Both need a path and a moving block velocity (+0x230 / +0x234 not both
// 0, the previous physics step); otherwise face enemy. Mode 4: with an enemy seen within 250 ms (+0x130 = now + 500) that is
// within 256 units (3D distance² <= 65536) and may be faced while moving (0x545580), face enemy; else face motion. Mode 5: the
// unit direction to the enemy; 0x545580 false -> face motion, else face enemy.
void World::faceEnemyOrMotion(Entity& e, FaceBlock b, bool mode5) {
    const ActorPath& p = e.path;
    const bool moving = p.count > 0 && (e.ai.blockVel[0] != 0.0f || e.ai.blockVel[1] != 0.0f);
    if (!moving) {
        faceEnemy(e, b);
        return;
    }
    Entity* enemy = entityOf(e.obj->get("enemy"));
    const Vec3 o = originOfObj(e.obj);
    auto dirTo = [&](const Entity& en) {
        const Vec3 eo = originOfObj(en.obj);
        Vec3 d{eo[0] - o[0], eo[1] - o[1], eo[2] - o[2]};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l != 0.0f) d = {d[0] / l, d[1] / l, d[2] / l};
        return d;
    };
    if (mode5) {
        // TODO(spec): 0x505cf0 dereferences the enemy without a check; without one the port faces as 0x505960 does.
        if (!enemy) {
            faceEnemy(e, b);
            return;
        }
        if (!mayFaceEnemyWhileMoving(e, dirTo(*enemy))) {
            faceMotion(e, b);
            return;
        }
        faceEnemy(e, b);
        return;
    }
    if (enemy && canSeeEnemyRecent(e, *enemy, 250)) {
        e.ai.enemyFaceMs = timeMs_ + 500;
        const Vec3 eo = originOfObj(enemy->obj);
        const float dx = eo[0] - o[0], dy = eo[1] - o[1], dz = eo[2] - o[2];
        if (dx * dx + dy * dy + dz * dz <= 65536.0f && mayFaceEnemyWhileMoving(e, dirTo(*enemy))) {
            faceEnemy(e, b);
            return;
        }
    }
    faceMotion(e, b);
}

// 0x505d80: the active block's mode selects the target: 2 motion, 3 enemy, 4 enemy or motion, 5 the stricter variant, 6 the
// claimed node's yaw when it has spawnflag 0x8000 (no distance test); 0 and 1 leave the block alone.
void World::selectActorFacing(Entity& e) {
    const FaceBlock b = activeFaceBlock(e);
    switch (b.mode) {
    case 2: faceMotion(e, b); break;
    case 3: faceEnemy(e, b); break;
    case 4: faceEnemyOrMotion(e, b, false); break;
    case 5: faceEnemyOrMotion(e, b, true); break;
    case 6:
        if (e.claimedNode >= 0 && (nodes_[size_t(e.claimedNode)].spawnflags & 0x8000))
            setFaceBlock(b, 0.0f, anglesOf(nodes_[size_t(e.claimedNode)].obj)[1]);
        break;
    default: break;
    }
}

// The tail of 0x504160: a non-zero animation yaw turns the actor (0x504aa0), then the facing selector (0x505d80), the body turn
// (0x504ca0) and the look turn (0x504be0).
void World::actorFacingFrame(Entity& e, float animYaw) {
    if (!e.ai.facingInit) initActorFacing(e);
    if (animYaw != 0.0f) turnActorByDelta(e, animYaw);
    selectActorFacing(e);
    actorBodyTurn(e);
    actorLookTurn(e);
}

// 0x504450 (unlinked actor) without the mode-1 path walk, which the states run through moveActor: anim mode 8 (nophysics) does
// nothing at all. Modes 2..7 take this frame's animation delta (0x503e40 -> 0x478550, goal weights per +0x220) and its yaw
// (0x423900 of the relative rotation, 2 / (a² + b²) with 0x591ed0, degrees by 0x591fd8): 2 xy delta (kind 2, no yaw), 3 no
// delta (kind 1), 4 xy delta (kind 2), 5 3D delta (kind 3), 6 3D delta (kind 4), 7 xy delta (kind 5); modes 0 and 1 have no yaw.
// After the turn the physics (0x501d00) rotates the delta by desiredangle (+0x124; v = x * forward - y * right + z) for kinds
// other than 1 and moves the block (World::moveActorKind).
void World::actorMoveFrame(Entity& e) {
    ActorAi& ai = e.ai;
    const int mode = ai.animMode;
    if (mode == 8) return;
    float yaw = 0.0f;
    Vec3 d{0, 0, 0};
    int kind = 0;
    // The port's path following forces anim mode 1 for its walk (World::followActorPath) while setActorAnimScript may then store
    // an animmode override; a frame that already walked the path is treated as mode 1 so the actor does not move twice.
    // TODO(spec): whether 0x552350 stores 1 directly or through 0x53ac90 (which applies the override +0x21c).
    const bool walked = ai.walkedMs == timeMs_;
    if (!walked && mode >= 2 && mode <= 7 && e.anim) {
        const AnimDelta ad = e.anim->delta(ai.animDeltaGoalWeights);
        if (mode != 2) {
            const float sq = ad.rot[0] * ad.rot[0] + ad.rot[1] * ad.rot[1];
            if (sq > 0.0f) {
                const float k = 2.0f / sq;
                yaw = std::atan2(ad.rot[1] * ad.rot[0] * k, 1.0f - k * ad.rot[0] * ad.rot[0]) * 57.2957763671875f;
            }
        }
        switch (mode) {
        case 2: d = {ad.trans[0], ad.trans[1], 0}; kind = 2; break;
        case 3: kind = 1; break;
        case 4: d = {ad.trans[0], ad.trans[1], 0}; kind = 2; break;
        case 5: d = {ad.trans[0], ad.trans[1], ad.trans[2]}; kind = 3; break;
        case 6: d = {ad.trans[0], ad.trans[1], ad.trans[2]}; kind = 4; break;
        case 7: d = {ad.trans[0], ad.trans[1], 0}; kind = 5; break;
        default: break;
        }
    }
    actorFacingFrame(e, yaw);
    if (kind == 0) return;
    if (kind == 1) {
        moveActorKind(e, 1, {0, 0, 0}, 0.05f);
        return;
    }
    const Value dv = e.obj->get("desiredangle");
    const float y = (dv.isNumber() ? dv.asFloat() : 0.0f) * 0.01745329238474369f;
    const float c = std::cos(y), s = std::sin(y);
    moveActorKind(e, kind, {d[0] * c - d[1] * s, d[0] * s + d[1] * c, d[2]}, 0.05f);
}

} // namespace iw2::game
