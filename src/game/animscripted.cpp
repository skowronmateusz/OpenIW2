// Scripted animations: animscripted, startscriptedanim, stopanimscripted and the actor's animscripted state
// (docs/spec/animscripted.md). The rules mirror 0x4f5d50 (the shared built-in), 0x53b190 (start), 0x53baf0 (the
// per-frame pose) and the state functions 0x54b060 / 0x54b070 / 0x54b0a0.
#include "game/world.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

constexpr float kFrameTime = 0.05f;      // 0x3d4ccccd
constexpr float kBlendTime = 0.2f;       // 0x3e4ccccd
constexpr float kOffsetSlide = 300.0f;   // 0x591f4c: inches per second the origin offset shrinks by
constexpr float kAngleLerpRate = 540.0f; // entity +0x1f0, set by G_InitGentity (0x4d79b2)

// 0x4240a0: into [0, 360).
float angleNormalize360(float a) {
    while (a < 0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

// 0x423900: yaw in degrees of the delta quaternion (z, w).
float deltaYaw(float z, float w) {
    const float n = 2.0f / (z * z + w * w);
    return std::atan2(w * z * n, 1.0f - n * z * z) * 57.2957763671875f; // 0x591fd8
}

// 0x53b9a0: the offset shrinks by 300 · dt inches (the exe's fast inverse square root with one Newton step);
// what is left is added to the origin, or the offset ends.
void slideOffset(Vec3& offset, Vec3& origin, float dt) {
    const float len2 = offset[2] * offset[2] + offset[1] * offset[1] + offset[0] * offset[0];
    if (len2 == 0.0f) return;
    int32_t bits;
    std::memcpy(&bits, &len2, 4);
    bits = 0x5f3759df - (bits >> 1);
    float y;
    std::memcpy(&y, &bits, 4);
    const float f = 1.0f - (1.5f - len2 * 0.5f * y * y) * y * dt * kOffsetSlide;
    if (f > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            offset[size_t(i)] *= f;
            origin[size_t(i)] += offset[size_t(i)];
        }
        return;
    }
    offset = {0, 0, 0};
}

// 0x53ba60: each angle offset moves towards 0 by rate · dt degrees; a non-zero rest is added to the angle.
void slideAngles(Vec3& offset, Vec3& angles, float dt, float rate) {
    const float step = dt * rate;
    for (int i = 0; i < 3; ++i) {
        float& o = offset[size_t(i)];
        if (o == 0.0f) continue;
        if (o > step) {
            o -= step;
            angles[size_t(i)] = angleNormalize360(o + angles[size_t(i)]);
        } else if (o < -step) {
            o += step;
            angles[size_t(i)] = angleNormalize360(o + angles[size_t(i)]);
        } else {
            o = 0.0f;
        }
    }
}

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Vec3 vecOf(const Value& v) { return v.isVector() ? v.asVector() : Vec3{0, 0, 0}; }

} // namespace

