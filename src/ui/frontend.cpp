#include "ui/frontend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace iw2::ui {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool inList(const std::vector<std::string>& l, const std::string& v) {
    for (const auto& s : l)
        if (lower(s) == lower(v)) return true;
    return false;
}
constexpr size_t kMaxOpenMenus = 16; // TODO(spec): the limit behind the "Too many menus opened" error
} // namespace

MenuStack::MenuStack(MenuLibrary& lib, MenuEnv& env) : lib_(lib), env_(env) {
    env_.open = [this](const std::string& n) { open(n); };
    env_.close = [this](const std::string& n) { close(n); };
    env_.closeAll = [this] { closeAll(); };
    env_.setFocus = [this](const std::string& n) {
        // TODO(spec): exe error "setFocus: could not find widget named '%s'" when missing.
        if (Menu* m = top())
            for (const auto& it : m->items)
                if (it->name == n) setFocusItem(it.get());
    };
    env_.setFocusByDvar = [this](const std::string& dvar) {
        // Items with `dvarTest <dvar>` and `focusDvar {values}` get the focus when the dvar value is in the list.
        Menu* m = top();
        if (!m) return;
        const std::string v = env_.getCvar ? env_.getCvar(dvar) : std::string();
        for (const auto& it : m->items)
            if (lower(it->dvarTest) == lower(dvar) && inList(it->focusDvar, v)) setFocusItem(it.get());
    };
    env_.showItem = [this](const std::string& n, bool show) {
        if (Menu* m = top())
            for (const auto& it : m->items)
                if (it->name == n || (!it->group.empty() && it->group == n)) shownOverrides_.emplace_back(it.get(), show);
    };
}

bool MenuStack::open(const std::string& name) {
    Menu* m = lib_.find(name);
    if (!m || stack_.size() >= kMaxOpenMenus) return false;
    std::erase(stack_, m);
    stack_.push_back(m);
    focus_ = nullptr;
    if (!m->soundLoop.empty() && lower(m->soundLoop) != loopAlias_ && env_.startLoop) {
        loopAlias_ = lower(m->soundLoop);
        env_.startLoop(m->soundLoop);
    }
    runItem(*m, m->onOpen);
    if (!focus_) {
        const auto sel = selectable(*m);
        if (!sel.empty()) focus_ = sel.front();
    }
    return true;
}

void MenuStack::close(const std::string& name) {
    Menu* m = lib_.find(name);
    if (!m) return;
    const auto it = std::find(stack_.begin(), stack_.end(), m);
    if (it == stack_.end()) return;
    const bool wasTop = stack_.back() == m;
    stack_.erase(it);
    // TODO(spec): the exe's stop/fade of the loop on close (0x4cf92c); here it stops when no open menu names the same loop.
    if (!loopAlias_.empty() && lower(m->soundLoop) == loopAlias_) {
        bool still = false;
        for (const Menu* o : stack_) still = still || lower(o->soundLoop) == loopAlias_;
        if (!still) {
            loopAlias_.clear();
            if (env_.stopLoop) env_.stopLoop();
        }
    }
    runItem(*m, m->onClose);
    if (wasTop) focus_ = nullptr;
    if (!stack_.empty() && !focus_) {
        const auto sel = selectable(*stack_.back());
        if (!sel.empty()) focus_ = sel.front();
    }
}

void MenuStack::closeAll() {
    while (!stack_.empty()) close(stack_.back()->name);
}

bool MenuStack::itemVisible(const Item& it) const {
    // show/hide commands set the item's visible flag; the dvarTest/showDvar/hideDvar gate still applies on top (the pause menu
    // has "newgame" and "restart" at the same rect, separated only by sv_running).
    bool flag = it.visible;
    for (auto o = shownOverrides_.rbegin(); o != shownOverrides_.rend(); ++o)
        if (o->first == &it) { flag = o->second; break; }
    if (!flag) return false;
    if (!it.dvarTest.empty()) {
        const std::string v = env_.getCvar ? env_.getCvar(it.dvarTest) : std::string();
        // TODO(spec): the exe handler of showDvar/hideDvar; the shipped menus show an item when the value is listed in
        // showDvar and hide it when listed in hideDvar.
        if (!it.showDvar.empty() && !inList(it.showDvar, v)) return false;
        if (!it.hideDvar.empty() && inList(it.hideDvar, v)) return false;
    }
    return true;
}

std::vector<const Item*> MenuStack::selectable(const Menu& m) const {
    std::vector<const Item*> out;
    for (const auto& it : m.items)
        if ((it->type == 1 || it->type == 10 || it->type == 11 || it->type == 12 || it->type == 13 || it->type == 14) && !it->decoration && itemVisible(*it)) out.push_back(it.get());
    return out;
}

