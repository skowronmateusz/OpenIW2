// Rhine (only with IW2_MAIN=<game main dir>): the fallen tree at (363, 13332, 99) is baked into the BSP as
// clip_nosight collision (contents 0x28031640, masked 0x8031640 = player/AI clip + 0x1000 + ...). Sight traces (mask
// 0x280d803, bit 0x1000) are blocked by it, bullet traces (Bullet_Fire mask 0x280e831, exe 0x5313c7) are not: the tree
// is sight cover for the AI, not bullet cover, as in the original masks. It also blocks canshoot (0x54cf40, mask
// 0x200c861: the shared bit 0x40), so the animscripts' canShootEnemy / ShootVolley gates fail behind it as well.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "rhine cover test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "rhine");
    w.start();
    const Vec3 c{363, 13332, 99};
    int sightOnly = 0, sightBlockedOnes = 0, shootBlockedOnes = 0;
    for (int dz = -60; dz <= 60; dz += 10)
        for (int dy = -150; dy <= 150; dy += 10) {
            const Vec3 a{c[0] - 300, c[1] + dy, c[2] + dz}, b{c[0] + 300, c[1] + dy, c[2] + dz};
            const TraceResult bullet = w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, Contents::BulletMask, -1, false);
            const TraceResult all = w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, 0xffffffffu, -1, false);
            if (all.fraction < bullet.fraction - 0.001f && (all.contents & 0x1000u)) {
                ++sightOnly;
                if (w.collision().sightBlocked(a, b, {0, 0, 0}, {0, 0, 0}, Contents::SightMask)) ++sightBlockedOnes;
                const TraceResult shoot = w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x200c861u, -1, false);
                if (shoot.fraction < bullet.fraction - 0.001f) ++shootBlockedOnes;
            }
        }
    std::cout << "clip_nosight-only rays " << sightOnly << ", sight-blocked " << sightBlockedOnes << ", canshoot-blocked "
              << shootBlockedOnes << "\n";
    if (sightOnly == 0 || sightBlockedOnes == 0 || shootBlockedOnes != sightOnly) {
        std::cerr << "expected clip_nosight collision that blocks sight and canshoot but not bullets at the rhine fallen tree\n";
        return 1;
    }
    return 0;
}
