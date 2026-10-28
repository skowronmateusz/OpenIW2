// The actor's turret state (AI state 2 of the state table at 0x56f720: start 0x549960, finish 0x549b20,
// think 0x54aba0; docs/spec/ai.md "AI states").
#include "game/world.h"
#include "game/poseslide.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace iw2::game {

using namespace script;

// Releases the turret (0x549a60): "turret_deactivate" while it was in use, owner cleared, "turretownerchange".
void World::releaseTurret(Entity& turret) {
    TurretState& ts = *turret.turret;
    if (ts.inUse) vm_.notify(turret.obj, "turret_deactivate");
    ts.inUse = false;
    ts.owner.reset();
    ts.flags &= ~0x200u;
    vm_.notify(turret.obj, "turretownerchange");
}

// Inside the goal widened by `extra` (0x502760): height within goalheight, horizontal distance within
// goalradius + extra. Without a goal the actor counts as inside.
// The goal volume (+0x778, setgoalvolume) contains the point (0x447830 with the point as a zero-size box); true without one.
bool World::goalVolumeHolds(Entity& e, const Vec3& p) {
    Entity* vol = entityOf(e.obj->get("$goalvolume"));
    return !vol || triggerContact(*vol, p, p);
}

// The working goal's volume (+0x75c, 0x505f70): the script volume while the working goal is the script's goal (mode 0), none
// for a goal entity or the path to the enemy.
bool World::workGoalVolumeHolds(Entity& e, const Vec3& p) {
    if (e.workGoal.set && !e.workGoal.scriptVolume) return true;
    return goalVolumeHolds(e, p);
}

// 0x5026d0 (extra 0): the height and the radius around the goal position, then the goal volume (0x447830) when set.
// With an extra radius (0x502760) there is no volume test.
bool World::actorInGoal(Entity& e, float extra) {
    Vec3 gp{};
    float r = 0, h = 0;
    if (!actorGoal(e, gp, r, h)) return true;
    const Vec3 o = e.obj->get("origin").asVector();
    const float dz = gp[2] - o[2];
    if (h * h < dz * dz) return false;
    const float dx = gp[0] - o[0], dy = gp[1] - o[1];
    if (!(dx * dx + dy * dy <= (extra + r) * (extra + r))) return false;
    return extra != 0.0f || workGoalVolumeHolds(e, o);
}

// Start (0x549960): take the recorded turret unless it is in use. TODO(spec): the actor is linked to the
// turret there (0x4d6540, no tag) and the link offset is set (0x4d6ca0); the pose step reads the offset's height, which
// the port computes from the current origins instead (World::turretPoseStep).
bool World::turretStateStart(Entity& e) {
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    if (!t || !t->turret || t->obj->removed) {
        e.usingTurret.reset();
        return false;
    }
    TurretState& ts = *t->turret;
    // 0x54997c: any turret in use (+0x112) is refused, also one this actor already owns; the recorded turret (+0x2114)
    // stays (0x549a50 returns 0 without clearing it; the exposed state then drops it).
    if (ts.inUse) return false;
    ts.inUse = true;
    ts.owner = e.obj;
    vm_.notify(t->obj, "turretownerchange");
    turretIdle(*t); // 0x51b850
    ts.flags = (ts.flags & ~0xe8u) | 0x100u;
    ts.rightArc = ts.setupRightArc;
    ts.leftArc = ts.setupLeftArc;
    ts.flags &= ~0x10u;
    // 0x549a28..0x549a3c: keepclaimednode / keepclaimednodeingoal (+0x740 / +0x741) cleared and a claimed node released
    // (0x5191a0). TODO(spec): 0x54e3f0 and 0x503e10, and the start argument that keeps flag 0x10 / +0x60 when it is 6.
    e.obj->set("keepclaimednode", Value(int32_t(0)));
    e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
    if (e.claimedNode >= 0) releaseNode(e);
    return true;
}

// Finish (0x549b20): release the turret this actor owns.
void World::turretStateFinish(Entity& e) {
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    if (t && t->turret && t->turret->inUse && t->turret->owner == e.obj) releaseTurret(*t);
    e.usingTurret.reset();
}

