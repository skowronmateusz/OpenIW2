// The in-game menu entries a player opens with keys (docs/spec/mp-chat.md "Quick messages", docs/spec/mp-ui.md): the
// ingame menu response "changeweapon" opens the player's team weapon menu, the quick message menus' responses reach
// _quickmessages.gsc in a team game. Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
using iw2::game::World;
void run(World& w, int n) { for (int i = 0; i < n; ++i) w.frame(); }
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) { std::cout << "IW2_MAIN not set, skipped\n"; return 0; }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "mp_toujane", true);
    w.mpStart("tdm");
    const int c0 = w.mpConnect("Local");
    w.mpBegin(c0);
    run(w, 2);
    w.mpMenuResponse(c0, w.vm().game()->get("menu_team").toString(), "axis");
    run(w, 1);
    CHECK(w.mpState().clients[0].openMenu == "weapon_german");
    w.mpMenuResponse(c0, "weapon_german", "mp40_mp");
    run(w, 2);
    CHECK(w.mpState().clients[0].openMenu.empty());
    w.takeClientCommands();
    w.mpState();

    // changeweapon (the "openscriptmenu ingame changeweapon" binding): the ingame menu closes, the weapon menu of the team opens.
    w.mpOpenScriptMenu("ingame", "changeweapon");
    run(w, 1);
    CHECK(w.mpState().clients[0].openMenu == "weapon_german");
    // The same pair as a network client's "mr" names it (any case of the menu name).
    w.mpMenuResponse(c0, "weapon_german", "back");
    run(w, 1);
    w.mpMenuResponse(c0, "ingame", "changeweapon");
    run(w, 1);
    CHECK(w.mpState().clients[0].openMenu == "weapon_german");
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    CHECK(w.vm().errors().empty());
    return failures ? 1 : 0;
}
