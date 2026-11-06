// Script errors that the executable raises and the port reproduces (docs/spec/builtins.md, ui.md): shellshock with an unknown name or a
// duration outside 0..60 (0x4f78f4, 0x4f7969), openmenu of a menu that was not precached (0x4f89f0), precachemenu with a full table
// (0x4f89b5) and linkto cycles (0x4ed957). Needs the game files (IW2_MAIN).
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

bool hasError(const iw2::script::VM& vm, const std::string& part) {
    for (const auto& e : vm.errors())
        if (e.find(part) != std::string::npos) return true;
    return false;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_scripterrors_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_scripterrors.gsc");
        f << "pre()\n{\n\tprecacheshellshock(\"victoryscreen\");\n}\n"
             "shock(name, secs)\n{\n\tself shellshock(name, secs);\n}\n"
             "openm(name)\n{\n\tself openmenu(name);\n}\n"
             "premenu(name)\n{\n\tprecachemenu(name);\n}\n"
             "cycle()\n{\n\ta = spawn(\"script_origin\", (0, 0, 0));\n\tb = spawn(\"script_origin\", (0, 0, 0));\n"
             "\ta linkto(b);\n\tb linkto(a);\n}\n"
             "chain()\n{\n\ta = spawn(\"script_origin\", (0, 0, 0));\n\tb = spawn(\"script_origin\", (0, 0, 0));\n"
             "\ta linkto(b);\n\tself.ok = 1;\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "moscow");
    w.prepareStart();
    if (!w.player()) return 1;
    auto run = [&](const char* fn, std::vector<iw2::script::Value> args = {}) {
        w.vm().start("maps/zz_scripterrors.gsc", fn, args, iw2::script::Value(w.player()->obj));
        w.vm().drainReady();
    };
    using iw2::script::Value;
    run("pre");
    const size_t base = w.vm().errors().size();
    run("shock", {Value(std::string("victoryscreen")), Value(2.0f)});
    CHECK(w.vm().errors().size() == base); // precached, in range: no error
    run("shock", {Value(std::string("nosuchshock")), Value(2.0f)});
    CHECK(hasError(w.vm(), "shellshock 'nosuchshock' was not precached"));
    run("shock", {Value(std::string("victoryscreen")), Value(61.0f)});
    CHECK(hasError(w.vm(), "duration 61 should be >= 0 and <= 60"));
    run("shock", {Value(std::string("victoryscreen")), Value(-1.0f)});
    CHECK(hasError(w.vm(), "duration -1 should be >= 0 and <= 60"));

    run("openm", {Value(std::string("zz_nomenu"))});
    CHECK(hasError(w.vm(), "Menu 'zz_nomenu' was not precached"));
    for (int i = 0; i < 40; ++i) run("premenu", {Value(std::string("zz_menu") + std::to_string(i))});
    CHECK(hasError(w.vm(), "Too many menus precached. Max allowed menus is 32"));

    run("cycle");
    CHECK(hasError(w.vm(), "failed to link entity due to link cycle"));
    const size_t n = w.vm().errors().size();
    run("chain"); // a plain link is no cycle
    CHECK(w.vm().errors().size() == n);

    std::error_code ec;
    fs::remove_all(dir, ec);
    return failures ? 1 : 0;
}
