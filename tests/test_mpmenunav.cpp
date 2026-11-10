// Multiplayer main menu navigation with the shipped ui_mp menus (docs/spec/mp-ui.md "Main menu navigation"): every way into a
// sub-menu and back out leaves exactly the menus the original keeps open, and ESC on the main menu outside a game closes nothing.
#include "game/clientinput.h"
#include "ui/esckey.h"
#include "ui/frontend.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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

std::string names(const ui::MenuStack& st) {
    std::vector<std::string> v;
    for (const ui::Menu* m : st.menus()) v.push_back(m->name);
    std::sort(v.begin(), v.end());
    std::string s;
    for (const auto& n : v) s += (s.empty() ? "" : " ") + n;
    return s;
}

// Click the first visible, selectable item of `menu` whose text is `text` (or name, when text is empty).
bool press(ui::MenuStack& st, const std::string& menu, const std::string& name, const std::string& text = {}) {
    for (const ui::Menu* m : st.menus()) {
        if (m->name != menu) continue;
        for (const auto& it : m->items) {
            if (it->name != name || (!text.empty() && it->text != text) || !st.itemVisible(*it) || it->decoration) continue;
            const float x = m->rect.x + it->rect.x + it->rect.w * 0.5f, y = m->rect.y + it->rect.y + it->rect.h * 0.5f;
            if (st.hover(x, y) != it.get()) continue;
            st.mouseMove(x, y);
            st.click(x, y);
            return true;
        }
    }
    return false;
}

