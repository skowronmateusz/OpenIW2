// Developer key bindings of the OpenIW2 page (fullbright, god mode, noclip), the screenshot key on F12, the slider value placement
// and the input rule of keyboard-only menus. Synthetic menus and configs always run; with IW2_MAIN the shipped default.cfg is checked
// for conflicts with the default keys.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "ui/frontend.h"
#include "ui/iw2options.h"
#include "ui/menu.h"
#include "ui/menuinput.h"
#include "ui/profile.h"

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
using game::ClientInput;

const char* kCfg = "unbindall\nbind w \"+forward\"\nbind SPACE \"+gostand\"\nbind F12 \"screenshotJPEG\"\nbind g \"+frag\"\n";

void defaults(const std::string& baseCfg) {
    ClientInput in;
    in.loadConfig(baseCfg);
    in.addCheatKeyDefaults();
    CHECK(in.keysFor(ui::kCheatCmdFullbright) == std::vector<std::string>{"L"});
    CHECK(in.keysFor(ui::kCheatCmdGod) == std::vector<std::string>{"F10"});
    CHECK(in.keysFor(ui::kCheatCmdNoclip) == std::vector<std::string>{"N"});
    // The defaults take no key of an existing binding: every other command keeps its keys.
    ClientInput base;
    base.loadConfig(baseCfg);
    for (const auto& [key, cmd] : base.bindings()) CHECK(in.commandFromKey(key) == cmd);
    CHECK(in.bindings().size() == base.bindings().size() + 3);
    in.addCheatKeyDefaults(); // idempotent
    CHECK(in.bindings().size() == base.bindings().size() + 3);
}

void conflicts() {
    ClientInput in;
    in.loadConfig(kCfg);
    in.addCheatKeyDefaults();
    // Binding noclip to a key of another command takes the key from it, as the shipped bind rows do (clientinput rebind).
    in.rebind(ui::kCheatCmdNoclip, "W");
    CHECK(in.commandFromKey("W") == ui::kCheatCmdNoclip && in.keysFor("+forward").empty());
    CHECK((in.keysFor(ui::kCheatCmdNoclip) == std::vector<std::string>{"N", "W"})); // second slot: "N or W"
    // The other way round: a shipped command bound to a developer key takes it from the developer command.
    in.rebind("+frag", "L");
    CHECK(in.keysFor(ui::kCheatCmdFullbright).empty() && in.commandFromKey("L") == "+frag");
    // One developer key onto another: the old owner loses it.
    in.rebind(ui::kCheatCmdGod, "N");
    CHECK(in.commandFromKey("N") == ui::kCheatCmdGod && (in.keysFor(ui::kCheatCmdNoclip) == std::vector<std::string>{"W"}));
    // Del clears, the profile text round-trips, and a reloaded profile keeps an unbound command unbound (no defaults added back
    // once the profile carries the marker; the caller decides that, here the config alone).
    in.rebind(ui::kCheatCmdGod, "");
    CHECK(in.keysFor(ui::kCheatCmdGod).empty());
    ClientInput again;
    again.loadConfig(in.formatBindings());
    CHECK(again.bindings() == in.bindings());
    // A default key that another command holds is not taken.
    ClientInput held;
    held.loadConfig("unbindall\nbind L \"+frag\"\n");
    held.addCheatKeyDefaults();
    CHECK(held.commandFromKey("L") == "+frag" && held.keysFor(ui::kCheatCmdFullbright).empty());
    CHECK(held.keysFor(ui::kCheatCmdGod).size() == 1);
    // A command that has a key already keeps it.
    ClientInput own;
    own.loadConfig("unbindall\nbind K \"iw2_fullbright\"\n");
    own.addCheatKeyDefaults();
    CHECK((own.keysFor(ui::kCheatCmdFullbright) == std::vector<std::string>{"K"}) && own.commandFromKey("L").empty());
    // The commands are recognised case-insensitively, nothing else is.
    CHECK(ui::cheatKeyFromCommand("IW2_NoClip") == ui::CheatKey::Noclip && ui::cheatKeyFromCommand("iw2_godmode") == ui::CheatKey::God &&
          ui::cheatKeyFromCommand("iw2_fullbright") == ui::CheatKey::Fullbright && ui::cheatKeyFromCommand("+forward") == ui::CheatKey::None &&
          ui::cheatKeyFromCommand("") == ui::CheatKey::None);
}

