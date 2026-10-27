// Menu event scripts run by the front end (action / onESC commands of ui/*.menu items). The commands are the ones
// the shipped script menus use; each is tied to exe evidence in menurun.cpp.
#pragma once

#include "ui/menu.h"

#include <functional>
#include <string>

namespace iw2::ui {

struct MenuEnv {
    std::function<std::string(const std::string&)> getCvar;
    std::function<void(const std::string&, const std::string&)> setCvar;
    // "scriptMenuResponse x": the front end sends "cmd mr <index of the menu> x" to the game.
    std::function<void(const std::string& menu, const std::string& response)> scriptMenuResponse;
    std::function<void(const std::string& menu)> close;
    std::function<void(const std::string& alias)> play;
    // Front-end hooks (optional; set by MenuStack, src/ui/frontend.h). Names are the menu/item names of the commands.
    std::function<void(const std::string& alias)> startLoop; // Menu_Open: the menu's soundLoop alias (0x4cb803..0x4cb835)
    std::function<void()> stopLoop;
    std::function<void()> closeAll;                                    // "ingameclose" (UI_SetActiveMenu(0): close all, unpause)
    std::function<void(const std::string& menu)> open;                 // "open"
    std::function<void(const std::string& item)> setFocus;             // "setfocus"
    std::function<void(const std::string& dvar)> setFocusByDvar;       // "setfocusbydvar"
    std::function<void(const std::string& item, bool show)> showItem;   // "show" / "hide"
    std::function<bool()> savegameExists;  // "savegameshow/savegamehide": TODO(spec) exact exe semantics; unset = no save
    std::function<bool()> restartAvailable; // "restarthide": unset = available
    std::function<void(const std::string& cmd)> exec;                   // console text of "exec" / "execnow" other than toggle
    std::function<void(const std::string& name, const Command& args)> uiScript; // "uiScript" other than update
    // Width in 640x480 virtual pixels of the item's localized text at its font/scale (slider track position, widget value text).
    std::function<void(const std::string& command, const std::string& key)> bindKey; // BIND capture result; key "" = clear (Del)
    // DVARENUM items (exe 0x4cd720): the value strings of the enum dvar named by `dvarEnumList`; unset = "not an enum dvar".
    std::function<std::vector<std::string>(const std::string& enumDvar)> enumValues;
    std::function<float(const Item&)> textWidth;
};

// Runs the commands of `script` for a menu named `menuName` (the target of its scriptMenuResponse commands).
void runScript(const Script& script, const std::string& menuName, MenuEnv& env);

// UI start-up cvars: ui_mousePitch = (m_pitch < 0), exe 0x4c30db.
void initUiCvars(MenuEnv& env);
// Default of a dvar the exe registers at start (empty when unknown). mis_none, mis_01..mis_10: int, default 0, range 0..20, flags
// 0x1001 (archive), registered at 0x4d02b4..0x4d0540. TODO(spec): the other registered UI dvars (cg_drawCrosshair, ...).
std::string registeredDefault(const std::string& name);

// The clickable item under (x, y) in the 640x480 virtual screen (visible button items), or nullptr.
const Item* itemAt(const Menu& menu, float x, float y);

} // namespace iw2::ui
