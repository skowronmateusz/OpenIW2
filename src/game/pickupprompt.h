// The pickup prompt of a weapon item hint (the cursor hint's text when the hint string is empty). VAs in the patch 1.3 MP executable:
// the prompt chooser 0x4c5a80, called by the cursor-hint draw 0x4c5c20 each frame; its inputs are the hinted weapon's definition and
// the player state of that frame, so it is chosen when the hint is drawn, never stored with the hint.
//
// Rules, in order:
//   1. a weapon of class item (class value 9), or one with an offhand class (definition +0x84 non-zero): "new weapon";
//   2. a weapon whose slot (definition +0x80: none 0, primary 1, primaryb 2) is a primary slot while either primary slot
//      byte of the player state (ps +0x554, bytes 1 and 2) is empty: "new weapon";
//   3. the player state's current weapon (ps +0xd4, the weapon number the snapshot carries) takes the same slot as the hinted
//      weapon: no prompt when it is the hinted weapon itself, else "swap";
//   4. else the player state's slot byte of the hinted weapon's slot holds the hinted weapon: no prompt, else "swap".
// No prompt leaves the cursor hint with its icon alone (the draw 0x4c5c20 takes its icon-only path for a null string).
#pragma once

#include <string>

namespace iw2::game {

struct PickupPromptInput {
    std::string weapon;             // the hinted weapon (lower case)
    bool itemClass = false;         // weaponClass item
    bool hasOffhandClass = false;   // offhandClass other than none
    int slot = 0;                   // the hinted weapon's weaponSlot: 0 none, 1 primary, 2 primaryb
    std::string slotWeapon[3];      // the player state's weapon in slot 1 and 2 ("" none); [0] is unused
    std::string current;            // the player state's current weapon ("" none)
    int currentSlot = 0;            // its weaponSlot
};

inline std::string pickupPrompt(const PickupPromptInput& in) {
    if (in.itemClass || in.hasOffhandClass) return "PLATFORM_PICKUPNEWWEAPON";
    if ((in.slot == 1 || in.slot == 2) && (in.slotWeapon[1].empty() || in.slotWeapon[2].empty())) return "PLATFORM_PICKUPNEWWEAPON";
    if (in.currentSlot == in.slot) return in.current == in.weapon ? std::string() : "PLATFORM_SWAPWEAPONS";
    return in.slotWeapon[in.slot] == in.weapon ? std::string() : "PLATFORM_SWAPWEAPONS";
}

} // namespace iw2::game
