// Front-end menu runtime: the open-menu stack, focus, dvar-driven item visibility, mouse and keyboard input.
// It sits on top of runScript (menurun.h) and the parsed menus (menu.h).
#pragma once

#include "ui/menurun.h"

#include <string>
#include <utility>
#include <vector>

namespace iw2::ui {

class MenuStack {
public:
    // `env` is the caller's environment; MenuStack installs its own open/close/focus/show hooks into it.
    MenuStack(MenuLibrary& lib, MenuEnv& env);

    // Opens a menu on top of the stack (runs onOpen, applies initial focus). TODO(spec): the limit behind "Too many menus opened".
    bool open(const std::string& name);
    void close(const std::string& name); // closes that menu (runs onClose)
    void closeAll();
    bool empty() const { return stack_.empty(); }
    const std::vector<Menu*>& menus() const { return stack_; }
    Menu* top() const { return stack_.empty() ? nullptr : stack_.back(); }

    // Item visibility: the item's own "visible" flag combined with dvarTest/showDvar/hideDvar and show/hide commands.
    bool itemVisible(const Item& it) const;
    // Pointer position in the 640x480 virtual screen: the focused item follows the hovered selectable item.
    void mouseMove(float x, float y);
    // Clicks at (x, y): runs the action of the hit item; returns true if an item was hit.
    bool click(float x, float y);
    enum class Key { Up, Down, Enter, Escape };
    // Keyboard navigation: up/down move the focus between selectable items, enter runs its action, escape the top menu's onESC.
    void key(Key k);

    const Item* focus() const { return focus_; }
    const Item* hover(float x, float y) const; // hit test of the top menu (TODO(spec): item vs menu alignment)
    // Items that can take focus in a menu, in file order.
    std::vector<const Item*> selectable(const Menu& m) const;

private:
    void setFocusItem(const Item* it);
    void runItem(const Menu& m, const Script& s);
    MenuLibrary& lib_;
    MenuEnv& env_;
    std::vector<Menu*> stack_;
    const Item* focus_ = nullptr;
    std::string loopAlias_; // soundLoop currently playing
    std::vector<std::pair<const Item*, bool>> shownOverrides_; // "show"/"hide" commands
};

} // namespace iw2::ui
