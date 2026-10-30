// hill400_assault (only with IW2_MAIN): misc_model hedgehogs are static models. Point traces with the bullet mask hit their xmodel
// collision surfaces (contents 1, surface type 13 from surface flags 0xd00000), sight traces are blocked by them, and a box trace
// (player movement) does not see them.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "static model test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "hill400_assault");
    if (w.collision().staticModelCount() < 100) {
        std::cerr << "expected the map's misc_model entities as static models\n";
        return 1;
    }
    const Vec c{-11427, 3974, 10399};
    int bulletHits = 0, sightBlocked = 0, boxHits = 0;
    for (int dz = 20; dz <= 60; dz += 10)
        for (int dy = -20; dy <= 20; dy += 10) {
            const Vec a{c[0] - 200, c[1] + dy, c[2] + dz}, b{c[0] + 200, c[1] + dy, c[2] + dz};
            const TraceResult t = w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, Contents::BulletMask, -1, false);
            if (t.fraction < 1 && t.contents == 1 && ((t.surfaceFlags >> 20) & 0x1f) == 13 && t.entity == -1) ++bulletHits;
            if (t.fraction < 1 && ((t.surfaceFlags >> 20) & 0x1f) == 13 &&
                w.collision().sightBlocked(a, b, {0, 0, 0}, {0, 0, 0}, Contents::SightMask))
                ++sightBlocked;
            const TraceResult bx = w.collision().trace(a, b, {-4, -4, -4}, {4, 4, 4}, 0x1, -1, false);
            if (bx.fraction < 1 && bx.surfaceFlags >> 20 == 0xd && bx.material == -1) ++boxHits;
        }
    std::cout << "bullet hits " << bulletHits << ", sight blocked " << sightBlocked << ", box " << boxHits << "\n";
    if (bulletHits == 0 || sightBlocked == 0 || boxHits != 0) {
        std::cerr << "expected hedgehog hits for point traces only\n";
        return 1;
    }
    return 0;
}
