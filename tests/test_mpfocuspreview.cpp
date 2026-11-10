// Focus preview in the shipped ui_mp script menus (docs/spec/ui.md "Focus scripts"): moving the focus to another button, with the
// pointer or the keys, runs that button's onFocus script, so the weapon image, grenade icons and stat bars on the right (and the
// team / description panels of the other menus) switch to the focused entry.
#include "ui/frontend.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2;

// Names shown / hidden by a script.
void targets(const ui::Script& s, std::set<std::string>& shown, std::set<std::string>& hidden) {
    for (const auto& c : s)
        if (c.size() > 1 && (c[0] == "show" || c[0] == "hide")) (c[0] == "show" ? shown : hidden).insert(c[1]);
}

bool anyVisible(const ui::MenuStack& st, const ui::Menu& m, const std::string& name) {
    for (const auto& it : m.items)
        if (it->name == name && st.itemVisible(*it)) return true;
    return false;
}

// Every selectable button with an onFocus script: focus it (pointer when `mouse`, else Down key steps), then the items its script
// shows are visible and the ones it only hides are not. Returns the number of buttons checked.
int checkMenu(ui::MenuLibrary& lib, ui::MenuStack& st, const std::string& file, const std::string& menu, bool mouse) {
    (void)file; // loaded once up front (reloading replaces the Menu objects the stack points at)
    st.closeAll();
    CHECK(st.open(menu));
    const ui::Menu* m = lib.find(menu);
    if (!m) return 0;
    int checked = 0;
    std::set<std::string> distinct;
    const auto sel = st.selectable(*m);
    // Last to first: the first button already has the focus after open, and a focus that does not change runs nothing.
    for (size_t i = sel.size(); i-- > 0;) {
        const ui::Item* it = sel[i];
        if (mouse) {
            const float x = m->rect.x + it->rect.x + it->rect.w * 0.5f, y = m->rect.y + it->rect.y + it->rect.h * 0.5f;
            if (st.hover(x, y) != it) continue;
            st.mouseMove(x, y);
        } else {
            for (size_t k = 0; k < sel.size() && st.focus() != it; ++k) st.key(ui::MenuStack::Key::Down);
        }
        CHECK(st.focus() == it);
        if (it->onFocus.empty()) continue;
        std::set<std::string> shown, hidden;
        targets(it->onFocus, shown, hidden);
        for (const auto& n : shown) {
            bool exists = false; // team menus show "spectator_info", which no item is named
            for (const auto& o : m->items) exists = exists || o->name == n;
            if (!exists) continue;
            if (!anyVisible(st, *m, n)) std::cerr << menu << " " << it->name << ": " << n << " not shown\n";
            CHECK(anyVisible(st, *m, n));
        }
        for (const auto& n : hidden)
            if (!shown.count(n) && anyVisible(st, *m, n)) {
                std::cerr << menu << " " << it->name << ": " << n << " still visible\n";
                ++failures;
            }
        distinct.insert(*shown.begin());
        ++checked;
    }
    std::cerr << menu << (mouse ? " (pointer)" : " (keys)") << ": " << checked << " buttons, " << distinct.size() << " previews\n";
    return checked;
}

void world(const char* main) {
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    std::map<std::string, std::string> cv{{"cl_ingame", "1"}};
    // Every weapon allowed (dvarTest ui_allow_* / showDvar "1" buttons).
    for (const char* w : {"greasegun", "m1carbine", "m1garand", "thompson", "bar", "springfield", "shotgun", "sten", "enfield",
                          "bren", "pps42", "nagant", "svt40", "nagantsniper", "ppsh", "mp40", "kar98k", "g43", "mp44",
                          "kar98ksniper", "joinauto", "joinallies", "joinaxis", "joinspectator"})
        cv[std::string("ui_allow_") + w] = "1";
    ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    env.ingameActive = [] { return true; };
    ui::MenuLibrary lib(vfs);
    for (const char* f : {"weapon_american", "weapon_british", "weapon_german", "weapon_russian", "team_americangerman",
                          "team_britishgerman", "team_russiangerman"})
        CHECK(lib.loadFile(std::string("ui_mp/scriptmenus/") + f + ".menu"));
    ui::MenuStack st(lib, env);
    for (const char* nation : {"american", "british", "german", "russian"}) {
        const std::string name = std::string("weapon_") + nation, file = "ui_mp/scriptmenus/" + name + ".menu";
        for (bool mouse : {true, false}) CHECK(checkMenu(lib, st, file, name, mouse) >= 5);
        // Pointer on the second button: the first button's preview is gone (the reported bug).
        st.closeAll();
        CHECK(st.open(name));
        const ui::Menu* m = lib.find(name);
        const auto sel = m ? st.selectable(*m) : std::vector<const ui::Item*>{};
        CHECK(sel.size() >= 2);
        if (sel.size() >= 2) {
            std::set<std::string> s0, h0, s1, h1;
            targets(sel[0]->onFocus, s0, h0);
            targets(sel[1]->onFocus, s1, h1);
            const auto pick = [&](const std::set<std::string>& s) {
                for (const auto& n : s) if (n != "weapon_propertiestext") return n;
                return std::string();
            };
            const std::string first = pick(s0);
            std::string second;
            for (size_t i = 1; i < sel.size() && (second.empty() || second == first); ++i) {
                std::set<std::string> s, h;
                targets(sel[i]->onFocus, s, h);
                second = pick(s);
                const ui::Item* it = sel[i];
                st.mouseMove(m->rect.x + it->rect.x + it->rect.w * 0.5f, m->rect.y + it->rect.y + it->rect.h * 0.5f);
            }
            CHECK(!first.empty() && !second.empty() && first != second);
            CHECK(anyVisible(st, *m, second));
            CHECK(!anyVisible(st, *m, first));
        }
    }
    for (const char* team : {"americangerman", "britishgerman", "russiangerman"}) {
        const std::string name = std::string("team_") + team;
        CHECK(checkMenu(lib, st, "ui_mp/scriptmenus/" + name + ".menu", name, true) >= 3);
    }
}

} // namespace

int main() {
    if (const char* main = std::getenv("IW2_MAIN")) world(main);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "mpfocuspreview ok\n";
    return failures ? 1 : 0;
}
