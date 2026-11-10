// Menu entries the multiplayer build does not offer (docs/spec/mp-ui.md "Hidden entries").
//
// The shipped multiplayer menus lead to features this build has no server side for (voting, CD key, PunkBuster, auto update,
// voice chat, muting, mods). The game files are not altered: the table in menuhide.cpp is applied to every `ui_mp/` menu file
// when it is parsed, so a hidden item or menu is not in the loaded library at all. It is therefore not drawn, not focusable,
// not hit-tested, has no key, and a `show` command cannot bring it back; an `open` of a hidden menu finds nothing and does
// nothing, like a menu whose file is missing.
#pragma once

#include "ui/menu.h"

#include <string>

namespace iw2::ui {

// Applies the hide table to the menu `m` parsed from `file`. Returns false when the whole menu is hidden (the caller drops
// it). Only files below "ui_mp/" are touched. `trace` (optional) receives one line per removed item or menu.
bool applyHideRules(Menu& m, const std::string& file, std::vector<std::string>* trace = nullptr);

// True for a menu name on the hidden-menu list (used by the front end when a script asks for it by name).
bool menuIsHidden(const std::string& name);

} // namespace iw2::ui
