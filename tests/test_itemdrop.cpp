// Item drop (G_RunItem 0x523040, World::runItems), only with IW2_MAIN=<game main dir>: weapon items dropped on toujane
// settle on the floor. A straight drop comes to rest at the floor point of a downward 0x491 box trace, one frame after the
// first hit (the re-based 1-unit probe from 0.125 above the floor ends at once); an item thrown at a wall does not stick to it
// but falls and rests on the floor below.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/world.h"
#include "script/vm.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;
using iw2::script::Value;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

struct Run {
    int framesToSettle = -1;
    int hitFrame = -1; // first frame that ended with trType 3 (a hit)
    bool removed = false;
    Vec3 at{};
};

static Run runItem(World& w, Entity& item, int maxFrames) {
    Run r;
    for (int i = 1; i <= maxFrames; ++i) {
        w.frame();
        if (item.obj->removed) { r.removed = true; return r; }
        const int type = item.obj->get("$trtype").asInt();
        if (type == 3 && r.hitFrame < 0) r.hitFrame = i;
        if (type == 0) {
            r.framesToSettle = i;
            r.at = item.obj->get("origin").asVector();
            return r;
        }
    }
    r.at = item.obj->get("origin").asVector();
    return r;
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "item drop test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    const Vec3 p = w.player()->obj->get("origin").asVector();
    const Vec3 mins{-1, -1, -1}, maxs{1, 1, 1};

    // 1. Straight drop from 40 units above the player's origin.
    {
        const Vec3 start{p[0], p[1], p[2] + 40.0f};
        const TraceResult down = w.collision().trace(start, {start[0], start[1], start[2] - 4096.0f}, mins, maxs, 0x491);
        Entity& item = w.debugDropItem("kar98k", start, {0, 0, 0});
        const Run r = runItem(w, item, 100);
        std::cout << "straight drop: settled after " << r.framesToSettle << " frames (first hit " << r.hitFrame << ") at " << r.at[0]
                  << " " << r.at[1] << " " << r.at[2] << ", floor trace z " << down.endPos[2] << "\n";
        CHECK(!r.removed);
        CHECK(r.framesToSettle > 0);
        CHECK(r.hitFrame > 0 && r.framesToSettle == r.hitFrame + 1);
        CHECK(std::fabs(r.at[0] - start[0]) < 1e-3f && std::fabs(r.at[1] - start[1]) < 1e-3f);
        CHECK(std::fabs(r.at[2] - down.endPos[2]) < 0.05f);
        CHECK(item.obj->get("$groundent").asInt() != World::kItemNoGround);
        // Resting items stay put.
        for (int i = 0; i < 20; ++i) w.frame();
        CHECK(!item.obj->removed && item.obj->get("origin").asVector()[2] == r.at[2]);
    }

    // 1b. A script-spawned item (maps/_spawner.gsc spawn_grenade_bag: spawn("weapon_Stielhandgranate", origin) 44 units above
    // a corpse, then setmodel / angles) falls under G_RunItem and rests on the floor.
    {
        const Vec3 start{p[0] + 24.0f, p[1], p[2] + 44.0f};
        const TraceResult down = w.collision().trace(start, {start[0], start[1], start[2] - 4096.0f}, mins, maxs, 0x491);
        w.vm().start("maps/_spawner", "spawn_grenade_bag", {Value(start), Value(Vec3{0, 30, 0}), Value("axis")});
        w.vm().drainReady();
        Entity* bag = nullptr;
        for (const auto& ep : w.entities())
            if (!ep->obj->removed && ep->obj->get("model").toString() == "xmodel/german_grenade_bag") bag = ep.get();
        CHECK(bag != nullptr);
        if (bag) {
            int settled = -1;
            for (int i = 1; i <= 40 && settled < 0; ++i) {
                w.frame();
                if (bag->obj->get("$trtype").isNumber() && bag->obj->get("$trtype").asInt() == 0 && bag->obj->get("$groundent").isNumber() &&
                    bag->obj->get("$groundent").asInt() != World::kItemNoGround)
                    settled = i;
            }
            const Vec3 at = bag->obj->get("origin").asVector();
            const Vec3 ang = bag->obj->get("angles").asVector();
            std::cout << "grenade bag (" << bag->classname << "): settled after " << settled << " frames at z " << at[2] << " (floor " << down.endPos[2]
                      << "), angles " << ang[0] << " " << ang[1] << " " << ang[2] << "\n";
            CHECK(settled > 3); // it falls 44 units under gravity (script spawn, handler 0x18), not the instant 4096 drop of a map item
            CHECK(std::fabs(at[2] - down.endPos[2]) < 0.05f);
            CHECK(ang[2] == 90.0f);
        }
    }

    // 2. Thrown at the nearest wall in one of 8 directions at 40 units up: it must end on the floor, not on the wall.
    {
        const Vec3 start{p[0], p[1], p[2] + 40.0f};
        float best = 1e9f;
        Vec3 dir{};
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) * float(M_PI) / 4.0f;
            const Vec3 d{std::cos(a), std::sin(a), 0};
            const TraceResult t = w.collision().trace(start, {start[0] + d[0] * 300, start[1] + d[1] * 300, start[2]}, mins, maxs, 0x491);
            if (t.fraction < 1.0f && std::fabs(t.normal[2]) < 0.3f && t.fraction * 300 < best && t.fraction * 300 > 20) {
                best = t.fraction * 300;
                dir = d;
            }
        }
        if (best < 1e8f) {
            Entity& item = w.debugDropItem("mp40", start, {dir[0] * 400.0f, dir[1] * 400.0f, 0});
            const Run r = runItem(w, item, 200);
            std::cout << "wall throw (wall " << best << " away): settled after " << r.framesToSettle << " frames at " << r.at[0] << " "
                      << r.at[1] << " " << r.at[2] << "\n";
            CHECK(!r.removed);
            CHECK(r.framesToSettle > 0);
            // Resting on something: a 2-unit box probe straight down ends almost at once.
            const TraceResult probe = w.collision().trace(r.at, {r.at[0], r.at[1], r.at[2] - 2.0f}, mins, maxs, 0x491);
            CHECK(probe.fraction < 0.1f);
            CHECK(item.obj->get("angles").asVector()[2] == 90.0f);
            // 3. Thrown at the same wall from 10 units in front of it: the hit (fraction >= 0.01) does not settle; the re-base drops
            // the item with zero velocity, so it ends at the foot of the wall below the hit point.
            const Vec3 near{start[0] + dir[0] * (best - 10), start[1] + dir[1] * (best - 10), start[2]};
            Entity& item2 = w.debugDropItem("mp40", near, {dir[0] * 400.0f, dir[1] * 400.0f, 0});
            const Run r2 = runItem(w, item2, 200);
            const float moved = (r2.at[0] - near[0]) * dir[0] + (r2.at[1] - near[1]) * dir[1];
            std::cout << "wall hit from 10 units: first hit " << r2.hitFrame << ", settled after " << r2.framesToSettle << " at " << r2.at[0]
                      << " " << r2.at[1] << " " << r2.at[2] << " (moved " << moved << " towards the wall)\n";
            CHECK(!r2.removed && r2.framesToSettle > r2.hitFrame + 1);
            CHECK(moved > 5.0f && moved < 10.5f);
            CHECK(r2.at[2] < near[2] - 5.0f);
        } else {
            std::cout << "no wall near the player start; wall throw skipped\n";
        }
    }
    // 4. Rocket destabilization (0x526960): a panzerfaust rocket (destabilizeDistance 1000, projectileSpeed 2800: +0x168 = 357 ms
    // after trTime = launch - 50 ms) turns by at most destabilizationAngleMax (10 deg) once, keeps its speed, and the next
    // interval is destabilizationBaseTime * 1000 = 100 ms. The offset tan(10 deg) * (c1, c2, c3), |c| <= 1, turns it by at most
    // atan(tan(10 deg) * sqrt(3)) = 17 deg.
    {
        const Vec3 start{p[0], p[1], p[2] + 60.0f};
        const float s = 1.0f / std::sqrt(1.25f);
        const Vec3 dir{s, 0, 0.5f * s};
        Entity& r = w.debugSpawnRocket("panzerfaust", start, dir);
        const int64_t t0 = w.timeMs();
        CHECK(r.rocket && std::fabs(r.rocket->destabMs - 1000.0f / 2800.0f * 1000.0f) < 0.01f);
        int64_t at = -1;
        for (int i = 0; i < 12 && !r.obj->removed; ++i) {
            w.frame();
            if (r.obj->removed) break;
            if (r.rocket->destabilized && at < 0) {
                at = w.timeMs();
                const Vec3 d = r.rocket->trDelta;
                const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const float cosA = (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / l;
                std::cout << "rocket destabilized at +" << (at - t0) << " ms, turned " << std::acos(std::min(1.0f, cosA)) * 180.0f / float(M_PI)
                          << " deg, speed " << l << ", next " << r.rocket->destabMs << " ms\n";
                CHECK(std::fabs(l - 2800.0f) < 0.5f);
                CHECK(cosA >= std::cos(17.5f * float(M_PI) / 180.0f) && cosA < 1.0f);
                CHECK(r.rocket->trTime == at);
                CHECK(std::fabs(r.rocket->destabMs - 100.0f) < 0.01f);
            }
        }
        // The first frame whose level time is past trTime + 357 = t0 - 50 + 357.
        CHECK(at > 0 && at > t0 + 307 && at <= t0 + 357);
        // A shooter with setstablemissile(true) (flag 0x20000, copied by G_FireRocket) fires rockets that never destabilize.
        w.player()->obj->set("$stablemissile", Value(true));
        Entity& st = w.debugSpawnRocket("panzerfaust", start, dir);
        const Vec3 d0 = st.rocket->trDelta;
        for (int i = 0; i < 12 && !st.obj->removed; ++i) w.frame();
        if (!st.obj->removed) CHECK(!st.rocket->destabilized && st.rocket->trDelta == d0);
        w.player()->obj->set("$stablemissile", Value(false));
    }
    // 5. Missile water entry (0x526b40): a grenade falling (trDelta.z = -200) onto a water surface near the rhine player start
    // sends the splash (event 0xb8) once and goes on below the surface.
    {
        iw2::fs::Vfs v2;
        v2.mountDirectory(main);
        World r(v2, "rhine");
        r.start();
        for (int i = 0; i < 3; ++i) r.frame();
        const Vec3 q = r.player()->obj->get("origin").asVector();
        bool found = false;
        Vec3 water{};
        for (int dx = -30; dx <= 30 && !found; ++dx)
            for (int dy = -30; dy <= 30 && !found; ++dy) {
                const Vec3 top{q[0] + dx * 100.0f, q[1] + dy * 100.0f, q[2] + 400.0f};
                if (r.collision().trace(top, top, {0, 0, 0}, {0, 0, 0}, 0x21).startSolid) continue;
                const TraceResult t = r.collision().trace(top, {top[0], top[1], top[2] - 3000.0f}, {0, 0, 0}, {0, 0, 0}, 0x21);
                if (t.fraction < 1.0f && (t.surfaceFlags & 0x1f00000u) == 0x1400000u) {
                    water = {t.endPos[0], t.endPos[1], t.endPos[2]};
                    found = true;
                }
            }
        CHECK(found);
        if (found) {
            r.debugSpawnGrenade("fraggrenade", {water[0], water[1], water[2] + 60.0f}, {0, 0, -200.0f}, 5000);
            int splashFrame = -1;
            for (int i = 1; i <= 20; ++i) {
                r.frame();
                if (splashFrame < 0 && r.stats.waterSplashes > 0) splashFrame = i;
            }
            std::cout << "water at " << water[0] << " " << water[1] << " " << water[2] << ": splash at frame " << splashFrame << ", splashes "
                      << r.stats.waterSplashes << "\n";
            CHECK(splashFrame > 0 && r.stats.waterSplashes == 1);
        }
    }
    return failures ? 1 : 0;
}
