// hill400_assault (only with IW2_MAIN): misc_model hedgehogs are static models. Point traces with the bullet mask hit their xmodel
// collision surfaces (contents 1, surface type 13 from surface flags 0xd00000), sight traces are blocked by them, and a box trace
// (player movement) does not see them.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/world.h"

#include <cmath>
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
    // The use scan's line test is the sight trace (0x44c1b0): a point sight trace has no 0.125 surface margin, while the
    // clip trace blocks a target within 0.125 of a face. Over random segments around the hedgehogs, every segment the clip
    // trace blocks but the sight trace does not is such a margin case: pushing its end 0.3 into the face blocks the sight too.
    {
        int clipOnly = 0, marginCases = 0;
        unsigned seed = 7;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1 << 24); };
        for (int i = 0; i < 20000; ++i) {
            const Vec a{c[0] + (rnd() - 0.5f) * 160, c[1] + (rnd() - 0.5f) * 160, c[2] + rnd() * 80};
            const Vec b{c[0] + (rnd() - 0.5f) * 160, c[1] + (rnd() - 0.5f) * 160, c[2] + rnd() * 80};
            const TraceResult t = w.collision().trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x11, -1, false);
            if (t.fraction >= 1.0f || t.startSolid || w.collision().sightBlocked(a, b, {0, 0, 0}, {0, 0, 0}, 0x11, -1, 0x7ff)) continue;
            // A grazing segment (almost parallel to the face) needs more than 0.3 of push to cross it; those are left out.
            const Vec d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            if (std::fabs(d[0] * t.normal[0] + d[1] * t.normal[1] + d[2] * t.normal[2]) < 0.2f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])) continue;
            ++clipOnly;
            const Vec deeper{b[0] - 0.3f * t.normal[0], b[1] - 0.3f * t.normal[1], b[2] - 0.3f * t.normal[2]};
            if (w.collision().sightBlocked(a, deeper, {0, 0, 0}, {0, 0, 0}, 0x11, -1, 0x7ff)) ++marginCases;
        }
        std::cout << "clip-only blocks " << clipOnly << ", margin cases " << marginCases << "\n";
        if (clipOnly == 0 || marginCases != clipOnly) {
            std::cerr << "expected the clip trace's surface margin to be the only difference from the sight line\n";
            return 1;
        }
    }
    return 0;
}
