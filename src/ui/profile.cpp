#include "ui/profile.h"

#include <cctype>
#include <sstream>

namespace iw2::ui {

bool isArchivedCvar(const std::string& name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    if (n == "sensitivity" || n == "m_pitch" || n == "snd_volume" || n == "g_gameskill" || n == "mis_difficulty") return true;
    return n.size() == 6 && n.compare(0, 4, "mis_") == 0 && std::isdigit(static_cast<unsigned char>(n[4])) && std::isdigit(static_cast<unsigned char>(n[5]));
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

} // namespace iw2::ui
