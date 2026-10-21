// Player weapon inventory (docs/spec/inventory.md): owned weapons, the "primary"/"primaryb" slots, the current
// offhand, and ammunition shared by ammo name (stock) and clip name (loaded rounds).
#pragma once

#include <map>
#include <set>
#include <string>

namespace iw2::game {

struct Inventory {
    std::set<std::string> owned;           // lower-case weapon names
    std::string slot[3];                   // [1] primary, [2] primaryb; empty = none
    std::string offhand;                   // current offhand weapon
    std::map<std::string, int> ammo;       // stock rounds by ammoName
    std::map<std::string, int> clip;       // loaded rounds by clipName
};

} // namespace iw2::game
