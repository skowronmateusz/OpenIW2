#include "ui/profile.h"

#include <fstream>
#include <iterator>

#include <cctype>
#include <sstream>

namespace iw2::ui {

bool isArchivedCvar(const std::string& name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    if (n == "sensitivity" || n == "m_pitch" || n == "snd_volume" || n == "g_gameskill" || n == "mis_difficulty" || n == "mis_none") return true;
    if (n == "r_gamma" || n == "r_ignorehwgamma") return true; // renderer registrations with flags 0x2001 / 0x2021 (archive), gfx 0x1000ab8a / 0x1000abc0
    if (n == "r_texturemode" || n == "r_anisotropy") return true; // renderer registrations with flags 0x2001 (archive), gfx 0x1000abb6 / 0x1000abd1
    if (n == "ai_corpsecount") return true; // flags 0x1003 at 0x4d887f (archive)
    // Client / cgame option dvars registered with flags 0x1001 (archive): cg_blood (0x4a249d), cg_drawCrosshair, cg_subtitles,
    // m_filter, cl_freelook (client block 0x40e2d5..0x40e400); r_swapInterval with 0x2021 (archive + latched, gfx 0x1000b1d1).
    if (n == "cg_blood" || n == "cg_drawcrosshair" || n == "cg_subtitles" || n == "m_filter" || n == "cl_freelook" || n == "r_swapinterval")
        return true;
    if (n == "iw2_hitmarker") return true; // openiw2 option (docs/debug-flags.md), archived like the shipped options
    if (n == "cg_hudgrenadeiconinscope") return true; // flags 0x1001 at 0x4a1622 (archive)
    if (n == "sv_lastsavegame") return true; // registered with flags 0x1001 (archive) at 0x449d78
    return n.size() == 6 && n.compare(0, 4, "mis_") == 0 && std::isdigit(static_cast<unsigned char>(n[4])) && std::isdigit(static_cast<unsigned char>(n[5]));
}

std::string immediateMenuDvar(const std::string& name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    return n == "ui_r_texturemode" ? "r_texturemode" : "";
}

std::map<std::string, std::string> parseConfig(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd, name;
        ls >> cmd >> name;
        for (auto& c : cmd) c = char(std::tolower(static_cast<unsigned char>(c)));
        if ((cmd != "seta" && cmd != "set") || name.empty()) continue;
        std::string rest;
        std::getline(ls >> std::ws, rest);
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
        out[name] = rest;
    }
    return out;
}

std::string formatConfig(const std::map<std::string, std::string>& cvars) {
    std::string out;
    for (const auto& [k, v] : cvars)
        if (isArchivedCvar(k)) out += "seta " + k + " \"" + v + "\"\n";
    return out;
}

std::string profilePlayerName(const std::filesystem::path& playersDir, const std::string& menuName) {
    if (!menuName.empty()) return menuName;
    std::string profile;
    if (std::ifstream af(playersDir / "active.txt"); af) std::getline(af, profile);
    while (!profile.empty() && (profile.back() == '\r' || profile.back() == ' ')) profile.pop_back();
    if (profile.empty()) return "player";
    if (std::ifstream cf(playersDir / profile / "config.cfg"); cf) {
        const auto cv = parseConfig(std::string(std::istreambuf_iterator<char>(cf), {}));
        if (const auto it = cv.find("name"); it != cv.end() && !it->second.empty()) return it->second;
    }
    return profile == "default" ? "player" : profile;
}

bool runsInAppBundle(std::string_view basePath) { return basePath.find(".app/Contents/") != std::string_view::npos; }

} // namespace iw2::ui