// 0x4d6ad0: the parent's tag frame, or its angles at its origin without a tag. TODO(spec): entityBones turns the
// skeleton by the parent's yaw only (pitch and roll of the parent are not in the tag frame).
bool World::linkParentFrame(const Link& l, Vec3& origin, Axis& axis) {
    Entity* p = entityByNumber(l.parent);
    if (!p || p->obj->removed) return false;
    origin = vecOf(p->obj->get("origin"));
    axis = anglesAxis(vecOf(p->obj->get("angles")));
    if (!l.tag.empty()) {
        const auto bones = entityBones(*p);
        if (auto b = bones.find(lowerStr(l.tag)); b != bones.end()) {
            origin = b->second.position;
            static const Vec3 units[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            for (int i = 0; i < 3; ++i) axis[size_t(i)] = assets::rotate(b->second.rotation, units[i]);
        }
    }
    return true;
}

// 0x53b190: the scripted frame, the tree change and the offsets that let the entity slide from where it stands.
void World::startScriptedAnim(Entity& e, const Vec3& origin, const Vec3& angles, int anim, int root, const std::string& notify,
                              bool deathplant) {
    if (!e.anim) // entity flag 0x2000
        throw std::runtime_error("entity (classname: '" + e.classname + "') does not currently support animscripted");
    if (!e.scripted) e.scripted = std::make_unique<ScriptedAnim>();
    ScriptedAnim& s = *e.scripted;
    s.root = root;
    s.started = false;
    s.anim = anim;
    s.deathplant = deathplant;
    s.origin = origin;
    Vec3 frameAngles = angles;
    if (deathplant) {
        // The origin is dropped to the ground between 36 above and 18 below it (trace 0x44bf00, box 0x56f704 /
        // 0x56f710, mask 0x820011) and the frame keeps only yaw and roll. TODO(spec): the trace box, the slope at the
        // animation's end (+0x50…+0x5c, 0x4fd890) and the per-frame planting (0x53bb84, 0x47f390) are not modelled.
        const Vec3 start{origin[0], origin[1], origin[2] + 36.0f}, end{origin[0], origin[1], origin[2] - 18.0f};
        const TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, 0x820011u);
        if (!tr.startSolid && tr.fraction < 1.0f) s.origin[2] = (end[2] - start[2]) * tr.fraction + start[2];
        frameAngles = {0.0f, angles[1], angles[2]};
    }
    s.axis = anglesAxis(frameAngles);
    // TODO(spec): for an actor with +0x1b8 ≠ 0 and +0x1c4 ≠ 0 the exe first re-sets the nodes +0x1d0 / +0x1d2 /
    // +0x1d4 to their current weights (0x478d10, 0x4795e0) and clears the root's children only with a root; the
    // root's children are always cleared here.
    e.anim->clearChildren(root, kBlendTime); // 0x4789a0
    AnimState::SetArgs a;
    a.weight = 1.0f;
    a.blendTime = kBlendTime;
    a.rate = 1.0f;
    a.notify = notify;
    a.restart = !e.anim->looping(anim); // restart unless the animation loops (0x53b7ca)
    e.anim->set(anim, a);
    // TODO(spec): 0x478220 (an immediate 0.05 s tree update up to the next notetrack) is not modelled, as for the
    // setanim built-ins.
    updateScriptedPose(e, false);
}

// The pose of the scripted animation at its current time (0x53bb3b…0x53bb7f) and, at the start, the offsets
// from it to where the entity stands (0x53b8a0…); every later frame the offsets slide to 0 (0x53bcb8…).
void World::updateScriptedPose(Entity& e, bool slide) {
    ScriptedAnim& s = *e.scripted;
    float qz, qw;
    std::array<float, 3> t;
    e.anim->nodeDelta(s.anim, qz, qw, t);
    Vec3 pos = axisApply(s.origin, s.axis, {t[0], t[1], t[2]});
    Vec3 ang = axisAngles(axisMul(yawAxis(deltaYaw(qz, qw)), s.axis));
    if (!slide) {
        const Vec3 o = vecOf(e.obj->get("origin")), ea = vecOf(e.obj->get("angles"));
        for (int i = 0; i < 3; ++i) {
            s.originOffset[size_t(i)] = o[size_t(i)] - pos[size_t(i)];
            s.anglesOffset[size_t(i)] = angleSubtract(ea[size_t(i)], ang[size_t(i)]);
        }
        return;
    }
    slideOffset(s.originOffset, pos, kFrameTime);
    slideAngles(s.anglesOffset, ang, kFrameTime, kAngleLerpRate);
    auto link = links_.find(e.number);
    if (link == links_.end()) {
        e.obj->set("origin", Value(pos));
        e.obj->set("angles", Value(ang));
        return;
    }
    // Linked (0x53bcf5): the pose moves with the parent since animscripted, then becomes the link offset (0x4d6ca0).
    Link& l = link->second;
    Vec3 po;
    Axis pa;
    if (!linkParentFrame(l, po, pa)) return;
    if (!l.hasStartFrame) { // TODO(spec): tagInfo +0x40 before any animscripted (startscriptedanim alone) is not read
        l.hasStartFrame = true;
        l.startOrigin = po;
        l.startAxis = pa;
    }
    const Vec3 localPos = axisLocal(l.startOrigin, l.startAxis, pos);
    const Axis localAxis = axisMul(anglesAxis(ang), axisTranspose(l.startAxis));
    l.fullFrame = true;
    l.originOffset = localPos;
    l.anglesOffset = axisAngles(localAxis);
    e.obj->set("origin", Value(axisApply(po, pa, localPos)));
    e.obj->set("angles", Value(axisAngles(axisMul(localAxis, pa))));
}

