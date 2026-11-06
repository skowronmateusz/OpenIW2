#include "ui/iw2options.h"

#include <cctype>

namespace iw2::ui {

namespace {
std::string lowerCopy(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

bool addOpenIw2Options(MenuLibrary& lib) {
    Menu* m = lib.find("options_game");
    if (!m) return false;
    const Item* value = nullptr;
    for (const auto& it : m->items) {
        if (lowerCopy(it->dvar) == "iw2_hitmarker") return true; // already added
        if (!value && lowerCopy(it->dvar) == "cg_drawcrosshair" && it->type == 12) value = it.get();
    }
    if (!value) return false;
    // The label of that row: the decoration item on the same line.
    const Item* label = nullptr;
    for (const auto& it : m->items)
        if (it.get() != value && it->decoration && !it->text.empty() && it->rect.y == value->rect.y) label = it.get();
    if (!label) return false;
    // Rows are 15 units apart: the crosshair row, blood, console (and the monkeytoy / restricted-console variants on the same line), the
    // language row (shown only with developer set), then the new row below them.
    const float y = value->rect.y + 60.0f;
    auto newLabel = std::make_unique<Item>(*label);
    newLabel->text = "Hit marker"; // not a localized key: shown as written
    newLabel->rect.y = y;
    newLabel->name = "iw2_hitmarker_label";
    auto newValue = std::make_unique<Item>(*value);
    newValue->dvar = "iw2_hitMarker";
    newValue->floatList = {{"@MENU_YES", 1.0f}, {"@MENU_NO", 0.0f}};
    newValue->rect.y = y;
    newValue->name = "iw2_hitmarker";
    m->items.push_back(std::move(newLabel));
    m->items.push_back(std::move(newValue));
    return true;
}

} // namespace iw2::ui
