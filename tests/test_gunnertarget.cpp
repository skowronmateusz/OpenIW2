// Target choice of an AI turret gunner (docs/spec/turrets.md "Gunner target choice"), on the decoytrenches MG42 gunners. Needs the
// game data (IW2_MAIN=<game main dir>); skipped without it.
// 1. An engaged sentient out of sight: the gun keeps firing at its last known position while it is younger than the suppression time.
// 2. Past that time the record loses its last known time and the gun lets go.
// 3. Manual turret, script target: without startfiring the gun aims but stays in state 0; with it the state moves on.
// 4. A threat the arcs cannot reach, in front of the gun: the gun levels first, then the gunner gives the turret up.
// 5. A hit gunner (flag 0x10) with such a threat gives the turret up at once.
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace iw2::game;
using iw2::script::Value;
using iw2::script::Vec3;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

struct Mount {
    Entity* gunner;
    Entity* gun;
};

std::vector<Mount> mounts(World& w) {
    std::vector<Mount> m;
    for (const auto& up : w.entities()) {
        Entity& e = *up;
        if (!e.isActor || e.ai.stack[0] != AiTurret || !e.usingTurret) continue;
        for (const auto& o : w.entities())
            if (o->obj == e.usingTurret && o->turret && (o->turret->flags & 0x20u)) m.push_back({&e, o.get()});
    }
    return m;
}

Vec3 ahead(Entity& gun, float yawOffset, float dist, float z) {
    const Vec3 o = gun.obj->get("origin").asVector();
    const float y = (gun.obj->get("angles").asVector()[1] + yawOffset) * float(M_PI) / 180.0f;
    return {o[0] + std::cos(y) * dist, o[1] + std::sin(y) * dist, z};
}

