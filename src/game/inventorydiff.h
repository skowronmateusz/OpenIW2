// Player inventory across a level change (iw2game --carry-out / --carry-from / --inventory-check, docs/debug-flags.md "Inventory
// across level changes"): a text form of the carried player state, the difference between the state handed to the next map and the
// inventory a few seconds into it, and a verdict that separates what the next map's own scripts explain (take / give calls) from a
// loss nothing in the scripts accounts for. Debug tooling only.
#pragma once

#include "game/inventory.h"
#include "game/world.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

// Text round trip of the carried state (one `key value...` line each; lists sorted).
std::string serializeCarried(const World::CarriedPlayer& c);
std::optional<World::CarriedPlayer> parseCarried(const std::string& text);

struct InventoryDiff {
    std::vector<std::string> lost, gained;                      // owned weapons
    std::map<std::string, std::pair<int, int>> ammoChanged;     // ammoName -> before, after (stock)
    std::map<std::string, std::pair<int, int>> clipChanged;     // clipName -> before, after (loaded)
    std::vector<std::string> slotChanged;                       // "primary: a -> b"
    std::optional<std::pair<std::string, std::string>> offhand; // before, after
    std::optional<std::pair<std::string, std::string>> weapon;  // current weapon before, after
    bool empty() const { return lost.empty() && gained.empty() && ammoChanged.empty() && clipChanged.empty() && slotChanged.empty() && !offhand && !weapon; }
};
InventoryDiff diffInventory(const Inventory& before, const std::string& weaponBefore, const Inventory& after, const std::string& weaponAfter);

struct InventoryVerdict {
    std::vector<std::string> explainedLost;   // lost, and a take / slot call of the level scripts accounts for it
    std::vector<std::string> unexplainedLost; // lost, nothing in the scripts takes it: a real loss
    bool levelSetsLoadout = false;            // the scripts called giveweapon / takeweapon / takeallweapons / setweaponslotweapon
};
InventoryVerdict judgeInventory(const Inventory& before, const InventoryDiff& d, const std::vector<World::LoadoutEvent>& events);

// Report lines (no trailing newline): the differences, then the verdict ("UNEXPLAINED-LOSS ..." marks a finding).
std::vector<std::string> formatInventoryReport(const InventoryDiff& d, const InventoryVerdict& v, const std::vector<World::LoadoutEvent>& events);

} // namespace iw2::game
