// Shipped grenade weapon files (only with IW2_MAIN=<game main dir>): the player's cooking (PM_Weapon 0x4df5a0, fuse ps+0x3c) runs
// only for a weapon with cookOffHold. No shipped single-player grenade sets it. openiw2 adds the option iw2_grenadeCook (default 1):
// frag grenades then cook as if cookOffHold were 1, smoke grenades do not; with 0 the shipped behaviour applies (fuse stays at fuseTime).
// Driven through the real offhand state machine with the real files.

#include "fs/vfs.h"
#include "game/playerweapon.h"

#include <cstdlib>
#include <iostream>
#include <map>
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

using iw2::game::PlayerWeapon;
using Def = PlayerWeapon::Def;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool load(iw2::fs::Vfs& vfs, const std::string& name, Def& out) {
    const auto d = vfs.read("weapons/sp/" + name);
    if (!d) return false;
    const std::string text(d->begin(), d->end());
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\\') parts.push_back(text.substr(start, i - start)), start = i + 1;
    if (parts.empty() || parts[0] != "WEAPONFILE") return false;
    for (size_t i = 1; i + 1 < parts.size(); i += 2) out[lower(parts[i])] = parts[i + 1];
    return true;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "grenade cook test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    const char* const names[] = {"fraggrenade", "mk1britishfrag", "rgd-33russianfrag", "smoke_grenade_american",
                                 "smoke_grenade_british", "smoke_grenade_german", "smoke_grenade_russian"};
    for (const bool option : {false, true}) // iw2_grenadeCook off (shipped behaviour) / on (openiw2 default)
    for (const char* n : names) {
        Def g;
        CHECK(load(vfs, n, g));
        CHECK(g["weapontype"] == "grenade");
        CHECK(g["cookoffhold"] == "0"); // the spec finding: nothing in the shipped SP data enables cooking
        const float fuse = std::stof(g["fusetime"]);
        const bool frag = g["offhandclass"] == "Frag Grenade";
        const bool cooks = option && frag; // smoke never cooks through the option

        const Def a{{"quickdroptime", "0.1"}, {"quickraisetime", "0.1"}, {"raisetime", "0"}};
        for (const float holdSeconds : {1.0f, 12.0f}) {
            PlayerWeapon w;
            w.grenadeCook = option;
            w.lookup = [&](const std::string& k) -> const Def* { return k == "g" ? &g : &a; };
            w.findOffhand = [&](const std::string& cls) { return cls == (frag ? "Frag Grenade" : "Smoke Grenade") ? std::string("g") : std::string(); };
            w.equip("a", a);
            PlayerWeapon::Input in;
            in.weapon = "a";
            w.update(0.01f, in);
            (frag ? in.frag : in.smoke) = true;
            int explodes = 0, throws = 0, fuseMs = -1, heldMs = -1;
            bool pulse = false;
            for (int i = 0; i < int(holdSeconds * 100) + 80; ++i) { // quick drop, pin pull (0.4 s), then held for holdSeconds
                w.update(0.01f, in);
                for (const auto& e : w.takeEvents()) explodes += e.kind == PlayerWeapon::Event::OffhandExplode;
                if (w.stateNumber() == 14) heldMs = w.fuseMs(), pulse = pulse || w.cooking();
            }
            if (cooks && holdSeconds > fuse + 1) { // held past the fuse: it went off in the hand
                CHECK(explodes == 1 && pulse);
                continue;
            }
            CHECK(explodes == 0);
            CHECK(pulse == cooks); // the crosshair pulse term (0x4bd2c0) follows the same test
            if (cooks) CHECK(heldMs < int(fuse * 1000.0f) - 400); else CHECK(heldMs == int(fuse * 1000.0f));
            in.frag = in.smoke = false;
            for (int i = 0; i < 3; ++i) {
                w.update(0.01f, in);
                for (const auto& e : w.takeEvents())
                    if (e.kind == PlayerWeapon::Event::OffhandThrow) ++throws, fuseMs = w.takeFuseMs();
            }
            CHECK(throws == 1);
            if (cooks) CHECK(fuseMs < int(fuse * 1000.0f) - 400 && fuseMs > 0); else CHECK(fuseMs == int(fuse * 1000.0f));
        }
    }
    if (failures) return 1;
    std::cout << "grenade cook tests passed\n";
    return 0;
}
