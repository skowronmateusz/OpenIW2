// Menu definition files (ui/*.menu, ui/*.txt menu lists; docs/spec/menus.md).
// The files are read through a small C-style preprocessor (#include, #define, #ifdef) and parsed into menus
// and items. Properties keep their raw argument tokens; the common ones are also decoded into fields.
#pragma once

#include "fs/vfs.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iw2::ui {

using Color = std::array<float, 4>;

// One command of an event script, e.g. {"open", "main_text"}.
using Command = std::vector<std::string>;
using Script = std::vector<Command>;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    int horzAlign = 0, vertAlign = 0; // HORIZONTAL_ALIGN_* / VERTICAL_ALIGN_* (ui/menudefinition.h)
};

// Properties shared by menus and items (the "window" part of a definition).
struct Window {
    std::string name;
    std::string group;
    Rect rect;
    float originX = 0, originY = 0;
    int style = 0;  // WINDOW_STYLE_*
    int border = 0; // WINDOW_BORDER_*
    float borderSize = 1;
    bool visible = false;
    bool decoration = false;
    int ownerDraw = 0;
    Color foreColor{1, 1, 1, 1}, backColor{0, 0, 0, 0}, borderColor{0, 0, 0, 0}, outlineColor{0, 0, 0, 0};
    std::string background;
    // Every property as written (lower-case key), for the ones without a decoded field.
    std::multimap<std::string, std::vector<std::string>> props;

    const std::vector<std::string>* prop(const std::string& key) const;
};

struct Item : Window {
    int type = 0; // ITEM_TYPE_*
    std::string text;
    float textScale = 0.55f;
    int textStyle = 0, textAlign = 0, textFont = 0;
    float textAlignX = 0, textAlignY = 0;
    bool autoWrapped = false;
    std::string dvar;
    std::string dvarTest;
    std::vector<std::string> showDvar, hideDvar, enableDvar, disableDvar;
    std::vector<std::string> focusDvar; // dvarTest values that give the item the initial focus
    // SLIDER / DECIMALFIELD: `dvarfloat "name" def min max`.
    bool hasRange = false;
    float dvarDef = 0, dvarMin = 0, dvarMax = 1;
    // MULTI: `dvarFloatList { "label" value ... }` (labels are @LOCALIZE keys).
    std::vector<std::pair<std::string, float>> floatList;
    std::string enumList; // `dvarEnumList "dvar"` (values come from the engine; TODO(spec))
    Script action, mouseEnter, mouseExit, onFocus, leaveFocus, accept;
    std::vector<std::pair<std::string, Script>> execKeys; // execKey "k" / execKeyInt n
};

struct Menu : Window {
    bool fullScreen = false;
    bool popup = false;
    Color focusColor{1, 1, 1, 1};
    std::string soundLoop;
    float blurWorld = 0;
    Script onOpen, onClose, onEsc;
    std::vector<std::pair<std::string, Script>> execKeys;
    std::vector<std::unique_ptr<Item>> items;
    std::string file;
};

class MenuLibrary {
public:
    explicit MenuLibrary(const fs::Vfs& vfs) : vfs_(vfs) {}

    // Loads a menu list (e.g. "ui/menus.txt": loadMenu { "ui/main.menu" } …) and every file it names.
    // Returns false when the list itself is missing.
    bool loadList(const std::string& path);
    // Loads one .menu file; menus with a name already loaded replace the old ones.
    bool loadFile(const std::string& path);

    Menu* find(const std::string& name);
    const std::vector<std::unique_ptr<Menu>>& menus() const { return menus_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    // Parses menu source text (with #include resolved through the VFS); exposed for tests.
    void parseSource(const std::string& source, const std::string& file);

private:
    const fs::Vfs& vfs_;
    std::vector<std::unique_ptr<Menu>> menus_;
    std::vector<std::string> errors_, warnings_;
};

// Decodes an event script from raw tokens (commands separated by ';').
Script parseScript(const std::vector<std::string>& tokens);

} // namespace iw2::ui
