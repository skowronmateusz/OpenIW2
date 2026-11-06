// animscripted deathplant ground drop (0x53b190): a box trace from 36 above to 18 below the origin (needs IW2_MAIN=<game main dir>).
#include "fs/vfs.h"
#include "game/animscripted.h"
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
        std::cout << "deathplant test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "moscow");
    w.start();
    for (int i = 0; i < 200; ++i) w.frame(); // the player settles on the ground
    Entity* player = nullptr;
    for (const auto& e : w.entities())
        if (e->isPlayer) player = e.get();
    CHECK(player != nullptr);
    if (!player) return 1;
    const Vec3 o = player->obj->get("origin").asVector();
    const CollisionWorld& cw = w.collision();
    const int pass = player->number;
    // The floor under the player: where a long box trace down stops (the player may hover a few units above it while settling).
    const TraceResult ft = cw.trace({o[0], o[1], o[2] + 10.0f}, {o[0], o[1], o[2] - 60.0f}, {-15, -15, 0}, {15, 15, 72}, 0x820011u, pass);
    CHECK(!ft.startSolid && ft.fraction < 1.0f);
    const float floorZ = o[2] + 10.0f - 70.0f * ft.fraction;

    // Floating 10 above the floor: dropped onto it.
    const Vec3 a = deathPlantOrigin(cw, {o[0], o[1], floorZ + 10.0f}, pass);
    CHECK(std::fabs(a[2] - floorZ) < 0.5f);
    CHECK(a[0] == o[0] && a[1] == o[1]);
    // The per-frame plant: lift follows the floor, and a miss changes nothing.
    std::array<float, 3> pf{o[0], o[1], floorZ + 10.0f};
    float lift = 0;
    deathPlantFrame(cw, pf, lift, pass);
    CHECK(std::fabs(pf[2] - floorZ) < 0.5f && std::fabs(lift + 10.0f) < 0.5f);
    std::array<float, 3> high{o[0], o[1], floorZ + 40.0f};
    float none = 0;
    deathPlantFrame(cw, high, none, pass);
    CHECK(high[2] == floorZ + 40.0f && none == 0);
    // Out of reach (the trace ends 22 above the floor): unchanged.
    const Vec3 b = deathPlantOrigin(cw, {o[0], o[1], floorZ + 40.0f}, pass);
    CHECK(b[2] == floorZ + 40.0f);
    // 17 above the floor is still inside the range (the trace ends 1 below it).
    const Vec3 c = deathPlantOrigin(cw, {o[0], o[1], floorZ + 17.0f}, pass);
    CHECK(std::fabs(c[2] - floorZ) < 0.5f);
    std::cout << "deathplant: floor " << floorZ << ", drop from +10 -> " << a[2] << "\n";
    return failures ? 1 : 0;
}
