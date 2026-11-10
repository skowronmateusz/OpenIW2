// Multiplayer "create server" front end (docs/spec/mp-ui.md): the gametype list, the arena map list, the create-server feeder and
// ownerdraw, the StartServer command, and the openiw2 extension (bots, weapon classes).
#pragma once

#include "fs/vfs.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace iw2::ui {

class MpServerSetup {
public:
    // A gametype script maps/mp/gametypes/<name>.gsc; `display` is the first token of maps/mp/gametypes/<name>.txt (a localized string
    // key such as MPUI_DEATHMATCH) or the name itself.
    struct Gametype {
        std::string name, display;
    };
    // One map block of an mp/*.arena file.
    struct Arena {
        std::string longName, map, loadScreen; // entry +0, +4, +8: "loadscreen_<map>"
        unsigned mask = 0;                      // +0x14: bit i = gametype i is listed (all bits without a gametype key)
        bool active = false;                    // +0xa0: allowed for the selected gametype
    };
    static constexpr int kMaxGametypes = 32; // 0x537da5
    static constexpr int kMaxArenas = 128;   // 0x54796d
    static constexpr int kFeederAllMaps = 4; // FEEDER_ALLMAPS
    static constexpr int kOwnerDrawNetGametype = 245; // UI_NETGAMETYPE
    static constexpr int kOwnerDrawStartMapCinematic = 255; // UI_STARTMAPCINEMATIC

    using GetCvar = std::function<std::string(const std::string&)>;
    using SetCvar = std::function<void(const std::string&, const std::string&)>;

    // Gametype list (0x537ce0). Returns the warnings the exe prints.
    std::vector<std::string> loadGametypes(const fs::Vfs& vfs);
    // Parses the arena text of one file into the list (0x547650); exposed for tests.
    void parseArenas(const std::string& text);
    // uiScript loadArenas (0x5352ce): the arena files, the gametype of g_gametype, the map filter and the first selection.
    // Returns the row the createserver_maps list selects.
    int loadArenas(const fs::Vfs& vfs, const GetCvar& get, const SetCvar& set);

    // UI_NETGAMETYPE key (0x533b70): step +1 (mouse1 / Enter) or -1 (mouse2), wrapping. Returns the row of the current map
    // in the filtered list (for the feeder selection).
    int cycleGametype(int step, const GetCvar& get, const SetCvar& set);
    // The localized-string key of the selected gametype (the ownerdraw's text).
    std::string gametypeLabel(const GetCvar& get) const;

    // FEEDER_ALLMAPS: rows are the active arenas in list order (0x537550 text = long name; 0x537b00 select).
    int mapCount() const;
    std::string mapText(int row) const;
    int arenaOfRow(int row) const; // -1 if out of range
    int rowOfArena(int arena) const;
    void selectMap(int row, const SetCvar& set); // ui_currentMap and ui_currentNetMap = arena index
    std::string loadScreenOfCurrent(const GetCvar& get) const;

    // uiScript StartServer (0x534d93) without the PunkBuster popup: cg_thirdPerson 0, dedicated from ui_dedicated, g_gametype from
    // ui_netGametype, and the returned console text "wait ; wait ; map <map>" (empty when there is no map).
    std::string startServer(const GetCvar& get, const SetCvar& set) const;

    const std::vector<Gametype>& gametypes() const { return gametypes_; }
    const std::vector<Arena>& arenas() const { return arenas_; }

private:
    int intCvar(const GetCvar& get, const std::string& name) const;
    void markActive(int gametype); // 0x536320
    std::vector<Gametype> gametypes_;
    std::vector<Arena> arenas_;
};

// Join-server screen (docs/spec/mp-ui.md "Join server").
constexpr int kOwnerDrawNetSource = 220;        // UI_NETSOURCE
constexpr int kOwnerDrawNetMapCinematic = 246;  // UI_NETMAPCINEMATIC
constexpr int kOwnerDrawServerRefreshDate = 247; // UI_SERVERREFRESHDATE
constexpr int kOwnerDrawJoinGametype = 253;     // UI_JOINGAMETYPE
constexpr int kShowFavoriteServers = 0x4;       // UI_SHOW_FAVORITESERVERS
constexpr int kShowNotFavoriteServers = 0x1000; // UI_SHOW_NOTFAVORITESERVERS
// The ownerdrawFlag test of MP 0x533a93: the favorites item needs ui_netSource 2, the other one any other source.
bool mpOwnerDrawFlagShown(int flag, int netSource);
// The UI_NETSOURCE key handler (MP 0x533cb0): wraps over 0..2; step -1 is mouse 2.
int mpCycleNetSource(int netSource, int step);
// The localized key of a source: EXE_LOCAL, EXE_INTERNET, EXE_FAVORITES (table at MP 0x5dcb44).
std::string mpNetSourceLabel(int netSource);

