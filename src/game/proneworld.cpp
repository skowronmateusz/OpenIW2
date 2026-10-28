// Prone built-ins and the actor prone state (checkprone 0x5406f0, enterprone 0x53d1b0, exitprone 0x53d280,
// setproneanimnodes 0x53d2f0, updateprone 0x53d430; helpers 0x514b30, 0x514c40, 0x5022b0, 0x514970, 0x5149e0).
// The fit test itself is src/game/prone.cpp; docs/spec/ai.md "Prone".
#include "game/prone.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {

bool proneLog() {
    static const bool on = std::getenv("IW2_PRONE_LOG") != nullptr;
    return on;
}

// Shared transition bookkeeping (0x514970, 0x514b30, 0x514c40, 0x53d430): a finished leave clears prone (returns
// false), a finished entry clears the length.
bool proneTick(ActorProne& p, int64_t now) {
    if (p.durMs < 0) {
        if (p.startMs - p.durMs < now) {
            p.prone = false;
            return false;
        }
    } else if (p.durMs > 0 && p.startMs + p.durMs < now) {
        p.durMs = 0;
    }
    return true;
}

// 0x5149e0: how far into prone the transition is (0 not prone, 1 settled), without touching the state.
float proneFraction(const ActorProne& p, int64_t now) {
    if (!p.prone) return 0.0f;
    if (p.durMs == 0) return 1.0f;
    if (p.durMs < 0) {
        if (p.startMs - p.durMs < now) return 0.0f;
        return 1.0f - float(int(now - p.startMs)) / float(-p.durMs);
    }
    if (p.startMs + p.durMs < now) return 1.0f;
    return float(int(now - p.startMs)) / float(p.durMs);
}

// 0x565994 (_ftol2): truncation.
int truncMs(float v) { return int(v); }

} // namespace

// Fit at the entity's origin and yaw (ent +0xdc, +0xec) with the outputs into +0x164 .. +0x16c and the result into +0x154.
static void actorFit(const CollisionWorld& cw, Entity& e, bool alreadyProne) {
    ActorProne& p = e.ai.prone;
    ProneFitArgs a;
    a.origin = e.obj->get("origin").asVector();
    const Value ang = e.obj->get("angles");
    a.yaw = ang.isVector() ? ang.asVector()[1] : 0.0f;
    a.alreadyProne = alreadyProne;
    const int pass = e.number;
    const ProneFitResult r = proneFit(a, [&cw, pass](const Vec& s, const Vec& t, const Vec& mn, const Vec& mx) {
        return cw.trace(s, t, mn, mx, 0x820011u, pass);
    });
    p.lift = 0.0f; // the fit stores int 0 at +0x164
    p.pitchBody = r.pitchBody;
    p.pitchLegs = r.pitchLegs;
    p.fits = r.fits ? 1 : 0;
    e.obj->set("proneok", Value(int32_t(p.fits)));
}

void World::proneEnter(Entity& e, int ms) {
    ActorProne& p = e.ai.prone;
    if (p.prone && !p.corpseBlend && proneTick(p, timeMs_)) {
        // Already prone (or leaving): only a different length restarts the timing, keeping the reached fraction.
        if (p.durMs == 0 || p.durMs == ms) return;
        const float f = proneFraction(p, timeMs_);
        if (f < 1.0f) p.startMs = timeMs_ - truncMs(float(ms) * f);
        p.durMs = ms;
        return;
    }
    p.corpseBlend = false;
    p.prone = true;
    p.tilt = true;
    p.startMs = timeMs_;
    p.durMs = ms;
    actorFit(*collision_, e, false);
    if (proneLog())
        std::fprintf(stderr, "[prone %lld] #%d enterprone %d ms at (%.0f %.0f %.0f) yaw %.0f -> fits %d (body %.1f legs %.1f)\n",
                     (long long)timeMs_, e.number, ms, e.obj->get("origin").asVector()[0], e.obj->get("origin").asVector()[1],
                     e.obj->get("origin").asVector()[2], e.obj->get("angles").isVector() ? e.obj->get("angles").asVector()[1] : 0.0f,
                     p.fits, p.pitchBody, p.pitchLegs);
}

void World::proneExit(Entity& e, int ms) {
    ActorProne& p = e.ai.prone;
    if (!p.prone || p.corpseBlend) return;
    if (!proneTick(p, timeMs_)) return;
    if (p.durMs != 0 && p.durMs != ms) {
        const float f = proneFraction(p, timeMs_);
        p.startMs = timeMs_ - truncMs(float(ms) * f);
    } else {
        p.startMs = timeMs_;
    }
    p.durMs = -ms;
    if (proneLog()) std::fprintf(stderr, "[prone %lld] #%d exitprone %d ms\n", (long long)timeMs_, e.number, ms);
}

