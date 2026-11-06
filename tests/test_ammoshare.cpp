// Shared ammunition (only with IW2_MAIN=<game main dir>). The exe registers one ammo definition per ammoName (first registered weapon
// owns the index, 0x4dbdc0) and prints a message when a later weapon with the same name has another maxAmmo; the HUD ammo counter then reads
// maxAmmo from the owner (0x4a76b0). The HUD here uses the current weapon's own maxAmmo, which is the same number exactly when the
// shipped single-player weapon files agree. This test pins that data property.

#include "fs/vfs.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "ammo share test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    std::map<std::string, std::set<std::string>> maxByAmmo;
    int files = 0;
    for (const std::string& path : vfs.list("weapons/sp/")) {
        const auto d = vfs.read(path);
        if (!d) continue;
        const std::string text(d->begin(), d->end());
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
            if (i == text.size() || text[i] == '\\') parts.push_back(text.substr(start, i - start)), start = i + 1;
        if (parts.empty() || parts[0] != "WEAPONFILE") continue;
        std::string ammo, maxAmmo;
        for (size_t i = 1; i + 1 < parts.size(); i += 2) {
            if (parts[i] == "ammoName") ammo = parts[i + 1];
            else if (parts[i] == "maxAmmo") maxAmmo = parts[i + 1];
        }
        if (ammo.empty()) continue;
        maxByAmmo[ammo].insert(maxAmmo);
        ++files;
    }
    int failures = 0;
    if (files == 0) std::cerr << "no weapon files found\n", ++failures;
    for (const auto& [ammo, maxes] : maxByAmmo)
        if (maxes.size() != 1) std::cerr << "ammo " << ammo << " has differing maxAmmo values\n", ++failures;
    return failures ? 1 : 0;
}
