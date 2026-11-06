// Vehicle use hint (0x534d40 case 0xc) and the live friendly-name dvars (0x5352e3). Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main() {
    using namespace iw2::game;
    const char* main = std::getenv("IW2_MAIN");
    if (!main) return 0;
    int failures = 0;
    auto check = [&](bool c, int line) {
        if (!c) {
            std::cerr << "line " << line << " failed\n";
            ++failures;
        }
    };
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World world(vfs, "toujane");
    world.start();
    // A vehicle whose turret weapon has a useHintString shows it; hint index 2 either way.
    const auto flak = world.vehicleUseHint("Flak88_Turret");
    check(flak.hint == 2 && flak.text == "PLATFORM_USEFLAK88", __LINE__);
    const auto none = world.vehicleUseHint("");
    check(none.hint == 2 && none.text.empty(), __LINE__);
    const auto tank = world.vehicleUseHint("tiger_Turret");
    check(tank.hint == 2 && tank.text.empty(), __LINE__);
    // Dvars: default when unset, the live value after a change (names compare case-insensitively).
    check(world.cvarFloat("g_friendlyNameDist", 15000.0f) == 15000.0f, __LINE__);
    world.cvars["g_friendlyNameDist"] = "300";
    check(world.cvarFloat("g_friendlynamedist", 15000.0f) == 300.0f, __LINE__);
    world.cvars["g_friendlyfireDist"] = "abc";
    check(world.cvarFloat("g_friendlyfireDist", 128.0f) == 128.0f, __LINE__);
    // setlookattext (0x4f8370) names a script_model; the crosshair scan (0x5354d6..0x535581) makes it the target inside g_friendlyNameDist and
    // raises the friendly flag 0x100000 inside g_friendlyfireDist. Probe the map's script_models from 100 units away on the x axis.
    {
        world.cvars["g_friendlyNameDist"] = "15000";
        world.cvars["g_friendlyfireDist"] = "128";
        int named = 0, flagged = 0, far = 0, unnamed = 0;
        std::vector<std::pair<int, iw2::script::Vec3>> models;
        for (const auto& e : world.entities())
            if (e->classname == "script_model" && !e->obj->removed && e->obj->get("origin").isVector())
                models.push_back({e->number, e->obj->get("origin").asVector()});
        for (const auto& [num, o] : models) {
            Entity* m = world.debugEntity(num);
            World::PlayerCommand cmd;
            cmd.viewAngles = {0, 0, 0};
            auto probe = [&](float back) {
                world.setPlayerOrigin({o[0] - back, o[1], o[2] + 20 - 60}, {0, 0, 0});
                world.setPlayerCommand(cmd);
                world.frame();
                return world.crosshairTarget() == num;
            };
            m->obj->set("$lookatname", iw2::script::Value(std::string("Test Name")));
            m->obj->set("$lookatsub", iw2::script::Value(std::string("TEST_SUB")));
            if (!probe(100)) { m->obj->set("$lookatname", iw2::script::Value(std::string())); continue; }
            ++named;
            check(world.crosshairNameLine() == "Test Name" && world.crosshairWeaponLine() == "TEST_SUB", __LINE__);
            if (world.playerCrosshairFlags() & 0x100000u) ++flagged;
            world.cvars["g_friendlyNameDist"] = "50"; // too far for the name: no target
            if (!probe(100)) ++far;
            world.cvars["g_friendlyNameDist"] = "15000";
            m->obj->set("$lookatname", iw2::script::Value(std::string())); // without a name nothing is targeted
            if (!probe(100)) ++unnamed;
            if (named >= 3) break;
        }
        check(named > 0, __LINE__);
        check(far == named && unnamed == named, __LINE__);
        check(flagged == named, __LINE__); // 100 < 128: the friendly flag
    }
    return failures;
}
