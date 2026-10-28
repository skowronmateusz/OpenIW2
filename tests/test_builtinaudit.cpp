// Script built-ins that were no-op stubs: attackbuttonpressed / usebuttonpressed / getplayerangles /
// getnormalizedmovement (player input queries, exe 0x538600..0x538820), getammocount (0x4f4540), setspawnerteam
// (0x4f8750) and playlooprumble (0x4f8c00). Needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "iw2_builtinaudit";
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_builtinaudit.gsc");
        f << "run()\n{\n"
             "\tlevel.attack = self attackbuttonpressed();\n"
             "\tlevel.use = self usebuttonpressed();\n"
             "\tlevel.angles = self getplayerangles();\n"
             "\tlevel.move = self getnormalizedmovement();\n"
             "\tlevel.ammonone = self getammocount(\"none\");\n"
             "\tself playlooprumble(\"x\");\n"
             "\tlevel.done = 1;\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "moscow");
    for (const char* n : {"attackbuttonpressed", "usebuttonpressed", "getplayerangles", "getnormalizedmovement", "getammocount",
                          "setspawnerteam", "playlooprumble"})
        CHECK(!w.stubbedBuiltins().count(n));
    w.prepareStart();
    CHECK(w.player() != nullptr);
    if (!w.player()) return 1;
    iw2::game::World::PlayerCommand cmd;
    cmd.forward = 127;
    cmd.right = -127;
    cmd.buttons = 0x20; // +usereload
    cmd.viewAngles = {10, 90, 0};
    w.setPlayerCommand(cmd);
    w.vm().start("maps/zz_builtinaudit.gsc", "run", {}, iw2::script::Value(w.player()->obj));
    w.vm().drainReady();
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    auto* lv = w.vm().level().get();
    CHECK(lv->get("done").isInt());
    CHECK(lv->get("attack").asInt() == 0);
    CHECK(lv->get("use").asInt() == 1);
    const auto a = lv->get("angles").asVector();
    CHECK(a[0] == 10 && a[1] == 90 && a[2] == 0);
    const auto m = lv->get("move").asVector();
    CHECK(std::fabs(m[0] - 1.0f) < 1e-5f && std::fabs(m[1] + 1.0f) < 1e-5f && m[2] == 0);
    CHECK(lv->get("ammonone").asInt() == 0);
    // The previous command's buttons count as well (the exe ORs client+0x8640 with +0x8634).
    cmd.buttons = 1;
    w.setPlayerCommand(cmd);
    w.vm().start("maps/zz_builtinaudit.gsc", "run", {}, iw2::script::Value(w.player()->obj));
    w.vm().drainReady();
    CHECK(lv->get("attack").asInt() == 1);
    CHECK(lv->get("use").asInt() == 1);
    // tankhunt flags its grenade trigger with enablegrenadetouchdamage; the class check must accept it (no script error).
    {
        iw2::game::World t(vfs, "tankhunt");
        t.start();
        for (int i = 0; i < 40; ++i) t.frame();
        for (const auto& e : t.vm().errors()) CHECK(e.find("supported on damage") == std::string::npos);
        CHECK(!t.stubbedBuiltins().count("enablegrenadetouchdamage") && !t.stubbedBuiltins().count("getgroundenttype"));
    }
    fs::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "builtinaudit ok\n";
    return failures ? 1 : 0;
}
