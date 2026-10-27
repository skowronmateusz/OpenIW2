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
        if (!sel.empty()) setFocusItem(sel.front());
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
        if (!sel.empty()) setFocusItem(sel.front());
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
        if ((it->type == 1 || it->type == 4 || it->type == 6 || it->type == 16 || it->type == 10 || it->type == 11 || it->type == 12 || it->type == 13 || it->type == 14) && !it->decoration && itemVisible(*it)) out.push_back(it.get());
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

void MenuStack::setFocusItem(const Item* it) {
    focus_ = it;
    if (it && isEditType(it->type)) startEdit(it);
    else editing_ = false;
}

void MenuStack::runItem(const Menu& m, const Script& s) { runScript(s, m.name, env_); }

void MenuStack::mouseMove(float x, float y) {
    if (dragItem_) {
        dragTo(x);
        return;
    }
    if (listDrag_.item) { // thumb drag (0x4cb100): startPos from the pointer
        listDrag_.mouseY = y;
        const Item& li = *listDrag_.item;
        const int maxScroll = listMaxScroll(li);
        const float range = li.rect.h - 32.0f - 2.0f - 16.0f;
        if (range > 0)
            lists_[&li].startPos = std::clamp(int((y - (listDrag_.menu->rect.y + li.rect.y + 17.0f) - 8.0f) * float(maxScroll) / range), 0, maxScroll);
        return;
    }
    const Item* it = hover(x, y);
    if (it && it != focus_) {
        setFocusItem(it);
        if (env_.play) env_.play("mouse_over"); // alias name of the menu hover sound
    }
    if (it && it->type == 6 && top()) listHover(*top(), *it, x, y);
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

// Index of the dvar's string in the item's dvarStrList (exe 0x4ca370: case-insensitive compare 0x43f6d0, no match = 0).
size_t MenuStack::strIndex(const Item& it) const {
    std::string cur = env_.getCvar ? env_.getCvar(it.dvar) : std::string();
    auto low = [](std::string x) { for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c))); return x; };
    cur = low(cur);
    for (size_t i = 0; i < it.strList.size(); ++i)
        if (low(it.strList[i].second) == cur) return i;
    return 0;
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
    if (it.type == 12 && !it.strList.empty()) return it.strList[strIndex(it)].first;
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
    if (it.type == 12 && !it.strList.empty()) { // string list: same stepping, the dvar gets the value string
        const int n = int(it.strList.size());
        const int next = ((int(strIndex(it)) + step) % n + n) % n;
        if (env_.setCvar) env_.setCvar(it.dvar, it.strList[size_t(next)].second);
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
    if (focus_ != it) setFocusItem(it);
    const Menu* m = top();
    if (m && it->type == 6) { // list: scrollbar zones or a row (sounds come from the item's own scripts)
        const int zone = listZone(*m, *it, x, y);
        if (zone) {
            listMouse(*m, *it, zone, y);
            if (zone == 0x400) listDrag_ = {it, m, y};
            else if (zone & 0x300) repeat_ = {it, m, zone, now_ + 500, 500, now_};
        } else {
            listHover(*m, *it, x, y);
            ListState& st = lists_[it];
            const ListLayout lay = listLayout(*it);
            const int count = env_.feederCount ? env_.feederCount(it->feeder) : 0;
            // 0x4c9c60, mouse key without a scrollbar zone: a second click within 300 ms on the selected, visible row runs doubleclick.
            if (now_ < st.lastClick && !it->doubleClick.empty() && st.cursor == st.hover && st.hover < lay.endPos && st.hover >= st.startPos)
                runItem(*m, it->doubleClick);
            st.lastClick = now_ + 300;
            if (st.cursor != st.hover) {
                if (st.cursor < 0) st.cursor = st.hover;
                if (st.hover < count) st.cursor = st.hover;
                if (env_.feederSelect && st.cursor >= 0 && st.cursor < count) env_.feederSelect(it->feeder, st.cursor);
            }
        }
        return true;
    }
    if (m && isEditType(it->type)) { // Menu_HandleKey mouse1 (0x4cbf6e..0x4cbfc9): a text-like item starts editing
        startEdit(it);
        return true;
    }
    if (env_.play) env_.play("mouse_click");
    if (m) {
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
    if (editing_ && editItem_ && focus_ == editItem_ && editKey(*m, k)) return;
    if (focus_ && focus_->type == 6 && listKey(*m, *focus_, k)) return;
    if (k == Key::Backspace || k == Key::Delete || k == Key::Home || k == Key::End || k == Key::Insert || k == Key::PageUp ||
        k == Key::PageDown || k == Key::WheelUp || k == Key::WheelDown)
        return;
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
    if (at != sel.end()) i = k == Key::Down || k == Key::Tab ? size_t(at - sel.begin() + 1) % sel.size() : (size_t(at - sel.begin()) + sel.size() - 1) % sel.size();
    setFocusItem(sel[i]);
    if (env_.play) env_.play("mouse_over");
}

} // namespace iw2::ui

