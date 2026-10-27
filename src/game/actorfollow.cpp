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
    while (true) {
        const TraceResult tr = collision_->trace(cur, {target[0], target[1], cur[2]}, mins, maxs, mask, pass);
        out = tr.endPos;
        if (!tr.startSolid && tr.fraction == 1.0f) break;
        Entity* hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
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

// 0x543c30: first the listed actors alone (0x44c5b0: the box, step 10 when that actor crouches, else 18, moved level
// towards the target), then the move check (0x543d30, step 18). A listed actor in the way fails with `out` = start.
bool World::aiReachPast(Vec3 start, const Vec3& target, const std::vector<int>& ents, int passEnt, uint32_t mask,
                        Vec3& out) {
    const Vec3 end{target[0], target[1], start[2]};
    for (int n : ents) {
        Entity* x = entityByNumber(n);
        // TODO(spec): actor +0x288 (taken as the crouch pose, as in the cover code).
        const float h = x && x->isActor && x->obj->get("anim_pose").toString() == "crouch" ? 10.0f : 18.0f;
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
    if (!f) return;
    if (f->obj->removed) {
        ai.followed = nullptr;
        return;
    }
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
    f = ai.followed;
    if (!f) {
        ++stats.followReleases;
        return;
    }
    if (d2 < 900.0f) return;
    if (ai.blocker == &e || ai.animMode != 1 || e.path.count <= 0) return;
    if ((mo[0] - fo[0]) * e.path.lookDir[0] + (mo[1] - fo[1]) * e.path.lookDir[1] > 0.0f) ai.followed = nullptr, ++stats.followReleases;
}

// 0x501c6f..0x501c8b (after the actor move 0x5487c0): +0x318 = the hit entity; when it is an actor that follows nobody it
// starts following this actor (its +0x2148 = this gentity).
void World::linkBumpedActor(Entity& e, int hit) {
    e.ai.bumpedEnt = hit;
    if (hit >= 0x7fe) return;
    // Gated: a followed actor skips the path walk (stepActorPath) and the exe moves it along the follower trail
    // (0x500d80 / 0x544ca0 / 0x505520 / 0x504f60), which is not ported; linking would freeze it. Opt in with IW2_ACTOR_FOLLOW=1.
    static const bool enabled = [] { const char* v = std::getenv("IW2_ACTOR_FOLLOW"); return v && v[0] == '1'; }();
    if (!enabled) return;
    Entity* o = entityByNumber(hit);
    if (o && o->isActor && o != &e && !o->ai.followed) o->ai.followed = &e, ++stats.followLinks;
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
