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
    return failures ? 1 : 0;
}