namespace iw2::ui {

void MenuStack::release() {
    dragItem_ = nullptr;
    listDrag_ = {};
    repeat_ = {};
}

// Scrollbar button auto-repeat (0x4cb100): the first repeat 500 ms after the press, then every `interval`, which starts at 500 and
// drops by 40 every 150 ms while it is above 20.
void MenuStack::tick(int nowMs) {
    now_ = nowMs;
    if (!repeat_.item) return;
    while (repeat_.interval > 20 && now_ - repeat_.lastDrop >= 150) {
        repeat_.interval -= 40;
        repeat_.lastDrop += 150;
    }
    if (now_ >= repeat_.nextFire) {
        listMouse(*repeat_.menu, *repeat_.item, repeat_.zone, 0);
        repeat_.nextFire = now_ + repeat_.interval;
    }
}

MenuStack::ListState MenuStack::listState(const Item& it) const {
    const auto f = lists_.find(&it);
    return f == lists_.end() ? ListState{} : f->second;
}

int MenuStack::listVisible(const Item& it) const { return it.elementHeight > 0 ? int(it.rect.h / it.elementHeight) : 0; }

int MenuStack::listMaxScroll(const Item& it) const {
    const int count = env_.feederCount ? env_.feederCount(it.feeder) : 0;
    return std::max(0, count - listVisible(it) + 1);
}

// 0x4ce280: rows from startPos while the remaining height (h - 2, minus elementHeight per row) allows; the row that leaves less
// than one element height is the last one drawn (it sets the padding); a list that runs out of rows leaves endPos one past the last.
MenuStack::ListLayout MenuStack::listLayout(const Item& it) const {
    ListLayout l;
    const ListState st = listState(it);
    l.first = l.endPos = st.startPos;
    if (it.elementHeight <= 0) return l;
    const int count = env_.feederCount ? env_.feederCount(it.feeder) : 0;
    float size = it.rect.h - 2.0f;
    for (int r = st.startPos; r < count; ++r) {
        ++l.rows;
        size -= it.elementHeight;
        if (size < it.elementHeight) {
            l.drawPadding = int(it.elementHeight - size);
            break;
        }
        ++l.endPos;
    }
    return l;
}

float MenuStack::listThumbY(const Menu& m, const Item& it) const {
    const float y = m.rect.y + it.rect.y;
    // TODO(spec): the clamp bounds of the dragged thumb (0x4c9480); here the track between the arrows.
    if (listDrag_.item == &it) return std::clamp(listDrag_.mouseY - 8.0f, y + 17.0f, y + it.rect.h - 17.0f);
    const int maxScroll = listMaxScroll(it);
    const float step = maxScroll > 0 ? (it.rect.h - 32.0f - 2.0f - 16.0f) / float(maxScroll) : 0.0f;
    return float(int(float(listState(it).startPos) * step + y + 17.0f)); // 0x4c93a0
}

int MenuStack::listZone(const Menu& m, const Item& it, float px, float py) const {
    const float ax = m.rect.x + it.rect.x + it.rect.w - 16.0f, y = m.rect.y + it.rect.y, h = it.rect.h;
    if (px < ax || px >= ax + 16.0f) return 0;
    const float thumb = listThumbY(m, it);
    if (py >= y && py < y + 16.0f) return 0x100;
    if (py >= y + h - 16.0f && py < y + h) return 0x200;
    if (py >= thumb && py < thumb + 16.0f) return 0x400;
    if (py >= y + 16.0f && py < thumb) return 0x800;
    if (py >= thumb + 16.0f && py < y + h - 16.0f) return 0x1000;
    return 0;
}

// 0x4c9880: pointer in the row area {x, y, w - 16, h - drawPadding} (no scrollbar zone): hover row = (y - 2 - top) / elementHeight +
// startPos, at most endPos.
void MenuStack::listHover(const Menu& m, const Item& it, float px, float py) {
    if (listZone(m, it, px, py) || it.elementHeight <= 0) return;
    const float x = m.rect.x + it.rect.x, y = m.rect.y + it.rect.y;
    const ListLayout lay = listLayout(it);
    if (px < x || px >= x + it.rect.w - 16.0f || py < y || py >= y + it.rect.h - float(lay.drawPadding)) return;
    ListState& st = lists_[&it];
    st.hover = std::min(int((py - 2.0f - y) / it.elementHeight) + st.startPos, lay.endPos);
}

// Scrollbar buttons of a mouse press (0x4c9c60 with the zone flags set).
void MenuStack::listMouse(const Menu&, const Item& it, int zone, float) {
    ListState& st = lists_[&it];
    const int vis = listVisible(it), maxScroll = listMaxScroll(it);
    if (zone & 0x100) st.startPos = std::max(0, st.startPos - 1);
    else if (zone & 0x200) st.startPos = std::min(maxScroll, st.startPos + 1);
    else if (zone & 0x800) st.startPos = std::max(0, st.startPos - vis);
    else if (zone & 0x1000) st.startPos = std::min(maxScroll, st.startPos + vis);
}

void MenuStack::listClampVisible(const Item& it, ListState& st) const {
    const int vis = listVisible(it);
    st.startPos = std::min(st.startPos, st.cursor);
    if (vis > 0 && st.cursor >= st.startPos + vis) st.startPos = st.cursor - vis + 1;
}

// Vertical listbox keys (0x4c9c60): up/down (and the wheel) move the selection, Home/End/PageUp/PageDown jump; the selection stays
// visible and the feeder is told. TODO(spec): the exact scroll of End/PageUp/PageDown.
bool MenuStack::listKey(const Menu&, const Item& it, Key k) {
    ListState& st = lists_[&it];
    const int count = env_.feederCount ? env_.feederCount(it.feeder) : 0;
    const int vis = listVisible(it);
    int cur = st.cursor;
    switch (k) {
    case Key::Up:
    case Key::WheelUp: --cur; break;
    case Key::Down:
    case Key::WheelDown: ++cur; break;
    case Key::PageUp: cur -= vis; break;
    case Key::PageDown: cur += vis; break;
    case Key::Home: cur = 0; st.startPos = 0; break;
    case Key::End: cur = count - 1; break;
    default: return false;
    }
    st.cursor = std::clamp(cur, 0, std::max(count - 1, 0));
    listClampVisible(it, st);
    if (env_.feederSelect && count > 0) env_.feederSelect(it.feeder, st.cursor);
    return true;
}

// Menu_SetFeederSelection (0x4cf5b0) on the open menu: every list of that feeder gets the selection; row 0 resets the scroll,
// a row above the first shown one scrolls up to it.
void MenuStack::setFeederSelection(int feeder, int row) {
    Menu* m = top();
    if (!m) return;
    for (const auto& itp : m->items) {
        if (itp->type != 6 || itp->feeder != feeder) continue;
        ListState& st = lists_[itp.get()];
        st.cursor = row;
        if (env_.feederSelect) env_.feederSelect(feeder, row);
        if (row == 0) st.hover = st.startPos = 0;
        else if (row < st.startPos) st.startPos = row;
    }
}

void MenuStack::startEdit(const Item* it) {
    editing_ = true;
    editItem_ = it;
    editCursor_ = 0;
    editOffset_ = 0;
    overstrike_ = true;
}

void MenuStack::editStore(const Item& it, const std::string& v) {
    if (env_.setCvar) env_.setCvar(it.dvar, v);
    // TODO(spec): the paint window (0x4cc170); here the window follows the cursor.
    if (it.maxPaintChars > 0) {
        if (editCursor_ < editOffset_) editOffset_ = editCursor_;
        if (editCursor_ >= editOffset_ + it.maxPaintChars) editOffset_ = editCursor_ - it.maxPaintChars + 1;
    }
}

void MenuStack::moveFocus(const Menu& m, int dir) {
    const auto sel = selectable(m);
    if (sel.empty()) return;
    const auto at = std::find(sel.begin(), sel.end(), focus_);
    size_t i = 0;
    if (at != sel.end()) i = dir > 0 ? size_t(at - sel.begin() + 1) % sel.size() : (size_t(at - sel.begin()) + sel.size() - 1) % sel.size();
    setFocusItem(sel[i]);
    if (env_.play) env_.play("mouse_over");
}

// Text field keys (0x4ca820). Enter runs the item's `accept` script and ends editing, Escape ends editing and still reaches the
// menu's onESC (returns false); Tab/Down and Up move the focus (0x4cb560 / 0x4cb480).
bool MenuStack::editKey(const Menu& m, Key k) {
    const Item& it = *editItem_;
    std::string s = env_.getCvar ? env_.getCvar(it.dvar) : std::string();
    const int len = int(s.size());
    editCursor_ = std::clamp(editCursor_, 0, len);
    switch (k) {
    case Key::Escape: editing_ = false; return false;
    case Key::Enter: editing_ = false; runItem(m, it.accept); return true;
    case Key::Left: if (editCursor_ > 0) --editCursor_; break;
    case Key::Right: if (editCursor_ < len) ++editCursor_; break;
    case Key::Home: editCursor_ = 0; editOffset_ = 0; break;
    case Key::End: editCursor_ = len; break;
    case Key::Insert: overstrike_ = !overstrike_; return true;
    case Key::Backspace: // TODO(spec): the "^N" colour code pairs are treated as one unit by the exe; here plain characters
        if (editCursor_ > 0) {
            s.erase(size_t(editCursor_ - 1), 1);
            --editCursor_;
            editStore(it, s);
        }
        return true;
    case Key::Delete:
        if (editCursor_ < len) {
            s.erase(size_t(editCursor_), 1);
            editStore(it, s);
        }
        return true;
    case Key::Down:
    case Key::Tab: moveFocus(m, 1); return true;
    case Key::Up: moveFocus(m, -1); return true;
    default: return false;
    }
    editStore(it, s);
    return true;
}

// Printable characters (0x4ca820, flag 0x400 events): VALIDFILEFIELD only takes [A-Za-z0-9_-] (0x43f670); an insert needs room
// (maxChars, 0 = none, at most 0xff), overstrike replaces the character under the cursor.
void MenuStack::textInput(const std::string& text) {
    if (!editItem()) return;
    const Item& it = *editItem_;
    for (unsigned char c : text) {
        if (c < 0x20 || c > 0x7e) continue; // TODO(spec): characters above 0x7f
        if (it.type == 16 && !(std::isalnum(c) || c == '_' || c == '-')) continue;
        std::string s = env_.getCvar ? env_.getCvar(it.dvar) : std::string();
        const int len = int(s.size());
        editCursor_ = std::clamp(editCursor_, 0, len);
        if (!overstrike_) {
            if (len >= 0xff || (it.maxChars > 0 && len >= it.maxChars)) continue;
            s.insert(size_t(editCursor_), 1, char(c));
        } else {
            if (it.maxChars > 0 && editCursor_ >= it.maxChars) continue;
            if (editCursor_ < len) s[size_t(editCursor_)] = char(c);
            else s.push_back(char(c));
        }
        ++editCursor_;
        editStore(it, s);
    }
}

} // namespace iw2::ui