const Item* MenuStack::hover(float x, float y) const {
    const Menu* m = top();
    if (!m) return nullptr;
    for (const Item* it : selectable(*m)) {
        const float ix = m->rect.x + it->rect.x, iy = m->rect.y + it->rect.y;
        if (x >= ix && x < ix + it->rect.w && y >= iy && y < iy + it->rect.h) return it;
    }
    return nullptr;
}

void MenuStack::setFocusItem(const Item* it) { focus_ = it; }

void MenuStack::runItem(const Menu& m, const Script& s) { runScript(s, m.name, env_); }

void MenuStack::mouseMove(float x, float y) {
    if (dragItem_) {
        dragTo(x);
        return;
    }
    const Item* it = hover(x, y);
    if (it && it != focus_) {
        setFocusItem(it);
        if (env_.play) env_.play("mouse_over"); // alias name of the menu hover sound
    }
}

float MenuStack::cvarValue(const Item& it) const {
    const std::string v = env_.getCvar ? env_.getCvar(it.dvar) : std::string();
    if (v.empty()) return it.hasRange ? it.dvarDef : 0.0f; // dvarfloat registers the dvar with its default
    return std::strtof(v.c_str(), nullptr);
}

float MenuStack::sliderTrackX(const Menu& m, const Item& it) const {
    // exe 0x4c9520: with text, the track starts 8 after the text rect (x + w); otherwise at the item rect x.
    const float x = m.rect.x + it.rect.x;
    if (it.text.empty()) return x;
    return x + it.textAlignX + (env_.textWidth ? env_.textWidth(it) : 0.0f) + 8.0f;
}

float MenuStack::sliderFraction(const Item& it) const {
    if (it.dvarMax == it.dvarMin) return 0.0f;
    const float v = std::clamp(cvarValue(it), it.dvarMin, it.dvarMax);
    return (v - it.dvarMin) / (it.dvarMax - it.dvarMin);
}

float MenuStack::valueTextX(const Menu& m, const Item& it) const {
    const float x = m.rect.x + it.rect.x + it.textAlignX;
    return it.text.empty() ? x : x + (env_.textWidth ? env_.textWidth(it) : 0.0f) + 8.0f;
}

std::string MenuStack::valueLabel(const Item& it) const {
    if (it.type == 11) return cvarValue(it) == 0.0f ? "EXE_NO" : "EXE_YES";
    if (it.type == 17) { // exe 0x4cd0e0: a float dvar is printed with "%.*f", precision 2 (0x42f3e0)
        // TODO(spec): the dvar type test (float only) and the language id source (0xc4d2ec+8); here a numeric
        // dvar (or the unset default) is formatted, other strings are shown as they are.
        const std::string raw = env_.getCvar ? env_.getCvar(it.dvar) : std::string();
        char* end = nullptr;
        double v = std::strtod(raw.c_str(), &end);
        if (raw.empty()) v = double(cvarValue(it));
        else if (end == raw.c_str() || *end) return raw;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.2f", v);
        std::string s = buf;
        const int lang = env_.language ? env_.language() : 0;
        if (lang == 1 || lang == 2 || lang == 3 || lang == 4 || lang == 6 || lang == 7 || lang == 14) // 0x42f410..0x42f429
            if (const size_t dot = s.find('.'); dot != std::string::npos) s[dot] = ',';
        return s;
    }
    if (it.type == 13) { // exe 0x4cd720: the domain string of the enum dvar at the item's current index
        if (it.enumList.empty()) return "<dvarEnumList not set>";
        if (!env_.enumValues) return "<not an enum dvar>";
        const auto names = env_.enumValues(it.enumList);
        const int i = int(cvarValue(it)); // TODO(spec): the index clamp of 0x4ca510
        return i >= 0 && size_t(i) < names.size() ? names[size_t(i)] : std::string();
    }
    if (it.type == 12 && !it.floatList.empty()) {
        const float v = cvarValue(it);
        for (const auto& e : it.floatList)
            if (e.second == v) return e.first;
        return it.floatList.front().first; // exe 0x4ca370: no match -> index 0
    }
    return {};
}

void MenuStack::dragTo(float x) {
    if (!dragItem_ || !dragMenu_) return;
    // exe 0x4cafb0: fraction = clamp((x - (trackX + 6)) / 84), value = min + fraction * (max - min), set as "%g".
    const float f = std::clamp((x - (sliderTrackX(*dragMenu_, *dragItem_) + 6.0f)) / 84.0f, 0.0f, 1.0f);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", double(dragItem_->dvarMin + f * (dragItem_->dvarMax - dragItem_->dvarMin)));
    if (env_.setCvar) env_.setCvar(dragItem_->dvar, buf);
}

