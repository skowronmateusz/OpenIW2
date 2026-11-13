// The OpenIW2 options page (src/ui/iw2options.*): the hit marker option moved here from Game Options, the Cheats option (single
// player only), the entry in the options menu, the profile, and the rules of the developer cheat keys. With IW2_MAIN the shipped
// single-player and multiplayer menu sets are loaded and checked.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <iostream>
#include <set>
#include <string>

#include "assets/font.h"
#include "fs/vfs.h"
#include "ui/iw2options.h"
#include "ui/menu.h"
#include "ui/frontend.h"
#include "ui/menurun.h"
#include "ui/mpserver.h"
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
	rect 0 0 370 480
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
menuDef
{
	name options_sound
	rect 0 0 370 480
	itemDef
	{
		rect 0 15 100 13
		dvar "snd_volume"
		text "y"
		type 11
		visible 1
		name rowvalue
	}
}
})";

bool pageOpen(const MenuStack& st, const std::string& name) {
    for (const Menu* m : st.menus())
        if (m->name == name) return true;
    return false;
}

// Every navigation entry of options_menu (an entry opening an options page or the OpenIW2 page) clicked while any of the pages is open,
// the pointer going through the page first: the entry must be reachable (page rect), exactly its own page must be open afterwards, and
// options_menu stays. Returns the smallest number of entries reached from one start page.
int checkNavigation(MenuLibrary& lib) {
    Menu* om = lib.find("options_menu");
    if (!om) return 0;
    struct Entry {
        const Item* it;
        std::string target;
    };
    std::vector<Entry> entries;
    std::vector<std::string> pages;
    for (const auto& it : om->items) {
        if (it->type != 1 || it->decoration) continue;
        for (const auto& c : it->action)
            if (c.size() > 1 && c[0] == "open" && (c[1].rfind("options_", 0) == 0 || c[1] == "iw2_options") && lib.find(c[1])) {
                if (c[1] == "options_graphics_defaults" || c[1] == "options_control_defaults") continue; // popups of their pages
                entries.push_back({it.get(), c[1]});
                if (std::find(pages.begin(), pages.end(), c[1]) == pages.end()) pages.push_back(c[1]);
            }
    }
    std::map<std::string, int> perStart;
    for (const std::string& start : pages)
        for (const Entry& e : entries) {
            std::map<std::string, std::string> cv;
            MenuEnv env;
            env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
            env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
            MenuStack st(lib, env);
            st.open("options_menu");
            st.open(start);
            const float x = om->rect.x + e.it->rect.x + e.it->rect.w * 0.5f, y = om->rect.y + e.it->rect.y + e.it->rect.h * 0.5f;
            st.mouseMove(x, y);
            if (st.focus() != e.it) continue; // an entry another visible entry covers or a dvar hides (the shipped menu has such duplicates)
            if (!st.click(x, y)) continue;
            ++perStart[start];
            for (const std::string& p : pages)
                if (pageOpen(st, p) != (p == e.target)) std::cerr << "start " << start << " entry " << e.target << " page " << p << " open=" << pageOpen(st, p) << "\n";
            for (const std::string& p : pages) CHECK(pageOpen(st, p) == (p == e.target));
            CHECK(pageOpen(st, "options_menu") && st.focusedMenu() && st.focusedMenu()->name == e.target);
        }
    int fewest = 1 << 30;
    for (const std::string& start : pages) fewest = std::min(fewest, perStart[start]);
    return pages.empty() ? 0 : fewest;
}

