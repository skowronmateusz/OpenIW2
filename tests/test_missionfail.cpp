// Mission failure after friendly fire (docs/spec/campaign.md "Death screen"): the death screen, the due loadgame_continue, and the
// reload of the level-start autosave. The reloaded world must be a clean, playable one (death-screen guard cleared, controls released,
// weapons kept, a second failure possible), and an unusable save must be reported so that the front end restarts the level.
#include "fs/vfs.h"
#include "game/savestate.h"
#include "game/world.h"
#include "netgame/clienterror.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <fstream>
#include <iostream>

using namespace iw2::game;

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
    // The save-file reader needs no game files.
    {
        const auto dir = std::filesystem::temp_directory_path() / ("iw2_missionfail_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        const auto bad = dir / "bad.svg";
        std::ofstream(bad, std::ios::binary) << "not a save";
        LoadedSave ls;
        CHECK(!readSaveFile((dir / "missing.svg").string(), "missing", ls).empty());
        CHECK(readSaveFile((dir / "missing.svg").string(), "missing", ls).rfind("Can't find savegame", 0) == 0);
        CHECK(!readSaveFile(bad.string(), "bad", ls).empty());
        // A failed load is one drop error (docs/spec/savegame.md section 5): the banner once, MENU_ERROR over the SP main menu.
        const std::string err = readSaveFile(bad.string(), "bad", ls);
        const std::string banner = loadErrorBanner(err);
        CHECK(banner == "********************\nERROR: " + err + "\n********************\n");
        const auto dlg = iw2::netgame::dropErrorDialog(err);
        CHECK(dlg.titleKey == "MENU_ERROR" && dlg.message == err && !dlg.multiplayer);
        CHECK(iw2::netgame::errorDialog(false, "EXE_SERVERISFULL").multiplayer);
        std::filesystem::remove_all(dir);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "world part skipped (IW2_MAIN not set)\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World a(vfs, "hill400_defend");
    a.start();
    std::vector<World::SaveFile> saves;
    for (int i = 0; i < 120; ++i) {
        a.frame();
        for (auto& f : a.takeSaveFiles()) saves.push_back(f);
    }
    CHECK(!saves.empty()); // the level-start autosave
    if (saves.empty()) return 1;

    // The friendly-fire failure: death screen, then the due change is a loadgame_continue.
    a.cvars["g_deathDelay"] = "0";
    a.vm().start("maps/_spawner", "killfriends_missionfail", {}, iw2::script::Value(a.vm().level()));
    std::optional<LevelChange> ch;
    for (int i = 0; i < 400 && !ch; ++i) {
        a.frame();
        ch = a.takeDueLevelChange();
    }
    CHECK(ch && ch->loadContinue && ch->failed);

    // The reload: the autosave goes into a fresh world.
    LoadedSave ls;
    CHECK(parseSaveFile(saves[0].bytes, ls).empty());
    World b(vfs, ls.info.mapName);
    b.cvars["sv_cheats"] = "1"; // the session's sv_cheats (command line) survives the restore; the save made without it
    b.prepareStart();
    restoreSave(b, ls);
    CHECK(b.cvars["sv_cheats"] == "1");
    CHECK(b.player() && b.isAlive(*b.player()));
    CHECK(!b.takeDueLevelChange());
    const size_t owned = b.player() ? b.inventory(*b.player()).owned.size() : 0;
    CHECK(owned >= 3);
    for (int i = 0; i < 140; ++i) b.frame(); // the intro's freeze ends by itself (script threads resume)
    CHECK(!b.controlsFrozen());
    CHECK(b.player() && b.isAlive(*b.player()));
    CHECK(b.inventory(*b.player()).owned.size() == owned);
    b.takeClientCommands();
    // A second failure in the reloaded world works: the death-screen guard (g_reloading) was not carried over.
    b.cvars["g_deathDelay"] = "0";
    b.vm().start("maps/_spawner", "killfriends_missionfail", {}, iw2::script::Value(b.vm().level()));
    std::optional<LevelChange> ch2;
    for (int i = 0; i < 400 && !ch2; ++i) {
        b.frame();
        ch2 = b.takeDueLevelChange();
    }
    CHECK(ch2 && ch2->loadContinue);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