// One activation of a widget: step +1/-1 for YESNO/MULTI, a pointer press for SLIDER (mouseX in virtual pixels).
bool MenuStack::activate(const Menu& m, const Item& it, int step, float mouseX, bool pointer) {
    if (it.type == 11) { // exe 0x4ca270: dvar = (dvar == 0), written as "%i"
        if (env_.setCvar) env_.setCvar(it.dvar, cvarValue(it) == 0.0f ? "1" : "0");
        return true;
    }
    if (it.type == 12 && !it.floatList.empty()) { // exe 0x4ca640 / 0x4ca5e0: next/previous list value, wrapping
        const int n = int(it.floatList.size());
        const float v = cvarValue(it);
        int cur = 0;
        for (int i = 0; i < n; ++i)
            if (it.floatList[size_t(i)].second == v) {
                cur = i;
                break;
            }
        const int next = ((cur + step) % n + n) % n;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g", double(it.floatList[size_t(next)].second));
        if (env_.setCvar) env_.setCvar(it.dvar, buf);
        return true;
    }
    if (it.type == 13) { // exe 0x4ca6f0: next/previous domain entry (0x4ca5e0), dvar set as "%i"
        const int n = env_.enumValues && !it.enumList.empty() ? int(env_.enumValues(it.enumList).size()) : 0;
        if (n == 0) return false;
        const int next = ((int(cvarValue(it)) + step) % n + n) % n;
        if (env_.setCvar) env_.setCvar(it.dvar, std::to_string(next));
        return true;
    }
    if (it.type == 14) { // exe 0x4ce100: Enter / mouse1 on a BIND item starts the capture
        capture_ = &it;
        return true;
    }
    if (it.type == 10 && it.hasRange) {
        if (pointer) { // exe 0x4cb323: a mouse key sets the value from the pointer; dragging starts on the thumb (0x4cb100)
            dragItem_ = &it;
            dragMenu_ = &m;
            dragTo(mouseX);
        } else { // exe 0x4cb261: arrow keys move by (max - min) * 0.05, clamped
            const float v = std::clamp(cvarValue(it) + float(step) * (it.dvarMax - it.dvarMin) * 0.05f, it.dvarMin, it.dvarMax);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", double(v));
            if (env_.setCvar) env_.setCvar(it.dvar, buf);
        }
        return true;
    }
    return false;
}

bool MenuStack::click(float x, float y) {
    const Item* it = hover(x, y);
    if (!it) return false;
    setFocusItem(it);
    if (env_.play) env_.play("mouse_click");
    if (const Menu* m = top()) {
        if (it->type == 1) runItem(*m, it->action);
        else activate(*m, *it, 1, x, true);
    }
    return true;
}

bool MenuStack::clickBack(float x, float y) {
    const Item* it = hover(x, y);
    if (!it || it->type != 12 || !top()) return false;
    setFocusItem(it);
    if (env_.play) env_.play("mouse_click");
    activate(*top(), *it, -1, x, false);
    return true;
}

void MenuStack::captureKey(const std::string& name) {
    if (!capture_) return;
    // exe 0x4ce156: flag 0x400 keys and the console key (0x60) are swallowed; Escape (0x1b) ends the capture; Del (0x7f)
    // clears the command's keys (0x4ce1ea); any other key is bound (0x4ce215..0x4ce257).
    if (name == "GRAVE" || name == "`") return;
    if (name == "ESCAPE") {
        capture_ = nullptr;
        return;
    }
    if (env_.bindKey) env_.bindKey(capture_->dvar, name == "DEL" ? std::string() : name);
    capture_ = nullptr;
}

void MenuStack::key(Key k) {
    Menu* m = top();
    if (!m) return;
    if (capture_) return;
    if (k == Key::Escape) {
        runItem(*m, m->onEsc); // TODO(spec): menus without onESC (main) simply ignore ESC
        return;
    }
    const auto sel = selectable(*m);
    if (sel.empty()) return;
    const auto at = std::find(sel.begin(), sel.end(), focus_);
    if (k == Key::Enter || k == Key::Left || k == Key::Right) {
        if (at == sel.end()) return;
        const Item& it = **at;
        if (it.type == 1) {
            if (k == Key::Enter) runItem(*m, it.action);
        } else if (it.type == 10) {
            if (k != Key::Enter) activate(*m, it, k == Key::Right ? 1 : -1, 0, false);
        } else {
            activate(*m, it, k == Key::Left ? -1 : 1, 0, false); // exe 0x4ca5e0: Enter/right -> next, left -> previous
        }
        return;
    }
    size_t i = 0;
    if (at != sel.end()) i = k == Key::Down ? size_t(at - sel.begin() + 1) % sel.size() : (size_t(at - sel.begin()) + sel.size() - 1) % sel.size();
    setFocusItem(sel[i]);
    if (env_.play) env_.play("mouse_over");
}

} // namespace iw2::ui
