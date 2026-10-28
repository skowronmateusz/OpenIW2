// Weapon item pickup rules: the touch test of G_TouchTriggers / BG_PlayerTouchesItem, the
// "can the item be grabbed" test (BG_CanItemBeGrabbed 0x4fad50) and the ammo an item carries (Pickup_Weapon 0x521170).
#pragma once

#include <array>
#include <cstdint>

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

} // namespace iw2::game
