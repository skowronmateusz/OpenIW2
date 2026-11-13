// openiw2 additions to the option menus: the "OpenIW2" page. The shipped menu files are not part of the repository and are never
// modified on disk; after they are loaded, the page (own menu text, src/ui/iw2options.cpp) is parsed into the library and an entry
// leading to it is added to the options menu in memory.
#pragma once

#include <cctype>
#include <string>

#include "ui/menu.h"

namespace iw2::ui {

// The commands behind the three developer key rows of the OpenIW2 page. They are not engine commands: ClientInput only keeps keys for
// them (rebind, save, "Unbound" / "or" labels like every BIND row) and iw2view reads the bound key. Lowercase, like the shipped ones.
constexpr const char* kCheatCmdFullbright = "iw2_fullbright";
constexpr const char* kCheatCmdGod = "iw2_godmode";
constexpr const char* kCheatCmdNoclip = "iw2_noclip";
// The command of the page's "Open log folder" button (docs/logging.md): iw2view opens the log folder in Finder.
constexpr const char* kOpenLogsCommand = "iw2_openlogs";

// Adds the page "iw2_options" and an "OpenIW2" entry in the options menu ("options_menu", below Game Options; the entries under it move
// down one line). The page holds the Hit marker option (cvar iw2_hitMarker), Show FPS (iw2_showFps), a note, the "Open log folder"
// button (command iw2_openlogs), and, unless `multiplayer`, the Cheats option (cvar
// iw2_cheats; single player only: the row is not in the multiplayer menu set and is also hidden by ui_multiplayer). Returns false,
// and changes nothing, when the options menu or its Game Options entry is not found (the options are then still settable with
// --cvar). Calling it twice adds the page once.
bool addOpenIw2Options(MenuLibrary& lib, bool multiplayer);

// The page as menu text (without the shipped menu files).
std::string openIw2MenuSource(bool multiplayer);

// The page's texts as (key, text) pairs for the language; Polish in Windows-1250.
std::vector<std::pair<std::string, std::string>> openIw2Strings(const std::string& language);

// openiw2 corrections of single shipped strings, applied after the game's string files load (Localization::set); Polish only, own
// text, Windows-1250. Empty for other languages. The two key binding labels: the hold-breath row (MENU_STEADY_SNIPER_RIFLE) names
// its action instead of the weapon, and the aim toggle row (MENU_TOGGLE_AIM_DOWN_THE_SIGHT) loses a stray apostrophe.
std::vector<std::pair<std::string, std::string>> openIw2StringOverrides(const std::string& language);

enum class CheatKey { None, Fullbright, God, Noclip };
// The developer key action a bound command stands for (case-insensitive), None for any other command.
inline CheatKey cheatKeyFromCommand(const std::string& command) {
    std::string c = command;
    for (char& ch : c) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return c == kCheatCmdFullbright ? CheatKey::Fullbright : c == kCheatCmdGod ? CheatKey::God : c == kCheatCmdNoclip ? CheatKey::Noclip : CheatKey::None;
}

// The developer cheat keys (fullbright, walk/fly, god cycle; default L, N, F10, rebindable on the OpenIW2 page) and the sv_cheats of a mission follow the Cheats option, in
// single player only. `extraSvCheats` is a --cvar sv_cheats=1. A multiplayer session (a multiplayer front end, a network or offline
// multiplayer game) never allows them, whatever the option says.
constexpr bool cheatKeysAllowed(bool cheatsOption, bool extraSvCheats, bool multiplayerSession) {
    return !multiplayerSession && (cheatsOption || extraSvCheats);
}
// The sv_cheats a single-player mission gets at its start from the option ("" = leave as it is).
inline std::string missionSvCheats(bool cheatsOption, bool multiplayerSession) {
    return (cheatsOption && !multiplayerSession) ? "1" : "";
}

} // namespace iw2::ui
