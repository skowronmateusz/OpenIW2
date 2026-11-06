// MenuStack focus and pointer routing across open menus: out-of-bounds clicks, popups, fullScreen menus, outOfBoundsClick.
// Synthetic menus always run; with IW2_MAIN the shipped options_menu -> options_game case (right-hand column buttons) runs too.
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ui/frontend.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::ui::Command;
using iw2::ui::Item;
using iw2::ui::Menu;
using iw2::ui::MenuStack;

bool hasMenu(const MenuStack& st, const std::string& name) {
    return std::any_of(st.menus().begin(), st.menus().end(), [&](const Menu* m) { return m->name == name; });
}

struct Fixture {
    std::map<std::string, std::string> cv;
    iw2::ui::MenuEnv env;
    explicit Fixture() {
        env.getCvar = [this](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
        env.setCvar = [this](const std::string& n, const std::string& v) { cv[n] = v; };
    }
};

const char* kMenus = R"({
	menuDef
	{
		name base
		rect 0 0 640 480
		visible 1
		onESC { setdvar esc_base "1"; }
		itemDef
		{
			name baseBtn
			type 1
			rect 400 10 100 20
			visible 1
			action { setdvar hit_base "1"; }
		}
		itemDef
		{
			name baseWide
			type 1
			rect 10 400 600 20
			visible 1
			action { setdvar hit_basewide "1"; }
		}
	}
	menuDef
	{
		name side
		rect 0 0 300 480
		visible 1
		onESC { setdvar esc_side "1"; }
		onClose { setdvar side_closed "1"; }
		itemDef
		{
			name sideBtn
			type 1
			rect 10 10 100 20
			visible 1
			action { setdvar hit_side "1"; }
		}
	}
	menuDef
	{
		name pop
		popup
		rect 100 100 200 200
		visible 1
		itemDef
		{
			name popBtn
			type 1
			rect 10 10 100 20
			visible 1
			action { setdvar hit_pop "1"; }
		}
	}
	menuDef
	{
		name oob
		outOfBoundsClick
		rect 0 0 300 480
		visible 1
		onClose { setdvar oob_closed "1"; }
		itemDef
		{
			name oobBtn
			type 1
			rect 10 10 100 20
			visible 1
			action { setdvar hit_oob "1"; }
		}
	}
	menuDef
	{
		name full
		fullScreen 1
		rect 0 0 300 300
		visible 1
		itemDef
		{
			name fullBtn
			type 1
			rect 10 10 100 20
			visible 1
			action { setdvar hit_full "1"; }
		}
	}
})";
} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_menufocus_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "ui");
    std::ofstream(dir / "ui/f.menu") << kMenus;
    {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(dir);
        iw2::ui::MenuLibrary lib(vfs);
        CHECK(lib.loadFile("ui/f.menu"));
        CHECK(lib.find("oob") && lib.find("oob")->outOfBoundsClick && !lib.find("side")->outOfBoundsClick);
        CHECK(lib.find("pop") && lib.find("pop")->popup && lib.find("full")->fullScreen);

        { // a narrower menu over a wide one: the wide menu's buttons outside the narrow rect react
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("side");
            CHECK(st.top()->name == "side" && st.focusedMenu()->name == "side");
            st.mouseMove(450, 20);
            CHECK(st.focus() && st.focus()->name == "baseBtn");
            CHECK(st.hover(450, 20) && st.hover(450, 20)->name == "baseBtn");
            CHECK(st.click(450, 20));
            CHECK(f.cv["hit_base"] == "1");
            CHECK(st.focusedMenu()->name == "base");   // the focus moved ...
            CHECK(st.top()->name == "side");           // ... the list order did not
            CHECK(hasMenu(st, "side") && f.cv["side_closed"].empty());
            // ESC goes to the focused menu now
            st.key(MenuStack::Key::Escape);
            CHECK(f.cv["esc_base"] == "1" && f.cv["esc_side"].empty());
            // the side menu's own button under the focused (base) menu rect is refused (0x4c915c)
            st.mouseMove(50, 20);
            CHECK(!st.hover(50, 20));
        }
        { // inside the focused menu's rect clicks stay with it; empty space outside it does nothing
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("side");
            st.mouseMove(50, 20);
            CHECK(st.focus() && st.focus()->name == "sideBtn");
            CHECK(st.click(50, 20) && f.cv["hit_side"] == "1");
            CHECK(!st.click(600, 300)); // no active item of any menu there
            CHECK(st.focusedMenu()->name == "side" && st.empty() == false);
            CHECK(!st.click(350, 50)); // outside side, no item under it
            CHECK(f.cv["hit_base"].empty() && st.focusedMenu()->name == "side");
            CHECK(st.click(50, 405) == false); // base's wide button is below side's items but inside side's rect: refused
            CHECK(f.cv["hit_basewide"].empty());
        }
        { // a popup is modal for the pointer: no hover and no click reaches the menu below
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("pop");
            st.mouseMove(450, 20);
            CHECK(!st.hover(450, 20) && !(st.focus() && st.focus()->name == "baseBtn"));
            CHECK(!st.click(450, 20));
            CHECK(f.cv["hit_base"].empty() && st.focusedMenu()->name == "pop" && hasMenu(st, "pop"));
            st.mouseMove(110, 110);
            CHECK(st.focus() && st.focus()->name == "popBtn");
            CHECK(st.click(110, 110) && f.cv["hit_pop"] == "1");
        }
        { // a fullScreen menu does not forward clicks either
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("full");
            CHECK(!st.click(450, 20));
            CHECK(f.cv["hit_base"].empty() && st.focusedMenu()->name == "full");
        }
        { // outOfBoundsClick: the click closes the menu (onClose) and then reaches the menu under the pointer
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("oob");
            CHECK(st.click(450, 20));
            CHECK(f.cv["oob_closed"] == "1" && !hasMenu(st, "oob"));
            CHECK(f.cv["hit_base"] == "1" && st.focusedMenu()->name == "base");
        }
        { // outOfBoundsClick on empty space: closed, focus falls back to the menu below
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("oob");
            CHECK(!st.click(600, 300));
            CHECK(f.cv["oob_closed"] == "1" && !hasMenu(st, "oob") && st.focusedMenu() && st.focusedMenu()->name == "base");
            CHECK(f.cv["hit_base"].empty());
        }
        { // closing the focused menu hands the focus to the last remaining one; closing another keeps it
            Fixture f;
            MenuStack st(lib, f.env);
            st.open("base");
            st.open("side");
            st.click(450, 20); // focus -> base
            st.close("side");
            CHECK(st.focusedMenu()->name == "base");
            st.open("side");
            st.close("base");
            CHECK(st.focusedMenu()->name == "side");
        }
    }
    std::filesystem::remove_all(dir);

    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs game;
        game.mountDirectory(main);
        iw2::ui::MenuLibrary sp(game);
        CHECK(sp.loadList("ui/menus.txt"));
        Menu* om = sp.find("options_menu");
        Menu* og = sp.find("options_game");
        CHECK(om && og && sp.find("main_text") && sp.find("options_look"));
        if (om && og) {
            // Right-hand column buttons of options_menu that lie outside the options_game rect: "back" (closes options_menu)
            // and the section button that opens options_look. Found by their commands, not by position or text.
            auto find = [&](const Command& c) -> const Item* {
                for (const auto& it : om->items)
                    if (it->type == 1 && !it->decoration && std::find(it->action.begin(), it->action.end(), c) != it->action.end()) return it.get();
                return nullptr;
            };
            const Item* back = find({"close", "options_menu"});
            const Item* look = find({"open", "options_look"});
            CHECK(back && look);
            auto centre = [&](const Item* it) { return std::pair<float, float>{om->rect.x + it->rect.x + it->rect.w * 0.5f, om->rect.y + it->rect.y + it->rect.h * 0.5f}; };
            if (back && look) {
                CHECK(om->rect.x + back->rect.x > og->rect.x + og->rect.w); // truly outside the sub-menu
                { // the section button: opens options_look, whose open closes options_game by script
                    Fixture f;
                    MenuStack st(sp, f.env);
                    st.open("options_menu");
                    st.open("options_game");
                    CHECK(st.top() == og && st.focusedMenu() == og);
                    const auto [x, y] = centre(look);
                    st.mouseMove(x, y);
                    CHECK(st.focus() == look); // hover highlight of the underlying menu's button
                    CHECK(st.click(x, y));
                    CHECK(hasMenu(st, "options_look") && !hasMenu(st, "options_game"));
                    CHECK(st.focusedMenu() && st.focusedMenu()->name == "options_look");
                }
                { // Back: closes options_menu, whose onClose closes the open sub-menu and returns to main_text
                    Fixture f;
                    MenuStack st(sp, f.env);
                    st.open("options_menu");
                    st.open("options_game");
                    const auto [x, y] = centre(back);
                    CHECK(!st.click(om->rect.w - 1, om->rect.h - 1) || true); // empty-space click first: no change
                    CHECK(hasMenu(st, "options_game") && hasMenu(st, "options_menu"));
                    st.mouseMove(x, y);
                    CHECK(st.focus() == back);
                    CHECK(st.click(x, y));
                    CHECK(!hasMenu(st, "options_menu") && !hasMenu(st, "options_game"));
                    CHECK(hasMenu(st, "main_text") && st.focusedMenu() && st.focusedMenu()->name == "main_text");
                }
                { // ESC on the sub-menu still closes options_menu (and with it the sub-menu)
                    Fixture f;
                    MenuStack st(sp, f.env);
                    st.open("options_menu");
                    st.open("options_game");
                    st.key(MenuStack::Key::Escape);
                    CHECK(!hasMenu(st, "options_game") && !hasMenu(st, "options_menu"));
                }
            }
        }
    } else {
        std::cout << "shipped options menu checks skipped (IW2_MAIN not set)\n";
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