// 0x53baf0, every frame while the block exists: without a tree or once the animation has finished the block is
// freed; after the first update the animation ends when it has played through (its notify name is dropped).
void World::updateScriptedAnim(Entity& e) {
    if (!e.scripted) return;
    ScriptedAnim& s = *e.scripted;
    if (!e.anim || s.anim == 0) {
        e.scripted.reset();
        return;
    }
    updateScriptedPose(e, true);
    if (!s.started) {
        s.started = true;
        return;
    }
    if (e.anim->playedThrough(s.anim)) {
        AnimState::SetArgs a; // 0x53bdf6: weight 1 over 0.2 s at rate 1, no notify
        e.anim->set(s.anim, a);
        s.anim = 0;
    }
}

// stopanimscripted (0x4f60d0): an actor whose predicted state is animscripted pops it; the animation loses its
// notify name and the block is freed.
void World::stopScriptedAnim(Entity& e) {
    if (e.isActor && e.ai.predicted[size_t(e.ai.predictedDepth)] == AiScripted) aiPopState(e);
    if (!e.scripted) return;
    if (e.scripted->anim && e.anim) {
        AnimState::SetArgs a; // TODO(spec): the arguments of this 0x4795e0 call are not read; those of 0x53bdf6
        e.anim->set(e.scripted->anim, a);
    }
    e.scripted.reset();
}

// Script models and other non-actors with a scripted block (0x52ccb0). TODO(spec): where G_RunFrame calls it.
void World::runScriptedModels() {
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& e = *entities_[i];
        if (e.scripted && !e.isActor && !e.obj->removed) updateScriptedAnim(e);
    }
}

// The animscripted state's think (0x54b0a0): node and path released, animscripts/scripted main as the animscript
// (movemode stop, anim mode 4); when that script or the scripted animation has ended the state is popped.
int World::scriptedThink(Entity& e) {
    auto pop = [&] {
        if (e.ai.predicted[size_t(e.ai.predictedDepth)] == AiScripted) aiPopState(e);
        return 1;
    };
    // TODO(spec): actor bytes +0x740 / +0x741 are cleared; their meaning is not read.
    if (e.claimedNode >= 0) releaseNode(e); // 0x5191a0
    clearActorPath(e);                     // 0x541780, +0x6b0 / +0x6b2 / +0x724
    setActorAnimScript(e, "scripted", 0, 4);
    if (actorAnimScriptDone(e)) return pop();
    updateScriptedAnim(e);
    if (!e.scripted) return pop();
    if (std::getenv("IW2_SCRIPTED_DEBUG") && timeMs_ % 2000 == 0) {
        const Vec3 o = vecOf(e.obj->get("origin")), a = vecOf(e.obj->get("angles"));
        print("DBG scripted " + std::to_string(e.number) + " at " + std::to_string(o[0]) + "," + std::to_string(o[1]) + "," +
              std::to_string(o[2]) + " yaw " + std::to_string(a[1]) + " anim " +
              (e.scripted->anim ? e.anim->tree().nodes()[size_t(e.scripted->anim)].name : std::string("-")) +
              (links_.count(e.number) ? " linked" : ""));
    }
    actorPerceive(e); // 0x4fe890
    // TODO(spec): 0x504950 (the actor's look angles from its pitch / yaw) is not modelled.
    e.velocity = {0, 0, 0}; // +0x230…, +0x250…
    return 0;
}

