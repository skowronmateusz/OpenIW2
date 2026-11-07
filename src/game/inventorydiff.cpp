#include "game/inventorydiff.h"

#include <set>
#include <sstream>

namespace iw2::game {

std::string serializeCarried(const World::CarriedPlayer& c) {
    std::ostringstream o;
    o << "weapon " << c.weapon << "\n";
    o << "owned";
    for (const auto& w : c.inventory.owned) o << " " << w;
    o << "\n";
    o << "primary " << c.inventory.slot[1] << "\n";
    o << "primaryb " << c.inventory.slot[2] << "\n";
    o << "offhand " << c.inventory.offhand << "\n";
    for (const auto& [k, v] : c.inventory.ammo) o << "ammo " << k << " " << v << "\n";
    for (const auto& [k, v] : c.inventory.clip) o << "clip " << k << " " << v << "\n";
    if (c.health) o << "health " << *c.health << "\n";
    if (c.maxhealth) o << "maxhealth " << *c.maxhealth << "\n";
    return o.str();
}

std::optional<World::CarriedPlayer> parseCarried(const std::string& text) {
    World::CarriedPlayer c;
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        any = true;
        if (key == "weapon") {
            ls >> c.weapon;
        } else if (key == "owned") {
            std::string w;
            while (ls >> w) c.inventory.owned.insert(w);
        } else if (key == "primary") {
            ls >> c.inventory.slot[1];
        } else if (key == "primaryb") {
            ls >> c.inventory.slot[2];
        } else if (key == "offhand") {
            ls >> c.inventory.offhand;
        } else if (key == "ammo" || key == "clip") {
            std::string n;
            int v = 0;
            if (!(ls >> n >> v)) return std::nullopt;
            (key == "ammo" ? c.inventory.ammo : c.inventory.clip)[n] = v;
        } else if (key == "health") {
            int v;
            if (ls >> v) c.health = v;
        } else if (key == "maxhealth") {
            int v;
            if (ls >> v) c.maxhealth = v;
        } else {
            return std::nullopt;
        }
    }
    if (!any) return std::nullopt;
    return c;
}

InventoryDiff diffInventory(const Inventory& b, const std::string& wb, const Inventory& a, const std::string& wa) {
    InventoryDiff d;
    for (const auto& w : b.owned)
        if (!a.owned.count(w)) d.lost.push_back(w);
    for (const auto& w : a.owned)
        if (!b.owned.count(w)) d.gained.push_back(w);
    auto cmp = [](const std::map<std::string, int>& x, const std::map<std::string, int>& y, std::map<std::string, std::pair<int, int>>& out) {
        std::set<std::string> keys;
        for (const auto& kv : x) keys.insert(kv.first);
        for (const auto& kv : y) keys.insert(kv.first);
        for (const auto& k : keys) {
            const int p = x.count(k) ? x.at(k) : 0, q = y.count(k) ? y.at(k) : 0;
            if (p != q) out[k] = {p, q};
        }
    };
    cmp(b.ammo, a.ammo, d.ammoChanged);
    cmp(b.clip, a.clip, d.clipChanged);
    static const char* const names[3] = {"", "primary", "primaryb"};
    for (int i = 1; i <= 2; ++i)
        if (b.slot[i] != a.slot[i])
            d.slotChanged.push_back(std::string(names[i]) + ": " + (b.slot[i].empty() ? "none" : b.slot[i]) + " -> " + (a.slot[i].empty() ? "none" : a.slot[i]));
    if (b.offhand != a.offhand) d.offhand = {b.offhand, a.offhand};
    if (wb != wa) d.weapon = {wb, wa};
    return d;
}

InventoryVerdict judgeInventory(const Inventory& before, const InventoryDiff& d, const std::vector<World::LoadoutEvent>& events) {
    InventoryVerdict v;
    bool takeAll = false;
    std::set<std::string> taken, slotsReplaced; // slotsReplaced: "primary" / "primaryb"
    for (const auto& e : events) {
        if (e.call == "giveweapon" || e.call == "takeweapon" || e.call == "takeallweapons" || e.call == "setweaponslotweapon") v.levelSetsLoadout = true;
        if (e.call == "takeallweapons") takeAll = true;
        if (e.call == "takeweapon") taken.insert(e.arg);
        if (e.call == "setweaponslotweapon") slotsReplaced.insert(e.arg.substr(0, e.arg.find(' ')));
    }
    for (const auto& w : d.lost) {
        const bool inSlot = (before.slot[1] == w && slotsReplaced.count("primary")) || (before.slot[2] == w && slotsReplaced.count("primaryb"));
        (takeAll || taken.count(w) || inSlot ? v.explainedLost : v.unexplainedLost).push_back(w);
    }
    return v;
}

std::vector<std::string> formatInventoryReport(const InventoryDiff& d, const InventoryVerdict& v, const std::vector<World::LoadoutEvent>& events) {
    auto nz = [](const std::string& s) { return s.empty() ? std::string("none") : s; };
    std::vector<std::string> out;
    if (d.empty()) out.push_back("inventory unchanged");
    for (const auto& w : d.lost) out.push_back("lost weapon " + w);
    for (const auto& w : d.gained) out.push_back("gained weapon " + w);
    for (const auto& s : d.slotChanged) out.push_back("slot " + s);
    if (d.offhand) out.push_back("offhand " + nz(d.offhand->first) + " -> " + nz(d.offhand->second));
    if (d.weapon) out.push_back("current weapon " + nz(d.weapon->first) + " -> " + nz(d.weapon->second));
    for (const auto& [k, p] : d.ammoChanged) out.push_back("ammo " + k + " " + std::to_string(p.first) + " -> " + std::to_string(p.second));
    for (const auto& [k, p] : d.clipChanged) out.push_back("clip " + k + " " + std::to_string(p.first) + " -> " + std::to_string(p.second));
    out.push_back(std::string("level scripts set the loadout: ") + (v.levelSetsLoadout ? "yes" : "no"));
    for (const auto& e : events) out.push_back("  " + std::to_string(e.ms) + " ms " + e.call + " " + e.arg + " at " + e.site);
    for (const auto& w : v.explainedLost) out.push_back("EXPLAINED-LOSS " + w + " (taken by the level scripts)");
    for (const auto& w : v.unexplainedLost) out.push_back("UNEXPLAINED-LOSS " + w + " (carried in, gone, and no script call takes it)");
    return out;
}

} // namespace iw2::game