// The text the MP load screen draws over the "connect" menu (docs/spec/mp-ui.md "Load screen"; VAs in the patch 1.3 MP
// executable 0x5390c0): the gametype's display string (0x532d90: the gametypes table, else the raw name; localized by the
// caller) centred at y 89 and the map's arena long name (0x532cd0: the arena files, else the map name) at y 119.
struct MpLoadScreenText {
    std::string gametype, map;
};
// `gametype` is the server's g_gametype, `map` the bare map name (mp_toujane). Reads maps/mp/gametypes/<gametype>.txt and the
// mp/*.arena files itself, so it works for any map start (menu, rotation, restart, a server joined).
MpLoadScreenText mpLoadScreenText(const fs::Vfs& vfs, const std::string& gametype, const std::string& map);
// Whether a finished map load stops at the SP "pregame" menu ("click to start the mission", cl_paused 1). SP: unless
// ui_autoContinue is set or the load was a quick reload (VAs in the patch 1.3 SP executable 0x449a2c / 0x4c32c4). An MP map
// (a name under mp/) never does: the patch 1.3 MP executable has neither the pregame menu nor ui_autoContinue, its client
// goes into the game when the load ends (docs/spec/mp-ui.md "Load screen").
bool loadWaitsForClick(const std::string& map, bool autoContinue, bool quickReload);
constexpr float kMpLoadScreenGametypeY = 89.0f, kMpLoadScreenMapY = 119.0f, kMpLoadScreenTextScale = 0.5f;

// Default value of a dvar the MP executable registers at start, for the cvars the multiplayer menus show (sv_hostname,
// sv_maxclients, sv_minPing, ...; docs/spec/mp-ui.md "Server dvars"). Case-insensitive; "" when not listed.
std::string mpRegisteredDefault(const std::string& name);

// The original keeps one dvar store for the menus and the server it runs, so a value the gametype script sets
// (setCvar, e.g. scr_killcam "1" when it was empty, _killcam.gsc:8-15) is what the create-server screen shows after
// that game. iw2view gives each world a copy of the menu cvars; this copies the multiplayer world's cvars back when the
// world ends (docs/spec/mp-ui.md "Server cvars").
template <class WorldCvars>
void carryWorldCvars(const WorldCvars& world, std::map<std::string, std::string>& ui) {
    for (const auto& [k, v] : world) ui[k] = v;
}
// Whether a server started from the menu (StartServer) takes LAN clients: always, as the original's listen server (docs/spec/net.md
// section 11); iw2ListenEnv is env IW2_LISTEN, "0" turns it off. The dedicated choice is a server of its own
// (docs/spec/mp-ui.md "Dedicated start").
bool menuStartListens(const char* iw2ListenEnv);

// ---- openiw2 extension: bots and weapon classes on the create-server screen (not in the original game) ----
namespace mpext {
// Weapon classes offered by the extension panel (toggle cvar ui_iw2_allow_<id>). The weapon classes rifle, bolt, smg, sniper,
// mg and shotgun are the game layer's classes (game::mpWeaponClassNames, World::mpWeaponClass decides each weapon's class);
// "grenades" is panel-only and maps to the two grenade cvars of _weapons.gsc.
struct WeaponClass {
    const char* id;    // cvar suffix: ui_iw2_allow_<id>
    const char* label; // panel text key (IW2_CLASS_*)
};
const std::vector<WeaponClass>& weaponClasses();
// The list the panel's toggles make, e.g. "rifle,bolt,grenades" (the primary classes and "grenades" that are on; empty when
// all are off). The caller checks it with game::mpParseWeaponList before it starts the server: a list without a primary class,
// or one that leaves a nation without a weapon, is refused with a message (a player could not pick a weapon and join).
std::string weaponList(const MpServerSetup::GetCvar& get);
// StartServer: iw2_weaponclasses = the enabled weapon classes ("none" when all are off), which the game turns into the
// scr_allow_* cvars at game start (World::mpStart -> mpSetAllowedWeaponClasses), and the grenade cvars from "grenades".
void applyWeaponClasses(const MpServerSetup::GetCvar& get, const MpServerSetup::SetCvar& set);
// Defaults of the extension cvars that are still unset (classes on, 0 bots, skill 1).
void initDefaults(const MpServerSetup::GetCvar& get, const MpServerSetup::SetCvar& set);
// Bot count for the listen server: ui_iw2_bots clamped to [0, sv_maxclients - 1] (no clamp when sv_maxclients is unset).
int botCount(const MpServerSetup::GetCvar& get);
int botSkill(const MpServerSetup::GetCvar& get); // ui_iw2_botskill clamped to 0..3
// Source of the extension menu "iw2_createserver_ext" (openiw2's own menu text, parsed with MenuLibrary::parseSource).
const char* menuSource();
// openiw2's own localized strings of the extension panel (keys IW2_*), English or Polish (language "polish"); the text is
// in the game's single-byte code page (Windows-1250 for Polish). Added to the game's localization, never replacing a key.
std::vector<std::pair<std::string, std::string>> localizedStrings(const std::string& language);
} // namespace mpext

} // namespace iw2::ui
