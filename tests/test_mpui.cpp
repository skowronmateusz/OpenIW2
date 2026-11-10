// Multiplayer create-server front end (docs/spec/mp-ui.md): gametype and arena lists, the map feeder, the UI_NETGAMETYPE ownerdraw,
// openForGameType, NUMERICFIELD, StartServer and the openiw2 extension. With IW2_MAIN set, the shipped ui_mp menus are driven
// headless from createserver to the start command.
#include <unistd.h>
#include "ui/frontend.h"
#include "ui/mpserver.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

struct Harness {
    std::map<std::string, std::string> cv;
    std::vector<std::string> log;
    ui::MenuEnv env;
    ui::MpServerSetup setup;
    ui::MenuStack* stack = nullptr;
    std::string started;
    Harness(const fs::Vfs& vfs) {
        env.getCvar = [this](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
        env.setCvar = [this](const std::string& n, const std::string& v) { cv[n] = v; };
        env.exec = [this](const std::string& c) { log.push_back("exec " + c); };
        env.uiScript = [this, &vfs](const std::string& n, const ui::Command&) {
            log.push_back("uiScript " + n);
            if (n == "loadarenas") {
                setup.loadArenas(vfs, env.getCvar, env.setCvar);
                if (stack) stack->setFeederSelection(ui::MpServerSetup::kFeederAllMaps, 0);
                ui::mpext::initDefaults(env.getCvar, env.setCvar);
                env.open("iw2_createserver_extbutton");
            } else if (n == "startserver") {
                started = setup.startServer(env.getCvar, env.setCvar);
                ui::mpext::applyWeaponClasses(env.getCvar, env.setCvar);
            }
        };
        env.feederCount = [this](int f) { return f == ui::MpServerSetup::kFeederAllMaps ? setup.mapCount() : 0; };
        env.feederText = [this](int f, int r, int) { return f == ui::MpServerSetup::kFeederAllMaps ? setup.mapText(r) : std::string(); };
        env.feederSelect = [this](int f, int r) { if (f == ui::MpServerSetup::kFeederAllMaps) setup.selectMap(r, env.setCvar); };
        env.ownerDrawKey = [this](int od, int step) {
            if (od != ui::MpServerSetup::kOwnerDrawNetGametype) return false;
            const int row = setup.cycleGametype(step, env.getCvar, env.setCvar);
            if (row >= 0 && stack) stack->setFeederSelection(ui::MpServerSetup::kFeederAllMaps, row);
            return true;
        };
        env.ownerDrawText = [this](int od) { return od == ui::MpServerSetup::kOwnerDrawNetGametype ? setup.gametypeLabel(env.getCvar) : std::string(); };
    }
};

// Centre of an item of an open menu, in virtual pixels.
bool centre(const ui::MenuStack& st, const std::string& menu, const std::string& item, float& x, float& y, int ownerDraw = 0) {
    for (const ui::Menu* m : st.menus()) {
        if (m->name != menu) continue;
        for (const auto& it : m->items)
            if ((!item.empty() && it->name == item) || (ownerDraw && it->ownerDraw == ownerDraw)) {
                x = m->rect.x + it->rect.x + it->rect.w * 0.5f, y = m->rect.y + it->rect.y + it->rect.h * 0.5f;
                return true;
            }
    }
    return false;
}

void synthetic() {
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_mpui_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps/mp/gametypes");
    std::filesystem::create_directories(dir / "mp");
    std::filesystem::create_directories(dir / "ui");
    for (const char* g : {"dm", "sd", "tdm", "_helper"}) std::ofstream(dir / "maps/mp/gametypes" / (std::string(g) + ".gsc")) << "main() {}\n";
    std::ofstream(dir / "maps/mp/gametypes/dm.txt") << "\"KEY_DM\"\n";
    std::ofstream(dir / "mp/a.arena") << "// test\n{\n map \"mp_one\"\n longname \"One\"\n gametype \"dm SD\"\n}\n{\n map \"mp_two\" longname \"Two\" gametype \"tdm\"\n}\n";
    std::ofstream(dir / "mp/b.arena") << "{ map \"mp_three\" longname \"Three\" }\n";
    std::ofstream(dir / "ui/t.menu") << R"({
	menuDef
	{
		name under
		rect 0 0 640 480
		visible 1
		itemDef
		{
			name lower
			type 1
			rect 10 400 100 20
			visible 1
			action { setdvar hitlower 1; }
		}
	}
	menuDef
	{
		name over
		rect 0 0 640 200
		visible 1
		itemDef
		{
			name gt
			type 1
			dvar "ui_netGametypeName"
			rect 10 10 100 20
			visible 1
			action { openForGameType "settings_%s"; }
		}
		itemDef
		{
			name gtclose
			type 1
			dvar "ui_netGametypeName"
			rect 10 40 100 20
			visible 1
			action { closeForGameType "settings_%s"; }
		}
		itemDef
		{
			name num
			type 9
			dvar "scr_x_limit"
			maxchars 4
			rect 10 70 100 20
			visible 1
		}
		itemDef
		{
			name od
			type 12
			ownerdraw 245
			rect 10 100 100 20
			visible 1
		}
	}
	menuDef
	{
		name settings_dm
		rect 400 0 100 100
		visible 1
	}
	menuDef
	{
		name pop
		rect 0 0 640 100
		popup
		visible 1
		itemDef
		{
			name p
			type 1
			rect 0 0 10 10
			visible 1
		}
	}
})";
    fs::Vfs vfs;
    vfs.mountDirectory(dir);
    Harness h(vfs);

    // Gametype list: *.gsc without the leading-underscore helpers, in file-list order; display name from <name>.txt.
    const auto warn = h.setup.loadGametypes(vfs);
    CHECK(warn.empty());
    CHECK(h.setup.gametypes().size() == 3);
    CHECK(h.setup.gametypes()[0].name == "dm" && h.setup.gametypes()[0].display == "KEY_DM");
    CHECK(h.setup.gametypes()[1].name == "sd" && h.setup.gametypes()[1].display == "sd");

    // loadArenas: g_gametype unset -> "dm"; arenas: One (dm, sd), Two (tdm), Three (no gametype key = all).
    h.setup.loadArenas(vfs, h.env.getCvar, h.env.setCvar);
    CHECK(h.setup.arenas().size() == 3);
    CHECK(h.cv["ui_netGametype"] == "0" && h.cv["ui_netGametypeName"] == "dm");
    CHECK(h.setup.mapCount() == 2);
    CHECK(h.setup.mapText(0) == "One" && h.setup.mapText(1) == "Three");
    CHECK(h.cv["ui_currentNetMap"] == "0" && h.cv["ui_currentMap"] == "0");
    CHECK(h.setup.arenas()[0].loadScreen == "loadscreen_mp_one");
    h.setup.selectMap(1, h.env.setCvar);
    CHECK(h.cv["ui_currentNetMap"] == "2");

    // UI_NETGAMETYPE: dm -> sd keeps "Three" (row 1); sd -> tdm also lists Two; backwards wraps tdm -> sd -> dm -> tdm.
    CHECK(h.setup.cycleGametype(1, h.env.getCvar, h.env.setCvar) == 1);
    CHECK(h.cv["ui_netGametypeName"] == "sd" && h.cv["ui_currentNetMap"] == "2");
    h.setup.selectMap(0, h.env.setCvar); // One
    CHECK(h.setup.cycleGametype(1, h.env.getCvar, h.env.setCvar) == 0); // tdm: One is not allowed -> row 0 (Two)
    CHECK(h.cv["ui_netGametypeName"] == "tdm" && h.cv["ui_currentNetMap"] == "1");
    h.setup.cycleGametype(1, h.env.getCvar, h.env.setCvar);
    CHECK(h.cv["ui_netGametypeName"] == "dm"); // wrap forward
    h.setup.cycleGametype(-1, h.env.getCvar, h.env.setCvar);
    CHECK(h.cv["ui_netGametypeName"] == "tdm"); // wrap backward
    CHECK(h.setup.gametypeLabel(h.env.getCvar) == "tdm");

    // StartServer: cvars and the console text.
    h.cv["ui_dedicated"] = "0";
    CHECK(h.setup.startServer(h.env.getCvar, h.env.setCvar) == "wait ; wait ; map mp_two\n");
    CHECK(h.cv["g_gametype"] == "tdm" && h.cv["dedicated"] == "0" && h.cv["cg_thirdPerson"] == "0");
    // A menu start always takes LAN clients, dedicated or not; only IW2_LISTEN=0 turns that off.
    CHECK(ui::menuStartListens(nullptr) && ui::menuStartListens("1") && !ui::menuStartListens("0"));

    // g_gametype picks the gametype at loadArenas.
    h.cv["g_gametype"] = "sd";
    h.setup.loadArenas(vfs, h.env.getCvar, h.env.setCvar);
    CHECK(h.cv["ui_netGametype"] == "1" && h.setup.mapCount() == 2);

    // Menus: openForGameType / closeForGameType, NUMERICFIELD, ownerdraw keys, hit test through the stack, popups.
    ui::MenuLibrary lib(vfs);
    CHECK(lib.loadFile("ui/t.menu"));
    ui::MenuStack st(lib, h.env);
    h.stack = &st;
    h.cv["ui_netGametypeName"] = "dm";
    st.open("under");
    st.open("over");
    CHECK(st.click(20, 20)); // openForGameType "settings_%s" with ui_netGametypeName
    CHECK(st.top() && st.top()->name == "settings_dm");
    CHECK(st.click(20, 50)); // closeForGameType
    CHECK(st.top() && st.top()->name == "over");
    st.click(20, 80);
    st.textInput("1a2");
    CHECK(h.cv["scr_x_limit"] == "12"); // digits only
    h.cv["g_gametype"] = "dm";
    h.setup.loadArenas(vfs, h.env.getCvar, h.env.setCvar);
    st.click(20, 110); // ownerdraw 245 next
    CHECK(h.cv["ui_netGametypeName"] == "sd");
    st.clickBack(20, 110);
    CHECK(h.cv["ui_netGametypeName"] == "dm");
    float vx = 0, vy = 0;
    CHECK(centre(st, "over", "od", vx, vy));
    CHECK(st.valueLabel(*st.hover(vx, vy)) == "KEY_DM");
    // A click below "over" is out of its bounds: the lower menu takes the click and the focus (docs/spec/ui.md "Menu focus and
    // mouse routing"); while the pointer is inside the focused menu's rect the items of the other menus are not reachable.
    CHECK(st.click(20, 410));
    CHECK(h.cv["hitlower"] == "1");
    CHECK(st.focusedMenu() && st.focusedMenu()->name == "under");
    st.open("pop");
    h.cv["hitlower"] = "0";
    CHECK(!st.click(20, 410)); // a popup takes the pointer alone
    CHECK(h.cv["hitlower"] == "0");
    st.reset();
    CHECK(st.empty() && st.focus() == nullptr);

    // Extension: defaults, weapon classes, bot clamp, the panel menus parse.
    std::map<std::string, std::string> ec;
    auto eg = [&](const std::string& n) { return ec.count(n) ? ec[n] : std::string(); };
    auto es = [&](const std::string& n, const std::string& v) { ec[n] = v; };
    ui::mpext::initDefaults(eg, es);
    CHECK(ec["ui_iw2_allow_sniper"] == "1" && ec["ui_iw2_bots"] == "0" && ec["ui_iw2_botskill"] == "1");
    ec["ui_iw2_allow_sniper"] = "0";
    ui::mpext::applyWeaponClasses(eg, es);
    CHECK(ec["iw2_weaponclasses"] == "rifle,bolt,smg,mg,shotgun"); // the game layer's class list (World::mpStart)
    CHECK(ec["scr_allow_fraggrenades"] == "1" && ec["scr_allow_smokegrenades"] == "1");
    CHECK(ui::mpext::weaponList(eg) == "rifle,bolt,smg,mg,shotgun,grenades");
    for (const char* id : {"rifle", "bolt", "smg", "mg", "shotgun"}) ec[std::string("ui_iw2_allow_") + id] = "0";
    CHECK(ui::mpext::weaponList(eg) == "grenades"); // grenades alone: the caller refuses the start (test_serveroptions)
    ec["ui_iw2_allow_grenades"] = "0";
    CHECK(ui::mpext::weaponList(eg).empty());
    ui::mpext::applyWeaponClasses(eg, es);
    CHECK(ec["iw2_weaponclasses"] == "none" && ec["scr_allow_fraggrenades"] == "0");
    ec["ui_iw2_bots"] = "15";
    CHECK(ui::mpext::botCount(eg) == 15);
    ec["sv_maxclients"] = "8";
    CHECK(ui::mpext::botCount(eg) == 7);
    ec["ui_iw2_botskill"] = "9";
    CHECK(ui::mpext::botSkill(eg) == 3);
    // One dvar store: the world's values (set by the gametype) come back to the menu cvars.
    std::map<std::string, std::string> uiCv{{"scr_teambalance", "1"}}, worldCv{{"scr_teambalance", "1"}, {"scr_killcam", "1"}};
    ui::carryWorldCvars(worldCv, uiCv);
    CHECK(uiCv["scr_killcam"] == "1" && uiCv["scr_teambalance"] == "1");
    ui::MenuLibrary extLib(vfs);
    extLib.parseSource(ui::mpext::menuSource(), "ext.menu");
    CHECK(extLib.errors().empty());
    const ui::Menu* panel = extLib.find("iw2_createserver_ext");
    CHECK(panel && panel->items.size() == 19);
    CHECK(extLib.find("iw2_createserver_extbutton") != nullptr);
    std::filesystem::remove_all(dir);
}