void World::proneRefit(Entity& e, bool fit) {
    ActorProne& p = e.ai.prone;
    if (fit) actorFit(*collision_, e, true);
    else p.lastBend = 360.0f;
    const float bend = proneAngleDelta(p.pitchLegs, p.pitchBody);
    if (p.lastBend == bend) return;
    // Bend below 0 weighs the down node by bend / down angle, above 0 the up node by bend / up angle (each at most
    // 0.99); straight takes the rest. Weights under 0.001 become 0; blend 0.05, rate 1 (0x478f10 + 0x4794b0).
    float down = 0.0f, up = 0.0f;
    if (bend < 0.0f) down = std::min(bend * p.invDown, 0.99f);
    else up = std::min(bend * p.invUp, 0.99f);
    const float straight = 1.0f - (bend < 0.0f ? down : up);
    auto setW = [&](int node, float wgt) {
        if (!e.anim || node <= 0) return;
        AnimState::SetArgs a;
        a.weight = wgt < 0.001f ? 0.0f : wgt;
        a.blendTime = 0.05f;
        a.rate = 1.0f;
        e.anim->set(node, a);
    };
    setW(p.nodeDown, down);
    setW(p.nodeStraight, straight);
    setW(p.nodeUp, up);
    p.lastBend = bend;
}

// Think part (0x4ff1de..0x4ff209): while prone and settled-or-entering, a move or turn since the previous think runs the
// fit again (already prone) and refreshes the leg weights; otherwise the tilt byte is cleared.
void World::actorProneFrame(Entity& e) {
    ActorProne& p = e.ai.prone;
    const Vec3 o = e.obj->get("origin").asVector();
    const Value av = e.obj->get("angles");
    const Vec3 ang = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
    const bool moved = !p.haveLast || o != p.lastOrigin || ang != p.lastAngles;
    p.haveLast = true;
    p.lastOrigin = o;
    p.lastAngles = ang;
    const bool active = p.prone && !p.corpseBlend && proneTick(p, timeMs_);
    if (!active) {
        p.tilt = false;
        return;
    }
    if (moved) proneRefit(e, true);
}

// Ground probe of the corpse fit (0x4fd460): from 30 above the origin a box (-4,-4,0)..(4,4,8) steps 10.9 along
// `dir` (blocked at once: slope 0, height of `fallback`), then drops from where it stopped to 30 below the origin
// (start-solid or no hit: the fallback point); the same backwards. Slope = pitch of front ground - back ground (0 when
// equal), the height (front z + fallback z + back z) / 3.
static float corpseGroundProbe(const CollisionWorld& cw, int pass, uint32_t mask, const Vec3& o, const Vec3& fallback,
                               const Vec3& dir, float& slope) {
    const Vec mins{-4.0f, -4.0f, 0.0f}, maxs{4.0f, 4.0f, 8.0f};
    auto probe = [&](float sign, Vec3& ground) {
        const Vec3 p{o[0], o[1], o[2] + 30.0f};
        const Vec3 q{p[0] + sign * dir[0] * 10.9f, p[1] + sign * dir[1] * 10.9f, p[2] + sign * dir[2] * 10.9f};
        const TraceResult t1 = cw.trace(p, q, mins, maxs, mask, pass);
        if (t1.fraction == 0.0f) return false;
        const Vec3 h{p[0] + (q[0] - p[0]) * t1.fraction, p[1] + (q[1] - p[1]) * t1.fraction, p[2] + (q[2] - p[2]) * t1.fraction};
        const Vec3 down{h[0], h[1], o[2] - 30.0f};
        const TraceResult t2 = cw.trace(h, down, mins, maxs, mask, pass);
        if (t2.startSolid || t2.fraction >= 1.0f) ground = fallback;
        else ground = {h[0], h[1], (down[2] - h[2]) * t2.fraction + h[2]};
        return true;
    };
    Vec3 front{}, back{};
    if (!probe(1.0f, front) || !probe(-1.0f, back)) {
        slope = 0.0f;
        return fallback[2];
    }
    if (front == back) slope = 0.0f;
    else slope = proneAngleNormalize180(pronePitchOf({front[0] - back[0], front[1] - back[1], front[2] - back[2]}));
    return (back[2] + fallback[2] + front[2]) * 0.333333343f;
}

