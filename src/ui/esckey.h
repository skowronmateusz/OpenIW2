// Where a press of the Escape key goes in iw2view (docs/spec/mp-ui.md "Escape key").
#pragma once

#include <string>

namespace iw2::ui {

enum class EscRoute {
    Menu,          // an open menu takes it (its onESC, MenuStack::key)
    Binding,       // MP level: the key binding runs (default_mp.cfg binds ESCAPE to togglemenu); nothing else may toggle too
    ToggleMenu,    // MP level without an Escape binding: toggle the menu directly
    PauseMenu,     // SP level: open the pause menu "main" (UI_SetActiveMenu(2))
    MainMenu,      // no level and no menu: the main menu opens again (SP 0x40c2c7 -> 0x40e0b0, UI_SetActiveMenu(1)); never a quit
};

inline EscRoute escRoute(bool menuOpen, bool inLevel, bool multiplayer, bool escBound) {
    if (menuOpen) return EscRoute::Menu;
    if (inLevel && multiplayer) return escBound ? EscRoute::Binding : EscRoute::ToggleMenu;
    if (inLevel) return EscRoute::PauseMenu;
    return EscRoute::MainMenu;
}

// "ingameclose <menu>" acts only in a game (docs/spec/mp-ui.md "Escape key"): the MP client tests its connection state for
// the active state (handler 0x53b170), which a level in iw2view is, local or networked; the SP handler (0x4c8320) tests
// dvar cl_paused instead.
inline bool ingameCloseActive(bool multiplayer, bool inLevel, bool clPaused) {
    return multiplayer ? inLevel : clPaused;
}

// The menu MP togglemenu (client 0x4107b0) opens when no menu is open: the script main menu named by the server's
// setClientCvar g_scriptMainMenu when it is a known menu, "main" otherwise.
inline std::string toggleMenuTarget(const std::string& scriptMainMenu, bool scriptMainMenuLoaded) {
    return !scriptMainMenu.empty() && scriptMainMenuLoaded ? scriptMainMenu : std::string("main");
}

} // namespace iw2::ui
