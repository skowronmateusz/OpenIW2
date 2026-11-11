// The OpenIW2 options page (src/ui/iw2options.*): the hit marker option moved here from Game Options, the Cheats option (single
// player only), the entry in the options menu, the profile, and the rules of the developer cheat keys. With IW2_MAIN the shipped
// single-player and multiplayer menu sets are loaded and checked.
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "fs/vfs.h"
#include "ui/iw2options.h"
#include "ui/menu.h"
#include "ui/menurun.h"
#include "ui/profile.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2::ui;

const Item* byName(const Menu& m, const std::string& n) {
    for (const auto& it : m.items)
        if (it->name == n) return it.get();
    return nullptr;
}
const Item* byText(const Menu& m, const std::string& t) {
    for (const auto& it : m.items)
        if (it->text == t) return it.get();
    return nullptr;
}
bool hasCommand(const Script& s, const char* cmd, const char* arg) {
    for (const auto& c : s)
        if (c.size() > 1 && c[0] == cmd && c[1] == arg) return true;
    return false;
}

// A synthetic options menu: Game Options at y 270, Reset below it, Back apart; an options_game page with the crosshair row.
const char* kOptions = R"({
menuDef
{
	name options_menu
	rect 0 0 640 480
	onClose
	{
		close options_game;
	}
	itemDef
	{
		visible 1
		type 1
		text "@MENU_SOUND"
		name resume
		rect 400 255 130 13
		action { play "mouse_click"; close options_game; open options_sound; }
	}
	itemDef
	{
		visible 1
		type 1
		text "@MENU_GAME_OPTIONS"
		name resume
		rect 400 270 130 13
		action { play "mouse_click"; close options_sound; open options_game; }
	}
	itemDef
	{
		visible 1
		type 1
		text "@MENU_RESET_SYSTEM_DEFAULTS"
		name resume
		rect 400 285 130 13
		action { play "mouse_click"; open options_graphics_defaults; }
	}
	itemDef
	{
		visible 1
		type 1
		text "@MENU_BACK"
		name resume
		rect 385 380 130 13
		action { close options_menu; open main_text; }
	}
}
menuDef
{
	name options_game
	rect 0 0 640 480
	itemDef
	{
		dvarFloatList { "ON" 1 "OFF" 0 }
		rect 0 15 100 13
		dvar "cg_drawcrosshair"
		text "x"
		type 12
		visible 1
		name rowvalue
	}
}
})";

void checkSet(bool multiplayer) {
    iw2::fs::Vfs vfs;
    MenuLibrary lib(vfs);
    lib.parseSource(kOptions, "ui/options.menu");
    Menu* opt = lib.find("options_menu");
    CHECK(opt && opt->items.size() == 4);
    CHECK(addOpenIw2Options(lib, multiplayer));
    CHECK(addOpenIw2Options(lib, multiplayer)); // idempotent
    CHECK(opt && opt->items.size() == 5);
    if (!opt) return;
    // The entry sits below Game Options, the entries under it moved down by one line, Back (another column) stays.
    const Item* entry = byName(*opt, "iw2_options_button");
    const Item* game = byText(*opt, "@MENU_GAME_OPTIONS");
    const Item* reset = byText(*opt, "@MENU_RESET_SYSTEM_DEFAULTS");
    const Item* back = byText(*opt, "@MENU_BACK");
    CHECK(entry && game && reset && back);
    if (entry && game && reset && back) {
        CHECK(entry->text == "@IW2_OPT_TITLE" && entry->rect.x == 400 && entry->rect.y == game->rect.y + 15 && game->rect.y == 270);
        CHECK(reset->rect.y == 300 && back->rect.y == 380);
        CHECK(hasCommand(entry->action, "open", "iw2_options") && !hasCommand(entry->action, "open", "options_game"));
    }
    // Opening another page closes the OpenIW2 page, and so does closing the options menu.
    const Item* sound = byText(*opt, "@MENU_SOUND");
    CHECK(sound && hasCommand(sound->action, "close", "iw2_options") && hasCommand(opt->onClose, "close", "iw2_options"));
    // The page: the hit marker option (three values), the Cheats option only in single player (and hidden by ui_multiplayer there).
    const Menu* page = lib.find("iw2_options");
    CHECK(page != nullptr);
    if (page) {
        const Item* hm = byName(*page, "iw2_hitmarker");
        CHECK(hm && hm->dvar == "iw2_hitMarker" && hm->floatList.size() == 3 && hm->floatList[0].second == 1.0f && hm->floatList[1].second == 2.0f &&
              hm->floatList[2].second == 0.0f);
        CHECK(byName(*page, "iw2_hitmarker_label") && byName(*page, "iw2_hitmarker_label")->decoration);
        CHECK(hasCommand(page->onEsc, "close", "options_menu"));
        const Item* ch = byName(*page, "iw2_cheats");
        if (multiplayer) {
            CHECK(!ch && !byName(*page, "iw2_cheats_label"));
            for (const auto& it : page->items) CHECK(it->dvar != "iw2_cheats" && it->text != "@IW2_OPT_CHEATS");
        } else {
            CHECK(ch && ch->type == 11 && ch->dvar == "iw2_cheats" && ch->dvarTest == "ui_multiplayer" && ch->hideDvar.size() == 1 && ch->hideDvar[0] == "1");
            const Item* cl = byName(*page, "iw2_cheats_label");
            CHECK(cl && cl->dvarTest == "ui_multiplayer" && cl->rect.y == ch->rect.y && ch->rect.y == 135);
        }
    }
    // Game Options is left alone: no hit marker row there any more.
    const Menu* g = lib.find("options_game");
    CHECK(g && g->items.size() == 1);
}

} // namespace

