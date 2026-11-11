// Which input an open menu takes. A menu that sets cl_bypassMouseInput 1 while it is open (the multiplayer quick-message menus, which are
// keyboard-only: number keys) leaves the mouse and the movement keys to the game: no cursor, mouse look and movement keep working, the
// menu takes only its execKey keys and Escape. A full-screen menu always takes everything.
#pragma once

namespace iw2::ui {

constexpr bool menuBypassesGameInput(bool menuOpen, bool topIsFullScreen, bool bypassCvar) { return menuOpen && !topIsFullScreen && bypassCvar; }

} // namespace iw2::ui
