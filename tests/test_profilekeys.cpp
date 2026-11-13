// The one key table of a profile for single player and multiplayer (docs/spec/controls.md "Profile key tables", an openiw2
// deviation): unified defaults (default.cfg plus the multiplayer-only binds of default_mp.cfg), the profile's config.cfg over
// them, the controls reset, and the one-time merge of profiles written with two tables (config_mp.cfg). With IW2_MAIN, also
// with the shipped files. With the path of apps/iw2view/main.cpp as argument: no level load rebuilds the table from a default file.
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

// Stand-ins for the shipped files: default_mp.cfg is default.cfg plus chat keys, a weapon-menu key and a second scoreboard key.
const std::map<std::string, std::string> kGame = {
    {"default.cfg", "unbindall\nbind W \"+forward\"\nbind S \"+back\"\nbind TAB \"+scores\"\nbind ESCAPE \"togglemenu\"\n"},
    {"default_mp.cfg", "unbindall\nbind W \"+forward\"\nbind S \"+back\"\nbind TAB \"+scores\"\nbind ESCAPE \"togglemenu\"\n"
                       "bind T \"chatmodepublic\"\nbind Y \"chatmodeteam\"\nbind X \"openscriptmenu ingame changeweapon\"\n"
                       "bind F4 \"+scores\"\nset g_gametype random\n"},
};
std::optional<std::string> gameFile(const std::string& n) {
    const auto it = kGame.find(n);
    return it == kGame.end() ? std::nullopt : std::optional<std::string>(it->second);
}

void synthetic(const fsys::path& players) {
    game::ProfileKeys keys;
    keys.loadDefaults(gameFile);
    CHECK(keys.mpOnly().size() == 4); // T, Y, X, F4 (F4 a second key of +scores)

    // A new profile: one table with the single-player keys and the multiplayer-only ones.
    const fsys::path dir = players / "p";
    fsys::create_directories(dir);
    game::ClientInput in;
    keys.load(in, dir);
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("T") == "chatmodepublic" && in.commandFromKey("Y") == "chatmodeteam");
    CHECK(in.commandFromKey("X") == "openscriptmenu ingame changeweapon");
    CHECK(in.keysFor("+scores").size() == 2 && in.commandFromKey("F4") == "+scores");

    // A key changed in either menu set is the same change (one table); it is saved in config.cfg only.
    in.rebind("+forward", ""), in.rebind("+forward", "K");        // e.g. the SP controls page
    in.rebind("chatmodepublic", ""), in.rebind("chatmodepublic", "U"); // e.g. the MP controls page
    CHECK(game::ProfileKeys::save(in, dir, "seta sensitivity \"7\"\n"));
    const std::string cfg = slurp(dir / "config.cfg");
    CHECK(has(cfg, "seta sensitivity \"7\"\n") && has(cfg, "bind K \"+forward\"") && has(cfg, "bind U \"chatmodepublic\""));
    CHECK(!has(cfg, "bind W ") && !has(cfg, "bind T "));
    CHECK(!fsys::exists(dir / "config_mp.cfg"));
    // The next start (or a profile load) reads it over the defaults: the defaults of a moved action do not come back.
    {
        game::ClientInput again;
        keys.load(again, dir);
        CHECK(again.commandFromKey("K") == "+forward" && again.commandFromKey("W").empty());
        CHECK(again.commandFromKey("U") == "chatmodepublic" && again.commandFromKey("T").empty());
        CHECK(again.commandFromKey("Y") == "chatmodeteam");
    }

    // Controls reset (either default file from the popup): the unified defaults.
    keys.setDefaults(in);
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("K").empty() && in.commandFromKey("T") == "chatmodepublic");
    CHECK(in.commandFromKey("F4") == "+scores" && in.commandFromKey("TAB") == "+scores");

    // The multiplayer-only defaults never replace a user's key: T taken by another action keeps it; chat stays without a key.
    {
        game::ClientInput t;
        t.loadConfig("unbindall\nbind W \"+forward\"\nbind T \"+back\"\nbind TAB \"+scores\"\nbind Q \"+scores\"\n");
        keys.addMpOnlyDefaults(t);
        CHECK(t.commandFromKey("T") == "+back" && t.keysFor("chatmodepublic").empty());
        CHECK(t.commandFromKey("Y") == "chatmodeteam");
        CHECK(t.commandFromKey("F4").empty()); // +scores has both slots (TAB, Q)
    }

    // Migration of a single-player profile of 0.3.0 and older (config.cfg without the multiplayer keys) together with a
    // config_mp.cfg of the two-table build: config.cfg wins, config_mp.cfg gives only actions without a key on free keys,
    // then the multiplayer-only defaults fill the rest.
    {
        const fsys::path old = players / "old";
        fsys::create_directories(old);
        std::ofstream(old / "config.cfg") << "seta name \"x\"\nunbindall\nbind UPARROW \"+forward\"\nbind S \"+back\"\nbind TAB \"+scores\"\n"
                                             "bind ESCAPE \"togglemenu\"\nbind J \"+melee\"\n";
        std::ofstream(old / "config_mp.cfg") << "unbindall\nbind I \"+forward\"\nbind S \"+back\"\nbind ESCAPE \"togglemenu\"\n"
                                                "bind J \"chatmodeteam\"\nbind O \"chatmodepublic\"\nbind TAB \"+scores\"\n";
        game::ClientInput m;
        keys.load(m, old);
        keys.mergeProfile(m, old);
        CHECK(m.commandFromKey("UPARROW") == "+forward" && m.commandFromKey("I").empty()); // the SP key wins
        CHECK(m.commandFromKey("J") == "+melee");                                            // taken key: not replaced
        CHECK(m.commandFromKey("O") == "chatmodepublic" && m.commandFromKey("T").empty());   // the user's MP chat key
        CHECK(m.commandFromKey("Y") == "chatmodeteam");                                      // MP J was taken: the default
        CHECK(m.commandFromKey("X") == "openscriptmenu ingame changeweapon" && m.commandFromKey("F4") == "+scores");
        // Saved as one table; config_mp.cfg is left alone.
        CHECK(game::ProfileKeys::save(m, old, "seta name \"x\"\n"));
        CHECK(has(slurp(old / "config.cfg"), "bind O \"chatmodepublic\"") && has(slurp(old / "config_mp.cfg"), "bind I \"+forward\""));
    }
    // A config.cfg that an older build filled with the multiplayer table (after a multiplayer session) is a full table already:
    // the merge changes nothing.
    {
        const fsys::path polluted = players / "polluted";
        fsys::create_directories(polluted);
        std::ofstream(polluted / "config.cfg") << "unbindall\nbind W \"+forward\"\nbind S \"+back\"\nbind TAB \"+scores\"\n"
                                                  "bind ESCAPE \"togglemenu\"\nbind T \"chatmodepublic\"\nbind Y \"chatmodeteam\"\n"
                                                  "bind X \"openscriptmenu ingame changeweapon\"\nbind F4 \"+scores\"\n";
        game::ClientInput m;
        keys.load(m, polluted);
        const std::string before = m.formatBindings();
        keys.mergeProfile(m, polluted);
        CHECK(m.formatBindings() == before);
    }
}

