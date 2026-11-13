// The SP and MP key tables of a profile (docs/spec/controls.md "Profile key tables"): a key changed in the multiplayer menus is
// kept when a multiplayer game starts or a server is joined, and is saved in the profile's config_mp.cfg, apart from the
// single-player table in config.cfg. With IW2_MAIN, also with the shipped default.cfg / default_mp.cfg.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

#include "fs/vfs.h"
#include "game/profilekeys.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2;
namespace fsys = std::filesystem;

std::string slurp(const fsys::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool has(const std::string& text, const std::string& line) { return text.find(line) != std::string::npos; }

// Shipped defaults stand-ins: the SP file has no chat or menu keys, the MP file binds T to chat and ESCAPE to togglemenu.
const std::map<std::string, std::string> kGame = {
    {"default.cfg", "unbindall\nbind W \"+forward\"\nbind S \"+back\"\nbind F12 \"screenshotJPEG\"\n"},
    {"default_mp.cfg", "unbindall\nbind W \"+forward\"\nbind S \"+back\"\nbind T \"chatmodepublic\"\nbind ESCAPE \"togglemenu\"\n"},
};
std::optional<std::string> gameFile(const std::string& n) {
    const auto it = kGame.find(n);
    return it == kGame.end() ? std::nullopt : std::optional<std::string>(it->second);
}

void synthetic(const fsys::path& dir) {
    game::ClientInput in;
    game::ProfileKeys keys;
    // A new profile: no files, the SP defaults run.
    keys.load(in, gameFile, dir, false);
    CHECK(!keys.multiplayer());
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("T").empty());
    // A key changed in the SP menus is saved in config.cfg; config_mp.cfg is not written while multiplayer never ran.
    in.rebind("+forward", "UPARROW");
    CHECK(keys.save(in, dir, "seta sensitivity \"7\"\n"));
    CHECK(has(slurp(dir / "config.cfg"), "seta sensitivity \"7\"\n") && has(slurp(dir / "config.cfg"), "bind UPARROW \"+forward\""));
    CHECK(!fsys::exists(dir / "config_mp.cfg"));

    // The multiplayer menus: the MP table (default_mp.cfg) runs. The SP change is not part of it (separate programs).
    keys.setMultiplayer(in, true);
    CHECK(keys.multiplayer());
    CHECK(in.commandFromKey("T") == "chatmodepublic" && in.commandFromKey("ESCAPE") == "togglemenu");
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("UPARROW").empty());
    // The tester's case: a key changed in the MP options before joining.
    in.rebind("+forward", ""), in.rebind("+forward", "K"); // Del clears the row, then the new key
    in.rebind("chatmodepublic", ""), in.rebind("chatmodepublic", "Y");
    // Starting the game, joining a server, and every later level load (map rotation, a new gamestate of a map_restart or a
    // round restart) enter multiplayer again: the table stays as it is, and so does a key changed in the in-game options.
    for (int level = 0; level < 3; ++level) {
        keys.setMultiplayer(in, true);
        CHECK(in.commandFromKey("K") == "+forward" && in.commandFromKey("W").empty());
        CHECK(in.commandFromKey("Y") == "chatmodepublic" && in.commandFromKey("T").empty());
        if (level == 0) in.rebind("+back", "J"); // in-game options of the first map
        CHECK(in.commandFromKey("J") == "+back");
    }
    // A profile write: config_mp.cfg gets the MP table, config.cfg keeps the SP table and the cvars.
    CHECK(keys.save(in, dir, "seta sensitivity \"7\"\n"));
    const std::string spText = slurp(dir / "config.cfg"), mpText = slurp(dir / "config_mp.cfg");
    CHECK(has(spText, "bind UPARROW \"+forward\"") && !has(spText, "bind K ") && !has(spText, "chatmodepublic"));
    CHECK(has(spText, "seta sensitivity"));
    CHECK(mpText.rfind("unbindall\n", 0) == 0 && has(mpText, "bind K \"+forward\"") && has(mpText, "bind Y \"chatmodepublic\""));
    CHECK(has(mpText, "bind J \"+back\""));
    CHECK(!has(mpText, "seta ") && !has(mpText, "UPARROW"));

    // The openiw2 key upgrade touches the SP table only, also while the MP table runs.
    keys.editSp(in, [](game::ClientInput& t) { t.rebind("+back", "DOWNARROW"); });
    CHECK(in.commandFromKey("J") == "+back" && in.commandFromKey("DOWNARROW").empty());
    CHECK(has(keys.bindings(in, false), "bind DOWNARROW \"+back\""));

    // Back to single player: the SP table again, with its change and the upgrade.
    keys.setMultiplayer(in, false);
    CHECK(in.commandFromKey("UPARROW") == "+forward" && in.commandFromKey("DOWNARROW") == "+back" && in.commandFromKey("T").empty());
    CHECK(has(keys.bindings(in, true), "bind K \"+forward\""));

    // The next start (or a profile load) in the multiplayer menus reads config_mp.cfg over default_mp.cfg.
    {
        game::ClientInput again;
        game::ProfileKeys k2;
        k2.load(again, gameFile, dir, true);
        CHECK(again.commandFromKey("K") == "+forward" && again.commandFromKey("Y") == "chatmodepublic" && again.commandFromKey("T").empty());
        CHECK(again.commandFromKey("ESCAPE") == "togglemenu");
        k2.setMultiplayer(again, false);
        CHECK(again.commandFromKey("UPARROW") == "+forward" && again.commandFromKey("K").empty());
    }
    // An SP start reads config.cfg; the MP table comes from config_mp.cfg on the switch, not from default_mp.cfg alone.
    {
        game::ClientInput again;
        game::ProfileKeys k3;
        k3.load(again, gameFile, dir, false);
        CHECK(again.commandFromKey("UPARROW") == "+forward");
        k3.setMultiplayer(again, true);
        CHECK(again.commandFromKey("K") == "+forward" && again.commandFromKey("W").empty());
    }
    // A profile load starts from the defaults (both default files begin with unbindall): keys of the previous profile go, and a
    // profile file without unbindall adds its lines to the defaults.
    {
        const fsys::path other = dir.parent_path() / "other";
        fsys::create_directories(other);
        std::ofstream(other / "config_mp.cfg") << "bind V \"chatmodeteam\"\n";
        keys.load(in, gameFile, other, true);
        CHECK(in.commandFromKey("K").empty() && in.commandFromKey("W") == "+forward" && in.commandFromKey("V") == "chatmodeteam");
        keys.setMultiplayer(in, false);
        CHECK(in.commandFromKey("UPARROW").empty() && in.commandFromKey("W") == "+forward");
    }
}

