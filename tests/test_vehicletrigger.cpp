// Toujane opening (only with IW2_MAIN=<game main dir>): the convoy drives its path through the vehicle trigger
// "killtank" (trigger_multiple spawnflags 16, touched by script vehicles: G_TouchTriggers 0x4e7320 from the vehicle think
// 0x512c50), the script then shoots the lead tank (death notify) and stops the other tanks (unload).
#include "fs/vfs.h"
#include "game/world.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "vehicle trigger test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "toujane");
    w.start();
    bool dead = false;
    for (int i = 0; i < 40 * 20 && !dead; ++i) { // 50 ms frames, 40 s: the original shot happens about 33 s in
        w.frame();
        const auto lead = w.vm().level()->get("leadtank");
        if (lead.isObject() && lead.asObject()->get("health").isNumber() && lead.asObject()->get("health").asFloat() <= 0.0f) dead = true;
    }
    if (!dead) {
        std::cerr << "lead tank was not destroyed within 40 s (killtank trigger not fired by a vehicle?)\n";
        return 1;
    }
    std::cout << "lead tank destroyed at " << w.timeMs() << " ms\n";
    return 0;
}
