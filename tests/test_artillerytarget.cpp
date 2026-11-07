// The decoytown artillery request (needs IW2_MAIN=<game main dir> for the level part). maps/_artillery.gsc:386-399 traces with
// bullettrace from the player's eye along the view and refuses the target (the radio man's "give me a real target" line,
// _artillery.gsc:187-192) when the trace misses or its surfacetype is "default". bullettrace (0x4f2230) names the surface type
// of the hit material (surfaceFlags >> 20, table 0x5a0344); a fixed "default" made every request fail.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/playerevents.h"
#include "game/world.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    CHECK(!std::strcmp(bulletTraceSurfaceType(1.0f, 6u << 20), "none"));     // a miss
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.5f, 0), "default"));         // type 0
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.5f, 6u << 20), "dirt"));
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.5f, 1u << 20), "bark"));
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.5f, 22u << 20), "asphalt"));
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.5f, 23u << 20), "default")); // past the table
    CHECK(!std::strcmp(bulletTraceSurfaceType(0.0f, (10u << 20) | 0x4), "grass")); // other flag bits ignored
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << (failures ? "FAILED\n" : "artillery target test: names OK, level part skipped (IW2_MAIN not set)\n");
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytown");
    w.start();
    w.frame();
    // The ground under the player is a valid target: a bullet-mask trace straight down hits a typed surface.
    const auto o = w.player()->obj->get("origin").asVector();
    const TraceResult tr = w.collision().trace({o[0], o[1], o[2] + 64}, {o[0], o[1], o[2] - 4096}, {0, 0, 0}, {0, 0, 0},
                                               Contents::BulletMaskNoCharacters, w.player()->number);
    const char* type = bulletTraceSurfaceType(tr.fraction, tr.surfaceFlags);
    std::cout << "ground under the player: fraction " << tr.fraction << " surface " << type << "\n";
    CHECK(tr.fraction < 1.0f);
    CHECK(std::strcmp(type, "default") != 0 && std::strcmp(type, "none") != 0);
    std::cout << (failures ? "FAILED\n" : "artillery target test OK\n");
    return failures ? 1 : 0;
}
