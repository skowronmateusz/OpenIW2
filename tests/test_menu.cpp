// Tests for the .menu preprocessor and parser using synthetic files. Set IW2_MAIN to a game main directory to
// also load the shipped single-player menu lists.

#include "ui/menu.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

void write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "iw2_test_menu";
    std::filesystem::remove_all(dir);
    write(dir / "ui/defs.h", "#define SIZE 160\n#define COLOR .5 .25 1 1 // comment\n#define HORIZONTAL_ALIGN_LEFT 1\n");
    write(dir / "ui/list.txt", "{\n loadMenu { \"ui/a.menu\" }\n}\n");
    write(dir / "ui/a.menu", R"(#include "ui/defs.h"
#ifdef MISSING
#define SIZE 1
#endif
{
	assetGlobalDef { fadeClamp 1.0 }
	menuDef
	{
		name "Compass"
		fullScreen 0
		rect -20 -140 SIZE SIZE HORIZONTAL_ALIGN_LEFT 3
		onOpen
		{
			open bg; setdvar ui_x "1";
		}
		/* block
		   comment */
		itemDef
		{
			name "face"
			rect 0 0 SIZE SIZE
			forecolor COLOR
			background "gfx/hud/x.tga"
			ownerdraw 84
			visible 1
			decoration
			dvartest "sv_running"
			showDvar { "1" }
			action { play "mouse_click"; close main; }
		}
	}
}
)");
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::ui::MenuLibrary lib(vfs);
    CHECK(lib.loadList("ui/list.txt"));
    for (const auto& e : lib.errors()) std::cerr << "menu error: " << e << "\n";
    CHECK(lib.errors().empty());
    const iw2::ui::Menu* m = lib.find("compass");
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->rect.x == -20 && m->rect.y == -140 && m->rect.w == 160 && m->rect.h == 160);
        CHECK(m->rect.horzAlign == 1 && m->rect.vertAlign == 3);
        CHECK(m->onOpen.size() == 2);
        CHECK(m->onOpen.size() == 2 && m->onOpen[1] == (iw2::ui::Command{"setdvar", "ui_x", "1"}));
        CHECK(m->items.size() == 1);
        if (!m->items.empty()) {
            const auto& it = *m->items[0];
            CHECK(it.name == "face");
            CHECK(it.foreColor[0] == 0.5f && it.foreColor[1] == 0.25f && it.foreColor[3] == 1.0f);
            CHECK(it.background == "gfx/hud/x.tga");
            CHECK(it.ownerDraw == 84 && it.visible && it.decoration);
            CHECK(it.dvarTest == "sv_running" && it.showDvar == std::vector<std::string>{"1"});
            CHECK(it.action.size() == 2 && it.action[1] == (iw2::ui::Command{"close", "main"}));
        }
    }
    std::filesystem::remove_all(dir);

    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs game;
        game.mountDirectory(main);
        iw2::ui::MenuLibrary sp(game);
        CHECK(sp.loadList("ui/menus.txt"));
        CHECK(sp.loadList("ui/hud.txt"));
        CHECK(sp.loadList("ui/ingame.txt"));
        // ui/menus.txt names ui/rec_restart.menu, which the game files do not contain.
        int parseErrors = 0;
        for (const auto& e : sp.errors())
            if (e != "missing menu file ui/rec_restart.menu") {
                std::cerr << "menu error: " << e << "\n";
                ++parseErrors;
            }
        CHECK(parseErrors == 0);
        CHECK(sp.find("main") && sp.find("Compass") && sp.find("objectiveinfo") && sp.find("pregame"));
        std::cout << sp.menus().size() << " shipped menus\n";
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
