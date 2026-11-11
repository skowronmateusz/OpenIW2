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
std::string row(const char* name, const char* labelKey, int type, const char* dvar, const char* list, int y, bool hideInMp) {
    std::string s;
    const std::string hide = hideInMp ? "\t\tdvartest \"ui_multiplayer\"\n\t\thideDvar { \"1\" }\n" : "";
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
        "{\nmenuDef\n{\n\tname iw2_options\n\tfullScreen 0\n\trect 0 0 640 480\n\tfocusColor .98 .827 .58 1\n\tonOpen { setdvar ui_background_gradient_show \"1\"; }\n"
        "\tonESC { close options_menu; }\n"
        "\titemDef\n\t{\n\t\tname iw2_title\n\t\ttype 0\n\t\ttext \"@IW2_OPT_TITLE\"\n\t\torigin 48 64\n\t\ttextscale .5\n\t\tforecolor 1 1 1 1\n\t\tvisible 1\n\t\tdecoration\n\t}\n";
    s += row("iw2_hitmarker", "@IW2_OPT_HITMARKER", 12, "iw2_hitMarker", "\t\tdvarFloatList { \"@MENU_YES\" 1 \"@IW2_OPT_HITMARKER_KILL\" 2 \"@MENU_NO\" 0 }\n", 0, false);
    if (!multiplayer) s += row("iw2_cheats", "@IW2_OPT_CHEATS", 11, "iw2_cheats", "", 15, true);
    s += "}\n}\n";
    return s;
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
    };
    const bool polish = strcasecmp(language.c_str(), "polish") == 0;
    std::vector<std::pair<std::string, std::string>> out;
    for (const Text& t : texts) out.emplace_back(t.key, polish ? t.pl : t.en);
    return out;
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

    // The other entries and the closing of the options menu close the page too (they close the other pages the same way).
    const Command close = {"close", "iw2_options"};
    for (const auto& it : options->items) {
        if (it->name == "iw2_options_button") continue;
        for (size_t i = 0; i < it->action.size(); ++i)
            if (it->action[i].size() > 1 && lowerCopy(it->action[i][0]) == "open" && lowerCopy(it->action[i][1]).rfind("options_", 0) == 0) {
                it->action.insert(it->action.begin() + long(i), close);
                break;
            }
    }
    options->onClose.push_back(close);
    return true;
}

} // namespace iw2::ui