void test(iw2::fs::Vfs& vfs, bool hitCase) {
    World w(vfs, "decoytrenches");
    w.start();
    w.spawnAllActors();
    for (int f = 0; f < 480; ++f) w.frame();
    auto all = mounts(w);
    std::cout << "gunners " << all.size() << "\n";
    CHECK(!all.empty());
    if (all.empty()) return;
    Entity& gunner = *all[0].gunner;
    Entity& gun = *all[0].gun;
    TurretState& ts = *gun.turret;
    Entity* threat = nullptr;
    const std::string own = gunner.obj->get("team").toString();
    for (const auto& up : w.entities())
        if (up->isActor && up->obj->get("team").toString() != own && up->obj->get("team").toString() != "neutral" && up->ai.stack[0] != AiTurret) threat = up.get();
    CHECK(threat);
    if (!threat) return;
    threat->obj->set("ignoreall", Value(int32_t(1)));
    threat->obj->set("ignoreme", Value(int32_t(0)));
    for (const Mount& m : all) m.gunner->obj->set("ignoreall", Value(int32_t(1)));
    // Nobody may target the gunner's side: the map keeps fighting and the animscripts' fire notetracks fire real bullets.
    // The threat, staged next to the gun, keeps its own enemy (ignoreall is not read by the engine here) and its shots at
    // that enemy hit the gunner in between. A dead gunner leaves the turret (0x4fdc69), which then turns to the drop
    // pitch on its own (0x51d2e2), and case 4 would test nothing.
    for (const auto& up : w.entities())
        if (up->isActor && up->obj->get("team").toString() == own) up->obj->set("ignoreme", Value(int32_t(1)));
    const float groundZ = gunner.obj->get("origin").asVector()[2];
    ts.target.reset();
    ts.hitBy.reset();
    ts.flags &= ~0x10u;
    ts.flags |= 2u;
    ts.suppressionTime = 3.0f;

    auto stage = [&](Entity& g, Entity& t, float yawOffset) {
        const Vec3 p = ahead(t, yawOffset, 150.0f, yawOffset == 0.0f ? t.obj->get("origin").asVector()[2] : groundZ); // far enough not to bump the gunner, which detaches him
        threat->obj->set("origin", Value(p));
        threat->obj->set("ignoreme", Value(int32_t(0))); // the level script sets it
        g.obj->set("enemy", Value(threat->obj));
        SentientInfo& i = g.sentientInfo[threat->obj.get()];
        i.lastKnownMs = w.timeMs();
        i.lastKnownPos = p;
    };
    if (hitCase) { // 5. A hit gunner: at once, without a turn.
        ts.rightArc = -5.0f;
        ts.leftArc = 5.0f;
        ts.flags |= 0x10u;
        ts.hitBy = threat->obj;
        ts.yaw = 3.0f;
        stage(gunner, gun, 150.0f); // far outside the arc; straight behind, the map blocks the sight the rule needs
        w.frame();
        CHECK(!(ts.inUse && ts.owner == gunner.obj));
        CHECK(ts.yaw == 3.0f);
        return;
    }

    // 1. Suppression.
    gunner.obj->set("enemy", Value());
    ts.engaged = threat->obj;
    ts.flags &= ~0x40u;
    SentientInfo& info = gunner.sentientInfo[threat->obj.get()];
    const Vec3 known = ahead(gun, 0.0f, 400.0f, gun.obj->get("origin").asVector()[2]);
    info.visible = false;
    info.lastKnownMs = w.timeMs() - 100;
    info.lastKnownPos = known;
    w.frame();
    CHECK(ts.engaged == threat->obj);
    CHECK(ts.flags & 0x40u);
    CHECK(std::fabs(ts.aimPoint[0] - known[0]) < 0.01f && std::fabs(ts.aimPoint[1] - known[1]) < 0.01f);
    CHECK(std::fabs(ts.aimPoint[2] - (known[2] + 32.0f)) < 0.01f); // no previous aim point: last known z + 32

    // 2. Stale.
    info.visible = false;
    info.lastKnownMs = w.timeMs() - 3000;
    gunner.obj->set("enemy", Value());
    w.frame();
    CHECK(info.lastKnownMs == 0);
    CHECK(!ts.engaged);
    CHECK(ts.wantAngles[0] == 0.0f && ts.wantAngles[1] == 0.0f);

    // 3. Manual: flag 4 decides the firing state.
    Entity* mark = nullptr;
    for (const auto& up : w.entities())
        if (up->obj->get("classname").toString() == "script_origin" || up->obj->get("classname").toString() == "script_model") mark = up.get();
    CHECK(mark);
    if (!mark) return;
    const Value markOrigin = mark->obj->get("origin");
    const Value markSpot(ahead(gun, 0.0f, 300.0f, gun.obj->get("origin").asVector()[2]));
    ts.flags &= ~(2u | 4u);
    ts.target = mark->obj;
    ts.pitch = ts.yaw = 0.0f;
    for (int f = 0; f < 10; ++f) {
        gunner.obj->set("enemy", Value());
        mark->obj->set("origin", markSpot); // it may be falling
        w.frame();
        CHECK(ts.state == 0);
    }
    CHECK(ts.flags & 0x40u);
    ts.flags |= 4u;
    ts.yawConvergenceTime = 0.0f;
    for (int f = 0; f < 10; ++f) {
        gunner.obj->set("enemy", Value());
        mark->obj->set("origin", markSpot);
        w.frame();
    }
    CHECK(ts.state != 0);
    ts.flags &= ~4u;
    ts.flags |= 2u;
    ts.target.reset();
    mark->obj->set("origin", markOrigin);

    // 4. Threat outside the arcs, in front: level, then leave.
    ts.rightArc = 20.0f; // straight ahead is clear ground and outside these arcs
    ts.leftArc = 30.0f;
    ts.pitch = 4.0f;
    ts.yaw = 4.0f;
    ts.hitBy = threat->obj;
    ts.yawConvergenceTime = ts.pitchConvergenceTime = 2.0f; // slow tracking turn
    ts.engageStartMs = w.timeMs();
    gunner.obj->set("ignoreall", Value(int32_t(0))); // ignoreall would clear the enemy in the frame
    gunner.obj->set("favoriteenemy", Value(threat->obj));
    int frames = 0;
    bool levelling = false;
    while (ts.owner == gunner.obj && ts.inUse && frames < 60) {
        stage(gunner, gun, 0.0f);
        w.frame();
        ++frames;
        if (ts.inUse && ts.wantAngles[0] == 0.0f && ts.wantAngles[1] == 0.0f) levelling = true;
    }
    std::cout << "left the gun after " << frames << " frames, pitch " << ts.pitch << " yaw " << ts.yaw << " levelling " << levelling << " owner " << (ts.owner == gunner.obj) << " inUse " << ts.inUse << "\n";
    CHECK(frames < 60);
    CHECK(frames >= 2 || levelling);
    CHECK(std::fabs(ts.pitch) < 0.01f && std::fabs(ts.yaw) < 0.01f);
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "gunner target test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    test(vfs, false);
    test(vfs, true);
    if (failures) return 1;
    std::cout << "gunner target tests passed\n";
    return 0;
}
