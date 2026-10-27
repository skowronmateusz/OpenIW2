// MenuStack: open stack, dvar-driven visibility, focus by dvar, mouse/keyboard input (synthetic menus).
#include "ui/frontend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

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
    const auto dir = std::filesystem::temp_directory_path() / "iw2_test_frontend";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "ui");
    std::ofstream(dir / "ui/t.menu") << R"({
	menuDef
	{
		name main
		rect 0 0 640 480
		visible 1
		itemDef
		{
			name newgame
			type 1
			rect 10 10 100 20
			visible 1
			dvartest sv_running
			showdvar { "0" }
			action { setdvar ui_selected_map "map x"; open diff; }
		}
		itemDef
		{
			name back
			type 1
			rect 10 40 100 20
			visible 1
			dvartest sv_running
			showdvar { "1" }
			action { close main; }
		}
		itemDef
		{
			name quit
			type 1
			rect 10 70 100 20
			visible 1
			action { exec "quit"; }
		}
	}
	menuDef
	{
		name diff
		rect 200 100 200 100
		visible 1
		popup
		onOpen { setfocus "b"; setfocusbydvar "g_gameskill"; }
		onESC { close diff; }
		itemDef
		{
			name a
			type 1
			rect 0 0 100 20
			visible 1
			dvartest g_gameskill
			focusdvar { "0" }
			action { exec "vstr ui_selected_map"; close diff; }
		}
		itemDef
		{
			name b
			type 1
			rect 0 20 100 20
			visible 1
			dvartest g_gameskill
			focusdvar { "1" }
			action { close diff; }
		}
	}
})";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::ui::MenuLibrary lib(vfs);
    CHECK(lib.loadFile("ui/t.menu"));
    std::map<std::string, std::string> cv;
    std::vector<std::string> log;
    iw2::ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    env.play = [&](const std::string& a) { log.push_back("play " + a); };
    env.exec = [&](const std::string& c) { log.push_back("exec " + c); };
    iw2::ui::MenuStack st(lib, env);

    cv["sv_running"] = "0";
    CHECK(st.open("main"));
    CHECK(st.focus() && st.focus()->name == "newgame");
    CHECK(st.selectable(*st.top()).size() == 2); // newgame, quit; back hidden
    cv["sv_running"] = "1";
    CHECK(st.selectable(*st.top()).size() == 2); // back, quit
    cv["sv_running"] = "0";

    // Hover moves focus and plays the hover sound; click runs the action (opens the popup, focus by g_gameskill).
    cv["g_gameskill"] = "1";
    st.mouseMove(20, 80);
    CHECK(st.focus()->name == "quit");
    CHECK(log.back() == "play mouse_over");
    CHECK(st.click(20, 20));
    CHECK(cv["ui_selected_map"] == "map x");
    CHECK(st.top()->name == "diff");
    CHECK(st.focus() && st.focus()->name == "b");
    // Keyboard: up -> a, enter -> vstr + close.
    st.key(iw2::ui::MenuStack::Key::Up);
    CHECK(st.focus()->name == "a");
    log.clear();
    st.key(iw2::ui::MenuStack::Key::Enter);
    CHECK(log.size() == 1 && log[0] == "exec vstr ui_selected_map");
    CHECK(st.top()->name == "main");
    // ESC on a popup closes it.
    st.open("diff");
    st.key(iw2::ui::MenuStack::Key::Escape);
    CHECK(st.top()->name == "main");
    st.closeAll();
    CHECK(st.empty());

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
