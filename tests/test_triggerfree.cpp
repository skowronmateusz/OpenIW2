// Engine entity frees free the script object (only with IW2_MAIN=<game main dir>): G_FreeEntity (0x4d7c30) hands every
// entity's script object to 0x4670b0 (0x4d7ea7), not only on a scripted delete (docs/spec/gsc-vm.md "Freeing a script object").
// A trigger_once frees itself after it fires, so the _load.gsc doAutoSpawn thread on it (endons only in its first call) ends
// instead of waiting for "trigger" on a removed entity forever. beltot has such a trigger_once that the player can reach from
// the start (trainyard, tankhunt and duhoc_defend only use trigger_multiple there or start no doAutoSpawn on a trigger_once).
#include "fs/vfs.h"
#include "game/world.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool waitsForTrigger(const std::vector<std::string>& threads) {
    for (const auto& t : threads)
        if (t.find("doautospawn") != std::string::npos) return true;
    return false;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "trigger free test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    for (const char* map : {"beltot"}) {
        iw2::game::World w(vfs, map);
        w.start();
        for (int i = 0; i < 4; ++i) w.frame();
        for (const auto& ep : w.entities()) {
            iw2::game::Entity& e = *ep;
            if (e.obj->removed || e.classname != "trigger_once" || !waitsForTrigger(w.vm().describeThreadsOf(e.obj))) continue;
            iw2::game::Vec3 at{};
            if (!w.debugTriggerEntryPoint(e, at)) continue;
            w.setPlayerOrigin(at, {0, 0, 0});
            for (int i = 0; i < 3; ++i) w.frame();
            if (!e.obj->removed) continue; // not fired from there (e.g. a flag on the trigger)
            const auto left = w.vm().describeThreadsOf(e.obj);
            if (waitsForTrigger(left)) {
                std::cerr << map << ": doAutoSpawn still alive on the freed trigger_once:";
                for (const auto& t : left) std::cerr << " " << t;
                std::cerr << "\n";
                return 1;
            }
            std::cout << map << ": trigger_once #" << e.number << " freed, its doAutoSpawn thread ended\n";
            return 0;
        }
    }
    std::cerr << "no trigger_once with a doAutoSpawn thread fired on the test maps\n";
    return 1;
}
