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
    std::function<void()> closeAll;                                    // close every menu (front-end helper)
    // "ingameclose <menu>" acts only in a game (docs/spec/ui.md "ingameclose"): SP when cl_paused is non-zero, MP when the client
    // connection is active. Unset = the SP rule on cl_paused.
    std::function<bool()> ingameActive;
    // A multiplayer game: "ingameclose <menu>" closes the named menu only (MP handler 0x53b170 -> 0x53acd0). Unset or false: the SP
    // handler (0x4c8320) runs UI_SetActiveMenu(0), which clears cl_paused and closes every menu.
    std::function<bool()> multiplayer;
    std::function<void(const std::string& menu)> open;                 // "open"
    std::function<void(const std::string& item)> setFocus;             // "setfocus"
    std::function<void(const std::string& dvar)> setFocusByDvar;       // "setfocusbydvar"
    std::function<void(const std::string& item, bool show)> showItem;   // "show" / "hide"
    std::function<void(const std::string& item, bool out)> fadeItem;     // "fadein" / "fadeout" (0x4c8150 / 0x4c81a0 -> 0x4c7d80)
    // "setitemcolor <item> <forecolor|backcolor|bordercolor> r g b a" (0x4c7ba0).
    std::function<void(const std::string& item, const std::string& which, const Color& c)> setItemColor;
    std::function<bool()> savegameExists;  // "savegameshow/savegamehide": shown iff the file of sv_lastSaveGame is a loadable save (0x4c8f20); unset = no save
    std::function<bool()> restartAvailable; // "restarthide": unset = available
    std::function<void(const std::string& cmd)> exec;                   // console text of "exec" / "execnow" other than toggle
    std::function<void(const std::string& name, const Command& args)> uiScript; // "uiScript" other than update
    // Width in 640x480 virtual pixels of the item's localized text at its font/scale (slider track position, widget value text).
    std::function<void(const std::string& command, const std::string& key)> bindKey; // BIND capture result; key "" = clear (Del)
    // DVARENUM items (exe 0x4cd720): the value strings of the enum dvar named by `dvarEnumList`; unset = "not an enum dvar".
    std::function<std::vector<std::string>(const std::string& enumDvar)> enumValues;
    std::function<int()> language; // language id of the ui_language list (0 english, 1 french, 2 german, ... 7 polish); unset = 0
    std::function<float(const Item&)> textWidth;
    // LISTBOX feeders (exe 0x4c5f60 text, 0x4c6050 selection, count globals such as 0x11071b0): row count of a feeder, the text
    // of a (row, column) cell, and the notification that a row was selected. Unset = empty list.
    std::function<int(int feeder)> feederCount;
    std::function<std::string(int feeder, int row, int column)> feederText;
    std::function<void(int feeder, int row)> feederSelect;
    // Ownerdraw items (MP UI_NETGAMETYPE 245): a key or click step (+1 next, -1 previous); true if the ownerdraw took it.
    std::function<bool(int ownerDraw, int step)> ownerDrawKey;
    // Whether a type-0 ownerdraw item is keyed (UI_NETSOURCE and UI_JOINGAMETYPE) and whether an ownerdrawFlag condition holds.
    std::function<bool(int ownerDraw)> ownerDrawKeyed;
    std::function<bool(int flag)> ownerDrawFlagShown;
    // The value text key of an ownerdraw item (empty: the item's normal value text).
    std::function<std::string(int ownerDraw)> ownerDrawText;
    // The dvar of the item whose action is running (set by MenuStack; "openForGameType" formats its argument with its value).
    std::string itemDvar;
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
