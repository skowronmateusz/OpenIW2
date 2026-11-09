// The driver's eye in a ridden tank (needs IW2_MAIN=<game main dir>). The driver is linked to tag_player through the
// generic link (0x4d6540 stores ent+0x1c8), so ClientEndFrame (0x4e7fd6) runs 0x4d6ec0 on it: eye = tag_player origin +
// tag up * view height (60 in a vehicle), origin = eye - (0, 0, 60). The old code put the eye at tag + (0, 0, 60), which on a
// tilted hull (88ridge settles at ~16 degrees of roll) moved the camera by ~17 units.
#include "game/vehicledrive.h"
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
        float bodyYaw = 0.0f, barrelYaw = 0.0f;
        bool hasBarrel = false;
        if (w.playerVehicleHudYaws(bodyYaw, barrelYaw, hasBarrel) && hasBarrel) {
            const Entity* tank = nullptr;
            for (const auto& up : w.entities())
                if (up->vehicle && up->vehicle->info && up->vehicle->info->type == 1 && w.player() && w.player()->usingVehicle == up->number) tank = up.get();
            CHECK(tank);
            if (tank) { // the barrel tag points along the turret: the HUD yaw is the hull yaw plus the turret yaw
                float d = std::fmod(std::fabs(barrelYaw - bodyYaw - tank->vehicle->turretYaw), 360.0f);
                if (d > 180.0f) d = 360.0f - d;
                std::cout << map << ": hud barrel off turret by " << d << "\n";
                CHECK(d < 1.0f);
            }
        }
        // A vehicle linked to the player at spawn gets the small body box of the first think (0x5128f0): radius 3, wheel samples within radius 1.
        for (const auto& ent : w.entities())
            if (ent->vehicle && w.player() && w.player()->usingVehicle == ent->number) {
                CHECK(ent->vehicle->bodySet);
                CHECK(std::fabs(ent->vehicle->bodyRadius - 3.0f) < 1e-3f);
                // Driving at a wall: the body box of radius 3 stops 3 units short of it, a point would reach it.
                const Vec3 c = ent->obj->get("origin").asVector();
                bool tested = false;
                for (int dx = -40; dx <= 40 && !tested; ++dx)
                    for (int dy = -40; dy <= 40 && !tested; ++dy) {
                        const Vec3 s{c[0] + float(dx) * 128.0f, c[1] + float(dy) * 128.0f, c[2] + 60.0f}, e2{s[0] + 200.0f, s[1], s[2]};
                        const auto tr = w.collision().trace(s, e2, {0, 0, 0}, {0, 0, 0}, 0x810211u, ent->number);
                        if (tr.startSolid || tr.fraction >= 1.0f || tr.normal[0] > -0.99f || tr.fraction * 200.0f < 40.0f) continue;
                        const float wallX = s[0] + tr.fraction * 200.0f;
                        auto drive = [&](float r) {
                            DriveBody b;
                            b.origin = {wallX - 30.0f, s[1], s[2]};
                            b.velocity = {600.0f, 0, 0};
                            DriveTrace dt;
                            dt.passEntity = ent->number;
                            dt.mins = {-r, -r, 0};
                            dt.maxs = {r, r, 2.0f * r};
                            for (int f = 0; f < 6; ++f) { b.velocity[0] = 600.0f; driveMove(w.collision(), b, {}, {}, dt); }
                            return wallX - b.origin[0];
                        };
                        tested = true;
                        CHECK(drive(0.0f) < 1.0f);
                        CHECK(std::fabs(drive(3.0f) - 3.0f) < 1.0f);
                    }
                CHECK(tested);
            }
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