void screenshotKey() {
    // A profile saved by 0.2.1: screenshot on F11, F12 free: back to F12.
    ClientInput in;
    in.loadConfig("unbindall\nbind w \"+forward\"\nbind F11 \"screenshotJpeg\"\n");
    CHECK(in.moveScreenshotBackToF12());
    CHECK(in.commandFromKey("F11").empty() && in.commandFromKey("F12") == "screenshotJpeg" && in.commandFromKey("W") == "+forward");
    CHECK(!in.moveScreenshotBackToF12()); // nothing left to move
    // The shipped binding on F12 stays; F12 taken by something else keeps F11 where it is; other commands on F11 are left alone.
    ClientInput shipped;
    shipped.loadConfig(kCfg);
    CHECK(!shipped.moveScreenshotBackToF12() && shipped.commandFromKey("F12") == "screenshotJPEG");
    ClientInput taken;
    taken.loadConfig("unbindall\nbind F11 \"screenshotJpeg\"\nbind F12 \"+frag\"\n");
    CHECK(!taken.moveScreenshotBackToF12() && taken.commandFromKey("F11") == "screenshotJpeg");
    ClientInput other;
    other.loadConfig("unbindall\nbind F11 \"+frag\"\n");
    CHECK(!other.moveScreenshotBackToF12() && other.commandFromKey("F11") == "+frag" && other.commandFromKey("F12").empty());
    // The migration marker is archived in the profile.
    CHECK(ui::isArchivedCvar("iw2_keysver") && ui::isArchivedCvar("iw2_showFps"));
}

const ui::Item* byName(const ui::Menu& m, const std::string& n) {
    for (const auto& it : m.items)
        if (it->name == n) return it.get();
    return nullptr;
}

void optionsPage() {
    for (const bool mp : {false, true}) {
        fs::Vfs vfs;
        ui::MenuLibrary lib(vfs);
        lib.parseSource(ui::openIw2MenuSource(mp), "openiw2/options.menu");
        const ui::Menu* page = lib.find("iw2_options");
        CHECK(page != nullptr);
        if (!page) continue;
        const char* const rows[][2] = {{"iw2_key_fullbright", ui::kCheatCmdFullbright}, {"iw2_key_god", ui::kCheatCmdGod}, {"iw2_key_noclip", ui::kCheatCmdNoclip}};
        const ui::Item* cheats = byName(*page, "iw2_cheats");
        float prevY = cheats ? cheats->rect.y : 0;
        for (const auto& r : rows) {
            const ui::Item* it = byName(*page, r[0]);
            if (mp) { // never in the multiplayer menu set
                CHECK(it == nullptr && byName(*page, std::string(r[0]) + "_label") == nullptr);
                continue;
            }
            CHECK(it && it->type == 14 && it->dvar == r[1]);
            CHECK(it && it->dvarTest == "iw2_cheats" && it->showDvar.size() == 1 && it->showDvar[0] == "1"); // only while Cheats is on
            const ui::Item* label = byName(*page, std::string(r[0]) + "_label");
            CHECK(label && label->decoration && label->dvarTest == "iw2_cheats" && it && label->rect.y == it->rect.y);
            if (it) { CHECK(it->rect.y == prevY + 15); prevY = it->rect.y; }
        }
        // The note moves below the rows while they are shown (two copies in single player, one in multiplayer).
        const ui::Item* note = byName(*page, "iw2_note");
        const ui::Item* noteKeys = byName(*page, "iw2_note_keys");
        CHECK(note != nullptr && (noteKeys == nullptr) == mp);
        if (!mp && note && noteKeys) {
            CHECK(note->hideDvar.size() == 1 && noteKeys->showDvar.size() == 1 && noteKeys->rect.y > prevY);
        }
        // Texts exist in English and Polish.
        for (const char* lang : {"english", "polish"}) {
            std::map<std::string, std::string> t;
            for (const auto& [k, v] : ui::openIw2Strings(lang)) t[k] = v;
            for (const char* k : {"IW2_OPT_KEY_FULLBRIGHT", "IW2_OPT_KEY_GOD", "IW2_OPT_KEY_NOCLIP"}) CHECK(t.count(k) && !t[k].empty());
        }
    }
}