void World::registerScriptedAnimBuiltins() {
    // 0x4f5d50 with 4…6 arguments: notify name, origin, angles, animation [, mode "normal" / "deathplant" [, root]].
    auto parse = [this](CallContext& c, Entity*& e, std::string& notify, Vec3& origin, Vec3& angles, int& anim, int& root,
                         bool& deathplant, Value& modeArg, Value& rootArg) {
        e = entityOf(c.self);
        if (!e) throw std::runtime_error("not an entity");
        if (c.args.size() > 6) throw std::runtime_error("too many parameters");
        if (c.args.size() < 4) throw std::runtime_error("Incorrect number of parameters for animscripted command");
        deathplant = false;
        if (c.args.size() > 4) {
            modeArg = c.arg(4);
            const std::string m = modeArg.isString() ? modeArg.asString() : std::string();
            if (modeArg.isDefined() && m != "normal" && m != "deathplant")
                throw std::runtime_error("Illegal mode " + m + " for animScripted. Valid modes are normal and deathplant");
            deathplant = m == "deathplant";
        }
        if (!e->anim) throw std::runtime_error("entity has no animtree");
        auto nodeOf = [&](const Value& v) {
            if (!std::holds_alternative<AnimRef>(v.v)) throw std::runtime_error("expected an animation, got " + v.typeName());
            const int n = e->anim->tree().find(std::get<AnimRef>(v.v).name);
            if (n < 0) throw std::runtime_error("animation " + std::get<AnimRef>(v.v).name + " is not in animtree " + e->animTree);
            return n;
        };
        root = 0;
        if (c.args.size() > 5) {
            rootArg = c.arg(5);
            root = nodeOf(rootArg);
        }
        anim = nodeOf(c.arg(3));
        angles = vecOf(c.arg(2));
        origin = vecOf(c.arg(1));
        notify = c.arg(0).toString();
        if (e->isActor && !isAlive(*e))
            throw std::runtime_error("tried to play a scripted animation on a dead AI; entity " + std::to_string(e->number) +
                                     " classname " + e->classname);
    };
    vm_.registerBuiltin("animscripted", [this, parse](CallContext& c) {
        Entity* e;
        std::string notify;
        Vec3 origin, angles;
        int anim, root;
        bool deathplant;
        Value modeArg, rootArg;
        parse(c, e, notify, origin, angles, anim, root, deathplant, modeArg, rootArg);
        if (e->isActor) {
            // Push the animscripted state, end the animscript, drop the notify of a running scripted animation and
            // hand the arguments to animscripts/scripted::init; its main starts the animation (0x4f5f2d…0x4f605d).
            aiPushState(*e, AiScripted);
            killActorAnimScript(*e);
            if (e->scripted && e->scripted->anim) {
                AnimState::SetArgs a;
                e->anim->set(e->scripted->anim, a);
            }
            vm_.start("animscripts/scripted.gsc", "init", {c.arg(0), c.arg(1), c.arg(2), c.arg(3), modeArg, rootArg}, Value(e->obj));
        } else {
            startScriptedAnim(*e, origin, angles, anim, root, notify, deathplant);
        }
        // A linked entity keeps the parent frame of this moment (0x4f60a3, tagInfo +0x40).
        if (auto l = links_.find(e->number); l != links_.end()) {
            Vec3 po;
            Axis pa;
            if (linkParentFrame(l->second, po, pa)) {
                l->second.hasStartFrame = true;
                l->second.startOrigin = po;
                l->second.startAxis = pa;
            }
        }
        return Value();
    });
    // startscriptedanim (0x53c520): actors only, straight to 0x53b190 (called by animscripts/scripted main).
    vm_.registerBuiltin("startscriptedanim", [this, parse](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        std::string notify;
        Vec3 origin, angles;
        int anim, root;
        bool deathplant;
        Value modeArg, rootArg;
        parse(c, e, notify, origin, angles, anim, root, deathplant, modeArg, rootArg);
        startScriptedAnim(*e, origin, angles, anim, root, notify, deathplant);
        return Value();
    });
    vm_.registerBuiltin("stopanimscripted", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("not an entity");
        stopScriptedAnim(*e);
        return Value();
    });
}

} // namespace iw2::game