void checkSet(bool multiplayer) {
    iw2::fs::Vfs vfs;
    MenuLibrary lib(vfs);
    lib.parseSource(kOptions, "ui/options.menu");
    Menu* opt = lib.find("options_menu");
    CHECK(opt && opt->items.size() == 3); // the Reset system defaults entry is hidden (menuhide.cpp)
    CHECK(addOpenIw2Options(lib, multiplayer));
    CHECK(addOpenIw2Options(lib, multiplayer)); // idempotent
    CHECK(opt && opt->items.size() == 4);
    if (!opt) return;
    // The entry sits below Game Options, the entries under it moved down by one line, Back (another column) stays.
    const Item* entry = byName(*opt, "iw2_options_button");
    const Item* game = byText(*opt, "@MENU_GAME_OPTIONS");
    const Item* back = byText(*opt, "@MENU_BACK");
    CHECK(entry && game && back && !byText(*opt, "@MENU_RESET_SYSTEM_DEFAULTS"));
    if (entry && game && back) {
        CHECK(entry->text == "@IW2_OPT_TITLE" && entry->rect.x == 400 && entry->rect.y == game->rect.y + 15 && game->rect.y == 270);
        CHECK(back->rect.y == 380);
        CHECK(hasCommand(entry->action, "open", "iw2_options") && !hasCommand(entry->action, "open", "options_game"));
    }
    // Opening another page closes the OpenIW2 page, and so does closing the options menu.
    const Item* sound = byText(*opt, "@MENU_SOUND");
    CHECK(sound && hasCommand(sound->action, "close", "iw2_options") && hasCommand(opt->onClose, "close", "iw2_options"));
    CHECK(checkNavigation(lib) == 3); // Sound, Game Options, OpenIW2 from each of the three pages
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
            CHECK(cl && cl->dvarTest == "ui_multiplayer" && cl->rect.y == ch->rect.y && ch->rect.y == 150);
        }
        // Show FPS: a yes/no row on the cvar the counter reads, in both menu sets (no hide condition).
        const Item* fps = byName(*page, "iw2_showfps");
        CHECK(fps && fps->type == 11 && fps->dvar == "iw2_showFps" && fps->dvarTest.empty() && fps->rect.y == 135);
        CHECK(byName(*page, "iw2_showfps_label") && byName(*page, "iw2_showfps_label")->decoration);
        // The note: wrapped decoration text inside the page.
        const Item* note = byName(*page, "iw2_note");
        CHECK(note && note->decoration && note->autoWrapped && note->text == "@IW2_OPT_NOTE" && note->rect.x + note->rect.w <= page->rect.w);
        // Open log folder: a button below the note in both sets that runs iw2_openlogs (docs/logging.md "Menu").
        const Item* logs = byName(*page, "iw2_openlogs");
        CHECK(logs && logs->type == 1 && logs->text == "@IW2_OPT_OPENLOGS" && hasCommand(logs->action, "exec", "iw2_openlogs") &&
              logs->rect.y > note->rect.y + 26 && !logs->decoration);
        // The page covers the left part only: the navigation column of the options menu (x >= 400) lies outside of it, as for the shipped pages.
        CHECK(page->rect.x == 0 && page->rect.w <= 385 && !page->fullScreen && !page->popup);
        for (const auto& it : page->items) CHECK(it->rect.x + it->rect.w <= page->rect.w);
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
    CHECK(registeredDefault("iw2_showFps") == "0" && isArchivedCvar("iw2_showFps") && isArchivedCvar("iw2_showfps"));
    CHECK(parseConfig(formatConfig({{"iw2_showFps", "1"}})).at("iw2_showFps") == "1");
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
    CHECK(en.size() == pl.size() && en.size() == 10);
    for (size_t i = 0; i < en.size() && i < pl.size(); ++i) CHECK(en[i].first == pl[i].first && !en[i].second.empty() && !pl[i].second.empty());
    CHECK(en[2].second == "Red on kill" && pl[2].second != en[2].second);
    {
        // The note says the settings are OpenIW2 additions that differ from the original game, in both languages; Polish uses Windows-1250.
        std::string noteEn, notePl, fpsPl;
        for (const auto& [k, t] : en) if (k == "IW2_OPT_NOTE") noteEn = t;
        for (const auto& [k, t] : pl) { if (k == "IW2_OPT_NOTE") notePl = t; if (k == "IW2_OPT_SHOWFPS") fpsPl = t; }
        CHECK(noteEn.find("OpenIW2") != std::string::npos && noteEn.find("original game") != std::string::npos);
        CHECK(notePl.find("OpenIW2") != std::string::npos && notePl.find("oryginalnej gry") != std::string::npos && notePl.find('\xBF') != std::string::npos);
        CHECK(fpsPl == "Poka\xBF FPS");
        std::string logsEn, logsPl;
        for (const auto& [k, t] : en) if (k == "IW2_OPT_OPENLOGS") logsEn = t;
        for (const auto& [k, t] : pl) if (k == "IW2_OPT_OPENLOGS") logsPl = t;
        CHECK(logsEn == "Open log folder" && logsPl == "Otw\xF3rz folder log\xF3w");
    }
    {
        // Polish corrections of two shipped strings; no override for other languages; Localization::set replaces an existing text.
        CHECK(openIw2StringOverrides("english").empty() && openIw2StringOverrides("german").empty());
        const auto ov = openIw2StringOverrides("polish");
        CHECK(ov.size() == 2 && ov[0].first == "MENU_STEADY_SNIPER_RIFLE" && ov[0].second == "Wstrzymaj oddech (snajper)");
        CHECK(ov.size() == 2 && ov[1].first == "MENU_TOGGLE_AIM_DOWN_THE_SIGHT" && ov[1].second == "Sta\xB3" "e celowanie przez przyrz\xB9" "dy" &&
              ov[1].second.find('\'') == std::string::npos);
        iw2::assets::Localization l;
        l.add("MENU_STEADY_SNIPER_RIFLE", "Karabin snajperski");
        for (const auto& [k, t] : ov) l.set(k, t);
        CHECK(l.find("menu_steady_sniper_rifle") && *l.find("menu_steady_sniper_rifle") == ov[0].second);
    }
    {
        // Key binding labels: the key.str references are KEY_UNBOUND / KEY_OR (the words "Unbound" / "or" in the strings of the language).
        const std::map<std::string, std::string> loc = {{"KEY_UNBOUND", "Nieprzypisany"}, {"KEY_OR", "lub"}, {"KEY_E", "E"}, {"KEY_F", "F"}, {"UNBOUND", "wrong"}, {"OR", "wrong"}};
        const auto find = [&](const std::string& k) -> const std::string* { const auto it = loc.find(k); return it == loc.end() ? nullptr : &it->second; };
        CHECK(keyBindingLabel({}, find) == "Nieprzypisany");
        CHECK(keyBindingLabel({"KEY_E"}, find) == "E");
        CHECK(keyBindingLabel({"KEY_E", "KEY_F"}, find) == "E lub F");
        CHECK(keyBindingLabel({"KEY_Q"}, find) == "KEY_Q"); // unknown reference as it is
        CHECK(keyBindingLabel({}, [](const std::string&) -> const std::string* { return nullptr; }) == "KEY_UNBOUND");
    }
    {
        const std::string src = openIw2MenuSource(false);
        std::set<std::string> keys;
        for (const auto& [k, t] : en) keys.insert("@" + k);
        for (size_t p = src.find("\"@IW2_"); p != std::string::npos; p = src.find("\"@IW2_", p + 1)) CHECK(keys.count(src.substr(p + 1, src.find('"', p + 1) - p - 1)) == 1);
    }

    {
        // Every menu openiw2 builds highlights like the shipped ones: the gold focus colour of the shipped menus (.98 .827 .58) and, on
        // its buttons and option rows, the hover sound; hovering an item focuses it.
        iw2::fs::Vfs vfs;
        MenuLibrary lib(vfs);
        lib.parseSource(openIw2MenuSource(false), "openiw2/options.menu");
        lib.parseSource(iw2::ui::mpext::menuSource(), "openiw2/createserver_ext.menu");
        int rows = 0;
        for (const auto& m : lib.menus()) {
            CHECK(m->focusColor[0] == 0.98f && m->focusColor[1] == 0.827f && m->focusColor[2] == 0.58f && m->focusColor[3] == 1.0f);
            for (const auto& it : m->items) {
                if (it->decoration || !(it->type == 1 || it->type == 11 || it->type == 12)) continue;
                ++rows;
                CHECK(hasCommand(it->mouseEnter, "play", "mouse_over"));
                MenuEnv env;
                env.getCvar = [](const std::string&) { return std::string(); };
                env.setCvar = [](const std::string&, const std::string&) {};
                MenuStack st(lib, env);
                st.open(m->name);
                st.mouseMove(m->rect.x + it->rect.x + 3, m->rect.y + it->rect.y + 3);
                CHECK(st.focus() == it.get());
            }
        }
        CHECK(lib.menus().size() == 3 && rows >= 8);
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
            CHECK(checkNavigation(lib) >= 6); // the visible entries from every page, the OpenIW2 page included
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
