#include "ui/frontend.h"

#include <algorithm>
#include <cctype>

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
    for (auto o = shownOverrides_.rbegin(); o != shownOverrides_.rend(); ++o)
        if (o->first == &it) return o->second;
    if (!it.visible) return false;
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
        if (it->type == 1 && !it->decoration && itemVisible(*it)) out.push_back(it.get());
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
    const Item* it = hover(x, y);
    if (it && it != focus_) {
        setFocusItem(it);
        if (env_.play) env_.play("mouse_over"); // alias name of the menu hover sound
    }
}

bool MenuStack::click(float x, float y) {
    const Item* it = hover(x, y);
    if (!it) return false;
    setFocusItem(it);
    if (env_.play) env_.play("mouse_click");
    if (const Menu* m = top()) runItem(*m, it->action);
    return true;
}

void MenuStack::key(Key k) {
    Menu* m = top();
    if (!m) return;
    if (k == Key::Escape) {
        runItem(*m, m->onEsc); // TODO(spec): menus without onESC (main) simply ignore ESC
        return;
    }
    const auto sel = selectable(*m);
    if (sel.empty()) return;
    const auto at = std::find(sel.begin(), sel.end(), focus_);
    if (k == Key::Enter) {
        if (at != sel.end()) runItem(*m, (*at)->action);
        return;
    }
    size_t i = 0;
    if (at != sel.end()) i = k == Key::Down ? size_t(at - sel.begin() + 1) % sel.size() : (size_t(at - sel.begin()) + sel.size() - 1) % sel.size();
    setFocusItem(sel[i]);
    if (env_.play) env_.play("mouse_over");
}

} // namespace iw2::ui