// A decimal field that shows a slider's value sits right of the slider track instead of over its end.
void sliderValue() {
    fs::Vfs vfs;
    ui::MenuLibrary lib(vfs);
    lib.parseSource("{\nmenuDef {\n name \"sl\"\n rect 0 0 400 100\n visible 1\n"
                    " itemDef { name bar\n type 10\n text \" \"\n dvarfloat \"sens\" 5 1 30\n rect 0 10 300 13\n origin 20 0\n textalignx 100\n visible 1 }\n"
                    " itemDef { name num\n type 17\n text \"\"\n dvarfloat \"sens\" 5 1 30\n rect 200 10 60 12\n origin 20 0\n visible 1\n decoration }\n"
                    " itemDef { name apart\n type 17\n text \"\"\n dvarfloat \"sens\" 5 1 30\n rect 200 40 60 12\n origin 20 0\n visible 1\n decoration }\n"
                    "}\n}\n",
                    "slider.menu");
    ui::MenuEnv env;
    env.getCvar = [](const std::string&) { return std::string(); };
    env.setCvar = [](const std::string&, const std::string&) {};
    env.textWidth = [](const ui::Item&) { return 4.0f; };
    ui::MenuStack st(lib, env);
    CHECK(st.open("sl"));
    const ui::Menu* m = st.top();
    const ui::Item* bar = byName(*m, "bar");
    const ui::Item* num = byName(*m, "num");
    const ui::Item* apart = byName(*m, "apart");
    CHECK(bar && num && apart);
    if (!bar || !num || !apart) return;
    const float track = st.sliderTrackX(*m, *bar); // 20 + 100 + 4 + 8
    CHECK(track == 132.0f);
    CHECK(num->rect.x == 220.0f); // starts inside the track (132..228)
    CHECK(st.valueTextX(*m, *num) == track + ui::MenuStack::kSliderTrackWidth + 8.0f);
    CHECK(st.valueTextX(*m, *num) >= track + ui::MenuStack::kSliderTrackWidth);
    CHECK(st.valueTextX(*m, *apart) == 220.0f); // another row: unchanged
}

// Keyboard-only menus (the multiplayer quick-message menus) leave the mouse and movement to the game.
void bypass() {
    CHECK(ui::menuBypassesGameInput(true, false, true));
    CHECK(!ui::menuBypassesGameInput(false, false, true));  // no menu open
    CHECK(!ui::menuBypassesGameInput(true, true, true));    // a full-screen menu takes everything
    CHECK(!ui::menuBypassesGameInput(true, false, false));  // an ordinary menu
    fs::Vfs vfs;
    ui::MenuLibrary lib(vfs);
    lib.parseSource("{\nmenuDef {\n name \"qm\"\n fullscreen 0\n rect 0 0 640 480\n onOpen { setDvar cl_bypassMouseInput \"1\" }\n"
                    " onClose { setDvar cl_bypassMouseInput \"0\" }\n execKey \"1\" { close qm }\n}\n"
                    "menuDef {\n name \"pause\"\n fullscreen 1\n rect 0 0 640 480\n}\n}\n",
                    "qm.menu");
    std::map<std::string, std::string> cv;
    ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    ui::MenuStack st(lib, env);
    const auto bypasses = [&] { return ui::menuBypassesGameInput(!st.empty(), st.takesAllKeys(), env.getCvar("cl_bypassMouseInput") == "1"); };
    CHECK(!bypasses());
    CHECK(st.open("qm"));
    CHECK(bypasses()); // open: no cursor, mouse look and movement stay
    CHECK(st.execKey('1')); // the number key still chooses
    CHECK(st.empty() && !bypasses());
    CHECK(st.open("pause"));
    CHECK(!bypasses());
}

} // namespace

int main() {
    defaults(kCfg);
    conflicts();
    screenshotKey();
    optionsPage();
    sliderValue();
    bypass();
    if (const char* main = std::getenv("IW2_MAIN")) {
        fs::Vfs vfs;
        vfs.mountDirectory(main);
        const auto cfg = vfs.read("default.cfg");
        CHECK(cfg.has_value());
        if (cfg) {
            const std::string text(cfg->begin(), cfg->end());
            defaults(text);
            ClientInput in;
            in.loadConfig(text);
            // The default keys are free in the shipped bindings; the screenshot stays on F12 and is not touched.
            for (const char* k : {"L", "F10", "N"}) CHECK(in.commandFromKey(k).empty());
            CHECK(!in.commandFromKey("F12").empty() && !in.moveScreenshotBackToF12());
        }
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