int main() {
    checkSet(false);
    checkSet(true);
    {
        iw2::fs::Vfs vfs;
        MenuLibrary empty(vfs);
        CHECK(!addOpenIw2Options(empty, false)); // no options menu, nothing added
        CHECK(empty.find("iw2_options") == nullptr);
    }
    // The option is saved in the profile (archived like the hit marker), default off.
    CHECK(registeredDefault("iw2_cheats") == "0" && isArchivedCvar("iw2_cheats") && isArchivedCvar("iw2_hitMarker"));
    const auto back = parseConfig(formatConfig({{"iw2_cheats", "1"}, {"iw2_hitMarker", "2"}, {"sv_cheats", "1"}}));
    CHECK(back.size() == 2 && back.at("iw2_cheats") == "1" && back.at("iw2_hitMarker") == "2" && !back.count("sv_cheats"));
    // Cheat keys: single player unlocks them with the option (or --cvar sv_cheats); a multiplayer session never does.
    CHECK(!cheatKeysAllowed(false, false, false));
    CHECK(cheatKeysAllowed(true, false, false) && cheatKeysAllowed(false, true, false));
    CHECK(!cheatKeysAllowed(true, false, true) && !cheatKeysAllowed(true, true, true) && !cheatKeysAllowed(false, true, true));
    CHECK(missionSvCheats(true, false) == "1" && missionSvCheats(false, false).empty() && missionSvCheats(true, true).empty());
    // Texts: English and Polish (Windows-1250) for every key the page uses.
    const auto en = openIw2Strings("english"), pl = openIw2Strings("polish");
    CHECK(en.size() == pl.size() && en.size() == 4);
    for (size_t i = 0; i < en.size() && i < pl.size(); ++i) CHECK(en[i].first == pl[i].first && !en[i].second.empty() && !pl[i].second.empty());
    CHECK(en[2].second == "Red on kill" && pl[2].second != en[2].second);
    {
        const std::string src = openIw2MenuSource(false);
        std::set<std::string> keys;
        for (const auto& [k, t] : en) keys.insert("@" + k);
        for (size_t p = src.find("\"@IW2_"); p != std::string::npos; p = src.find("\"@IW2_", p + 1)) CHECK(keys.count(src.substr(p + 1, src.find('"', p + 1) - p - 1)) == 1);
    }

    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs game;
        game.mountDirectory(main);
        for (const bool mp : {false, true}) {
            MenuLibrary lib(game);
            lib.loadList(mp ? "ui_mp/menus.txt" : "ui/menus.txt");
            CHECK(lib.errors().empty());
            CHECK(addOpenIw2Options(lib, mp));
            CHECK(lib.errors().empty());
            const Menu* opt = lib.find("options_menu");
            const Menu* page = lib.find("iw2_options");
            CHECK(opt && page);
            if (!opt || !page) continue;
            const Item* entry = byName(*opt, "iw2_options_button");
            CHECK(entry && hasCommand(entry->action, "open", "iw2_options"));
            CHECK(byName(*page, "iw2_hitmarker") && (byName(*page, "iw2_cheats") == nullptr) == mp);
            // The shipped Game Options page has no hit marker row any more.
            const Menu* g = lib.find("options_game");
            CHECK(g != nullptr);
            if (g)
                for (const auto& it : g->items) CHECK(it->dvar != "iw2_hitMarker");
        }
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