// The shipped files: the MP table has the MP keys of default_mp.cfg, the SP table those of default.cfg; a changed MP key is
// kept through the switch into a game.
void shipped(const char* main, const fsys::path& dir) {
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    const auto read = [&](const std::string& n) -> std::optional<std::string> {
        const auto f = vfs.read(n);
        return f ? std::optional<std::string>(std::string(f->begin(), f->end())) : std::nullopt;
    };
    game::ClientInput in;
    game::ProfileKeys keys;
    keys.load(in, read, dir, false);
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("T").empty()); // default.cfg: no chat key
    keys.setMultiplayer(in, true);
    CHECK(in.commandFromKey("T") == "chatmodepublic" && in.commandFromKey("X") == "openscriptmenu ingame changeweapon"); // default_mp.cfg:54, :58
    in.rebind("+forward", "I");
    keys.setMultiplayer(in, true);
    CHECK(in.commandFromKey("I") == "+forward" && in.keysFor("+forward").size() <= 2);
    CHECK(keys.save(in, dir, ""));
    CHECK(has(slurp(dir / "config_mp.cfg"), "bind I \"+forward\"") && !has(slurp(dir / "config.cfg"), "bind I "));
}

} // namespace

int main() {
    const fsys::path base = fsys::temp_directory_path() / ("iw2_profilekeys_" + std::to_string(::getpid()));
    fsys::remove_all(base);
    fsys::create_directories(base / "players" / "p");
    synthetic(base / "players" / "p");
    if (const char* main = std::getenv("IW2_MAIN")) {
        fsys::create_directories(base / "players" / "shipped");
        shipped(main, base / "players" / "shipped");
    }
    fsys::remove_all(base);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