// Think (0x54aba0). Leaving (the turret is gone or not ours, the actor is outside its goal + 92 unless the turret
// ignores goals): release it and replace the state with exposed, returning 1 (think again). Otherwise run the
// turret weapon's animscript (weapon key "script", e.g. "mg42/stand"; turret script table 0x1c7e128).
int World::turretStateThink(Entity& e) {
    clearActorPath(e);
    Entity* t = e.usingTurret ? entityOf(Value(e.usingTurret)) : nullptr;
    auto leave = [&] {
        turretStateFinish(e);
        aiSetState(e, AiExposed);
        return 1;
    };
    if (!t || !t->turret || t->obj->removed || !(t->turret->inUse && t->turret->owner == e.obj)) return leave();
    TurretState& ts = *t->turret;
    // 0x54acc5..0x54ad0c: unless the claimed node is kept (0x4fe6f0), the friendly chain and the working goal are updated
    // (0x505ed0, 0x505f70). TODO(spec): the pathToEnemy reset before it (0x502fc0, an enemy known within 5 s).
    if (!keepClaimedNode(e)) {
        followFriendlyChain(e);
        updateWorkingGoal(e);
    }
    if (!(ts.flags & 0x2000u) && !actorInGoal(e, 92.0f)) return leave();
    actorPerceive(e);
    const auto* w = weapon(ts.weapon);
    auto it = w ? w->find("script") : decltype(w->end()){};
    if (!w || it == w->end() || it->second.empty()) {
        print("turret weapon " + ts.weapon + " has no script");
        return leave();
    }
    // 0x54ae56: movemode 0 (stop), anim mode 1. TODO(spec): 0x515040 / 0x518f20 / 0x519050 before it, and the debug state
    // string at actor +0x21a0 (0x54adb2..0x54adf7, "auto_turret_idle" etc.).
    setActorAnimScript(e, it->second, 0, 1);
    // 0x54ae63: still the owner (0x54afd0), else give the turret up (0x54b020) and become exposed.
    if (!(ts.inUse && ts.owner == e.obj)) return leave();
    e.ai.animDeltaGoalWeights = false; // +0x220 = 0
    return turretPoseStep(e, *t);
}

// Obstruction check of the pose step (0x54a8bc..0x54ab4d), after the move: the move bumped an entity (+0x318 not 0x7ff), the
// state was set at least 1 s ago (+0x20), the turret pitch / yaw lie inside its arcs (topArc..bottomArc,
// rightArc..leftArc) and the slide offsets (turret info +0x90..+0xa4) are 0. A bumped entity without a sentient (+0x104;
// the world entity 0x7fe has none) narrows the arc on the side the gun points to by 1 degree (yaw < 0: rightArc + 1, else
// leftArc - 1, with a "capping" warning); an arc that crosses 0 is set to 0 and the gunner detaches with the warning
// "... detaching from turret due to obstruction". Bumping a sentient detaches at once. Detaching gives the turret up
// (0x54b020) and replaces the state with exposed (0x4feb00, think again). TODO(spec): for a bumped sentient of another
// team the exe also calls 0x4f9f30 and 0x4fa070 (0x54aafa..0x54ab45), not ported.
bool World::turretObstruction(Entity& e, Entity& t) {
    TurretState& ts = *t.turret;
    if (e.ai.bumpedEnt == 0x7ff || timeMs_ - e.ai.baseStateMs < 1000) return false;
    if (!(ts.pitch <= ts.bottomArc) || !(ts.pitch >= ts.topArc) || !(ts.yaw <= ts.leftArc) || !(ts.yaw >= ts.rightArc)) return false;
    for (int a = 0; a < 3; ++a)
        if (ts.gunnerOriginOffset[size_t(a)] != 0.0f || ts.gunnerAnglesOffset[size_t(a)] != 0.0f) return false;
    Entity* g = e.ai.bumpedEnt == 0x7fe ? nullptr : entityByNumber(e.ai.bumpedEnt);
    auto detach = [&] {
        turretStateFinish(e);     // 0x54b020
        aiSetState(e, AiExposed); // 0x4feb00 with 1
        return true;
    };
    if (g && isSentient(*g)) return detach();
    const Value tov = t.obj->get("origin");
    const Vec3 to = tov.isVector() ? tov.asVector() : Vec3{0, 0, 0};
    char buf[256];
    bool crossed;
    if (ts.yaw < 0.0f) {
        ts.rightArc += 1.0f;
        crossed = !(ts.rightArc <= 0.0f);
        if (crossed) ts.rightArc = 0.0f;
        else std::snprintf(buf, sizeof buf, "WARNING: capping rightarc of turret at (%.2f, %.2f, %.2f) to %.2f\n", to[0], to[1], to[2], -ts.rightArc);
    } else {
        ts.leftArc -= 1.0f;
        crossed = ts.leftArc < 0.0f;
        if (crossed) ts.leftArc = 0.0f;
        else std::snprintf(buf, sizeof buf, "WARNING: capping leftarc of turret at (%.2f, %.2f, %.2f) to %.2f\n", to[0], to[1], to[2], ts.leftArc);
    }
    if (!crossed) {
        print(buf);
        return false;
    }
    const Value eov = e.obj->get("origin");
    const Vec3 eo = eov.isVector() ? eov.asVector() : Vec3{0, 0, 0};
    std::snprintf(buf, sizeof buf,
                  "WARNING: AI %d at (%.2f, %.2f, %.2f) with turret angles (%.2f, %.2f) detaching from turret due to obstruction\n",
                  e.number, eo[0], eo[1], eo[2], ts.pitch, ts.yaw);
    print(buf);
    return detach();
}