// The shipped files: the defaults are default.cfg plus the multiplayer controls of default_mp.cfg:53-63.
void shipped(const char* main, const fsys::path& dir) {
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    game::ProfileKeys keys;
    keys.loadDefaults([&](const std::string& n) -> std::optional<std::string> {
        const auto f = vfs.read(n);
        return f ? std::optional<std::string>(std::string(f->begin(), f->end())) : std::nullopt;
    });
    game::ClientInput in;
    keys.load(in, dir);
    CHECK(in.commandFromKey("W") == "+forward" && in.commandFromKey("ESCAPE") == "togglemenu");
    CHECK(in.commandFromKey("T") == "chatmodepublic" && in.commandFromKey("Y") == "chatmodeteam" && in.commandFromKey("V") == "mp_QuickMessage");
    CHECK(in.commandFromKey("Z") == "+talk" && in.commandFromKey("X") == "openscriptmenu ingame changeweapon");
    CHECK(in.commandFromKey("F1") == "vote yes" && in.commandFromKey("F2") == "vote no");
    CHECK(in.commandFromKey("F4") == "+scores" && in.commandFromKey("TAB") == "+scores");
    // The table equals the shipped multiplayer table (default_mp.cfg binds every default.cfg key the same way).
    game::ClientInput mp;
    if (const auto f = vfs.read("default_mp.cfg")) mp.loadConfig(std::string(f->begin(), f->end()));
    CHECK(mp.bindings() == in.bindings());
}

// main.cpp must not rebuild the key table from a default file on a level load (the 0.3.0 bug: every multiplayer level load,
// join and round restart ran default_mp.cfg over the user's keys).
void source(const char* mainCpp) {
    const std::string src = slurp(mainCpp);
    CHECK(!src.empty());
    CHECK(!has(src, "vfs.read(\"default_mp.cfg\")") && !has(src, "vfs.read(\"default.cfg\")"));
}

} // namespace

int main(int argc, char** argv) {
    const fsys::path base = fsys::temp_directory_path() / ("iw2_profilekeys_" + std::to_string(::getpid()));
    fsys::remove_all(base);
    fsys::create_directories(base / "players");
    synthetic(base / "players");
    if (const char* main = std::getenv("IW2_MAIN")) {
        fsys::create_directories(base / "players" / "shipped");
        shipped(main, base / "players" / "shipped");
    }
    if (argc > 1) source(argv[1]);
    fsys::remove_all(base);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
