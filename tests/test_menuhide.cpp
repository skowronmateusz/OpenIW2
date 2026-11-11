// Hidden multiplayer menu entries (src/ui/menuhide.*, docs/spec/mp-ui.md "Hidden entries"). The synthetic part always runs;
// with IW2_MAIN the shipped ui_mp menu set is loaded and every hidden path is checked. Debug: IW2_HIDE_TRACE=1 lists what the
// table removed, IW2_HIDE_DUMP=<menu> lists that menu's items.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "fs/vfs.h"
#include "ui/frontend.h"
#include "ui/menu.h"
#include "ui/menuhide.h"

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

bool lowerEq(std::string a, std::string b) {
    for (auto& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
    return a == b;
}

const Item* byName(const Menu& m, const std::string& n) {
    for (const auto& it : m.items)
        if (it->name == n) return it.get();
    return nullptr;
}


// The menu parser reads one property per line: break the compact one-line itemDefs of the test sources.
std::string lines(std::string s) {
    std::string out;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '{') {
            out += "\n{\n";
            ++depth;
        } else if (c == '}') {
            out += "\n}\n";
            --depth;
        } else if (c == ' ' && depth >= 2) {
            // property keywords start a new line
            static const char* const keys[] = {"name ", "type ", "rect ", "origin ", "visible ", "dvartest ", "showDvar", "hideDvar", "decoration", "action", "text ", "dvar ", "execKey"};
            bool kw = false;
            for (const char* k : keys)
                if (s.compare(i + 1, std::strlen(k), k) == 0) kw = true;
            out += kw ? "\n" : " ";
        } else out += c;
    }
    return out;
}

struct Env {
    std::map<std::string, std::string> cv;
    MenuEnv env;
    std::vector<std::string> responses;
    Env() {
        env.getCvar = [this](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
        env.setCvar = [this](const std::string& n, const std::string& v) { cv[n] = v; };
        env.scriptMenuResponse = [this](const std::string&, const std::string& r) { responses.push_back(r); };
    }
};

// The in-game menu shape: five 24-high choices, one of them (the vote) listed twice for its enabled / disabled look, the mute
// choice, and a bar at the bottom that must not move.
const char* kIngame = R"({
	menuDef
	{
		name "ingame"
		visible 1
		rect 0 0 640 480
		execKey "3" { scriptMenuResponse "muteplayer" }
		execKey "4" { scriptMenuResponse "callvote" }
		itemDef { name button_changeweapon type 1 rect 0 0 128 24 origin 80 84 visible 1 action { setdvar t_weapon 1 } }
		itemDef { name button_changeteam type 1 rect 0 0 128 24 origin 80 108 visible 1 action { setdvar t_team 1 } }
		itemDef { name button_muteplayer type 1 rect 0 0 128 24 origin 80 132 visible 1 action { scriptMenuResponse "muteplayer" } }
		itemDef { name button_callvote type 1 rect 0 0 128 24 origin 80 156 visible 1 dvartest "ui_allowvote" showDvar { "1" } action { scriptMenuResponse "callvote" } }
		itemDef { name button_callvote visible 1 type 1 origin 80 156 rect 0 0 128 24 decoration dvartest "ui_allowvote" hideDvar { "1" } }
		itemDef { name button_leavegame type 1 rect 0 0 128 24 origin 80 180 visible 1 action { setdvar t_leave 1 } }
		itemDef { name bar type 0 rect -128 416 896 2 visible 1 decoration }
	}
	menuDef { name "callvote" visible 1 rect 0 0 640 480 itemDef { name button_restartmap visible 1 type 1 rect 0 0 100 20 } }
	menuDef { name "other" rect 0 0 640 480 visible 1 itemDef { name goto_vote type 1 rect 0 0 100 20 visible 1 action { open callvote } } itemDef { name ok type 1 rect 0 30 100 20 visible 1 } }
})";

// A settings list: label and control on one line, 15 apart.
const char* kSettings = R"({
	menuDef
	{
		name "createserver_serversettings"
		rect 0 0 640 480
		visible 1
		itemDef { text "@A" type 1 rect 0 240 1 13 origin 100 0 visible 1 decoration }
		itemDef { type 11 dvar "sv_a" rect 0 240 320 13 origin 100 0 visible 1 }
		itemDef { text "@VOTE" type 1 rect 0 255 1 13 origin 100 0 visible 1 decoration }
		itemDef { type 11 dvar "g_allowvote" rect 0 255 320 13 origin 100 0 visible 1 }
		itemDef { text "@B" type 1 rect 0 270 1 13 origin 100 0 visible 1 decoration }
		itemDef { type 11 dvar "sv_b" rect 0 270 320 13 origin 100 0 visible 1 }
		itemDef { text "@C" type 1 rect 0 285 1 13 origin 100 0 visible 1 decoration }
		itemDef { type 11 dvar "sv_c" rect 0 285 320 13 origin 100 0 visible 1 }
	}
})";

