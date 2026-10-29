// Libya tanks are hit by bullets and rockets (needs IW2_MAIN=<game main dir>): a script_vehicle has ent+0xa2 = 4
// (0x50d844), so locational traces (Bullet_Fire 0x5313db, the missile trace 0x526865) test its xmodel collision surfaces
// (0x44b840 -> 0x474440) instead of its collmap brush, which is clip_nosight (contents 0x8031640) and in no weapon mask.
// Movement traces (not locational) still clip against the collmap brush.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/pmove.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "vehicle hit test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "libya");
    w.start();
    for (int i = 0; i < 200; ++i) w.frame();
    Entity* tank = nullptr;
    for (const auto& e : w.entities())
        if (e->vehicle && !e->obj->removed && e->obj->get("model").toString() == "xmodel/vehicle_crusader2") {
            tank = e.get();
            break;
        }
    CHECK(tank != nullptr);
    if (!tank) return 1;
    const Vec3 o = tank->obj->get("origin").asVector();
    std::cout << "tank " << tank->number << " at " << o[0] << " " << o[1] << " " << o[2] << "\n";

    // The collmap clip is a movement clip: a movement-mask trace stops at it, a bullet-mask brush trace does not.
    int moveHits = 0, bulletBrushHits = 0, bulletHits = 0;
    for (float dz : {30.f, 50.f, 70.f})
        for (float dy : {-30.f, 0.f, 30.f}) {
            const Vec3 a{o[0] - 500, o[1] + dy, o[2] + dz}, b{o[0] + 500, o[1] + dy, o[2] + dz};
            if (w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x281c011u, -1).entity == tank->number) ++moveHits;
            if (w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, Contents::BulletMask, -1).entity == tank->number) ++bulletBrushHits;
            // Bullet_Fire (locational) from the player.
            Entity* h = w.debugFireBullet(a, b);
            if (h == tank) ++bulletHits;
        }
    std::cout << "movement hits " << moveHits << "/9, bullet-mask brush hits " << bulletBrushHits << ", Bullet_Fire hits " << bulletHits << "/9\n";
    CHECK(moveHits > 0);
    CHECK(bulletBrushHits == 0);
    CHECK(bulletHits > 0);

    // A walking player box (0x281c011) and a vehicle box (0x810211) stop at the hull.
    int boxHits = 0;
    for (float dy : {-40.f, 0.f, 40.f}) {
        const Vec3 a{o[0] - 200, o[1] + dy, o[2] + 40}, b{o[0] + 200, o[1] + dy, o[2] + 40};
        { const auto t = w.collision().trace(a, b, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1); std::cout << "walk dy " << dy << " frac " << t.fraction << " ent " << t.entity << " start " << t.startSolid << "\n"; if (t.entity == tank->number) ++boxHits; }
        if (w.collision().trace(a, b, {-60, -60, 0}, {60, 60, 60}, 0x810211u, -1).entity == tank->number) ++boxHits;
    }
    std::cout << "box hits " << boxHits << "/6\n";
    CHECK(boxHits == 6);

    // Walking into the hull with pmove stops outside the collmap (the player cannot be inside the tank).
    {
        PlayerState ps;
        ps.origin = {o[0] - 200, o[1], o[2] + 40};
        PlayerInput in;
        in.forward = 1;
        in.yaw = 0;
        for (int f = 0; f < 200; ++f) playerMove(w.collision(), ps, in, 1.0f / 60.0f);
        const bool inside = w.collision().trace(ps.origin, ps.origin, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1).startSolid;
        std::cout << "walked to x " << ps.origin[0] - o[0] << " (tank at 0) inside " << inside << "\n";
        CHECK(!inside);
        CHECK(ps.origin[0] < o[0]);
    }

    // A rocket flying at the hull explodes on it.
    Entity& r = w.debugSpawnRocket("panzerfaust", {o[0] - 600, o[1], o[2] + 50}, {1, 0, 0});
    for (int i = 0; i < 100 && !r.obj->removed; ++i) w.frame();
    std::cout << "rocket removed " << r.obj->removed << " at x " << r.obj->get("origin").asVector()[0] << " tank x " << o[0] << "\n";
    CHECK(r.obj->removed);
    CHECK(std::fabs(r.obj->get("origin").asVector()[0] - tank->obj->get("origin").asVector()[0]) < 250);
    // A destroyed tank (the libya death script calls freevehicle: script_vehicle_corpse, vehicle state dropped) keeps its
    // contents and brush model (0x511c80 writes neither), so walking and driving still stop at the wreck.
    {
        World w2(vfs, "libya");
        w2.start();
        for (int i = 0; i < 40; ++i) w2.frame();
        Entity* t2 = nullptr;
        for (const auto& e : w2.entities())
            if (e->vehicle && !e->obj->removed && e->obj->get("model").toString() == "xmodel/vehicle_crusader2") { t2 = e.get(); break; }
        CHECK(t2 != nullptr);
        if (!t2) return 1;
        w2.debugDamage(*t2, 100000);
        for (int i = 0; i < 100; ++i) w2.frame();
        CHECK(t2->classname == "script_vehicle_corpse");
        const Vec3 c = t2->obj->get("origin").asVector();
        int wreckHits = 0;
        for (float dy : {-30.f, 0.f, 30.f}) {
            const Vec3 a{c[0] - 300, c[1] + dy, c[2] + 40}, b{c[0] + 300, c[1] + dy, c[2] + 40};
            if (w2.collision().trace(a, b, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1).entity == t2->number) ++wreckHits; // walking
            if (w2.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x810211u, -1).entity == t2->number) ++wreckHits;         // a driven vehicle's ray
        }
        std::cout << "wreck hits " << wreckHits << "/6\n";
        CHECK(wreckHits == 6);
        PlayerState ps;
        ps.origin = {c[0] - 200, c[1], c[2] + 40};
        PlayerInput in;
        in.forward = 1;
        for (int f = 0; f < 200; ++f) playerMove(w2.collision(), ps, in, 1.0f / 60.0f);
        CHECK(!w2.collision().trace(ps.origin, ps.origin, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1).startSolid);
        CHECK(ps.origin[0] < c[0]);
    }
    std::cout << (failures ? "FAILED\n" : "vehicle hit test OK\n");
    return failures ? 1 : 0;
}
