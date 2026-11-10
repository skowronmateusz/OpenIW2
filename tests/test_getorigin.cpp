// getorigin on brush-model entities (0x4ee250): the centre of the linked world box, not the "origin" key. On eldaba the objective
// for the German headquarters documents is placed with getorigin of the (origin-less) "documents" trigger_use. Needs the game
// files (IW2_MAIN).
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_getorigin_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_getorigin.gsc");
        f << "trig()\n{\n\te = getent(\"documents\", \"targetname\");\n\tobjective_add(5, \"active\", \"T\", e getorigin());\n}\n"
             "plain()\n{\n\tobjective_add(6, \"active\", \"T\", self getorigin());\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "eldaba");
    w.prepareStart();
    if (!w.player()) return 1;
    auto run = [&](const char* fn) {
        w.vm().start("maps/zz_getorigin.gsc", fn, {}, iw2::script::Value(w.player()->obj));
        w.vm().drainReady();
    };
    run("trig");
    const auto p = w.objectives()[5].positions[0];
    // the trigger has no origin key; its centre is somewhere in the headquarters, never the zero vector
    CHECK(std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) > 1.0f);
    // an entity without the brush-model flag answers its own origin
    run("plain");
    const auto o = w.player()->obj->get("origin").asVector();
    const auto q = w.objectives()[6].positions[0];
    CHECK(q[0] == o[0] && q[1] == o[1] && q[2] == o[2]);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
