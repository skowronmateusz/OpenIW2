// Weapon item pickup rules: the touch test of G_TouchTriggers / BG_PlayerTouchesItem, the
// "can the item be grabbed" test (BG_CanItemBeGrabbed 0x4fad50) and the ammo an item carries (Pickup_Weapon 0x521170).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace iw2::game {

// G_TouchTriggers (0x4e7320) collects the entities inside the box origin ± (40, 40, 52) around the player's origin
// (0x591f78 / 0x59230c); for an item (entity type 2) BG_PlayerTouchesItem (0x4facc0) then needs the player origin minus
// the item position within [-36, 36] in x and y and within [-88, 18] in z (0x591f5c / 0x5921ac, 0x591f44 / 0x5921a8).
// Weapon items have the box (-1, -1, -1)..(1, 1, 1) (FinishSpawningItem 0x5225e0).
bool playerTouchesItem(const std::array<float, 3>& playerOrigin, const std::array<float, 3>& itemOrigin);

// BG_CanItemBeGrabbed (0x4fad50), weapon items: an offhand weapon (offhandClass set) or weaponClass "item" and an owned
// weapon need room for more ammunition (0x4dcc00); any other weapon can only be taken by use (not by touch).
bool canGrabWeaponItem(bool offhandOrItemClass, bool owned, bool touch, int room);

// What an item carries (entity +0x148 stock / +0x154 clip: -1 = none, 0 = not set yet) once Pickup_Weapon has resolved it.
struct ItemAmmo {
    int stock = 0;
    int clip = 0;
};

// The start of Pickup_Weapon (0x521170): an unset stock is a random draw from dropAmmoMin..dropAmmoMax (the draw is
// (rand() >> 17) % (max - min) + min, a range of equal values gives that value) or, with both 0, the default
// round((r + 1) * (clipSize - 1) * 0.5) + 1 for r = (rand() >> 17) / 32768; the stock is limited to maxAmmo; an unset
// clip then takes min(clipSize, stock) from the stock (not with a negative stock) and the clip is limited to clipSize.
// `seed` is the game's rand() state (0x5a56f4, seed = seed * 0x343fd + 0x269ec3).
ItemAmmo resolveItemAmmo(ItemAmmo item, int dropAmmoMin, int dropAmmoMax, int clipSize, int maxAmmo, uint32_t& seed);

// The ammunition a non-player dropper puts on the weapon item (0x5220f0 with no client, 0x522185..0x52231f), before the item stores
// it as +0x148 / +0x154 (-1 for 0): with dropAmmoMin and dropAmmoMax both 0 the total is round((r1 + 1) * (clipSize - 1) * 0.5) + 1
// and the clip round(total * (r2 / 65536 + 0.25)); a negative max gives nothing; else the total is (max - min) range draw (equal: min),
// none when <= 0, and the clip a draw % clipSize (0 for clipSize 0) up to the total; stock = total - clip. Draws: rand() >> 17 (r1 / 32768).
ItemAmmo actorDropAmmo(int dropAmmoMin, int dropAmmoMax, int clipSize, uint32_t& seed);

// After an owned weapon's ammunition was taken only in part (`added` of stock + clip): what stays on the item.
ItemAmmo itemAmmoAfterPartial(ItemAmmo item, int added);

// The sound of the pickup events raised on the player by Touch_Item (0x521a90) after a successful Pickup_Weapon: event 0x90 (a new
// weapon) plays the weapon's pickupSound alias, event 0x91 (ammunition added to an owned weapon) its ammoPickupSound alias. The
// client handler (0x4bad52, sound start 0x4a34a0) reads the table entry (0x1040c80 + index * 0x24): +0x1c is the alias looked up
// (0x428320) from the weapon's pickupSound key (the item table copy of weapon field +0x98, entry setup 0x4b4960 / 0x4b49e9) and +0x20
// the one looked up from ammoPickupSound (0x4b3b9d). A key that is empty or names no loaded alias leaves the entry 0 and the weapon
// registration (0x4b3b3b..0x4b3ba7) stores the fixed aliases weap_pickup (0x580434) / weap_ammo_pickup (0x580420) instead.
// `exists` is the alias lookup. Nearly every single-player weapon file leaves ammoPickupSound empty, so the fixed alias plays.
std::string pickupSoundAlias(const std::string& key, bool ammoOnly, const std::function<bool(const std::string&)>& exists);

// What the client does for the local player's pickup events 0x90 / 0x91 besides the sound (handler 0x4bad52 -> 0x4ba680, only when the event's
// entity is the local client). 0x4ba680 reads the item table entry (0x5a6714 / 0x5a6718: weapon items only): a weapon of class "item" does nothing;
// an offhand weapon (def +0x84 set) runs 0x4b8ba0 when no offhand is selected yet (0xf79700 == 0): it stores the weapon and touches the offhandinfo
// HUD fade (0xf79a48 = level time, menu flag +0xe8 |= 4); any other weapon runs the weapon select 0x4b8360 when no weapon is selected (0xf796e0 == 0):
// it stores the weapon as selected, stamps the weapon-change time 0xf796e4, touches offhandinfo (0x4a6130(4)) and the ammo display
// (0xf79a40) and ends a toggled aim. With something already selected the pickup draws no icon, text or flash of its own (the text
// message is the separate "gm" client command of Pickup_Weapon).
enum class PickupHudAction { None, SelectPrimary, TouchOffhand };
PickupHudAction pickupHudAction(bool itemClass, bool offhand, bool primarySelected, bool offhandSelected);

} // namespace iw2::game