std::vector<std::string> focusOrder(MenuStack& st, const std::string& menu, int steps) {
    std::vector<std::string> out;
    st.open(menu);
    for (int i = 0; i < steps; ++i) {
        out.push_back(st.focus() ? st.focus()->name : std::string("-"));
        st.key(MenuStack::Key::Down);
    }
    return out;
}

} // namespace

int main() {
    iw2::fs::Vfs none;
    {
        MenuLibrary lib(none);
        lib.parseSource(lines(kIngame), "ui_mp/scriptmenus/ingame.menu");
        const Menu* m = lib.find("ingame");
        CHECK(m != nullptr);
        if (m) {
            CHECK(!byName(*m, "button_callvote") && !byName(*m, "button_muteplayer"));
            CHECK(m->execKeys.empty()); // keys 3 and 4 are gone
            const Item* leave = byName(*m, "button_leavegame");
            const Item* team = byName(*m, "button_changeteam");
            CHECK(leave && team && leave->rect.y == team->rect.y + 24); // no gap where the two choices were
            const Item* bar = byName(*m, "bar");
            CHECK(bar && bar->rect.y == 416); // the bottom bar does not move
            CHECK(m->items.size() == 4);
        }
        CHECK(lib.find("callvote") == nullptr); // the vote menu is not registered
        const Menu* other = lib.find("other");
        CHECK(other && !byName(*other, "goto_vote") && byName(*other, "ok")); // the entry that opened it is gone

        Env e;
        e.cv["ui_allowvote"] = "1"; // the server allows votes: still nothing to focus
        MenuStack st(lib, e.env);
        CHECK(!st.open("callvote")); // an open of the hidden menu finds nothing
        CHECK(st.empty());
        const auto order = focusOrder(st, "ingame", 4);
        CHECK(order[0] == "button_changeweapon" && order[1] == "button_changeteam" && order[2] == "button_leavegame" && order[3] == "button_changeweapon");
        // The pointer: the leave choice answers at its new place, nothing answers where the vote was.
        CHECK(st.click(90, 140));
        CHECK(e.cv["t_leave"] == "1");
        CHECK(!st.click(90, 160));
        CHECK(!st.click(90, 190));
        CHECK(!st.execKey('4') && !st.execKey('3'));
        CHECK(e.responses.empty());
    }
    // A file that is not below ui_mp/ (the single-player menus) is never touched.
    {
        MenuLibrary lib(none);
        lib.parseSource(lines(kIngame), "ui/scriptmenus/ingame.menu");
        const Menu* m = lib.find("ingame");
        CHECK(m && byName(*m, "button_callvote") && byName(*m, "button_muteplayer") && m->execKeys.size() == 2);
        CHECK(lib.find("callvote") != nullptr);
    }
    // A removed settings row takes its label with it and the rows below move up by one line.
    {
        MenuLibrary lib(none);
        lib.parseSource(lines(kSettings), "ui_mp/createserver.menu");
        const Menu* m = lib.find("createserver_serversettings");
        CHECK(m && m->items.size() == 6);
        if (m) {
            std::vector<float> ys;
            for (const auto& it : m->items) {
                CHECK(it->dvar != "g_allowvote" && it->text != "@VOTE");
                ys.push_back(it->rect.y);
            }
            std::sort(ys.begin(), ys.end());
            CHECK((ys == std::vector<float>{240, 240, 255, 255, 270, 270}));
        }
    }

    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs game;
        game.mountDirectory(main);
        MenuLibrary mp(game);
        mp.loadList("ui_mp/menus.txt");
        mp.loadList("ui_mp/ingame.txt");
        for (const char* f : {"callvote", "ingame", "muteplayer", "quickcommands", "quickresponses", "quickstatements", "serverinfo_ctf", "serverinfo_dm", "serverinfo_hq",
                              "serverinfo_sd", "serverinfo_tdm", "team_americangerman", "team_britishgerman", "team_russiangerman", "weapon_american", "weapon_british",
                              "weapon_german", "weapon_russian"})
            mp.loadFile(std::string("ui_mp/scriptmenus/") + f + ".menu");
        mp.loadFile("ui_mp/wm_quickmessage.menu");
        if (std::getenv("IW2_HIDE_TRACE"))
            for (const auto& l : mp.hideTrace()) std::cout << l << "\n";
        if (const char* d = std::getenv("IW2_HIDE_DUMP"))
            if (const auto* m = mp.find(d))
                for (const auto& it : m->items)
                    std::cout << "x=" << it->rect.x << " y=" << it->rect.y << " w=" << it->rect.w << " h=" << it->rect.h << " t=" << it->type << " vis=" << it->visible << " name=" << it->name << " text=" << it->text << " dvar=" << it->dvar << " test=" << it->dvarTest << "\n";
        CHECK(mp.errors().empty());

        // Hidden menus are not there, and nothing in the loaded set leads to them or uses their commands or dvars.
        for (const char* n : {"callvote", "changemap", "changegametype", "kickplayer", "muteplayer", "cdkey_menu", "startpb_popmenu", "joinpb_popmenu", "auconfirm", "options_voice", "mods_menu"})
            CHECK(mp.find(n) == nullptr && menuIsHidden(n));
        const std::set<std::string> badScripts = {"getcdkey", "verifycdkey", "loadmods", "runmod", "votemap", "votetypemap", "votetempban", "votekick", "muteplayer", "setpbclstatus"};
        const std::set<std::string> badDvars = {"g_allowvote", "ui_allowvote", "sv_voice", "sv_punkbuster", "cl_punkbuster", "vote yes", "vote no", "+talk", "ui_browsershowpunkbuster", "ui_browsermod",
                                                "cl_updateavailable", "ui_cdkeyvalid", "cdkey1", "cdkey2", "cdkey3", "cdkey4", "cdkey5"};
        const std::set<std::string> badResponses = {"callvote", "muteplayer"};
        int scanned = 0;
        for (const auto& m : mp.menus()) {
            auto scan = [&](const Script& s) {
                for (const auto& c : s) {
                    if (c.size() > 1 && lowerEq(c[0], "open") && menuIsHidden(c[1])) { std::cerr << m->name << " opens " << c[1] << "\n"; CHECK(false); }
                    if (c.size() > 1 && lowerEq(c[0], "uiscript")) {
                        std::string n = c[1];
                        for (auto& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                        if (badScripts.count(n)) { std::cerr << m->name << " uiScript " << n << "\n"; CHECK(false); }
                    }
                    if (c.size() > 1 && lowerEq(c[0], "scriptmenuresponse") && (badResponses.count(c[1]))) { std::cerr << m->name << " response " << c[1] << "\n"; CHECK(false); }
                }
            };
            for (const Script* s : {&m->onOpen, &m->onClose, &m->onEsc}) scan(*s);
            for (const auto& k : m->execKeys) scan(k.second);
            for (const auto& it : m->items) {
                ++scanned;
                for (const Script* s : {&it->action, &it->mouseEnter, &it->mouseExit, &it->onFocus, &it->leaveFocus, &it->accept, &it->doubleClick}) scan(*s);
                for (const auto& k : it->execKeys) scan(k.second);
                std::string dv = it->dvar, dt = it->dvarTest;
                for (auto& ch : dv) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                for (auto& ch : dt) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (badDvars.count(dv) || badDvars.count(dt)) { std::cerr << m->name << " item " << it->name << " dvar " << dv << dt << "\n"; CHECK(false); }
            }
        }
        CHECK(scanned > 500);

        // Settings and option lists keep one line per 15 pixels down their column (no gap).
        auto columnSteps = [&](const char* menu, float x, float step, float firstY) {
            const Menu* m = mp.find(menu);
            CHECK(m != nullptr);
            if (!m) return;
            std::set<float> ys;
            for (const auto& it : m->items)
                if (it->rect.x == x && it->rect.y >= firstY && it->type != 0 && it->rect.w > 0 && it->rect.h > 0 && it->rect.h < 20) ys.insert(it->rect.y);
            float prev = firstY - step;
            for (const float y : ys) { if (y != prev + step) std::cerr << menu << ": gap before y=" << y << "\n"; CHECK(y == prev + step); prev = y; }
            if (ys.size() < 5) std::cerr << menu << ": only " << ys.size() << " rows\n";
            CHECK(ys.size() >= 5);
        };
        columnSteps("options_multi", 50, 15, 120);
        columnSteps("createserver_serversettings", 50, 15, 120);
        columnSteps("settings_sw", 5, 15, 33);
        columnSteps("filter_popmenu", 10, 15, 75);
        // The options tab list keeps its 15 pixel steps right of the voice tab's place.
        if (const Menu* m = mp.find("options_menu")) {
            std::set<float> ys;
            for (const auto& it : m->items)
                if (it->rect.x == 400 && it->type == 1) ys.insert(it->rect.y);
            CHECK(ys.count(270) && ys.count(285) && ys.count(300) && !ys.count(315));
        }

        // The in-game menu of a running server that allows votes: three choices, in order, no vote, no mute.
        {
            const Menu* m = mp.find("ingame");
            CHECK(m != nullptr);
            Env e;
            e.cv["ui_allowvote"] = "1";
            e.cv["ui_allow_weaponchange"] = "1";
            MenuStack st(mp, e.env);
            const auto order = focusOrder(st, "ingame", 5);
            CHECK(order[0] == "button_changeweapon" && order[1] == "button_changeteam");
            for (const auto& n : order) CHECK(n != "button_callvote" && n != "button_muteplayer");
            CHECK(!st.execKey('4') && !st.execKey('3') && e.responses.empty());
            if (m) {
                const Item* team = byName(*m, "button_changeteam");
                const Item* leave = byName(*m, "button_leavegame");
                CHECK(team && leave && leave->rect.y == team->rect.y + 24);
            }
        }
        // The server list: no voice and PunkBuster headers or column text, the others side by side, same total width.
        if (const Menu* m = mp.find("joinserver")) {
            CHECK(!byName(*m, "Mod") && !byName(*m, "modicon") && !byName(*m, "modColumn"));
            CHECK(!byName(*m, "voice") && !byName(*m, "voiceicon") && !byName(*m, "voiceColumn") && !byName(*m, "pb") && !byName(*m, "pbicon") && !byName(*m, "pbColumn"));
            float prevEnd = -1;
            for (const char* n : {"password", "hardware", "server", "map", "Players", "Type", "pure", "Ping"}) {
                const Item* it = byName(*m, n);
                CHECK(it != nullptr);
                if (!it) continue;
                if (prevEnd >= 0) CHECK(it->rect.x - prevEnd >= 0 && it->rect.x - prevEnd <= 2);
                prevEnd = it->rect.x + it->rect.w;
            }
            CHECK(prevEnd == 612); // the header row still ends where the table ends
            for (const auto& it : m->items)
                if (it->type == 6 && it->feeder == 2) {
                    CHECK(it->columns.size() == 11);
                    CHECK(it->columns[6].hidden && it->columns[8].hidden && it->columns[9].hidden);
                    for (size_t i = 0; i < it->columns.size(); ++i) CHECK(i == 6 || i == 8 || i == 9 || !it->columns[i].hidden);
                    CHECK(it->columns[3].pos == 344 && it->columns[7].pos == 551 && it->columns[10].pos == 566);
                }
        }

        // The single-player option pages (kSpRules): rows without an effect in openiw2 are gone, with every dvartest variant, and the
        // rows below close up. Both menu sets load these pages, so the same holds in a running level.
        {
            MenuLibrary sp(game);
            sp.loadFile("ui/options_graphics.menu");
            sp.loadFile("ui/options_sound.menu");
            CHECK(sp.errors().empty());
            if (const char* d = std::getenv("IW2_HIDE_DUMP_SP"))
                if (const auto* m = sp.find(d))
                    for (const auto& it : m->items)
                        std::cout << "x=" << it->rect.x << " y=" << it->rect.y << " t=" << it->type << " name=" << it->name << " text=" << it->text << " dvar=" << it->dvar << " test=" << it->dvarTest << "\n";
            const std::set<std::string> gone = {"ui_r_texturemode", "ui_r_rendererpreference", "r_multigpu", "ui_sc_enable", "ui_r_aspectratio", "ui_snd_khz", "ui_mss_3d_provider"};
            for (const char* mn : {"options_graphics", "options_sound"}) {
                const Menu* m = sp.find(mn);
                CHECK(m != nullptr);
                if (!m) continue;
                for (const auto& it : m->items) {
                    std::string dv = it->dvar, dt = it->dvarTest;
                    for (auto& ch : dv) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    for (auto& ch : dt) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    if (gone.count(dv) || gone.count(dt)) { std::cerr << mn << ": " << dv << dt << " still there\n"; CHECK(false); }
                }
            }
            // Kept rows: the anti-aliasing row and the sync row are still there.
            if (const Menu* m = sp.find("options_graphics")) {
                bool aa = false, vsync = false;
                for (const auto& it : m->items) {
                    if (lowerEq(it->dvar, "ui_r_aasamples")) aa = true;
                    if (lowerEq(it->dvar, "ui_r_swapinterval")) vsync = true;
                }
                CHECK(aa && vsync);
                // The labels of the list run down the column at 15 pixel steps with no gap.
                std::set<float> ys;
                for (const auto& it : m->items)
                    if (it->rect.x == 50 && it->type == 1 && it->rect.y >= 120 && it->rect.y <= 240) ys.insert(it->rect.y);
                float prev = 105;
                for (const float y : ys) { if (y != prev + 15) std::cerr << "options_graphics: gap before y=" << y << "\n"; CHECK(y == prev + 15); prev = y; }
                CHECK(ys.size() == 9 && prev == 240);
            }
        }
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
