// Multiplayer in-game menus (docs/spec/mp.md "Script menus", "Local client view"): openMenu / closeMenu reach the local
// client's front end as popupopen / popupclose, setClientCvar values are the local client's cvars, openscriptmenu and
// the menu responses drive the gametype (team, weapon, the weapon chosen for the next respawn), iprintln and client HUD
// elements of other clients stay off the local screen, the MP key bindings. Needs the game files (IW2_MAIN).

#include "game/clientinput.h"
#include "game/textmessage.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
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

using iw2::game::ClientCommand;
using iw2::game::Entity;
using iw2::game::World;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}
// The menu names the local client was told to open (popupopen <index>) and the number of closes, since the last call.
struct Popups {
    std::vector<std::string> opened;
    int closed = 0;
};
Popups popups(World& w) {
    Popups p;
    for (const ClientCommand& c : w.takeClientCommands()) {
        if (c.name == "popupopen") p.opened.push_back(w.menuNames()[size_t(std::atoi(c.arg.c_str()))]);
        if (c.name == "popupclose") ++p.closed;
    }
    return p;
}
bool hasPrint(World& w, const std::string& text) {
    for (const ClientCommand& c : w.takeClientCommands())
        if ((c.name == "gm" || c.name == "gmb") && c.arg.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    // The MP bindings: x runs openscriptmenu, ESC togglemenu (console commands for the front end), TAB +scores.
    {
        iw2::game::ClientInput in;
        in.loadConfig("unbindall\nbind x \"openscriptmenu ingame changeweapon\"\nbind ESCAPE \"togglemenu\"\nbind TAB \"+scores\"\n");
        in.key("X", true);
        in.key("X", false);
        in.key("ESCAPE", true);
        const auto cmds = in.takeConsoleCommands();
        CHECK(cmds.size() == 2 && cmds[0] == "openscriptmenu ingame changeweapon" && cmds[1] == "togglemenu");
        CHECK(in.takeConsoleCommands().empty());
        in.key("TAB", true);
        CHECK(in.objectivesHeld());
        in.key("TAB", false);
        CHECK(!in.objectivesHeld());
    }

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    w.takeClientCommands();
    CHECK(w.mpLocalClient() == 0);

    // Begin: the gametype opens the server info menu for the local client (precached name, popupopen with its index).
    const int c0 = w.mpConnect("local");
    Entity* p0 = w.mpClientEntity(c0);
    w.mpBegin(c0);
    run(w, 2);
    Popups p = popups(w);
    CHECK(p.opened.size() == 1 && p.opened[0] == "serverinfo_dm");
    // setClientCvar: the script main menu and the menu gates are the local client's cvars.
    CHECK(w.mpLocalCvar("g_scriptMainMenu") && *w.mpLocalCvar("g_scriptMainMenu") == "ingame");
    CHECK(w.mpLocalCvar("ui_allow_weaponchange") && *w.mpLocalCvar("ui_allow_weaponchange") == "0");
    CHECK(w.mpLocalCvar("G_SCRIPTMAINMENU") != nullptr); // cvar names are case-insensitive
    CHECK(w.mpLocalCvar("no_such_cvar") == nullptr);

    // A second client: its menus are not the local screen's.
    const int c1 = w.mpConnect("second");
    Entity* p1 = w.mpClientEntity(c1);
    w.mpBegin(c1);
    run(w, 2);
    CHECK(w.mpState().clients[1].openMenu == "serverinfo_dm");
    CHECK(popups(w).opened.empty());

    // serverinfo "close" -> team menu; team "axis" -> close + weapon menu (_menus.gsc:187-193, 144-158).
    w.mpMenuResponse(c0, "serverinfo_dm", "close");
    run(w, 1);
    p = popups(w);
    CHECK(p.closed >= 1 && !p.opened.empty() && p.opened.back() == "team_britishgerman");
    w.mpMenuResponse(c0, "team_britishgerman", "axis");
    run(w, 1);
    p = popups(w);
    CHECK(!p.opened.empty() && p.opened.back() == "weapon_german");
    CHECK(*w.mpLocalCvar("g_scriptMainMenu") == "weapon_german");
    CHECK(*w.mpLocalCvar("ui_allow_weaponchange") == "1");
    CHECK(w.mpLocalCvar("ui_allow_kar98k") && *w.mpLocalCvar("ui_allow_kar98k") == "1"); // scr_allow_* gates (_weapons.gsc)
    w.mpMenuResponse(c0, "weapon_german", "kar98k_mp");
    run(w, 2);
    CHECK(field(p0, "sessionstate") == "playing");
    CHECK(field(p0, "$weapon") == "kar98k_mp");

    // openscriptmenu ingame changeweapon (the x key): the ingame menu's response opens the team's weapon menu; the
    // name is matched case-insensitively.
    w.takeClientCommands();
    w.mpOpenScriptMenu("INGAME", "changeweapon");
    run(w, 1);
    p = popups(w);
    CHECK(!p.opened.empty() && p.opened.back() == "weapon_german");
    w.mpOpenScriptMenu("ingame", ""); // needs both arguments: nothing is sent
    run(w, 1);
    CHECK(popups(w).opened.empty());

    // A weapon chosen while alive is used at the next respawn (client 1 through the same menu responses).
    w.mpMenuResponse(c1, "serverinfo_dm", "close");
    run(w, 1);
    w.mpMenuResponse(c1, "team_britishgerman", "allies");
    run(w, 1);
    w.mpMenuResponse(c1, "weapon_british", "thompson_mp");
    run(w, 2);
    CHECK(field(p1, "$weapon") == "thompson_mp");
    w.mpMenuResponse(c1, "ingame", "changeweapon");
    run(w, 1);
    CHECK(w.mpState().clients[1].openMenu == "weapon_british");
    w.takeClientCommands();
    w.mpMenuResponse(c1, "weapon_british", "enfield_mp");
    run(w, 2);
    // dm.gsc:933 tells only that client ("self iprintln") which weapon it respawns with: not on the local screen.
    CHECK(!hasPrint(w, "MP_YOU_WILL_RESPAWN_WITH"));
    bool told = false;
    for (const auto& l : w.mpState().prints) told |= l.rfind("iprintln[1] MP_YOU_WILL_RESPAWN_WITH", 0) == 0;
    CHECK(told);
    CHECK(field(p1, "sessionstate") == "playing");
    CHECK(field(p1, "$weapon") == "thompson_mp"); // still the old weapon in this life
    w.mpDebugDamage(c1, c0, 500, "MOD_RIFLE_BULLET");
    run(w, 70);
    w.mpSetButtons(c1, 0x8);
    run(w, 3);
    w.mpSetButtons(c1, 0);
    CHECK(field(p1, "sessionstate") == "playing");
    CHECK(field(p1, "$weapon") == "enfield_mp");

    // The function form (dm.gsc:171, a connect) goes to every client, the local one included.
    w.takeClientCommands();
    std::vector<std::string> printed;
    w.print = [&printed](const std::string& m) { printed.push_back(m); };
    const int c2 = w.mpConnect("third");
    w.mpBegin(c2);
    run(w, 2);
    {
        const auto cmds = w.takeClientCommands();
        std::string arg;
        for (const ClientCommand& c : cmds)
            if (c.name == "gm" && c.arg.rfind("MP_CONNECTED", 0) == 0) arg = c.arg;
        CHECK(!arg.empty());
        // The client logs the message localized ("&&1" takes the name), never the raw &"MP_CONNECTED" reference.
        const std::string mpConnected = "&&1 joined";
        CHECK(arg.find("<object") == std::string::npos);
        const std::string text = iw2::game::localizeTextMessage(arg, [&](const std::string& k) { return k == "MP_CONNECTED" ? &mpConnected : nullptr; });
        CHECK(text == "third^7 joined"); // the player argument is its name + "^7"
        if (text != "third^7 joined") std::cerr << "localized: [" << text << "] from [" << arg << "]\n";
        for (const auto& m : printed) CHECK(m.find("&\"MP_CONNECTED\"") == std::string::npos);
    }

    // Client HUD elements: the gametype's per-client score elements (_hud_playerscore.gsc) are drawn only for their owner.
    int mine = 0, theirs = 0;
    for (const auto& h : w.hudElems()) {
        const auto owner = h->get("$owner");
        if (!owner.isNumber()) continue;
        (owner.asInt() == 0 ? mine : theirs)++;
        CHECK(w.mpHudElemVisible(h) == (owner.asInt() == 0));
    }
    CHECK(mine > 0 && theirs > 0);

    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
