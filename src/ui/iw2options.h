// openiw2 additions to the shipped option menus. The shipped menu files are not part of the repository and are never modified on
// disk; after they are loaded, the entries of the openiw2 options are added to the parsed menus in memory.
#pragma once

#include "ui/menu.h"

namespace iw2::ui {

// Adds the row "Hit marker" (cvar iw2_hitMarker, Yes / No) at the end of the game options menu ("options_game"), built as a copy of the
// shipped crosshair row (label item + value item) so it takes the menu's own look. Returns false, and changes nothing, when the menu or
// the template row is not found (the option is then still settable with --cvar). Calling it twice adds the row once.
bool addOpenIw2Options(MenuLibrary& lib);

} // namespace iw2::ui
