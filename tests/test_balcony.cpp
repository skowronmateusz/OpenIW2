// Balcony death (animscripts/death.gsc -> predict.gsc stumbleWall / tumbleWall): kills the standing actors with script_balcony set
// on the maps that have them and requires that the scripts run without errors (needs IW2_MAIN).
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace iw2::game;

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "balcony test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_balcony_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/baltest.gsc") << "main()\n{\n\tself dodamage(100000, self.origin);\n}\n";
    int failures = 0, killed = 0;
    for (const char* map : {"tankhunt", "eldaba", "matmata", "toujane_ride"}) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        vfs.mountDirectory(dir.string());
        World w(vfs, map);
        w.start();
        for (int i = 0; i < 100; ++i) w.frame();
        w.spawnAllActors();
        for (int i = 0; i < 40; ++i) w.frame();
        std::vector<Entity*> targets;
        for (const auto& e : w.entities())
            if (e->isActor && !e->obj->removed && e->obj->get("script_balcony").isDefined()) targets.push_back(e.get());
        const size_t before = w.vm().errors().size();
        for (Entity* e : targets) {
            // One at a time: the prediction counter is global (0x1c6104c) and tumbleWall waits between begin and end.
            w.vm().start("maps/baltest", "main", {}, iw2::script::Value(e->obj));
            ++killed;
            for (int i = 0; i < 100; ++i) w.frame();
        }
        for (int i = 0; i < 400; ++i) w.frame();
        const size_t after = w.vm().errors().size();
        std::cout << map << ": balcony actors " << targets.size() << ", new script errors " << (after - before) << "\n";
        for (size_t i = before; i < after; ++i) {
            const std::string& err = w.vm().errors()[i];
            std::cout << "  " << err << "\n";
            if (err.find("tag_flash") == std::string::npos) ++failures; // the eldaba tag_flash error is a known, separate issue
        }
    }
    std::filesystem::remove_all(dir);
    std::cout << "killed " << killed << "\n";
    return failures ? 1 : 0;
}
