// Front-end menu runtime: the open-menu stack, focus, dvar-driven item visibility, mouse and keyboard input.
// It sits on top of runScript (menurun.h) and the parsed menus (menu.h).
#pragma once

#include "ui/menurun.h"

#include <map>
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
    void release();
    // Right mouse button on a MULTI item selects the previous value (exe 0x4ca5e0 key 0xc9).
    bool clickBack(float x, float y);
    enum class Key { Up, Down, Enter, Escape, Left, Right, Backspace, Delete, Home, End, Insert, PageUp, PageDown, Tab, WheelUp, WheelDown };
    // Keyboard navigation: up/down move the focus between selectable items, enter runs its action, escape the top menu's onESC.
    void key(Key k);

    // Time base in milliseconds: double-click timing and the scrollbar button auto-repeat. Call once per frame.
    void tick(int nowMs);

    // ---- LISTBOX items (type 6; exe paint 0x4ce280, keys 0x4c9c60, hover 0x4c9880, mouse press 0x4cb100) ----
    // Per-item state. `cursor` is the selected row (item +0x2dc), startPos/endPos/hover/drawPadding the listbox def fields.
    struct ListState {
        int startPos = 0;
        int cursor = 0;
        int hover = 0;
        int lastClick = 0; // click time + 300 ms
    };
    // Geometry of one paint (exe 0x4ce280): the rows drawn from `first`, `endPos` as the exe leaves it, the partial-row padding.
    struct ListLayout {
        int first = 0, rows = 0, endPos = 0, drawPadding = 0;
    };
    ListState listState(const Item& it) const;
    ListLayout listLayout(const Item& it) const;
    int listVisible(const Item& it) const;   // ftol(h / elementHeight)
    int listMaxScroll(const Item& it) const; // 0x4c9320
    // Top of the scrollbar thumb in virtual pixels (0x4c93a0 / 0x4c9480: follows the pointer while dragged).
    float listThumbY(const Menu& m, const Item& it) const;
    // Scrollbar column zone under the point (0x4c9630): 0x100 up, 0x200 down, 0x400 thumb, 0x800 page up, 0x1000 page down, 0 none.
    int listZone(const Menu& m, const Item& it, float x, float y) const;
    // Menu_SetFeederSelection (0x4cf5b0): the list items of `feeder` get the row as selection (row 0 also resets the scroll).
    void setFeederSelection(int feeder, int row);

    // ---- EDITFIELD / VALIDFILEFIELD items (types 4, 16; exe paint 0x4cd0e0, keys 0x4ca820) ----
    // Editing starts when such an item takes the focus (0x4ca7b0): cursor 0, paint offset 0, overstrike on.
    const Item* editItem() const { return editing_ && focus_ == editItem_ ? editItem_ : nullptr; }
    int editCursor() const { return editCursor_; }
    int editOffset() const { return editOffset_; }
    bool editOverstrike() const { return overstrike_; }
    // True while an edit field takes typed characters: the platform layer turns OS text input on only then
    // (SDL3 delivers no text events until it is started, and game keys must not open an input method).
    bool wantsTextInput() const { return !stack_.empty() && editItem() != nullptr; }
    // Characters typed (ASCII 0x20..0x7e), applied to the editing item.
    void textInput(const std::string& text);

    // BIND items (type 14, exe 0x4ce100): Enter or a click starts the capture; the next key (captureKey) is bound to
    // the item's command. Escape cancels, Del clears, the grave key is ignored.
    bool capturing() const { return capture_ != nullptr; }
    const Item* captureItem() const { return capture_; }
    void captureKey(const std::string& keyName);

    // ---- Item fades and colour overrides (window flags +0xe8: 0x04 visible, 0x10 fading out, 0x20 fading in) ----
    // Colours of an item after setitemcolor and fades.
    Color foreColor(const Item& it) const;
    Color backColor(const Item& it) const;
    // One paint-time fade step (Window_Paint 0x4c7270 for FILLED items with a background: the back colour's alpha; item text paint
    // 0x4ce990 / 0x4cc450: the fore colour's alpha). Once the menu's fadeCycle has passed since the last step (real ms, tick()), the
    // alpha drops by fadeAmount (fading out; at <= 0 the item stops fading and is hidden) or rises by fadeInAmount (fading in; clamped
    // to fadeClamp, where the fade ends).
    void paintFade(const Menu& m, const Item& it, bool back);
    // Debug (single-frame --shot): ends every running fade as if enough paints had passed.
    void settleFades();

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
    bool listKey(const Menu& m, const Item& it, Key k);
    void listMouse(const Menu& m, const Item& it, int zone, float y);
    void listHover(const Menu& m, const Item& it, float x, float y);
    void listClampVisible(const Item& it, ListState& st) const;
    bool editKey(const Menu& m, Key k);
    void startEdit(const Item* it);
    void editStore(const Item& it, const std::string& v);
    static int editStep(const std::string& s, int cur, int n);
    void moveFocus(const Menu& m, int dir);
    static bool isEditType(int type) { return type == 4 || type == 16; }
    struct ListDrag {
        const Item* item = nullptr;
        const Menu* menu = nullptr;
        float mouseY = 0;
    };
    struct Repeat {
        const Item* item = nullptr;
        const Menu* menu = nullptr;
        int zone = 0, nextFire = 0, interval = 500, lastDrop = 0;
    };
    std::map<const Item*, ListState> lists_;
    ListDrag listDrag_;
    Repeat repeat_;
    int now_ = 0;
    bool editing_ = false;
    const Item* editItem_ = nullptr;
    int editCursor_ = 0, editOffset_ = 0;
    bool overstrike_ = true;
    const Item* capture_ = nullptr;
    const Item* dragItem_ = nullptr;
    const Menu* dragMenu_ = nullptr;
    MenuLibrary& lib_;
    MenuEnv& env_;
    std::vector<Menu*> stack_;
    const Item* focus_ = nullptr;
    std::string loopAlias_; // soundLoop currently playing
    std::vector<std::pair<const Item*, bool>> shownOverrides_; // "show"/"hide" commands
    struct ItemRuntime {
        bool fadingIn = false, fadingOut = false;
        int nextTime = 0; // window +0x1c8
        bool hasFore = false, hasBack = false;
        Color fore{}, back{};
    };
    std::map<const Item*, ItemRuntime> runtime_;
    std::vector<const Item*> matching(const std::string& nameOrGroup) const; // 0x4c77d0 / 0x4c78a0 on the top menu
};

} // namespace iw2::ui