// Corpse ground fit (0x4fd890): slope along the facing (pitch) and, when that is under 30 degrees, across it (roll,
// else 0); the lift is the mean probe height minus the origin z, cancelled when below 0 and the standing box
// (-15,-15,0)..(15,15,72) cannot move 1 unit down (0x44c1b0).
static void corpseGroundFit(const CollisionWorld& cw, int pass, uint32_t mask, const Vec3& o, float yaw, float& pitch,
                            float& roll, float* lift) {
    const float r = yaw * 0.0174532924f;
    const Vec3 fwd{std::cos(r), std::sin(r), 0.0f}, right{std::sin(r), -std::cos(r), 0.0f}; // 0x421f00
    float h = corpseGroundProbe(cw, pass, mask, o, o, fwd, pitch);
    if (std::fabs(pitch) >= 30.0f) roll = 0.0f;
    else h = (corpseGroundProbe(cw, pass, mask, o, o, right, roll) + h) * 0.5f;
    if (!lift) return;
    const float d = h - o[2];
    if (d < 0.0f && cw.sightBlocked(o, {o[0], o[1], o[2] - 1.0f}, {-15, -15, 0}, {15, 15, 72}, mask, pass, 0x7ff)) {
        *lift = 0.0f;
        return;
    }
    *lift = d;
}

void World::corpseProneFit(Entity& e, bool smooth) {
    ActorProne& p = e.ai.prone;
    if (!p.corpseBlend) return;
    const uint32_t mask = 0x820011u; // clip mask (0x2820011 / 0x820011) & 0xfdff3fff
    const Vec3 o = e.obj->get("origin").asVector();
    const Value av = e.obj->get("angles");
    const float yaw = av.isVector() ? av.asVector()[1] : 0.0f;
    if (!smooth) {
        corpseGroundFit(*collision_, e.number, mask, o, yaw, p.pitchBody, p.pitchLegs, &p.lift);
        return;
    }
    // Smoothed: each angle moves at most 6 degrees towards its target per call, the lift at most 0.6.
    float tA = 0, tB = 0, tC = 0;
    corpseGroundFit(*collision_, e.number, mask, o, yaw, tA, tB, &tC);
    auto stepAngle = [](float& cur, float target) {
        const float d = proneAngleDelta(target, cur);
        if (std::fabs(d) > 6.0f) cur += d < 0.0f ? -6.0f : 6.0f;
        else cur = target;
    };
    stepAngle(p.pitchBody, tA);
    stepAngle(p.pitchLegs, tB);
    const float dc = tC - p.lift;
    if (std::fabs(dc) > 0.6f) p.lift += dc < 0.0f ? -0.6f : 0.6f;
    else p.lift = tC;
}

// Death start (0x548980) and corpse conversion (0x4fdba0): an actor that is not prone, not in anim mode 8
// ("nophysics") and not linked starts the corpse blend (500 ms, zero angles; at the conversion only when it has not
// started yet) and is fitted to the ground at once.
void World::proneCorpseStart(Entity& e, bool conversion) {
    ActorProne& p = e.ai.prone;
    if (e.ai.animMode == 8 || links_.count(e.number)) return;
    if (p.prone && !p.corpseBlend && proneTick(p, timeMs_)) return;
    if (!conversion || !p.corpseBlend) {
        p.startMs = timeMs_;
        p.prone = true;
        p.corpseBlend = true;
        p.durMs = 500;
        p.pitchBody = 0.0f;
        p.pitchLegs = 0.0f;
    }
    corpseProneFit(e, false);
    if (proneLog())
        std::fprintf(stderr, "[prone %lld] #%d corpse blend (%s) at (%.0f %.0f %.0f) yaw %.0f pitch %.1f roll %.1f lift %.1f\n",
                     (long long)timeMs_, e.number, conversion ? "corpse" : "death", e.obj->get("origin").asVector()[0],
                     e.obj->get("origin").asVector()[1], e.obj->get("origin").asVector()[2],
                     e.obj->get("angles").isVector() ? e.obj->get("angles").asVector()[1] : 0.0f, p.pitchBody, p.pitchLegs, p.lift);
}

bool World::actorProneTilt(const Entity& e, int64_t nowMs, float& pitch, float& roll, float& lift) const {
    if (!e.isActor) return false;
    const ActorProne& p = e.ai.prone;
    const bool prone = p.prone && !p.corpseBlend && !(p.durMs < 0 && p.startMs - p.durMs < nowMs);
    if (!prone && !p.corpseBlend) return false;
    const float f = proneFraction(p, nowMs);
    pitch = f * p.pitchBody;
    roll = p.corpseBlend ? f * p.pitchLegs : 0.0f;
    lift = f * p.lift;
    return true;
}

