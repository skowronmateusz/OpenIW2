// openiw2 additions to the option menus: the "OpenIW2" page. The shipped menu files are not part of the repository and are never
// modified on disk; after they are loaded, the page (own menu text, src/ui/iw2options.cpp) is parsed into the library and an entry
// leading to it is added to the options menu in memory.
#pragma once

#include <string>

#include "ui/menu.h"

namespace iw2::ui {

// Adds the page "iw2_options" and an "OpenIW2" entry in the options menu ("options_menu", below Game Options; the entries under it move
// down one line). The page holds the Hit marker option (cvar iw2_hitMarker) and, unless `multiplayer`, the Cheats option (cvar
// iw2_cheats; single player only: the row is not in the multiplayer menu set and is also hidden by ui_multiplayer). Returns false,
// and changes nothing, when the options menu or its Game Options entry is not found (the options are then still settable with
// --cvar). Calling it twice adds the page once.
bool addOpenIw2Options(MenuLibrary& lib, bool multiplayer);

// The page as menu text (without the shipped menu files).
std::string openIw2MenuSource(bool multiplayer);

// The page's texts as (key, text) pairs for the language; Polish in Windows-1250.
std::vector<std::pair<std::string, std::string>> openIw2Strings(const std::string& language);

// The developer cheat keys (L fullbright, N walk/fly, F10 god cycle) and the sv_cheats of a mission follow the Cheats option, in
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
