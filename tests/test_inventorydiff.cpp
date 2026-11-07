// Inventory across a level change (src/game/inventorydiff.h): the carried-state text round trip, the difference and the verdict that
// tells a loss the next map's scripts explain from one nothing explains.
#include "game/inventorydiff.h"

#include <iostream>

using namespace iw2::game;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

Inventory carried() {
    Inventory i;
    i.owned = {"enfield", "thompson", "mk1britishfrag"};
    i.slot[1] = "enfield";
    i.slot[2] = "thompson";
    i.offhand = "mk1britishfrag";
    i.ammo = {{"enfield", 90}, {"thompson", 260}};
    i.clip = {{"enfield", 10}, {"thompson", 20}};
    return i;
}
World::LoadoutEvent ev(const char* call, const char* arg) { return {50, call, arg, "maps/_loadout.gsc:10 give_loadout"}; }
} // namespace

int main() {
    // Round trip.
    World::CarriedPlayer c;
    c.inventory = carried();
    c.weapon = "thompson";
    c.health = 275;
    c.maxhealth = 275;
    const auto back = parseCarried(serializeCarried(c));
    CHECK(back && back->weapon == "thompson" && back->inventory.owned == c.inventory.owned && back->inventory.slot[2] == "thompson");
    CHECK(back && back->inventory.ammo == c.inventory.ammo && back->inventory.clip == c.inventory.clip && back->health == 275);
    CHECK(!parseCarried("") && !parseCarried("bogus line\n"));

    // No change.
    CHECK(diffInventory(carried(), "thompson", carried(), "thompson").empty());

    // Lost second weapon, nothing in the scripts takes it: unexplained.
    Inventory after = carried();
    after.owned.erase("thompson");
    after.slot[2].clear();
    after.ammo["thompson"] = 0;
    auto d = diffInventory(carried(), "thompson", after, "enfield");
    CHECK(d.lost.size() == 1 && d.lost[0] == "thompson" && d.gained.empty());
    CHECK(d.slotChanged.size() == 1 && d.weapon && d.weapon->second == "enfield" && d.ammoChanged.count("thompson") == 1);
    auto v = judgeInventory(carried(), d, {ev("giveweapon", "enfield")});
    CHECK(v.unexplainedLost.size() == 1 && v.explainedLost.empty() && v.levelSetsLoadout);
    auto lines = formatInventoryReport(d, v, {});
    bool flagged = false;
    for (const auto& l : lines) flagged = flagged || l.rfind("UNEXPLAINED-LOSS thompson", 0) == 0;
    CHECK(flagged);

    // The same loss with a takeallweapons, a takeweapon of that weapon, or a replaced slot is explained.
    CHECK(judgeInventory(carried(), d, {ev("takeallweapons", "")}).unexplainedLost.empty());
    CHECK(judgeInventory(carried(), d, {ev("takeweapon", "thompson")}).explainedLost.size() == 1);
    CHECK(judgeInventory(carried(), d, {ev("takeweapon", "enfield")}).unexplainedLost.size() == 1);
    CHECK(judgeInventory(carried(), d, {ev("setweaponslotweapon", "primaryb bren")}).explainedLost.size() == 1);
    CHECK(judgeInventory(carried(), d, {ev("setweaponslotweapon", "primary bren")}).unexplainedLost.size() == 1);
    CHECK(!judgeInventory(carried(), d, {}).levelSetsLoadout);

    // Gained weapons, offhand changes and ammo are listed but not losses.
    Inventory more = carried();
    more.owned.insert("binoculars");
    more.offhand = "smoke";
    more.ammo["enfield"] = 100;
    d = diffInventory(carried(), "thompson", more, "thompson");
    CHECK(d.lost.empty() && d.gained.size() == 1 && d.offhand && d.ammoChanged.at("enfield").second == 100);
    CHECK(judgeInventory(carried(), d, {}).unexplainedLost.empty());

    if (failures) return 1;
    std::cout << "inventorydiff OK\n";
    return 0;
}
