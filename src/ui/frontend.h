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
    // Ends a slider drag (mouse button released).
    void release() { dragItem_ = nullptr; }
    // Right mouse button on a MULTI item selects the previous value (exe 0x4ca5e0 key 0xc9).
    bool clickBack(float x, float y);
    enum class Key { Up, Down, Enter, Escape, Left, Right };
    // Keyboard navigation: up/down move the focus between selectable items, enter runs its action, escape the top menu's onESC.
    void key(Key k);

    // BIND items (type 14, exe 0x4ce100): Enter or a click starts the capture; the next key (captureKey) is bound to
    // the item's command. Escape cancels, Del clears, the grave key is ignored.
    bool capturing() const { return capture_ != nullptr; }
    const Item* captureItem() const { return capture_; }
    void captureKey(const std::string& keyName);

    const Item* focus() const { return focus_; }
    const Item* hover(float x, float y) const; // hit test of the top menu (TODO(spec): item vs menu alignment)
    // Widget geometry (exe 0x4c9520/0x4cafb0): x of the slider track start in virtual pixels.
    float sliderTrackX(const Menu& m, const Item& it) const;
    // Slider fraction 0..1 of the item's dvar (the dvarfloat default when the dvar is unset).
    float sliderFraction(const Item& it) const;
    // x of the value text of YESNO/MULTI items (exe 0x4cd3a0/0x4cd570): after the item text + 8, or at the text origin.
    float valueTextX(const Menu& m, const Item& it) const;
    // The value text key of YESNO ("EXE_YES"/"EXE_NO") and MULTI (the list label of the current value) items.
    std::string valueLabel(const Item& it) const;
    size_t strIndex(const Item& it) const;
    // Items that can take focus in a menu, in file order.
    std::vector<const Item*> selectable(const Menu& m) const;

private:
    void setFocusItem(const Item* it);
    void runItem(const Menu& m, const Script& s);
    bool activate(const Menu& m, const Item& it, int step, float mouseX, bool pointer);
    void dragTo(float x);
    float cvarValue(const Item& it) const;
    const Item* capture_ = nullptr;
    const Item* dragItem_ = nullptr;
    const Menu* dragMenu_ = nullptr;
    MenuLibrary& lib_;
    MenuEnv& env_;
    std::vector<Menu*> stack_;
    const Item* focus_ = nullptr;
    std::string loopAlias_; // soundLoop currently playing
    std::vector<std::pair<const Item*, bool>> shownOverrides_; // "show"/"hide" commands
};

} // namespace iw2::ui
