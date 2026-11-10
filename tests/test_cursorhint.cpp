// The held cursor hint (game::CursorHintFade) and the pickup prompt (game::pickupPrompt): no game data needed.
#include "game/cursorhint.h"
#include "game/pickupprompt.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

using iw2::game::CursorHintFade;
using iw2::game::PickupPromptInput;
using iw2::game::pickupPrompt;

CursorHintFade::Hint weaponHint(const char* w, const char* text) { return {5, w, text}; }

void testHold() {
    CursorHintFade f;
    CHECK(f.update({}, 1000) < 0.0f);  // nothing held, nothing drawn
    CHECK(f.update(weaponHint("kar98k_mp", "PLATFORM_SWAPWEAPONS"), 5000) == 1.0f);
    // The hint is gone: the last one is held for cg_hintFadeTime (100 ms), full alpha while more than the 100 ms span remains
    // (here the span equals the time, so the alpha falls from the first frame).
    const float a50 = f.update({}, 5050);
    CHECK(a50 > 0.45f && a50 < 0.55f);
    // Icon and text are the same held hint: both still there while it fades.
    CHECK(f.held().weapon == "kar98k_mp" && f.held().text == "PLATFORM_SWAPWEAPONS");
    CHECK(f.update({}, 5100) < 0.0f);  // expired
    CHECK(!f.valid());
    CHECK(f.update({}, 5101) < 0.0f);
    // A hint coming back while fading takes over at full alpha.
    f.update(weaponHint("a_mp", ""), 6000);
    f.update({}, 6040);
    CHECK(f.update(weaponHint("b_mp", "T"), 6050) == 1.0f && f.held().weapon == "b_mp");
}

// Bug: a hint held from the previous game stayed on screen in the next one. The held hint carries its start time from the old
// frame clock; the new game's clock restarts near 0, so the hint never expired (the elapsed time was negative). Two guards:
// reset() forgets it, and a clock that went backwards under a held hint drops it too.
void testNewGame() {
    CursorHintFade f;
    f.update(weaponHint("kar98k_mp", "PLATFORM_SWAPWEAPONS"), 600000);
    f.update({}, 600010);
    f.reset();
    CHECK(f.update({}, 40) < 0.0f);
    CHECK(!f.valid() && f.held().weapon.empty() && f.held().text.empty());

    CursorHintFade g;
    g.update(weaponHint("kar98k_mp", "PLATFORM_SWAPWEAPONS"), 600000);
    CHECK(g.update({}, 40) < 0.0f);  // without a reset: the clock restarted, the old hint is not drawn
    CHECK(!g.valid());
    // A hint of the new game works normally.
    CHECK(g.update(weaponHint("mp40_mp", "X"), 60) == 1.0f);
}

PickupPromptInput base() {
    PickupPromptInput in;
    in.weapon = "kar98k_mp";
    in.slot = 1;
    in.slotWeapon[1] = "mp40_mp";
    in.slotWeapon[2] = "m1garand_mp";
    in.current = "mp40_mp";
    in.currentSlot = 1;
    return in;
}

void testPrompt() {
    // Both primary slots held, the hinted weapon is for the slot of the current weapon: swap.
    CHECK(pickupPrompt(base()) == "PLATFORM_SWAPWEAPONS");
    // A free primary slot: a new weapon.
    PickupPromptInput a = base();
    a.slotWeapon[2].clear();
    CHECK(pickupPrompt(a) == "PLATFORM_PICKUPNEWWEAPON");
    // An item class weapon or one with an offhand class (a grenade): new weapon, whatever the slots.
    a = base();
    a.itemClass = true;
    CHECK(pickupPrompt(a) == "PLATFORM_PICKUPNEWWEAPON");
    a = base();
    a.hasOffhandClass = true;
    CHECK(pickupPrompt(a) == "PLATFORM_PICKUPNEWWEAPON");
    // The hinted weapon is the current one: no prompt (the hint draws its icon alone).
    a = base();
    a.weapon = a.current;
    CHECK(pickupPrompt(a).empty());
    // The hinted weapon is for the other slot than the current weapon: swap unless it sits there already.
    a = base();
    a.slot = 2;
    CHECK(pickupPrompt(a) == "PLATFORM_SWAPWEAPONS");
    a.weapon = "m1garand_mp";
    CHECK(pickupPrompt(a).empty());
    // The picked-up weapon's swap: the dropped weapon is hinted while the new one is the current weapon of the player state.
    // The prompt follows the player state's current weapon, so it is the swap text at once.
    a = base();
    a.weapon = "mp40_mp";       // the weapon that was dropped, lying on the ground now
    a.current = "kar98k_mp";    // the player state's weapon after the swap
    a.slotWeapon[1] = "kar98k_mp";
    CHECK(pickupPrompt(a) == "PLATFORM_SWAPWEAPONS");
    // The view weapon's animation state still names the old weapon while it is lowered: that is not an input here, but had it been
    // the current weapon the prompt would be empty (the bug).
    a.current = "mp40_mp";
    CHECK(pickupPrompt(a).empty());
    // A weapon without a slot compares slot 0.
    a = base();
    a.slot = 0;
    a.currentSlot = 0;
    a.current = "colt_mp";
    a.weapon = "luger_mp";
    CHECK(pickupPrompt(a) == "PLATFORM_SWAPWEAPONS");
    a.weapon = "colt_mp";
    CHECK(pickupPrompt(a).empty());
}
} // namespace

int main() {
    testHold();
    testNewGame();
    testPrompt();
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "cursorhint: ok\n";
    return failures ? 1 : 0;
}
