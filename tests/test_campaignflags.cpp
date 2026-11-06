// objective_string's message by objective state (docs/spec/campaign.md, 0x4f18c0) and the g_reloading guard of changelevel /
// cinematic (0x4f3f2a, 0x4f4229). Needs the game files (IW2_MAIN).
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

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

int count(const std::vector<iw2::game::ClientCommand>& v, const std::string& name, const std::string& arg = {}) {
    int n = 0;
    for (const auto& c : v)
        if (c.name == name && (arg.empty() || c.arg == arg)) ++n;
    return n;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_campaignflags_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_campaignflags.gsc");
        f << "add(i, st)\n{\n\tobjective_add(i, st, \"T\");\n}\n"
             "str(i)\n{\n\tobjective_string(i, \"NEWTEXT\");\n}\n"
             "strquiet(i)\n{\n\tobjective_string_nomessage(i, \"NEWTEXT\");\n}\n"
             "fail()\n{\n\tmissionfailed();\n}\n"
             "chg()\n{\n\tchangelevel(\"moscow\", 0, 1);\n\tcinematic(\"x\", 1);\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "moscow");
    w.prepareStart();
    if (!w.player()) return 1;
    auto run = [&](const char* fn, int a, const char* st = nullptr) {
        std::vector<iw2::script::Value> args{iw2::script::Value(int32_t(a))};
        if (st) args.push_back(iw2::script::Value(std::string(st)));
        w.vm().start("maps/zz_campaignflags.gsc", fn, args, iw2::script::Value(w.player()->obj));
        w.vm().drainReady();
    };
    // active: the new text is announced; current likewise (a later frame so the 100 ms throttle is past)
    run("add", 3, "active");
    w.takeClientCommands();
    run("str", 3);
    CHECK(count(w.takeClientCommands(), "obj_update", "NEWTEXT") == 1);
    run("strquiet", 3);
    CHECK(count(w.takeClientCommands(), "obj_update") == 0);
    // done: nothing; failed: the failed message
    run("add", 4, "done");
    w.takeClientCommands();
    run("str", 4);
    {
        const auto cmds = w.takeClientCommands();
        CHECK(count(cmds, "obj_update") + count(cmds, "obj_complete") == 0);
    }
    run("add", 5, "failed");
    w.takeClientCommands();
    run("str", 5);
    CHECK(count(w.takeClientCommands(), "obj_update", "GAME_OBJECTIVEFAILED") == 1);
    // g_reloading (the death screen): changelevel and cinematic are dropped
    w.takeClientCommands();
    run("chg", 0);
    CHECK(count(w.takeClientCommands(), "scr_fade") == 2); // the level change and the cinematic request each schedule a fade
    w.vm().start("maps/zz_campaignflags.gsc", "fail", {}, iw2::script::Value(w.player()->obj));
    w.vm().drainReady();
    w.takeClientCommands();
    run("chg", 0);
    CHECK(count(w.takeClientCommands(), "scr_fade") == 0);
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    fs::remove_all(dir);
    return failures ? 1 : 0;
}
