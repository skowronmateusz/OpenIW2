#include "ui/iw2options.h"

#include <cctype>
#include <cstring>
#include <strings.h>

namespace iw2::ui {

namespace {
std::string lowerCopy(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One label + value row of the page: rows lie 15 apart from origin (50, 120), the value 170 right of the label.
// `gate`: kNoGate, kHideInMp (hidden while ui_multiplayer is 1) or kOnWithCheats (shown only while the Cheats option is on).
enum Gate { kNoGate, kHideInMp, kOnWithCheats, kOnWithEnhanced };
std::string row(const char* name, const char* labelKey, int type, const char* dvar, const char* list, int y, Gate gate) {
    std::string s;
    const std::string hide = gate == kHideInMp         ? "\t\tdvartest \"ui_multiplayer\"\n\t\thideDvar { \"1\" }\n"
                             : gate == kOnWithCheats   ? "\t\tdvartest \"iw2_cheats\"\n\t\tshowDvar { \"1\" }\n"
                             : gate == kOnWithEnhanced ? "\t\tdvartest \"r_enhanced\"\n\t\tshowDvar { \"1\" }\n"
                                                       : "";
    const std::string common =
        "\t\torigin 50 120\n\t\ttextstyle 3\n\t\ttextaligny 11\n\t\ttextscale .25\n\t\tstyle 1\n\t\tbackcolor 0 0 0 0\n\t\tforecolor .9 .9 .9 1\n\t\tvisible 1\n";
    s += std::string("\titemDef\n\t{\n\t\tname ") + name + "_label\n\t\ttype 1\n\t\ttext \"" + labelKey + "\"\n\t\trect 0 " + std::to_string(y) + " 1 13\n\t\ttextalignx 0\n" + common + hide + "\t\tdecoration\n\t}\n";
    s += std::string("\titemDef\n\t{\n\t\tname ") + name + "\n\t\ttype " + std::to_string(type) + "\n\t\ttext \" \"\n\t\tdvar \"" + dvar + "\"\n" + list + "\t\trect 0 " + std::to_string(y) +
         " 320 13\n\t\ttextalignx 170\n" + common + hide + "\t\taction { play \"mouse_click\"; }\n\t\tmouseEnter { play \"mouse_over\"; }\n\t}\n";
    return s;
}
} // namespace

std::string openIw2MenuSource(bool multiplayer) {
    // openiw2's own menu (not from the game files). Item types: 1 BUTTON, 11 YESNO, 12 MULTI.
    std::string s =
        "{\nmenuDef\n{\n\tname iw2_options\n\tfullScreen 0\n\trect 0 0 370 480\n\tfocusColor .98 .827 .58 1\n\tonOpen { setdvar ui_background_gradient_show \"1\"; }\n"
        "\tonESC { close options_menu; }\n"
        "\titemDef\n\t{\n\t\tname iw2_title\n\t\ttype 0\n\t\ttext \"@IW2_OPT_TITLE\"\n\t\torigin 48 64\n\t\ttextscale .5\n\t\tforecolor 1 1 1 1\n\t\tvisible 1\n\t\tdecoration\n\t}\n";
    s += row("iw2_hitmarker", "@IW2_OPT_HITMARKER", 12, "iw2_hitMarker", "\t\tdvarFloatList { \"@MENU_YES\" 1 \"@IW2_OPT_HITMARKER_KILL\" 2 \"@MENU_NO\" 0 }\n", 0, kNoGate);
    s += row("iw2_showfps", "@IW2_OPT_SHOWFPS", 11, "iw2_showFps", "", 15, kNoGate);
    if (!multiplayer) {
        s += row("iw2_cheats", "@IW2_OPT_CHEATS", 11, "iw2_cheats", "", 30, kHideInMp);
        // The three developer key rows (single player only, shown while Cheats is on): BIND items on the commands ClientInput keeps
        // keys for, so they rebind, show "Unbound" / "or" and save like the shipped rows.
        s += row("iw2_key_fullbright", "@IW2_OPT_KEY_FULLBRIGHT", 14, kCheatCmdFullbright, "", 45, kOnWithCheats);
        s += row("iw2_key_god", "@IW2_OPT_KEY_GOD", 14, kCheatCmdGod, "", 60, kOnWithCheats);
        s += row("iw2_key_noclip", "@IW2_OPT_KEY_NOCLIP", 14, kCheatCmdNoclip, "", 75, kOnWithCheats);
    }
    // The note under the rows: word-wrapped decoration text inside the page's width. In single player it sits below the key rows
    // while Cheats is on and right below the Cheats row while it is off (two copies, shown one at a time).
    const auto note = [](const char* name, int y, const char* gate) {
        return std::string("\titemDef\n\t{\n\t\tname ") + name + "\n\t\ttype 0\n\t\ttext \"@IW2_OPT_NOTE\"\n\t\trect 0 " + std::to_string(y) +
               " 300 13\n\t\torigin 50 120\n\t\ttextalignx 0\n\t\ttextaligny 11\n\t\ttextscale .22\n" + gate +
               "\t\tforecolor .7 .7 .7 1\n\t\tautowrapped\n\t\tvisible 1\n\t\tdecoration\n\t}\n";
    };
    if (multiplayer) s += note("iw2_note", 62, "");
    else {
        s += note("iw2_note", 62, "\t\tdvartest \"iw2_cheats\"\n\t\thideDvar { \"1\" }\n");
        s += note("iw2_note_keys", 97, "\t\tdvartest \"iw2_cheats\"\n\t\tshowDvar { \"1\" }\n");
    }
    // "Open log folder" (docs/logging.md "Menu"): a button below the note in both menu sets; its command iw2_openlogs opens the
    // log folder in Finder (iw2view).
    s += "\titemDef\n\t{\n\t\tname iw2_openlogs\n\t\ttype 1\n\t\ttext \"@IW2_OPT_OPENLOGS\"\n\t\trect 0 140 200 13\n\t\torigin 50 120\n"
         "\t\ttextstyle 3\n\t\ttextalignx 0\n\t\ttextaligny 11\n\t\ttextscale .25\n\t\tstyle 1\n\t\tbackcolor 0 0 0 0\n\t\tforecolor .9 .9 .9 1\n"
         "\t\tvisible 1\n\t\taction { play \"mouse_click\"; exec \"" + std::string(kOpenLogsCommand) + "\"; }\n\t\tmouseEnter { play \"mouse_over\"; }\n\t}\n";
    // "Enhanced graphics": the button to the enhanced rendering page (docs/render-enhanced.md "Menu").
    s += "\titemDef\n\t{\n\t\tname iw2_enhanced_button\n\t\ttype 1\n\t\ttext \"@IW2_ENH_TITLE\"\n\t\trect 0 160 200 13\n\t\torigin 50 120\n"
         "\t\ttextstyle 3\n\t\ttextalignx 0\n\t\ttextaligny 11\n\t\ttextscale .25\n\t\tstyle 1\n\t\tbackcolor 0 0 0 0\n\t\tforecolor .9 .9 .9 1\n"
         "\t\tvisible 1\n\t\taction { play \"mouse_click\"; close iw2_options; open " + std::string(kEnhancedPage) + "; }\n"
         "\t\tmouseEnter { play \"mouse_over\"; }\n\t}\n";
    s += "}\n}\n";
    return s;
}

namespace {
bool g_enhMetalFx = false, g_enhRayTracing = false;
}
void setEnhancedAvailability(bool metalFx, bool rayTracing) { g_enhMetalFx = metalFx, g_enhRayTracing = rayTracing; }
bool enhancedRowAvailable(const EnhancedRow& r) {
    return r.needs == EnhancedRow::Always || (r.needs == EnhancedRow::MetalFx && g_enhMetalFx) || (r.needs == EnhancedRow::RayTracing && g_enhRayTracing);
}

std::string enhancedMenuSource() {
    std::string s =
        "{\nmenuDef\n{\n\tname " + std::string(kEnhancedPage) + "\n\tfullScreen 0\n\trect 0 0 370 480\n\tfocusColor .98 .827 .58 1\n"
        "\tonOpen { setdvar ui_background_gradient_show \"1\"; }\n"
        "\tonESC { close " + std::string(kEnhancedPage) + "; open iw2_options; }\n"
        "\titemDef\n\t{\n\t\tname iw2_enh_title\n\t\ttype 0\n\t\ttext \"@IW2_ENH_TITLE\"\n\t\torigin 48 64\n\t\ttextscale .5\n\t\tforecolor 1 1 1 1\n\t\tvisible 1\n\t\tdecoration\n\t}\n";
    const std::string presetOnOff = "\t\tdvarFloatList { \"@IW2_ENH_PRESET\" -1 \"@MENU_ON\" 1 \"@MENU_OFF\" 0 }\n";
    // Row spacing: 15 units, closer when more rows are shown (the stage 5 rows) so the page ends no lower than it does without
    // them (above the bottom line): the gaps between the non-master rows take at most what the always-shown rows take at 15.
    int shown = 0, always = 0;
    for (const EnhancedRow& r : enhancedRows()) {
        if (r.master) continue;
        shown += enhancedRowAvailable(r);
        always += r.needs == EnhancedRow::Always;
    }
    const int step = std::min(15, 15 * std::max(always - 1, 1) / std::max(shown - 1, 1));
    int y = 0;
    for (const EnhancedRow& r : enhancedRows()) {
        if (!enhancedRowAvailable(r)) continue;
        std::string list;
        if (r.kind == EnhancedRow::PresetOnOff) list = presetOnOff;
        else {
            list = "\t\tdvarFloatList {";
            for (const auto& [label, value] : r.choices) list += " \"" + label + "\" " + value;
            list += " }\n";
        }
        s += row(r.name, r.label, 12, r.cvar, list.c_str(), y, r.master ? kNoGate : kOnWithEnhanced);
        y += r.master ? 20 : step;
    }
    // The experimental note, always shown (above the rows, under the title).
    s += "\titemDef\n\t{\n\t\tname iw2_enh_note\n\t\ttype 0\n\t\ttext \"@IW2_ENH_NOTE\"\n\t\trect 0 -36 300 13\n\t\torigin 50 120\n"
         "\t\ttextalignx 0\n\t\ttextaligny 11\n\t\ttextscale .22\n\t\tforecolor 1 .75 .4 1\n\t\tautowrapped\n\t\tvisible 1\n\t\tdecoration\n\t}\n";
    s += "}\n}\n";
    return s;
}

const std::vector<EnhancedRow>& enhancedRows() {
    using C = std::vector<std::pair<std::string, std::string>>;
    static const std::vector<EnhancedRow> rows = {
        {"iw2_enh_master", "@IW2_ENH_MASTER", "r_enhanced", EnhancedRow::Choices, C{{"@MENU_OFF", "0"}, {"@MENU_ON", "1"}}, true},
        {"iw2_enh_preset", "@IW2_ENH_QUALITY", "r_enhPreset", EnhancedRow::Choices, C{{"@MENU_LOW", "0"}, {"@MENU_MEDIUM", "1"}, {"@MENU_HIGH", "2"}}, false},
        {"iw2_enh_sun", "@IW2_ENH_SUN", "r_enhSunShadows", EnhancedRow::Choices,
         C{{"@IW2_ENH_PRESET", "-1"}, {"@MENU_OFF", "0"}, {"@MENU_LOW", "1"}, {"@MENU_MEDIUM", "2"}, {"@MENU_HIGH", "3"}}, false},
        {"iw2_enh_world", "@IW2_ENH_WORLD", "r_enhWorldShadows", EnhancedRow::Choices,
         C{{"@IW2_ENH_PRESET", "-1"}, {"@MENU_OFF", "0"}, {"@MENU_MEDIUM", "1"}, {"@MENU_HIGH", "2"}}, false},
        {"iw2_enh_rtshadow", "@IW2_ENH_RTSHADOW", "r_enhRTShadows", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false, EnhancedRow::RayTracing},
        {"iw2_enh_ao", "@IW2_ENH_AO", "r_enhAO", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_rtao", "@IW2_ENH_RTAO", "r_enhRTContactAO", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false, EnhancedRow::RayTracing},
        {"iw2_enh_contact", "@IW2_ENH_CONTACT", "r_enhContactShadows", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_taa", "@IW2_ENH_TAA", "r_enhTAA", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_metalfx", "@IW2_ENH_METALFX", "r_enhMetalFX", EnhancedRow::Choices, C{{"@MENU_OFF", "0"}, {"@MENU_ON", "1"}}, false, EnhancedRow::MetalFx},
        {"iw2_enh_scale", "@IW2_ENH_SCALE", "r_enhRenderScale", EnhancedRow::Choices,
         C{{"@IW2_ENH_SCALE_NATIVE", "1"}, {"@IW2_ENH_SCALE_Q", "0.77"}, {"@IW2_ENH_SCALE_B", "0.67"}, {"@IW2_ENH_SCALE_P", "0.5"}}, false, EnhancedRow::MetalFx},
        {"iw2_enh_ggx", "@IW2_ENH_GGX", "r_enhGGX", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_ssr", "@IW2_ENH_SSR", "r_enhSSR", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_rtrefl", "@IW2_ENH_RTREFL", "r_enhRTReflections", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false, EnhancedRow::RayTracing},
        {"iw2_enh_lights", "@IW2_ENH_LIGHTS", "r_enhLightShadows", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_rtlights", "@IW2_ENH_RTLIGHTS", "r_enhRTLightShadows", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false, EnhancedRow::RayTracing},
        {"iw2_enh_rtbehind", "@IW2_ENH_RTBEHIND", "r_enhRTBeyondView", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false, EnhancedRow::RayTracing},
        {"iw2_enh_lamps", "@IW2_ENH_LAMPS", "r_enhMapLightSpec", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_probes", "@IW2_ENH_PROBES", "r_enhProbes", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_shafts", "@IW2_ENH_SHAFTS", "r_enhVolumetric", EnhancedRow::Choices,
         C{{"@IW2_ENH_PRESET", "-1"}, {"@MENU_OFF", "0"}, {"@MENU_ON", "0.35"}, {"@IW2_ENH_STRONG", "0.7"}}, false},
        {"iw2_enh_bloom", "@IW2_ENH_BLOOM", "r_enhBloom", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_exposure", "@IW2_ENH_EXPOSURE", "r_enhAutoExposure", EnhancedRow::PresetOnOff, {}, false},
        {"iw2_enh_grade", "@IW2_ENH_GRADE", "r_enhGrade", EnhancedRow::Choices, C{{"@IW2_ENH_NEUTRAL", "0"}, {"@IW2_ENH_FILM", "1"}}, false},
        {"iw2_enh_mblur", "@IW2_ENH_MBLUR", "r_enhMotionBlur", EnhancedRow::Choices, C{{"@MENU_OFF", "0"}, {"@MENU_ON", "1"}}, false},
        {"iw2_enh_dof", "@IW2_ENH_DOF", "r_enhDOF", EnhancedRow::Choices, C{{"@MENU_OFF", "0"}, {"@MENU_ON", "1"}}, false},
        {"iw2_enh_edr", "@IW2_ENH_EDR", "r_enhEdr", EnhancedRow::Choices, C{{"@MENU_ON", "1"}, {"@MENU_OFF", "0"}}, false},
    };
    return rows;
}

std::vector<std::pair<std::string, std::string>> openIw2Strings(const std::string& language) {
    struct Text {
        const char *key, *en, *pl;
    };
    static const Text texts[] = {
        {"IW2_OPT_TITLE", "OpenIW2", "OpenIW2"},
        {"IW2_OPT_HITMARKER", "Hit marker", "Znacznik trafienia"},
        {"IW2_OPT_HITMARKER_KILL", "Red on kill", "Czerwony przy zabiciu"},
        {"IW2_OPT_CHEATS", "Cheats (single player)", "Kody (jeden gracz)"},
        {"IW2_OPT_KEY_FULLBRIGHT", "Fullbright", "Pe\xB3na jasno\x9C\xE6"},
        {"IW2_OPT_KEY_GOD", "God mode", "Tryb Boga"},
        {"IW2_OPT_KEY_NOCLIP", "Noclip (walk / fly)", "Chodzenie / latanie"},
        {"IW2_OPT_SHOWFPS", "Show FPS", "Poka\xBF FPS"},
        {"IW2_OPT_OPENLOGS", "Open log folder", "Otw\xF3rz folder log\xF3w"},
        {"IW2_ENH_TITLE", "Enhanced graphics", "Enhanced graphics"},
        {"IW2_ENH_MASTER", "Enhanced graphics (next start)", "Enhanced graphics (next start)"},
        {"IW2_ENH_QUALITY", "Quality", "Quality"},
        {"IW2_ENH_PRESET", "Preset", "Preset"},
        {"IW2_ENH_STRONG", "Strong", "Strong"},
        {"IW2_ENH_SUN", "Shadows of moving objects", "Shadows of moving objects"},
        {"IW2_ENH_WORLD", "Sharp world shadows", "Sharp world shadows"},
        {"IW2_ENH_AO", "Ambient occlusion", "Ambient occlusion"},
        {"IW2_ENH_CONTACT", "Contact shadows", "Contact shadows"},
        {"IW2_ENH_TAA", "Temporal anti-aliasing", "Temporal anti-aliasing"},
        {"IW2_ENH_METALFX", "MetalFX anti-aliasing", "MetalFX anti-aliasing"},
        {"IW2_ENH_RTREFL", "RT reflections", "RT reflections"},
        {"IW2_ENH_RTSHADOW", "RT shadows of moving objects", "RT shadows of moving objects"},
        {"IW2_ENH_RTAO", "RT contact occlusion", "RT contact occlusion"},
        {"IW2_ENH_RTLIGHTS", "RT shadows of flashes", "RT shadows of flashes"},
        {"IW2_ENH_RTBEHIND", "RT behind the camera", "RT behind the camera"},
        {"IW2_ENH_SCALE", "MetalFX render resolution", "MetalFX render resolution"},
        {"IW2_ENH_SCALE_NATIVE", "Native", "Native"},
        {"IW2_ENH_SCALE_Q", "Quality (77%)", "Quality (77%)"},
        {"IW2_ENH_SCALE_B", "Balanced (67%)", "Balanced (67%)"},
        {"IW2_ENH_SCALE_P", "Performance (50%)", "Performance (50%)"},
        {"IW2_ENH_GGX", "Highlights and reflections", "Highlights and reflections"},
        {"IW2_ENH_SSR", "Screen reflections (water, metal)", "Screen reflections (water, metal)"},
        {"IW2_ENH_LIGHTS", "Shadows of flashes and lamps", "Shadows of flashes and lamps"},
        {"IW2_ENH_LAMPS", "Lamp highlights", "Lamp highlights"},
        {"IW2_ENH_PROBES", "Room reflections", "Room reflections"},
        {"IW2_ENH_SHAFTS", "Sun shafts", "Sun shafts"},
        {"IW2_ENH_BLOOM", "Bloom", "Bloom"},
        {"IW2_ENH_EXPOSURE", "Auto exposure", "Auto exposure"},
        {"IW2_ENH_GRADE", "Colour grade", "Colour grade"},
        {"IW2_ENH_NEUTRAL", "Neutral", "Neutral"},
        {"IW2_ENH_FILM", "Period film", "Period film"},
        {"IW2_ENH_MBLUR", "Motion blur", "Motion blur"},
        {"IW2_ENH_DOF", "Depth of field when aiming", "Depth of field when aiming"},
        {"IW2_ENH_EDR", "HDR display output (next start)", "HDR display output (next start)"},
        {"IW2_ENH_NOTE", "Experimental: Enhanced graphics may cause visual glitches and bugs.",
         "Funkcja eksperymentalna: Enhanced graphics mo\xBF" "e powodowa\xE6 b\xB3\xEA" "dy i usterki obrazu."},
        {"IW2_OPT_NOTE", "These settings are OpenIW2 additions and differ from the original game.", "Te ustawienia to dodatki OpenIW2 i r\xF3\xBFni\xB9 si\xEA od oryginalnej gry."},
    };
    const bool polish = strcasecmp(language.c_str(), "polish") == 0;
    std::vector<std::pair<std::string, std::string>> out;
    for (const Text& t : texts) out.emplace_back(t.key, polish ? t.pl : t.en);
    return out;
}

std::vector<std::pair<std::string, std::string>> openIw2StringOverrides(const std::string& language) {
    if (strcasecmp(language.c_str(), "polish") != 0) return {};
    return {{"MENU_STEADY_SNIPER_RIFLE", "Wstrzymaj oddech (snajper)"},
            {"MENU_TOGGLE_AIM_DOWN_THE_SIGHT", "Sta\xB3" "e celowanie przez przyrz\xB9" "dy"}};
}

bool addOpenIw2Options(MenuLibrary& lib, bool multiplayer) {
    Menu* options = lib.find("options_menu");
    if (!options) return false;
    if (lib.find("iw2_options")) return true; // already added
    const Item* tmpl = nullptr;
    for (const auto& it : options->items)
        if (it->type == 1 && lowerCopy(it->text) == "@menu_game_options" && it->rect.x == 400) tmpl = it.get();
    if (!tmpl) return false;
    lib.parseSource(openIw2MenuSource(multiplayer), "openiw2/options.menu");
    lib.parseSource(enhancedMenuSource(), "openiw2/enhanced.menu");
    if (!lib.find("iw2_options")) return false;

    // The entry: a copy of the Game Options entry, one line below it; what lay at or below that line in its column moves down.
    const float y = tmpl->rect.y + 15.0f;
    for (const auto& it : options->items)
        if (it->type == 1 && it->rect.x == tmpl->rect.x && it->rect.y >= y && it->rect.y < 340.0f) it->rect.y += 15.0f;
    auto entry = std::make_unique<Item>(*tmpl);
    entry->name = "iw2_options_button";
    entry->text = "@IW2_OPT_TITLE";
    entry->rect.y = y;
    for (auto& c : entry->action)
        if (c.size() > 1 && lowerCopy(c[0]) == "open") c[1] = "iw2_options";
    options->items.push_back(std::move(entry));

    // Every entry that opens a page closes all the other pages first (and the OpenIW2 page): two pages open at once are drawn over
    // each other. The shipped entries miss some (the Game Options entry does not close its own page, which our copy would inherit; the
    // single-player Game Options entry has the Shoot page's close commented out), so the missing closes are added here.
    std::vector<std::string> pages = {"iw2_options", kEnhancedPage};
    for (const auto& m : lib.menus()) {
        const std::string n = lowerCopy(m->name);
        if (n.rfind("options_", 0) == 0 && n != "options_menu") pages.push_back(m->name);
    }
    for (const auto& it : options->items)
        for (size_t i = 0; i < it->action.size(); ++i) {
            const Command& c = it->action[i];
            if (!(c.size() > 1 && lowerCopy(c[0]) == "open")) continue;
            const std::string target = lowerCopy(c[1]);
            if (target.rfind("options_", 0) != 0 && target != "iw2_options" && target != kEnhancedPage) continue;
            if (target == "options_graphics_defaults" || target == "options_control_defaults" || target == "options_graphics_texture") break; // popups of their pages
            for (const std::string& pg : pages) {
                if (lowerCopy(pg) == target) continue;
                bool closed = false;
                for (const auto& k : it->action) closed = closed || (k.size() > 1 && lowerCopy(k[0]) == "close" && lowerCopy(k[1]) == lowerCopy(pg));
                if (!closed) it->action.insert(it->action.begin() + long(i++), Command{"close", pg});
            }
            break;
        }
    const Command close = {"close", "iw2_options"};
    options->onClose.push_back(close);
    options->onClose.push_back(Command{"close", kEnhancedPage});
    return true;
}

} // namespace iw2::ui
