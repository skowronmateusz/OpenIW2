#include "ui/menuhide.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstring>
#include <set>

namespace iw2::ui {
namespace {

std::string low(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool same(const std::string& a, const char* b) { return low(a) == low(b); }

// Whole menus that are dropped. Every item of every ui_mp menu whose scripts open one of them goes too (the entry that
// leads there). To hide another screen, add its menu name here.
const char* const kHiddenMenus[] = {
    // voting: callvote.menu defines all four
    "callvote", "changemap", "changegametype", "kickplayer",
    // mute players
    "muteplayer",
    // CD key
    "cdkey_menu",
    // PunkBuster popups
    "startpb_popmenu", "joinpb_popmenu",
    // auto update
    "auto_update", "auconfirm",
    // voice chat options
    "options_voice",
    // mods
    "mods_menu",
};

// One hidden item. An item matches when every non-empty field equals the item's (case-insensitive): `name`, `text` (the
// raw @KEY), `dvar` (the dvar the control edits or that its dvarTest reads). `row` also removes every other item of the
// menu on the same line and column (label next to its control), so a row goes as a whole.
// Layout after the removal, so no gap stays:
//   pullY/span/sameX  the remaining items below a removed line (up to `span` below it, in the same column when `sameX`)
//                     move up by pullY, once per removed line;
//   alsoDrop          further items removed by name (an icon, a column background);
//   moves             named items move by (dx, dy) and grow by dw;
//   listbox, cols     the same for the columns of a listbox: column `col` moves by dpos and grows by dwidth (a column
//                     keeps its index, the feeder fills columns by index); `hideCol` is no longer drawn.
struct Move { const char* name; float dx, dy, dw; };
struct ColMove { int col; int dpos, dwidth; };
struct HideRule {
    const char* menu;
    const char* name = "";
    const char* text = "";
    const char* dvar = "";
    bool row = false;
    float pullY = 0, span = 0;
    bool sameX = false;
    std::vector<const char*> alsoDrop = {}; // further items removed by name
    std::vector<Move> moves = {};
    const char* listbox = nullptr;
    int hideCol = -1;
    std::vector<ColMove> cols = {};
};

// Rows of a settings list: each row is one line, the rest of the list is at most 400 below.
constexpr float kRow = 15, kList = 400;

// clang-format off
const HideRule kRules[] = {
// ---- voting: in-game menu entries (they sit in a column of 24-high choices; the choices below move up) ----
    {"ingame", "button_callvote", "", "", false, 24, 30, true},
    // ---- mute players ----
    {"ingame", "button_muteplayer", "", "", false, 24, 30, true},
// ---- voting: key bindings (vote yes / vote no) ----
    {"options_multi", "", "", "vote yes", true, kRow, kList, true},
    {"options_multi", "", "", "vote no", true, kRow, kList, true},
// ---- voting: "allow voting" server setting ----
    {"createserver_serversettings", "", "", "g_allowvote", true, kRow, kList, true},
    {"settings_sw", "", "", "g_allowvote", true, kRow, kList, true},
// ---- voice chat: push-to-talk binding, server setting, options tab ----
    {"options_multi", "", "", "+talk", true, kRow, kList, true},
    {"createserver_serversettings", "", "", "sv_voice", true, kRow, kList, true},
    {"options_menu", "", "@MENU_VOICECHAT", "", false, kRow, 30, true},
// ---- connection speed (dvar rate, ISDN / LAN-cable-xDSL): the client always sends 25000 ----
    {"options_multi", "", "", "rate", true, kRow, kList, true},
// ---- CD key ----
    {"multi_menu", "message_cdkey"},
// ---- PunkBuster: client option, server setting, browser filter ----
    {"options_multi", "", "", "cl_punkbuster", true, kRow, kList, true},
    {"createserver_serversettings", "", "", "sv_punkbuster", true, kRow, kList, true},
    {"filter_popmenu", "", "", "ui_browserShowPunkbuster", false, kRow, 30, false},
// ---- auto update: the notice windows of the main menu (shown only while cl_updateavailable is 1) ----
    {"*", "", "", "cl_updateavailable"},
// ---- server list: the voice (index 6), mod (index 8) and PunkBuster (index 9) columns. Each removal gives its width (20) to the
//      server name column, which is the one that is wide; what lies right of the server column up to the removed one moves
//      right by 20, so the columns stay side by side and the table keeps its width. ----
    {"joinserver", "voice", "", "", false, 0, 0, false,
        {"voiceicon", "voiceColumn"},
        {{"serverColumn", 0, 0, 20}, {"server", 0, 0, 20},
         {"mapColumn", 20, 0, 0}, {"map", 20, 0, 0}, {"playerColumn", 20, 0, 0}, {"Players", 20, 0, 0}, {"typeColumn", 20, 0, 0}, {"Type", 20, 0, 0}},
        "serverlist", 6, {{2, 0, 20}, {3, 20, 0}, {4, 20, 0}, {5, 20, 0}}},
    {"joinserver", "pb", "", "", false, 0, 0, false,
        {"pbicon", "pbColumn"},
        {{"serverColumn", 0, 0, 20}, {"server", 0, 0, 20},
         {"mapColumn", 20, 0, 0}, {"map", 20, 0, 0}, {"playerColumn", 20, 0, 0}, {"Players", 20, 0, 0}, {"typeColumn", 20, 0, 0}, {"Type", 20, 0, 0},
         {"pureColumn", 20, 0, 0}, {"pure", 20, 0, 0}, {"pureicon", 20, 0, 0}, {"modColumn", 20, 0, 0}, {"Mod", 20, 0, 0}, {"modicon", 20, 0, 0}},
        "serverlist", 9, {{2, 0, 20}, {3, 20, 0}, {4, 20, 0}, {5, 20, 0}, {7, 20, 0}, {8, 20, 0}}},
    {"joinserver", "Mod", "", "", false, 0, 0, false,
        {"modicon", "modColumn"},
        {{"serverColumn", 0, 0, 20}, {"server", 0, 0, 20},
         {"mapColumn", 20, 0, 0}, {"map", 20, 0, 0}, {"playerColumn", 20, 0, 0}, {"Players", 20, 0, 0}, {"typeColumn", 20, 0, 0}, {"Type", 20, 0, 0},
         {"pureColumn", 20, 0, 0}, {"pure", 20, 0, 0}, {"pureicon", 20, 0, 0}},
        "serverlist", 8, {{2, 0, 20}, {3, 20, 0}, {4, 20, 0}, {5, 20, 0}, {7, 20, 0}}},
    // Mods: the filter row. It runs after the PunkBuster row, so what lies below has already closed that gap.
    {"filter_popmenu", "", "", "ui_browserMod", false, kRow, 60, false},
};

// The option rows of the single-player pages (files below ui/; both menu sets load these pages) that have no effect in openiw2.
// The graphics page's label and its control sit in one column, so `row` takes the label with the control and the rows below close
// up; the sound page's control lies 30 left of its label, so each is matched on its own (nothing lies below those rows, so nothing
// has to move).
const HideRule kSpRules[] = {
    // filtering is always anisotropic (deliberate deviation, docs/spec/material.md "Texture sampler state")
    {"options_graphics", "", "", "ui_r_texturemode", true, kRow, kList, true},
    {"options_graphics", "", "", "ui_r_rendererpreference", true, kRow, kList, true}, // one renderer path
    {"options_graphics", "", "", "r_multigpu", true, kRow, kList, true},              // no SLI
    {"options_graphics", "", "", "ui_r_aspectratio", true, kRow, kList, true},         // nothing reads r_aspectRatio; the view follows the window
    {"options_graphics", "", "", "ui_sc_enable", true, kRow, kList, true},            // no shadow pass
    // openiw2 has no in-game console: the "Enable console" row (its label and its control, one line) goes and the rows below close up
    {"options_game", "", "@MENU_ENABLE_CONSOLE", "", true, kRow, kList, true},
    {"options_sound", "", "", "ui_snd_khz"},
    {"options_sound", "", "@MENU_SOUND_QUALITY"},
    {"options_sound", "", "", "ui_mss_3d_provider"},
    {"options_sound", "", "@MENU_SOUND_PROVIDER"},
    // "Reset system defaults": its confirm popup runs setRecommended (hardware-detected graphics settings), which openiw2 does not
    // implement. TODO(spec): the command's settings; the entry is the last of its column, so nothing moves.
    {"options_menu", "", "@MENU_RESET_SYSTEM_DEFAULTS"},
};
// clang-format on

struct MenuKey { const char* menu; const char* key; };
// Menu-level execKey shortcuts that lead to a hidden entry.
const MenuKey kKeys[] = {
    {"ingame", "3"}, // mute players
    {"ingame", "4"}, // call vote
};

// The parser already folds an item's `origin` into its rect.
float effX(const Item& it) { return it.rect.x; }
float effY(const Item& it) { return it.rect.y; }

bool matches(const HideRule& r, const Item& it) {
    if (!*r.name && !*r.text && !*r.dvar) return false;
    if (*r.name && !same(it.name, r.name)) return false;
    if (*r.text && !same(it.text, r.text)) return false;
    if (*r.dvar && !same(it.dvar, r.dvar) && !same(it.dvarTest, r.dvar)) return false;
    return true;
}

void forEachScript(const Item& it, const std::function<void(const Script&)>& f) {
    for (const Script* s : {&it.action, &it.mouseEnter, &it.mouseExit, &it.onFocus, &it.leaveFocus, &it.accept, &it.doubleClick}) f(*s);
    for (const auto& [k, s] : it.execKeys) f(s);
}

bool opensHiddenMenu(const Item& it) {
    bool hit = false;
    forEachScript(it, [&](const Script& s) {
        for (const auto& c : s)
            if (c.size() > 1 && same(c[0], "open") && menuIsHidden(c[1])) hit = true;
    });
    return hit;
}

// The kRules / kSpRules loop: removes what a rule matches (and the rest of its line when `row`), then closes the gaps.
void applyRules(Menu& m, const HideRule* rules, size_t count, const std::function<void(const std::string&)>& note) {
    for (size_t ri = 0; ri < count; ++ri) {
        const HideRule& r = rules[ri];
        if (!same(m.name, r.menu) && std::strcmp(r.menu, "*") != 0) continue;
        std::vector<float> lines; // effective y of every removed line, original positions
        std::set<const Item*> drop;
        float removedX = 0;
        for (const auto& it : m.items)
            if (matches(r, *it)) { drop.insert(it.get()); lines.push_back(effY(*it)); removedX = effX(*it); }
        if (drop.empty()) continue;
        for (const auto& it : m.items) {
            for (const char* nm : r.alsoDrop)
                if (same(it->name, nm)) drop.insert(it.get());
            if (r.row)
                for (const float y : lines)
                    if (std::fabs(effY(*it) - y) < 0.01f && std::fabs(effX(*it) - removedX) < 0.01f) drop.insert(it.get());
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        for (const auto& it : m.items) {
            if (drop.count(it.get())) continue;
            if (r.pullY != 0) {
                int n = 0; // removed lines above this item
                for (const float y : lines)
                    if (effY(*it) > y + 0.01f && effY(*it) <= y + r.span && (!r.sameX || std::fabs(effX(*it) - removedX) < 0.01f)) ++n;
                it->rect.y -= r.pullY * float(n);
            }
            for (const Move& mv : r.moves)
                if (same(it->name, mv.name)) { it->rect.x += mv.dx; it->rect.y += mv.dy; it->rect.w += mv.dw; }
            if (r.listbox && same(it->name, r.listbox)) {
                for (const ColMove& c : r.cols)
                    if (c.col >= 0 && size_t(c.col) < it->columns.size()) { it->columns[size_t(c.col)].pos += c.dpos; it->columns[size_t(c.col)].width += c.dwidth; }
                if (r.hideCol >= 0 && size_t(r.hideCol) < it->columns.size()) it->columns[size_t(r.hideCol)].hidden = true;
            }
        }
        for (const Item* d : drop) note(m.name + " item name=" + d->name + " text=" + d->text + " dvar=" + d->dvar);
        std::erase_if(m.items, [&](const std::unique_ptr<Item>& it) { return drop.count(it.get()) != 0; });
    }
}

} // namespace

bool menuIsHidden(const std::string& name) {
    for (const char* h : kHiddenMenus)
        if (same(name, h)) return true;
    return false;
}

bool applyHideRules(Menu& m, const std::string& file, std::vector<std::string>* trace) {
    const bool mp = file.rfind("ui_mp/", 0) == 0;
    const bool sp = !mp && file.rfind("ui/", 0) == 0;
    if (!mp && !sp) return true;
    auto note = [&](const std::string& s) { if (trace) trace->push_back(file + ": " + s); };

    if (sp) { // the single-player pages: only their own option rows (kSpRules), no hidden menus or keys
        applyRules(m, kSpRules, std::size(kSpRules), note);
        return true;
    }

    if (menuIsHidden(m.name)) { note("menu " + m.name + " dropped"); return false; }

    std::erase_if(m.execKeys, [&](const auto& k) {
        for (const auto& mk : kKeys)
            if (same(m.name, mk.menu) && k.first == mk.key) { note(m.name + " execKey " + k.first); return true; }
        return false;
    });

    applyRules(m, kRules, std::size(kRules), note);

    // Entries leading to a hidden menu.
    std::erase_if(m.items, [&](const std::unique_ptr<Item>& it) {
        if (!opensHiddenMenu(*it)) return false;
        note(m.name + " item " + it->name + " opens a hidden menu");
        return true;
    });
    return true;
}

} // namespace iw2::ui