void world(const char* main) {
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    std::map<std::string, std::string> cv{{"cl_ingame", "0"}, {"com_playerProfile", "p"}, {"ui_playerProfileCount", "1"}};
    std::vector<std::string> log;
    ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    env.exec = [&](const std::string& c) { log.push_back(c); };
    env.uiScript = [&](const std::string& n, const ui::Command&) { log.push_back("uiScript " + n); };
    bool inGame = false; // the front end: not connected to a server
    env.ingameActive = [&] { return inGame; };
    ui::MenuLibrary lib(vfs);
    CHECK(lib.loadList("ui_mp/menus.txt"));
    ui::MenuStack st(lib, env);
    const std::string home = "bg main main_text";
    CHECK(st.open("main"));
    CHECK(names(st) == home);

    // ESC on the main menu (main_text has the focus; ui_mp/main.menu:86-94): only menus that are not open are closed and
    // "ingameclose main" does nothing outside a game, so the menu stays.
    st.key(ui::MenuStack::Key::Escape);
    CHECK(names(st) == home);
    st.key(ui::MenuStack::Key::Escape);
    CHECK(names(st) == home);

    // Join game (main.menu:206-214) -> joinserver; its Back button (joinserver.menu:971-977) reopens main.
    CHECK(press(st, "main_text", "play", "@MENU_JOIN_GAME"));
    CHECK(names(st) == "bg joinserver main");
    CHECK(press(st, "joinserver", "back"));
    CHECK(names(st) == home);
    // ... and its ESC (joinserver.menu:35-41) reopens main_text.
    CHECK(press(st, "main_text", "play", "@MENU_JOIN_GAME"));
    st.key(ui::MenuStack::Key::Escape);
    CHECK(names(st) == home);

    // Start new server (main.menu:269-277) -> createserver and its three panels; Back and ESC.
    CHECK(press(st, "main_text", "play", "@MENU_START_NEW_SERVER"));
    CHECK(names(st) == "bg createserver createserver_buttons createserver_maps createserver_serversettings main");
    CHECK(press(st, "createserver_buttons", "back"));
    CHECK(names(st) == home);
    CHECK(press(st, "main_text", "play", "@MENU_START_NEW_SERVER"));
    st.key(ui::MenuStack::Key::Escape);
    CHECK(names(st) == home);

    // The refused start: error_popmenu over the create-server screen; its close (ESC) must not leave for the main menu.
    {
        CHECK(press(st, "main_text", "play", "@MENU_START_NEW_SERVER"));
        const std::string below = names(st);
        st.errorKeepsScreen(true);
        CHECK(st.open("error_popmenu"));
        st.key(ui::MenuStack::Key::Escape);
        CHECK(names(st) == below);
        CHECK(press(st, "createserver_buttons", "back"));
        CHECK(names(st) == home);
    }

    // Every other button of the main menu: open it, then ESC until the main menu is back alone (probe).
    for (const char* name : {"options", "player_select", "playselection", "quit"}) {
        if (!press(st, "main_text", name)) { std::cerr << name << ": not pressable\n"; continue; }
        std::string path = names(st);
        for (int i = 0; i < 4 && names(st) != home; ++i) { st.key(ui::MenuStack::Key::Escape); path += " | " + names(st); }
        std::cerr << name << ": " << path << "\n";
        CHECK(names(st) == home);
    }
    CHECK(names(st) == home);

    // In an MP level (docs/spec/mp-ui.md "Escape key"): default_mp.cfg binds ESCAPE to togglemenu; one press queues exactly one
    // togglemenu and the viewer must not toggle a second time on the same press.
    const auto cfg = vfs.read("default_mp.cfg");
    CHECK(cfg.has_value());
    game::ClientInput input;
    if (cfg) input.loadConfig(std::string(cfg->begin(), cfg->end()));
    const auto escKeys = input.keysFor("togglemenu");
    const bool escBound = std::find(escKeys.begin(), escKeys.end(), "ESCAPE") != escKeys.end();
    CHECK(escBound);
    input.key("ESCAPE", true);
    input.key("ESCAPE", false);
    const auto cmds = input.takeConsoleCommands();
    CHECK(cmds.size() == 1 && cmds[0] == "togglemenu");
    CHECK(ui::escRoute(false, true, true, escBound) == ui::EscRoute::Binding);
    CHECK(ui::escRoute(false, true, true, false) == ui::EscRoute::ToggleMenu);
    CHECK(ui::escRoute(true, true, true, escBound) == ui::EscRoute::Menu);
    CHECK(ui::escRoute(false, true, false, false) == ui::EscRoute::PauseMenu);
    CHECK(ui::escRoute(false, false, false, false) == ui::EscRoute::MainMenu);

    // The in-game menu (g_scriptMainMenu "ingame", ui_mp/scriptmenus/ingame.menu): ESC closes it; key 5 opens leavegame, whose
    // yes button closes it and runs "disconnect".
    st.closeAll();
    CHECK(lib.loadFile("ui_mp/scriptmenus/ingame.menu"));
    CHECK(st.open("ingame") && names(st) == "ingame");
    st.key(ui::MenuStack::Key::Escape);
    CHECK(st.menus().empty());
    CHECK(st.open("ingame"));
    CHECK(st.execKey('5'));
    CHECK(names(st) == "leavegame");
    st.key(ui::MenuStack::Key::Escape);
    CHECK(st.menus().empty());
    CHECK(st.open("ingame") && st.execKey('5'));
    log.clear();
    CHECK(press(st, "leavegame", "button_yes"));
    CHECK(st.menus().empty());
    CHECK(std::find(log.begin(), log.end(), "disconnect") != log.end());

    // "ingameclose" is active in every MP level (local listen server or network client: the connection is active) and in SP
    // only while cl_paused is set.
    CHECK(ui::ingameCloseActive(true, true, false));
    CHECK(!ui::ingameCloseActive(true, false, false));
    CHECK(ui::ingameCloseActive(false, true, true));
    CHECK(!ui::ingameCloseActive(false, true, false));
    CHECK(!ui::ingameCloseActive(false, false, false));

    // togglemenu opens the server's g_scriptMainMenu once known (on a network client it arrives as a "v" server command), "main"
    // otherwise. In a game, ESC closes either one again: no menu is left, so the cursor goes back to mouse look.
    CHECK(ui::toggleMenuTarget("ingame", true) == "ingame");
    CHECK(ui::toggleMenuTarget("ingame", false) == "main");
    CHECK(ui::toggleMenuTarget("", false) == "main");
    inGame = true;
    st.closeAll();
    CHECK(st.open(ui::toggleMenuTarget("ingame", true)) && names(st) == "ingame");
    st.key(ui::MenuStack::Key::Escape);
    CHECK(st.menus().empty());
    CHECK(st.open(ui::toggleMenuTarget("", false)) && names(st) == home);
    st.key(ui::MenuStack::Key::Escape); // main_text's onESC: "ingameclose main"
    CHECK(st.menus().empty());
    inGame = false;

    // A menu the server opened (server command "t", e.g. the team menu): ESC closes it.
    CHECK(lib.loadFile("ui_mp/scriptmenus/team_britishgerman.menu"));
    CHECK(st.open("team_britishgerman") && names(st) == "team_britishgerman");
    st.key(ui::MenuStack::Key::Escape);
    CHECK(st.menus().empty());
}

} // namespace

int main() {
    if (const char* main = std::getenv("IW2_MAIN")) world(main);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "mpmenunav ok\n";
    return failures ? 1 : 0;
}