void World::registerProneBuiltins() {
    auto actorSelf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        return *e;
    };
    // An animation argument of the actor's tree (0x46e530).
    auto animNode = [](Entity& e, const Value& v) -> int {
        if (!e.anim) throw std::runtime_error("entity has no animtree");
        if (!std::holds_alternative<script::AnimRef>(v.v)) throw std::runtime_error("expected an animation, got " + v.typeName());
        const int n = e.anim->tree().find(std::get<script::AnimRef>(v.v).name);
        if (n < 0) throw std::runtime_error("animation " + std::get<script::AnimRef>(v.v).name + " is not in animtree " + e.animTree);
        return n;
    };
    // checkprone(origin, yaw, alreadyProne) (0x5406f0, actor only): the fit test 0x4fb530 with radius 15, height 48,
    // length 66, strict, the actor as pass entity and mask 0x810011 | 0x10000; the answer is returned as an int.
    vm_.registerBuiltin("checkprone", [this, actorSelf](CallContext& c) {
        Entity* e = &actorSelf(c);
        ProneFitArgs a;
        a.origin = c.arg(0).asVector();
        a.yaw = c.arg(1).asFloat();
        a.alreadyProne = c.arg(2).asInt() != 0;
        const int pass = e->number;
        const ProneFitResult r = proneFit(a, [this, pass](const Vec& s, const Vec& t, const Vec& mn, const Vec& mx) {
            return collision_->trace(s, t, mn, mx, 0x820011u, pass);
        });
        if (proneLog())
            std::fprintf(stderr, "[prone %lld] #%d checkprone (%.0f %.0f %.0f) yaw %.0f already %d -> %d (body %.1f legs %.1f)\n",
                         (long long)timeMs_, e->number, a.origin[0], a.origin[1], a.origin[2], a.yaw, int(a.alreadyProne),
                         int(r.fits), r.pitchBody, r.pitchLegs);
        return Value(r.fits ? 1 : 0);
    });
    // setproneanimnodes(downAngle, upAngle, down, straight, up) (0x53d2f0): down angle < 0 and up angle > 0 (errors
    // "Down angle (parameter 1) must be set to be less than 0." / "Up angle (parameter 2) must be set to be greater than
    // 0."); stores their reciprocals and the three leg nodes.
    vm_.registerBuiltin("setproneanimnodes", [actorSelf, animNode](CallContext& c) {
        const float downAngle = c.arg(0).asFloat(), upAngle = c.arg(1).asFloat();
        Entity& e = actorSelf(c);
        if (!(downAngle < 0.0f)) throw std::runtime_error("Down angle (parameter 1) must be set to be less than 0.");
        if (!(upAngle > 0.0f)) throw std::runtime_error("Up angle (parameter 2) must be set to be greater than 0.");
        ActorProne& p = e.ai.prone;
        p.invDown = 1.0f / downAngle;
        p.invUp = 1.0f / upAngle;
        p.nodeDown = animNode(e, c.arg(2));
        p.nodeStraight = animNode(e, c.arg(3));
        p.nodeUp = animNode(e, c.arg(4));
        return Value();
    });
    // enterprone(seconds) (0x53d1b0): needs setproneanimnodes first ("Must call SetProneAnimNodes before calling
    // EnterProne": both reciprocals 0 or any node 0), then 0x514b30 with trunc(seconds * 1000) ms.
    vm_.registerBuiltin("enterprone", [this, actorSelf](CallContext& c) {
        Entity& e = actorSelf(c);
        const ActorProne& p = e.ai.prone;
        if ((p.invDown == 0.0f && p.invUp == 0.0f) || p.nodeDown <= 0 || p.nodeStraight <= 0 || p.nodeUp <= 0)
            throw std::runtime_error("Must call SetProneAnimNodes before calling EnterProne");
        proneEnter(e, truncMs(c.arg(0).asFloat() * 1000.0f));
        return Value();
    });
    // exitprone(seconds) (0x53d280): 0x514c40 with trunc(seconds * 1000) ms.
    vm_.registerBuiltin("exitprone", [this, actorSelf](CallContext& c) {
        Entity& e = actorSelf(c);
        proneExit(e, truncMs(c.arg(0).asFloat() * 1000.0f));
        return Value();
    });
    // updateprone(animA, animB, weight, blendTime, rate) (0x53d430): only while prone and not a corpse blend; after the
    // transition bookkeeping (a finished leave just clears prone) both animations get setanim (0x4795e0) with the
    // same weight / blend / rate, then the leg weights are refreshed from the last fit (0x5022b0 without a fit).
    vm_.registerBuiltin("updateprone", [this, actorSelf, animNode](CallContext& c) {
        Entity& e = actorSelf(c);
        ActorProne& p = e.ai.prone;
        if (!p.prone || p.corpseBlend) return Value();
        if (!proneTick(p, timeMs_)) return Value();
        const int a = animNode(e, c.arg(0)), b = animNode(e, c.arg(1));
        AnimState::SetArgs s;
        s.weight = c.arg(2).asFloat();
        s.blendTime = c.arg(3).asFloat();
        s.rate = c.arg(4).asFloat();
        e.anim->set(a, s);
        e.anim->set(b, s);
        proneRefit(e, false);
        return Value();
    });
}

} // namespace iw2::game
