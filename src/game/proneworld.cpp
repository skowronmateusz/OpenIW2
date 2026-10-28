// Prone built-ins and the actor prone state (checkprone 0x5406f0, enterprone 0x53d1b0, exitprone 0x53d280,
// setproneanimnodes 0x53d2f0, updateprone 0x53d430; helpers 0x514b30, 0x514c40, 0x5022b0, 0x514970, 0x5149e0).
// The fit test itself is src/game/prone.cpp; docs/spec/ai.md "Prone".
#include "game/prone.h"
#include "game/world.h"

#include <algorithm>
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
    p.out = 0;
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
        std::fprintf(stderr, "[prone %lld] #%d enterprone %d ms -> fits %d (body %.1f legs %.1f)\n", (long long)timeMs_, e.number,
                     ms, p.fits, p.pitchBody, p.pitchLegs);
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

bool World::actorProneTilt(const Entity& e, float& pitch, float& lift) const {
    if (!e.isActor) return false;
    const ActorProne& p = e.ai.prone;
    if (!p.prone || p.corpseBlend) return false; // TODO(spec): the corpse blend (mode 2, +0x158 set by 0x4fdd75)
    if (p.durMs < 0 && p.startMs - p.durMs < timeMs_) return false;
    const float f = proneFraction(p, timeMs_);
    pitch = f * p.pitchBody;
    lift = f * float(p.out);
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
