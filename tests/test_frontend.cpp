// MenuStack: open stack, dvar-driven visibility, focus by dvar, mouse/keyboard input (synthetic menus).
#include "ui/frontend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
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
    std::ofstream(dir / "ui/w.menu") << R"({
	menuDef
	{
		name opts
		rect 10 20 400 300
		visible 1
		itemDef
		{
			name vol
			type 10
			text " "
			dvarfloat "snd_volume" 0.8 0 1
			rect -30 0 200 13
			origin 50 100
			visible 1
		}
		itemDef
		{
			name yn
			type 11
			text "x"
			dvar "flag"
			rect 0 0 200 13
			origin 0 120
			visible 1
		}
		itemDef
		{
			name multi
			type 12
			text "x"
			dvarFloatList { "@A" 0 "@B" 1 "@C" 2 }
			dvar "mode"
			rect 0 0 200 13
			origin 0 140
			visible 1
		}
	}
})";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::ui::MenuLibrary lib(vfs);
    CHECK(lib.loadFile("ui/t.menu"));
    CHECK(lib.loadFile("ui/w.menu"));
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

    // Options widgets: origin adds to the rect (exe 0x4d1dd0); slider, yes/no and multi items.
    {
        env.textWidth = [](const iw2::ui::Item&) { return 10.0f; };
        CHECK(st.open("opts"));
        const iw2::ui::Menu* m = st.top();
        const iw2::ui::Item& vol = *m->items[0];
        CHECK(vol.hasRange && vol.dvar == "snd_volume" && vol.dvarDef == 0.8f && vol.dvarMax == 1.0f);
        CHECK(vol.rect.x == 20 && vol.rect.y == 100);
        CHECK(m->items[2]->floatList.size() == 3 && m->items[2]->floatList[1].second == 1.0f);
        CHECK(st.selectable(*m).size() == 3);
        // track x = menu x + item x + text width + 8 = 10 + 20 + 10 + 8; thumb at the dvarfloat default 0.8 while the dvar is unset
        CHECK(st.sliderTrackX(*m, vol) == 48.0f);
        CHECK(std::abs(st.sliderFraction(vol) - 0.8f) < 1e-5f);
        // a click sets the value from the pointer: fraction = (x - (track + 6)) / 84
        CHECK(st.click(48.0f + 6.0f + 42.0f, 20 + 105));
        CHECK(cv["snd_volume"] == "0.5");
        st.mouseMove(48.0f + 6.0f + 84.0f, 20 + 105); // drag to the end
        CHECK(cv["snd_volume"] == "1");
        st.release();
        st.mouseMove(48.0f + 6.0f + 10.0f, 20 + 105); // no drag after release
        CHECK(cv["snd_volume"] == "1");
        st.key(iw2::ui::MenuStack::Key::Left); // arrows: 5% of the range
        CHECK(cv["snd_volume"] == "0.95");
        // yes/no: label by value, click toggles ("%i")
        const iw2::ui::Item& yn = *m->items[1];
        CHECK(st.valueLabel(yn) == "EXE_NO");
        CHECK(st.click(10 + 5, 20 + 125));
        CHECK(cv["flag"] == "1" && st.valueLabel(yn) == "EXE_YES");
        CHECK(st.valueTextX(*m, yn) == 10 + 0 + 10 + 8);
        // multi: next / previous with wrap, unmatched value shows index 0
        const iw2::ui::Item& mu = *m->items[2];
        CHECK(st.valueLabel(mu) == "@A");
        CHECK(st.click(15, 20 + 145));
        CHECK(cv["mode"] == "1" && st.valueLabel(mu) == "@B");
        CHECK(st.clickBack(15, 20 + 145));
        CHECK(cv["mode"] == "0");
        CHECK(st.clickBack(15, 20 + 145));
        CHECK(cv["mode"] == "2");
        st.closeAll();
    }

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