void world(const char* main) {
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    Harness h(vfs);
    CHECK(h.setup.loadGametypes(vfs).empty());
    CHECK(h.setup.gametypes().size() == 5); // ctf dm hq sd tdm
    ui::MenuLibrary lib(vfs);
    CHECK(lib.loadList("ui_mp/menus.txt"));
    lib.parseSource(ui::mpext::menuSource(), "openiw2/createserver_ext.menu");
    // ui_mp/menus.txt loads five files the game data does not ship (ui/options_view, ui/options_defaults,
    // ui/options_credits, ui/rec_restart, ui_mp/in_rec_restart): warnings only, like the original, and the buttons that
    // open those menus (options.menu "open options_view" / "open options_defaults") do nothing.
    for (const auto& e : lib.errors()) std::cerr << "menu error: " << e << "\n";
    CHECK(lib.errors().empty());
    int notFound = 0;
    for (const auto& w : lib.warnings()) notFound += w.rfind("menu file not found: ", 0) == 0;
    CHECK(notFound == 5);
    ui::MenuStack st(lib, h.env);
    CHECK(!st.open("options_view") && !st.open("options_defaults") && !st.open("in_rec_restart"));
    h.stack = &st;
    CHECK(st.open("main"));
    // Background menu (docs/spec/ui.md "Background menu"): "bg" is on the stack only because main's onOpen opens it, and
    // main's onClose closes it; an in-game script menu opened on its own has no "bg" under it.
    auto hasBg = [&] { bool b = false; for (const ui::Menu* m : st.menus()) b = b || m->name == "bg"; return b; };
    CHECK(hasBg());
    st.closeAll();
    CHECK(st.menus().empty());
    CHECK(lib.loadFile("ui_mp/scriptmenus/team_britishgerman.menu"));
    CHECK(st.open("team_britishgerman") && !hasBg());
    CHECK(lib.find("team_britishgerman")->blurWorld == 5.0f);
    st.closeAll();
    CHECK(st.open("main"));
    // ui_mp/main.menu: the "start new server" button opens createserver (main.menu:277); its onOpen runs loadArenas.
    CHECK(st.open("createserver"));
    CHECK(h.cv["ui_netGametypeName"] == "dm");
    const int dmMaps = h.setup.mapCount();
    CHECK(dmMaps == 15); // mp/CoD2maps.arena lists dm on every map
    CHECK(h.setup.mapText(0) == "Beltot, France");
    float x = 0, y = 0;
    // UI_NETGAMETYPE on createserver_serversettings: next gametype.
    CHECK(centre(st, "createserver_serversettings", "", x, y, ui::MpServerSetup::kOwnerDrawNetGametype));
    st.mouseMove(x, y);
    st.click(x, y);
    CHECK(h.cv["ui_netGametypeName"] == "hq");
    CHECK(st.valueLabel(*st.focus()) == "MPUI_HEADQUARTERS");
    st.clickBack(x, y);
    CHECK(h.cv["ui_netGametypeName"] == "dm");
    // Map list row 2 (Burgundy).
    CHECK(centre(st, "createserver_maps", "maplist", x, y));
    const ui::Menu* maps = lib.find("createserver_maps");
    const ui::Item* list = nullptr;
    for (const auto& it : maps->items) if (it->name == "maplist") list = it.get();
    CHECK(list != nullptr);
    if (list) {
        const float rowY = maps->rect.y + list->rect.y + 1 + list->elementHeight * 2.5f;
        st.mouseMove(maps->rect.x + list->rect.x + 20, rowY);
        st.click(maps->rect.x + list->rect.x + 20, rowY);
        const int arena = std::atoi(h.cv["ui_currentNetMap"].c_str());
        CHECK(h.setup.arenas()[size_t(arena)].map == "mp_burgundy");
    }
    // "Game type settings" button: openForGameType settings_dm with the button's dvar ui_netGametypeName.
    CHECK(centre(st, "createserver_buttons", "button_serversettings", x, y));
    st.click(x, y);
    bool dm = false, ss = false;
    for (const ui::Menu* m : st.menus()) dm = dm || m->name == "settings_dm", ss = ss || m->name == "createserver_serversettings";
    CHECK(dm && !ss);
    // Extension button: the panel replaces the settings column; bots and a class toggle.
    CHECK(centre(st, "iw2_createserver_extbutton", "iw2_button_ext", x, y));
    st.click(x, y);
    bool panel = false;
    dm = false;
    for (const ui::Menu* m : st.menus()) dm = dm || m->name == "settings_dm", panel = panel || m->name == "iw2_createserver_ext";
    CHECK(panel && !dm);
    CHECK(centre(st, "iw2_createserver_ext", "iw2_bots", x, y));
    for (int i = 0; i < 4; ++i) st.click(x, y);
    CHECK(h.cv["ui_iw2_bots"] == "4");
    CHECK(centre(st, "iw2_createserver_ext", "iw2_allow_smg", x, y));
    st.click(x, y);
    CHECK(h.cv["ui_iw2_allow_smg"] == "0");
    // Start: StartServer.
    CHECK(centre(st, "createserver_buttons", "accept", x, y));
    st.click(x, y);
    CHECK(h.started == "wait ; wait ; map mp_burgundy\n");
    CHECK(h.cv["g_gametype"] == "dm");
    CHECK(h.cv["iw2_weaponclasses"] == "rifle,bolt,sniper,mg,shotgun"); // the game turns it into scr_allow_* (World::mpStart)
}

} // namespace

int main() {
    synthetic();
    if (const char* main = std::getenv("IW2_MAIN")) world(main);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "mpui ok\n";
    return failures ? 1 : 0;
}
