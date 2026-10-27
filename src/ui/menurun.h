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
};

// Runs the commands of `script` for a menu named `menuName` (the target of its scriptMenuResponse commands).
void runScript(const Script& script, const std::string& menuName, MenuEnv& env);

// UI start-up cvars: ui_mousePitch = (m_pitch < 0), exe 0x4c30db.
void initUiCvars(MenuEnv& env);

// The clickable item under (x, y) in the 640x480 virtual screen (visible button items), or nullptr.
const Item* itemAt(const Menu& menu, float x, float y);

} // namespace iw2::ui
