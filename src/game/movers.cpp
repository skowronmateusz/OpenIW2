// Movers and linking, following CoD2SP_s.exe 1.3 (docs/spec/movers.md).
#include "game/movers.h"

#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

float length(const MVec& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
MVec scaled(const MVec& v, float s) { return {v[0] * s, v[1] * s, v[2] * s}; }
MVec plus(const MVec& a, const MVec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
MVec minus(const MVec& a, const MVec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
MVec normalized(const MVec& v) {
    const float l = length(v);
    return l > 0 ? scaled(v, 1.0f / l) : MVec{0, 0, 0};
}

} // namespace

float angleSubtract(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// 0x4fae80. t is clamped to the duration for the stopping types 3, 7 and 8.
MVec Trajectory::evaluate(int64_t nowMs) const {
    float t = float(nowMs - startMs);
    if (type == 3 || type == 7 || type == 8) t = std::clamp(t, 0.0f, float(durationMs));
    const float ts = t * 0.001f;
    switch (type) {
    case 2:
    case 3: return plus(base, scaled(delta, ts));
    case 4: return plus(base, scaled(delta, std::sin(durationMs > 0 ? t / float(durationMs) * 2.0f * float(M_PI) : 0.0f)));
    case 5: {
        MVec p = plus(base, scaled(delta, ts));
        p[2] -= 400.0f * ts * ts;
        return p;
    }
    case 7: {
        const float durS = float(durationMs) * 0.001f;
        return plus(base, scaled(normalized(delta), durS > 0 ? length(delta) / durS * ts * ts * 0.5f : 0.0f));
    }
    case 8: {
        const float durS = float(durationMs) * 0.001f;
        return minus(plus(base, scaled(delta, ts)), scaled(normalized(delta), durS > 0 ? length(delta) / durS * ts * ts * 0.5f : 0.0f));
    }
    default: return base;
    }
}

void startMove(MoverChannel& ch, const MVec& current, const MVec& target, int totalMs, int accelMs, int decelMs, int64_t nowMs) {
    ch.active = true;
    ch.target = target;
    ch.decelMs = decelMs;
    ch.tr.startMs = nowMs;
    ch.tr.base = current;
    const MVec diff = minus(target, current);
    if (accelMs == 0 && decelMs == 0) {
        ch.tr.type = 3;
        ch.tr.durationMs = totalMs;
        ch.tr.delta = scaled(diff, 1.0f / (float(totalMs) * 0.001f));
        ch.linearMs = totalMs;
        ch.speed = length(ch.tr.delta);
        ch.velocity = ch.tr.delta;
        ch.phase1End = current;
        ch.phase2End = target;
        return;
    }
    ch.linearMs = totalMs - accelMs - decelMs;
    ch.speed = 2.0f * length(diff) / (float(2 * totalMs - accelMs - decelMs) * 0.001f);
    ch.velocity = scaled(normalized(diff), ch.speed);
    if (accelMs > 0) {
        ch.tr.type = 7;
        ch.tr.durationMs = accelMs;
        ch.tr.delta = ch.velocity;
        ch.phase1End = ch.tr.evaluate(nowMs + accelMs);
    } else {
        ch.phase1End = current;
        ch.tr.type = ch.linearMs > 0 ? 3 : 8;
        ch.tr.durationMs = ch.linearMs > 0 ? ch.linearMs : decelMs;
        ch.tr.delta = ch.velocity;
    }
    ch.phase2End = plus(ch.phase1End, scaled(ch.velocity, float(ch.linearMs) * 0.001f));
}

// rotatevelocity planner (0x532e20): accel = decel = 0 gives a type 3 trajectory of the total time with delta = velocity
// (the spin stops at the end); otherwise lin = total − accel − decel, accel > 0: type 7 up to the full speed |v|,
// accel = 0: type 3 for lin (type 8 when lin = 0); phase2End = phase1End + v·lin; the final value is phase2End, plus
// v·decel/2 (a type 8 ramp) when decel > 0.
void startVelocityMove(MoverChannel& ch, const MVec& cur, const MVec& v, int totalMs, int accelMs, int decelMs, int64_t nowMs) {
    ch = {};
    ch.active = true;
    ch.tr = {3, nowMs, totalMs, cur, v};
    ch.velocity = v;
    ch.speed = length(v);
    ch.decelMs = decelMs;
    if (accelMs == 0 && decelMs == 0) {
        ch.linearMs = totalMs;
        ch.phase1End = cur;
        ch.phase2End = ch.target = ch.tr.evaluate(nowMs + totalMs);
        return;
    }
    ch.linearMs = totalMs - accelMs - decelMs;
    if (accelMs > 0) {
        ch.tr.type = 7;
        ch.tr.durationMs = accelMs;
        ch.phase1End = ch.tr.evaluate(nowMs + accelMs);
    } else {
        ch.phase1End = cur;
        ch.tr.type = ch.linearMs != 0 ? 3 : 8;
        ch.tr.durationMs = ch.linearMs != 0 ? ch.linearMs : decelMs;
    }
    ch.phase2End = plus(ch.phase1End, scaled(v, float(ch.linearMs) * 0.001f));
    ch.target = decelMs > 0 ? Trajectory{8, 0, decelMs, ch.phase2End, v}.evaluate(decelMs) : ch.phase2End;
}

bool stepMove(MoverChannel& ch, int64_t nowMs) {
    while (ch.active && ch.tr.startMs + ch.tr.durationMs <= nowMs) {
        const int64_t end = ch.tr.startMs + ch.tr.durationMs;
        if (ch.tr.type == 7 && ch.linearMs > 0) {
            ch.tr = {3, end, ch.linearMs, ch.phase1End, ch.velocity};
        } else if ((ch.tr.type == 3 || ch.tr.type == 7) && ch.decelMs > 0) {
            ch.tr = {8, end, ch.decelMs, ch.phase2End, scaled(normalized(minus(ch.target, ch.phase2End)), ch.speed)};
        } else {
            if (ch.tr.type == 5) ch.tr.base = ch.tr.evaluate(nowMs);
            else ch.tr.base = ch.target;
            ch.tr.type = 0;
            ch.active = false;
            return true;
        }
    }
    return false;
}

void World::runMovers() {
    for (auto it = movers_.begin(); it != movers_.end();) {
        Entity* e = entityByNumber(it->first);
        if (!e || e->obj->removed) {
            it = movers_.erase(it);
            continue;
        }
        MoverState& m = it->second;
        for (MoverChannel* ch : {&m.pos, &m.apos}) {
            if (!ch->active) continue;
            const bool done = stepMove(*ch, timeMs_);
            const MVec v = done ? ch->tr.base : ch->tr.evaluate(timeMs_);
            e->obj->set(ch->angles ? "angles" : "origin", Value(Vec3{v[0], v[1], v[2]}));
            if (done) {
                if (ch->angles) {
                    // Angles are normalised when a rotation ends (0x532960).
                    Vec3 a = e->obj->get("angles").asVector();
                    for (float& x : a) x = angleSubtract(x, 0.0f);
                    e->obj->set("angles", Value(a));
                }
                vm_.notify(e->obj, ch->angles ? "rotatedone" : "movedone");
            }
        }
        ++it;
    }
    // Linked entities follow their parent (TODO(spec): the original update order and tag handling, 0x4ed770).
    for (const auto& [child, link] : links_) {
        Entity* c = entityByNumber(child);
        Entity* p = entityByNumber(link.parent);
        if (!c || !p || c->obj->removed || p->obj->removed) continue;
        if (link.fullFrame) {
            // G_GeneralLink: the offset frame taken in the parent's tag frame (0x4d6ad0).
            Vec3 po;
            Axis pa;
            if (!linkParentFrame(link, po, pa)) continue;
            c->obj->set("origin", Value(axisApply(po, pa, link.originOffset)));
            c->obj->set("angles", Value(axisAngles(axisMul(anglesAxis(link.anglesOffset), pa))));
            continue;
        }
        const Value po = p->obj->get("origin"), pa = p->obj->get("angles");
        Vec3 base = po.isVector() ? po.asVector() : Vec3{0, 0, 0}, ang = pa.isVector() ? pa.asVector() : Vec3{0, 0, 0};
        if (!link.tag.empty()) {
            const auto bones = entityBones(*p);
            if (auto b = bones.find(lowerStr(link.tag)); b != bones.end()) base = b->second.position;
        }
        const float yaw = ang[1] * float(M_PI) / 180.0f;
        const auto& o = link.originOffset;
        c->obj->set("origin", Value(Vec3{base[0] + o[0] * std::cos(yaw) - o[1] * std::sin(yaw),
                                         base[1] + o[0] * std::sin(yaw) + o[1] * std::cos(yaw), base[2] + o[2]}));
        c->obj->set("angles", Value(Vec3{ang[0] + link.anglesOffset[0], ang[1] + link.anglesOffset[1], ang[2] + link.anglesOffset[2]}));
    }
}

void World::registerMoverBuiltins() {
    auto mover = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || (e->classname != "script_brushmodel" && e->classname != "script_model" && e->classname != "script_origin"))
            throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) +
                                     " is not a script_brushmodel, script_model, or script_origin");
        return *e;
    };
    // Time arguments (0x533510): total > 0, accel and decel ≥ 0 (default 0), accel + decel ≤ total.
    auto times = [](CallContext& c, size_t first, int& total, int& accel, int& decel) {
        const float t = c.arg(first).asFloat();
        const float a = c.args.size() > first + 1 ? c.arg(first + 1).asFloat() : 0.0f;
        const float d = c.args.size() > first + 2 ? c.arg(first + 2).asFloat() : 0.0f;
        if (t <= 0) throw std::runtime_error("total time must be positive");
        if (a < 0) throw std::runtime_error("accel time must be nonnegative");
        if (d < 0) throw std::runtime_error("decel time must be nonnegative");
        if (a + d > t) throw std::runtime_error("accel time plus decel time is greater than total time");
        total = int(t * 1000.0f);
        accel = int(a * 1000.0f);
        decel = int(d * 1000.0f);
    };
    auto current = [this](Entity& e, MoverChannel& ch, const char* field) {
        // A running move continues from its current position (0x532b0a).
        if (ch.active) return ch.tr.evaluate(timeMs_);
        const Value fv = e.obj->get(field);
        const Vec3 v = fv.isVector() ? fv.asVector() : Vec3{0, 0, 0};
        return MVec{v[0], v[1], v[2]};
    };
    auto moveTo = [this, current](Entity& e, const MVec& target, int total, int accel, int decel) {
        MoverChannel& ch = movers_[e.number].pos;
        ch.angles = false;
        startMove(ch, current(e, ch, "origin"), target, total, accel, decel, timeMs_);
    };
    auto rotateTo = [this, current](Entity& e, const MVec& target, int total, int accel, int decel) {
        MoverChannel& ch = movers_[e.number].apos;
        ch.angles = true;
        startMove(ch, current(e, ch, "angles"), target, total, accel, decel, timeMs_);
    };
    vm_.registerBuiltin("moveto", [mover, times, moveTo](CallContext& c) {
        Entity& e = mover(c);
        int t, a, d;
        times(c, 1, t, a, d);
        const Vec3 p = c.arg(0).asVector();
        moveTo(e, {p[0], p[1], p[2]}, t, a, d);
        return Value();
    });
    for (int axis = 0; axis < 3; ++axis) {
        static const char* const moveNames[3] = {"movex", "movey", "movez"};
        static const char* const rotNames[3] = {"rotatepitch", "rotateyaw", "rotateroll"};
        vm_.registerBuiltin(moveNames[axis], [mover, times, moveTo, axis](CallContext& c) {
            Entity& e = mover(c);
            int t, a, d;
            times(c, 1, t, a, d);
            Vec3 p = e.obj->get("origin").asVector();
            p[size_t(axis)] += c.arg(0).asFloat();
            moveTo(e, {p[0], p[1], p[2]}, t, a, d);
            return Value();
        });
        vm_.registerBuiltin(rotNames[axis], [mover, times, rotateTo, axis](CallContext& c) {
            Entity& e = mover(c);
            int t, a, d;
            times(c, 1, t, a, d);
            Vec3 p = e.obj->get("angles").asVector();
            p[size_t(axis)] += c.arg(0).asFloat();
            rotateTo(e, {p[0], p[1], p[2]}, t, a, d);
            return Value();
        });
    }
    vm_.registerBuiltin("rotateto", [mover, times, rotateTo](CallContext& c) {
        Entity& e = mover(c);
        int t, a, d;
        times(c, 1, t, a, d);
        const Vec3 cur = e.obj->get("angles").asVector(), want = c.arg(0).asVector();
        MVec target;
        for (int k = 0; k < 3; ++k) target[size_t(k)] = cur[size_t(k)] + angleSubtract(want[size_t(k)], cur[size_t(k)]);
        rotateTo(e, target, t, a, d);
        return Value();
    });
    // rotatevelocity (0x533db0 → planner 0x532e20, see startVelocityMove); the start is the current angles.
    vm_.registerBuiltin("rotatevelocity", [this, mover, times, current](CallContext& c) {
        Entity& e = mover(c);
        int t, a, d;
        times(c, 1, t, a, d);
        MoverChannel& ch = movers_[e.number].apos;
        const MVec cur = current(e, ch, "angles");
        const Vec3 vel = c.arg(0).asVector();
        startVelocityMove(ch, cur, {vel[0], vel[1], vel[2]}, t, a, d, timeMs_);
        ch.angles = true;
        return Value();
    });
    vm_.registerBuiltin("movegravity", [this, mover](CallContext& c) {
        Entity& e = mover(c);
        const float t = c.arg(1).asFloat();
        if (t <= 0) throw std::runtime_error("total time must be positive");
        MoverChannel& ch = movers_[e.number].pos;
        const Vec3 cur = e.obj->get("origin").asVector(), vel = c.arg(0).asVector();
        ch = {};
        ch.active = true;
        ch.tr = {5, timeMs_, int(t * 1000.0f), {cur[0], cur[1], cur[2]}, {vel[0], vel[1], vel[2]}};
        return Value();
    });
    // launch (0x4f7be0): sets the position trajectory to type 5 (gravity) starting now at the current origin with the given
    // velocity (trTime = level time, trDelta = vector), then relinks (0x44b2d0). The duration is not written; the movement
    // lasts until the entity is deleted. TODO(spec): collision/landing of a launched script_model (not examined).
    vm_.registerBuiltin("launch", [this, mover](CallContext& c) {
        Entity& e = mover(c);
        MoverChannel& ch = movers_[e.number].pos;
        const Vec3 cur = e.obj->get("origin").asVector(), vel = c.arg(0).asVector();
        ch = {};
        ch.active = true;
        ch.tr = {5, timeMs_, 600000, {cur[0], cur[1], cur[2]}, {vel[0], vel[1], vel[2]}};
        return Value();
    });
    // solid / notsolid (0x533f70 / 0x534050): errors on script_origin; collision of moving brush models is not
    // connected yet (needs the submodel traces of the collision spec).
    vm_.registerBuiltin("solid", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (e && e->classname == "script_origin") throw std::runtime_error("solid called on a script_origin");
        if (e) { e->obj->set("$notsolid", Value()); e->obj->set("$contents", Value()); } // 0x533480 / 0x533440: the spawn contents again
        return Value();
    });
    // setcandamage (0x533eb0): the take-damage flag (ent+0x111) of a script_brushmodel, script_model or
    // script_origin; other entities raise the mover error.
    vm_.registerBuiltin("setcandamage", [mover](CallContext& c) {
        mover(c).obj->set("$takedamage", Value(c.arg(0).truthy()));
        return Value();
    });
    vm_.registerBuiltin("notsolid", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (e && e->classname == "script_origin") throw std::runtime_error("notsolid called on a script_origin");
        if (e) { e->obj->set("$notsolid", Value(true)); e->obj->set("$contents", Value()); } // 0x5334db: contents 0
        return Value();
    });
    vm_.registerBuiltin("linkto", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        Entity* p = entityOf(c.arg(0));
        if (!e || !p) throw std::runtime_error("linkto needs two entities");
        // linkto(parent [, tag [, originOffset, anglesOffset]]) (0x4ed770): an empty tag name is no tag. With up to
        // two arguments the child keeps its placement in the parent's tag frame (0x4d6540, then 0x4d6ca0); with
        // offsets they are the placement (0x4d66a0). TODO(spec): the "failed to link" errors (missing tag, parent
        // without a model, link loops).
        Link l;
        l.parent = p->number;
        l.fullFrame = true;
        l.tag = c.args.size() > 1 && c.arg(1).isString() ? c.arg(1).asString() : std::string();
        if (c.args.size() > 2) {
            if (c.arg(2).isVector()) { const Vec3 o = c.arg(2).asVector(); l.originOffset = {o[0], o[1], o[2]}; }
            if (c.args.size() > 3 && c.arg(3).isVector()) { const Vec3 o = c.arg(3).asVector(); l.anglesOffset = {o[0], o[1], o[2]}; }
        } else {
            Vec3 po;
            Axis pa;
            linkParentFrame(l, po, pa);
            const Value cov = e->obj->get("origin"), cav = e->obj->get("angles");
            const Vec3 co = cov.isVector() ? cov.asVector() : Vec3{0, 0, 0}, ca = cav.isVector() ? cav.asVector() : Vec3{0, 0, 0};
            l.originOffset = axisLocal(po, pa, co);
            l.anglesOffset = axisAngles(axisMul(anglesAxis(ca), axisTranspose(pa)));
        }
        links_[e->number] = l;
        return Value();
    });
    vm_.registerBuiltin("unlink", [this](CallContext& c) {
        // 0x4ede30: a player also drops its link view fraction and mode.
        if (Entity* e = entityOf(c.self)) {
            links_.erase(e->number);
            if (e->isPlayer) playerLink_.reset();
        }
        return Value();
    });
    vm_.registerBuiltin("enablelinkto", [](CallContext& c) {
        if (c.self.isObject()) {
            if (c.self.asObject()->get("$linktoenabled").truthy()) throw std::runtime_error("entity already has linkTo enabled");
            c.self.asObject()->set("$linktoenabled", Value(true));
        }
        return Value();
    });
    // istouching(other) (0x4ef100): one entity is the shape (a brush model, trigger_radius or trigger_disk), the other
    // the box at its origin + r.mins / r.maxs, tested with SV_EntityContact (0x447830, triggerContact). When self is not
    // a shape, other is taken as the shape; two shapes are an error. Box bounds: actors (-15,-15,0)-(15,15,72)
    // (0x4fc640), the player (-15,-15,0)-(15,15,70); TODO(spec) other entities' r.mins/r.maxs (taken as a point here),
    // and two plain boxes (0x447830 then traces against a temporary box model; tested as box overlap here).
    vm_.registerBuiltin("istouching", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        Entity* b = entityOf(c.arg(0));
        if (!a || !b) return Value(false);
        auto isShape = [](Entity& e) {
            const Value m = e.obj->get("model");
            return (m.isString() && !m.asString().empty() && m.asString()[0] == '*') || e.classname == "trigger_radius" ||
                   e.classname == "trigger_disk";
        };
        Entity* shape = a;
        Entity* box = b;
        if (isShape(*a)) {
            if (isShape(*b)) throw std::runtime_error("istouching cannot be called on 2 brush/cylinder entities");
        } else {
            shape = b;
            box = a;
        }
        auto bounds = [](Entity& e, Vec3& mn, Vec3& mx) {
            const Value ov = e.obj->get("origin");
            const Vec3 o = ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
            const float r = e.isActor || e.isPlayer ? 15.0f : 0.0f, h = e.isActor ? 72.0f : e.isPlayer ? 70.0f : 0.0f;
            mn = {o[0] - r, o[1] - r, o[2]};
            mx = {o[0] + r, o[1] + r, o[2] + h};
        };
        Vec3 mn, mx;
        bounds(*box, mn, mx);
        if (isShape(*shape)) return Value(triggerContact(*shape, mn, mx));
        Vec3 smn, smx;
        bounds(*shape, smn, smx);
        for (size_t k = 0; k < 3; ++k)
            if (mn[k] > smx[k] || smn[k] > mx[k]) return Value(false);
        return Value(true);
    });
}

} // namespace iw2::game
