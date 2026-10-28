// Vehicle use hint (0x534d40 case 0xc) and the live friendly-name dvars (0x5352e3). Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

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
    return failures;
}
