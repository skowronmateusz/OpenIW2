#include "fs/gamedir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

namespace iw2::fs {

namespace {
std::string lowerName(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string twoDigits(int n) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", n);
    return b;
}
} // namespace

GameDirCheck checkGameDir(const std::filesystem::path& dir) {
    GameDirCheck r;
    std::set<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fe;
        if (e.is_regular_file(fe)) names.insert(lowerName(e.path().filename().string()));
    }
    for (int i = 0; i <= 15; ++i) {
        const std::string n = "iw_" + twoDigits(i) + ".iwd";
        if (!names.count(n)) r.missing.push_back(n);
    }
    // localized_<language>_iwNN.iwd: count the packs 00..10 of each language.
    std::map<std::string, std::set<int>> sets;
    for (const auto& n : names) {
        if (n.rfind("localized_", 0) != 0 || n.size() < 10 + 9) continue;
        const size_t end = n.rfind("_iw");
        if (end == std::string::npos || end <= 10 || n.size() != end + 9 || n.compare(end + 5, 4, ".iwd") != 0) continue;
        const std::string num = n.substr(end + 3, 2);
        if (!std::isdigit(static_cast<unsigned char>(num[0])) || !std::isdigit(static_cast<unsigned char>(num[1]))) continue;
        const int k = std::stoi(num);
        if (k <= 10) sets[n.substr(10, end - 10)].insert(k);
    }
    for (const auto& [lang, packs] : sets)
        if (packs.size() == 11) { r.language = lang; break; }
    if (r.language.empty()) {
        // Report the most complete set's gaps, or a whole set when there is none.
        std::string best = "<language>";
        size_t bestCount = 0;
        for (const auto& [lang, packs] : sets)
            if (packs.size() > bestCount) best = lang, bestCount = packs.size();
        for (int i = 0; i <= 10; ++i)
            if (!bestCount || !sets[best].count(i)) r.missing.push_back("localized_" + best + "_iw" + twoDigits(i) + ".iwd");
    }
    return r;
}

std::optional<std::filesystem::path> readMainPath(const std::filesystem::path& settingsFile) {
    std::ifstream f(settingsFile);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("mainPath=", 0) == 0 && line.size() > 9) return std::filesystem::path(line.substr(9));
    }
    return std::nullopt;
}

bool writeMainPath(const std::filesystem::path& settingsFile, const std::filesystem::path& mainPath) {
    std::vector<std::string> keep;
    {
        std::ifstream f(settingsFile);
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("mainPath=", 0) != 0) keep.push_back(line);
    }
    std::error_code ec;
    std::filesystem::create_directories(settingsFile.parent_path(), ec);
    std::ofstream o(settingsFile, std::ios::trunc);
    if (!o) return false;
    o << "mainPath=" << mainPath.string() << "\n";
    for (const auto& l : keep) o << l << "\n";
    return bool(o);
}

bool rotateLog(const std::filesystem::path& log) {
    std::error_code ec;
    std::filesystem::create_directories(log.parent_path(), ec);
    if (!std::filesystem::is_directory(log.parent_path(), ec)) return false;
    std::filesystem::path old = log;
    old += ".1";
    if (std::filesystem::exists(log, ec)) std::filesystem::rename(log, old, ec); // rename replaces an older .1
    return true;
}

} // namespace iw2::fs
