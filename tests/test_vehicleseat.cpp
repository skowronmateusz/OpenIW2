// The driver's eye in a ridden tank (needs IW2_MAIN=<game main dir>). The driver is linked to tag_player through the
// generic link (0x4d6540 stores ent+0x1c8), so ClientEndFrame (0x4e7fd6) runs 0x4d6ec0 on it: eye = tag_player origin +
// tag up * view height (60 in a vehicle), origin = eye - (0, 0, 60). The old code put the eye at tag + (0, 0, 60), which on a
// tilted hull (88ridge settles at ~16 degrees of roll) moved the camera by ~17 units.
#include "fs/vfs.h"
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

static float dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "vehicle seat test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    for (const char* map : {"88ridge", "libya"}) {
        World w(vfs, map);
        w.start();
        Vec3 tag{}, up{}, eye{};
        bool riding = false;
        for (int i = 0; i < 400 && !riding; ++i) {
            w.frame();
            riding = w.playerVehicleSeat(tag, up, eye);
        }
        std::cout << map << ": riding " << riding << "\n";
        CHECK(riding);
        if (!riding) continue;
        float maxTilt = 0.0f;
        for (int i = 0; i < 160; ++i) { // 8 s: the hull settles on the ground and the turret swings
            w.frame();
            CHECK(w.playerVehicleSeat(tag, up, eye));
            const Vec3 want{tag[0] + up[0] * 60.0f, tag[1] + up[1] * 60.0f, tag[2] + up[2] * 60.0f};
            const float err = dist(eye, want);
            if (err > 0.5f) {
                std::cout << "frame " << i << ": eye " << eye[0] << " " << eye[1] << " " << eye[2] << " want " << want[0] << " " << want[1]
                          << " " << want[2] << "\n";
                CHECK(err <= 0.5f);
                break;
            }
            const float flat = dist(eye, Vec3{tag[0], tag[1], tag[2] + 60.0f});
            if (flat > maxTilt) maxTilt = flat;
        }
        std::cout << map << ": eye differs from tag + (0,0,60) by up to " << maxTilt << " units\n";
    }
    std::cout << (failures ? "FAILED\n" : "vehicle seat test OK\n");
    return failures ? 1 : 0;
}