namespace {

constexpr float kPoseFrameTime = 0.05f;  // 0x3d4ccccd
constexpr float kPoseAngleRate = 540.0f; // 0x44070000
constexpr float kPoseBlendRate = 20.0f;  // 0x591e18: weight per second of the final sets

// 0x423570: (x, y) turned by an angle in degrees.
void rotateXY(std::array<float, 3>& v, float deg) {
    const float r = deg * 0.0174532924f; // 0x591f50
    const float c = std::cos(r), s = std::sin(r);
    const float x = c * v[0] - s * v[1];
    v[1] = s * v[0] + c * v[1];
    v[0] = x;
}

// 0x421160: (x, y) normalized in place unless zero.
void normalize2(float& x, float& y) {
    const float l = std::sqrt(x * x + y * y);
    if (l != 0.0f) {
        x /= l;
        y /= l;
    }
}

} // namespace

// The turret gunner's pose step (0x549ba0, from the turret think 0x54ae93). The setturretanim node (actor +0x2118) has one
// child per gunner height and each of those one grandchild per turret yaw step (generic_human.atr standMG42gunner_aim /
// _fire); "child j" below is in the exe's order, the reverse of the file (ascending delta height; yaw steps right to left).
// Every frame:
// 1. The turret's tag_weapon in model space (0x4725f0 / 0x4d7460, with the bone controls) gives the turret yaw (0x421920 of
//    its frame 0x420270) and the height h = the gunner's height in the turret frame (link offset z, tagInfo +0x3c) minus
//    the tag's height.
// 2. The node's descendants are zeroed at once (0x4789a0 with 0). Children are probed in order: the child goes to weight 1
//    (blend 1, rate 1; 0x478f10 + 0x4794b0); in it f = count * 0.5 - yaw / animHorRotateInc (weapon +0x564), clamped to
//    [0, count - 1], picks grandchildren k = trunc(f) (weight 1 - (f - k)) and k + 1 (weight f - k, when not 0); weights
//    below 0.001 are 0. The child's delta at its current time by goal weights (0x4785d0) gives its height z; the probe
//    stops at the first child with z >= h.
// 3. The descendants are zeroed again and the final weights are set with the blend time 1 / (|weight - goal| * 20) (0 when
//    equal; the weight is read after the zeroing): the last probed child's two grandchildren; a stop at child i
//    (0 < i < count) sets child i to t = (h - z(i-1)) / (z(i) - z(i-1)), child i-1 to 1 - t and child i-1's two
//    grandchildren; a stop at the first child or no stop (the gunner is outside the animated height range) sets that child
//    to 1 and, once the gunner has arrived, bounds the turret pitch (step 5).
// 4. The node's delta (0x4785d0), turned by the turret yaw (0x423570) and added to tag_weapon, with the yaw of its rotation
//    plus the turret yaw, is taken into the turret entity's frame (0x424ad0, 0x422770, 0x424b70). On the first step (turret
//    flag 0x20 clear) the offsets gunner - pose are stored (turret info +0x90 / +0x9c); every step they slide to 0
//    (0x53b9a0, 0x53ba60: 0.05 s, 540 degrees per second). The angles become the entity angles and the facing blocks
//    (0x504950, 0x5049a0); the gunner moves to the posed origin through the actor physics (+0x264 = 1, +0x250 = the xy
//    difference, 0x501d00), or, when the turret itself is linked, is placed there. The velocities +0x230 / +0x250 are zeroed.
// 5. Turret pitch bound (flags 0x200 / 0x400, info +0xa8), only while the offsets are 0: outside the range the angle between
//    tag_aim->tag_weapon and tag_aim->the needed gun height (both as (2D distance, height); acos of the dot, 90 when negative)
//    sets +0xa8 = pitch - angle with 0x200 (below the range) or pitch + angle with 0x600 (above); inside the range +0xa8 moves
//    back by 0.1 per step and 0x200 is cleared once it passes topArc (0x400) or bottomArc. The turret turn reads it
//    (World::turretTurn, 0x51b779: the pitch beyond it becomes the bone-control drop e+0x78).
int World::turretPoseStep(Entity& e, Entity& t) {
    TurretState& ts = *t.turret;
    if (!e.anim) return 0;
    AnimState& an = *e.anim;
    const auto& nodes = an.tree().nodes();

    // Model-space tag (DObjAnimMat: origin and quaternion) of the turret's body with its bone controls.
    auto tagLocal = [&](const char* tag, Vec3& pos, assets::Quat& rot) {
        const auto models = entityModels(t);
        if (models.empty()) return false;
        const HitBoxModel& m = models.front();
        for (size_t i = 0; i < m.parts->bones.size(); ++i) {
            std::string n = m.parts->bones[i].name;
            for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (n != tag) continue;
            pos = {m.position[i][0], m.position[i][1], m.position[i][2]};
            rot = m.rotation[i];
            return true;
        }
        return false;
    };
    auto giveUp = [&](const char* msg) {
        print(msg);
        turretStateFinish(e);     // 0x54b020
        aiSetState(e, AiExposed); // 0x4feb00 with 1
        return 1;
    };

    Vec3 wpos;
    assets::Quat wrot;
    if (!tagLocal("tag_weapon", wpos, wrot)) {
        // 0x549c04: released when this actor owns it (0x549a60), forgotten either way; the state becomes exposed.
        print("WARNING: aborting turret behavior since 'tag_weapon' does not exist\n");
        if (ts.owner == e.obj) releaseTurret(t);
        e.usingTurret.reset();
        aiSetState(e, AiExposed);
        return 1;
    }
    if (!e.turretAnimSet) return giveUp("WARNING: aborting turret behavior since no turret animation specified\n");

    const auto* wdef = weapon(ts.weapon);
    float rotInc = 0.0f; // weapon +0x564 animHorRotateInc
    if (wdef)
        if (auto it = wdef->find("animhorrotateinc"); it != wdef->end()) rotInc = float(std::atof(it->second.c_str()));
    const std::array<float, 3> fwd = assets::rotate(wrot, {1.0f, 0.0f, 0.0f});
    const float tagYaw = (fwd[0] == 0.0f && fwd[1] == 0.0f) ? 0.0f : std::atan2(fwd[1], fwd[0]) * 180.0f / 3.14159274f; // 0x421920
    const Value tov = t.obj->get("origin"), tav = t.obj->get("angles"), eov = e.obj->get("origin");
    const Vec3 tOrigin = tov.isVector() ? tov.asVector() : Vec3{0, 0, 0};
    const Axis tAxis = anglesAxis(tav.isVector() ? tav.asVector() : Vec3{0, 0, 0});
    const Vec3 eOrigin = eov.isVector() ? eov.asVector() : Vec3{0, 0, 0};
    const float linkZ = axisLocal(tOrigin, tAxis, eOrigin)[2];
    const float h = linkZ - wpos[2];

    const int top = e.turretAnim;
    auto setNode = [&](int n, float goal, float blendTime) {
        AnimState::SetArgs a;
        a.weight = goal;
        a.blendTime = blendTime;
        a.rate = 1.0f;
        an.set(n, a);
    };
    // The final sets: the blend time from the distance between the current weight and the goal (0x478000).
    auto rateSet = [&](int n, float goal) {
        const float d = std::fabs(an.nodeStates()[size_t(n)].weight - goal) * kPoseBlendRate;
        setNode(n, goal < 0.001f ? 0.0f : goal, d != 0.0f ? 1.0f / d : 0.0f);
    };

    // The exe's tree lists a node's children in reverse file order: the .atr parser inserts each child at the head of its
    // parent's list (0x452841, the head insertion of 0x4643a0) and the tree builder numbers them walking from that head
    // (0x4530c0 -> 0x475090). The port keeps the file order, so child j in exe order is children[count - 1 - j]. (With it
    // the gunner height children come in ascending delta height, as the probe expects.)
    auto exeChild = [&](int n, int j) {
        const auto& c = nodes[size_t(n)].children;
        return c[c.size() - 1 - size_t(j)];
    };
    an.clearChildren(top, 0.0f);
    const int count = int(nodes[size_t(top)].children.size());
    if (count == 0) throw std::runtime_error("anim '" + nodes[size_t(top)].name + "' has no children");
    int i = 0, child = 0, k = 0, prevK = 0;
    float frac = 0.0f, prevFrac = 0.0f, z = 0.0f, prevZ = 0.0f;
    float qz = 0.0f, qw = 1.0f;
    std::array<float, 3> trans{0, 0, 0};
    for (; i < count; ++i) {
        child = exeChild(top, i);
        setNode(child, 1.0f, 1.0f);
        const int gc = int(nodes[size_t(child)].children.size());
        if (gc == 0) throw std::runtime_error("anim '" + nodes[size_t(child)].name + "' has no children");
        float f = float(gc) * 0.5f - tagYaw / rotInc;
        // A NaN (animHorRotateInc 0 at yaw 0) would reach the exe's integer conversion; it takes the upper end here.
        if (f < 0.0f) f = 0.0f;
        else if (!(f < float(gc - 1))) f = float(gc - 1);
        k = int(f); // 0x565994: truncation
        frac = f - float(k);
        setNode(exeChild(child, k), 1.0f - frac < 0.001f ? 0.0f : 1.0f - frac, 1.0f);
        if (frac != 0.0f) setNode(exeChild(child, k + 1), frac < 0.001f ? 0.0f : frac, 1.0f);
        an.nodeDelta(child, qz, qw, trans);
        z = trans[2];
        if (!(z < h)) break;
        prevZ = z;
        prevK = k;
        prevFrac = frac;
    }
    an.clearChildren(top, 0.0f);
    rateSet(exeChild(child, k), 1.0f - frac);
    if (frac != 0.0f) rateSet(exeChild(child, k + 1), frac);
    const bool arrived = (ts.flags & 0x20u) && ts.gunnerOriginOffset[0] == 0.0f && ts.gunnerOriginOffset[1] == 0.0f &&
                         ts.gunnerOriginOffset[2] == 0.0f;
    if (i != 0 && i != count) {
        if (!arrived) {
            ts.flags &= ~0x200u;
        } else if (ts.flags & 0x200u) {
            if (ts.flags & 0x400u) {
                ts.pitchLimit -= 0.1f; // 0x59202c
                if (!(ts.pitchLimit > ts.topArc)) ts.flags &= ~0x200u;
            } else {
                ts.pitchLimit += 0.1f;
                if (!(ts.pitchLimit < ts.bottomArc)) ts.flags &= ~0x200u;
            }
        }
        const float tw = (h - prevZ) / (z - prevZ);
        rateSet(child, tw);
        const int prevChild = exeChild(top, i - 1);
        rateSet(prevChild, 1.0f - tw);
        rateSet(exeChild(prevChild, prevK), 1.0f - prevFrac);
        if (prevFrac != 0.0f) rateSet(exeChild(prevChild, prevK + 1), prevFrac); // 0x4794f0
    } else {
        Vec3 apos;
        assets::Quat arot;
        if (!tagLocal("tag_aim", apos, arot)) return giveUp("WARNING: aborting turret behavior since 'tag_aim' does not exist\n");
        if (arrived) {
            const float dx = apos[0] - wpos[0], dy = apos[1] - wpos[1];
            const float dist = std::sqrt(dx * dx + dy * dy); // 0x420ec0
            float ax = dist, ay = wpos[2] - apos[2];
            float bx = dist, by = linkZ - z - apos[2];
            normalize2(ax, ay);
            normalize2(bx, by);
            const float dot = by * ay + bx * ax;
            const float angle = dot < 0.0f ? 90.0f : dot <= 1.0f ? std::acos(dot) * 57.2957763671875f : 0.0f;
            if (i == 0) {
                ts.pitchLimit = ts.pitch - angle;
                ts.flags = (ts.flags & ~0x400u) | 0x200u;
            } else {
                ts.pitchLimit = angle + ts.pitch;
                ts.flags |= 0x600u;
            }
        } else {
            ts.flags &= ~0x200u;
        }
        rateSet(child, 1.0f);
    }

    // 0x54a65a: the pose in the turret's frame.
    an.nodeDelta(top, qz, qw, trans);
    rotateXY(trans, tagYaw);
    const Vec3 local{trans[0] + wpos[0], trans[1] + wpos[1], trans[2] + wpos[2]};
    Vec3 pos = axisApply(tOrigin, tAxis, local);
    Vec3 ang = axisAngles(axisMul(yawAxis(pose::deltaYaw(qz, qw) + tagYaw), tAxis));
    if (!(ts.flags & 0x20u)) {
        ts.flags |= 0x20u;
        const Value eav = e.obj->get("angles");
        const Vec3 ea = eav.isVector() ? eav.asVector() : Vec3{0, 0, 0};
        for (int a = 0; a < 3; ++a) {
            ts.gunnerOriginOffset[size_t(a)] = eOrigin[size_t(a)] - pos[size_t(a)];
            ts.gunnerAnglesOffset[size_t(a)] = angleSubtract(ea[size_t(a)], ang[size_t(a)]); // 0x423fd0
        }
    }
    pose::slideOffset(ts.gunnerOriginOffset, pos, kPoseFrameTime);
    pose::slideAngles(ts.gunnerAnglesOffset, ang, kPoseFrameTime, kPoseAngleRate);
    e.obj->set("angles", Value(ang));
    faceBlockToAngles(e, true); // 0x504950 + 0x5049a0
    if (links_.find(t.number) == links_.end()) {
        // +0x264 = 1, +0x250 = (dx, dy, 0), then the physics 0x501d00 (kind 1 is not turned by desiredangle).
        // TODO(spec): 0x4e7150(+0x294, +0x298) and the link offset update 0x4d6ca0.
        moveActorKind(e, 1, {pos[0] - eOrigin[0], pos[1] - eOrigin[1], 0.0f}, kPoseFrameTime);
        if (turretObstruction(e, t)) return 1;
    } else {
        e.obj->set("origin", Value(pos)); // 0x54ab68 (then 0x4d6ca0 with 1)
    }
    for (float& v : e.ai.slideVel) v = 0.0f; // +0x230
    e.velocity = {0, 0, 0};
    if (std::getenv("IW2_TURRET_POSE_DEBUG") && timeMs_ % 1000 == 0) {
        const Vec3 o = e.obj->get("origin").asVector();
        print("DBG turretpose " + std::to_string(e.number) + " turret " + std::to_string(t.number) + " node " + nodes[size_t(top)].name +
              " probe " + std::to_string(i) + "/" + std::to_string(count) + " h " + std::to_string(h) + " tagYaw " +
              std::to_string(tagYaw) + " at " + std::to_string(o[0]) + "," + std::to_string(o[1]) + "," + std::to_string(o[2]) +
              " pose " + std::to_string(pos[0]) + "," + std::to_string(pos[1]) + "," + std::to_string(pos[2]) + " yaw " +
              std::to_string(ang[1]) + "\n" + an.debugString());
    }
    return 0;
}

} // namespace iw2::game
