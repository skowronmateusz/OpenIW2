// Touch pickup of weapon items (G_TouchTriggers 0x4e7320 -> BG_PlayerTouchesItem 0x4facc0 -> Touch_Item 0x521a90 ->
// BG_CanItemBeGrabbed 0x4fad50 -> Pickup_Weapon 0x521170).
#include "game/itempickup.h"
#include "game/world.h"

#include <algorithm>
#include <set>

namespace iw2::game {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool isOffhandLike(const std::string& offhandClass, const std::string& weaponClass) {
    return (!offhandClass.empty() && offhandClass != "none") || weaponClass == "item";
}
} // namespace

// Room for more ammunition (0x4dcc00): clip-only weapons count the clip against clipSize, others the stock against
// maxAmmo; a shared cap (sharedAmmoCapName group, sharedAmmoCap) limits the total over the owned weapons of the group.
int World::weaponRoom(Inventory& inv, const std::string& w) {
    const bool clipOnly = weaponInt(w, "clipOnly") != 0;
    const std::string a = weaponField(w, "ammoName"), c = weaponField(w, "clipName");
    int room = clipOnly ? weaponInt(w, "clipSize") - inv.clip[c] : weaponInt(w, "maxAmmo") - inv.ammo[a];
    const std::string group = weaponField(w, "sharedAmmoCapName");
    if (!group.empty()) {
        int total = 0;
        std::set<std::string> counted; // weapons sharing a clip / ammo pool count it once
        for (const std::string& o : inv.owned) {
            if (weaponField(o, "sharedAmmoCapName") != group) continue;
            const std::string oc = weaponField(o, "clipName"), oa = weaponField(o, "ammoName");
            if (!counted.insert("c:" + oc).second) continue;
            total += weaponInt(o, "clipOnly") ? inv.clip[oc] : inv.ammo[oa] + inv.clip[oc];
        }
        room = std::min(room, weaponInt(w, "sharedAmmoCap") - total);
    }
    return room;
}

// Stock shown by the HUD (0x4dcd60). No shared cap group (def +0x1e4 < 0): the clip of a clipOnly weapon, else the stock. With a group:
// the sum over the owned weapons of the same group of their clip (clipOnly) or stock, each clip pool and each stock pool counted once
// (separate per-index "already counted" flags for clips and stocks, 0x4dcd90 / 0x4dcda2).
int World::hudStock(Inventory& inv, const std::string& w) {
    const std::string group = weaponField(w, "sharedAmmoCapName");
    if (group.empty()) {
        return weaponInt(w, "clipOnly") ? inv.clip[weaponField(w, "clipName")] : inv.ammo[weaponField(w, "ammoName")];
    }
    int total = 0;
    std::set<std::string> clips, stocks;
    for (const std::string& o : inv.owned) {
        if (weaponField(o, "sharedAmmoCapName") != group) continue;
        if (weaponInt(o, "clipOnly")) {
            const std::string c = weaponField(o, "clipName");
            if (clips.insert(c).second) total += inv.clip[c];
        } else {
            const std::string a = weaponField(o, "ammoName");
            if (stocks.insert(a).second) total += inv.ammo[a];
        }
    }
    return total;
}

int World::hudGroupStock(const std::string& w) {
    if (!player_ || weaponField(w, "sharedAmmoCapName").empty()) return -1;
    Inventory& inv = inventory(*player_);
    return inv.owned.count(w) ? hudStock(inv, w) : -1;
}

// After an ammo add (0x520e40 tail, 0x520f3d..0x520ff5): a weapon in a shared-cap group (def +0x1e4 >= 0) whose room (0x4dcc00) went
// negative gives the excess back: from the clip of a clipOnly weapon (+0x340), else from the stock, not below 0; a clipOnly weapon left
// with an empty clip is taken away (0x4dc8a0).
void World::trimSharedAmmo(Inventory& inv, const std::string& w) {
    if (weaponField(w, "sharedAmmoCapName").empty()) return;
    const int room = weaponRoom(inv, w);
    if (room >= 0) return;
    if (weaponInt(w, "clipOnly") != 0) {
        int& c = inv.clip[weaponField(w, "clipName")];
        c += room;
        if (c <= 0) {
            c = 0;
            takeWeapon(inv, w);
        }
    } else {
        int& a = inv.ammo[weaponField(w, "ammoName")];
        a = std::max(0, a + room);
    }
}

void World::debugSpawnWeaponItem(const std::string& name) {
    if (!player_) return;
    Entity& item = spawnEntity("weapon_" + lower(name));
    item.obj->set("origin", player_->obj->get("origin"));
}

void World::touchItems() {
    if (!autoPickup_ || !player_ || !isAlive(*player_)) return;
    const Value po = player_->obj->get("origin");
    if (!po.isVector()) return;
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& e = *entities_[i];
        if (e.obj->removed || e.classname.compare(0, 7, "weapon_") != 0 || !weapon(lower(e.classname.substr(7)))) continue;
        const Value io = e.obj->get("origin");
            if (!io.isVector() || !playerTouchesItem(po.asVector(), io.asVector())) continue;
        if (touchWeaponItem(e)) e.obj->removed = true;
    }
}

// Returns true when the item was used up. TODO(spec): the HUD pickup call 0x4ba680, the flag 0x9bef24 of the touch
// notify and the AI-drop path of 0x521170; the offhand replacement rule of 0x531e30 (an offhand is only set when none).
bool World::touchWeaponItem(Entity& item) {
    const std::string w = lower(item.classname.substr(7));
    Inventory& inv = inventory(*player_);
    const bool owned = inv.owned.count(w) != 0;
    const bool offhandLike = isOffhandLike(lower(weaponField(w, "offhandClass")), lower(weaponField(w, "weaponClass")));
    const int room = weaponRoom(inv, w);
    if (!canGrabWeaponItem(offhandLike, owned, true, room)) return false;

    // The item's ammunition is resolved on the first touch and kept on the item (a partial pickup leaves the rest).
    ItemAmmo ammo;
    if (item.obj->get("$ammoStock").isNumber()) {
        ammo = {item.obj->get("$ammoStock").asInt(), item.obj->get("$ammoClip").asInt()};
    } else {
        ammo = resolveItemAmmo({}, weaponInt(w, "dropAmmoMin"), weaponInt(w, "dropAmmoMax"), weaponInt(w, "clipSize"),
                               weaponInt(w, "maxAmmo"), meleeSeed_);
    }
    const std::string display = weaponField(w, "displayName");
    const bool clipOnly = weaponInt(w, "clipOnly") != 0;
    bool used = true;
    if (owned) {
        const int total = ammo.stock + ammo.clip;
        const int added = std::min(room, total);
        if (added <= 0) return false;
        // The ammunition goes to the stock, or to the clip of a clip-only weapon.
        const std::string a = weaponField(w, "ammoName"), c = weaponField(w, "clipName");
        if (clipOnly) inv.clip[c] += added;
        else inv.ammo[a] += added;
        used = added >= total;
        if (!used) { // g_ai on (default): the remainder stays on the item
            const ItemAmmo rest = itemAmmoAfterPartial(ammo, added);
            item.obj->set("$ammoStock", Value(rest.stock));
            item.obj->set("$ammoClip", Value(rest.clip));
        }
        clientCommands_.push_back({"gm", std::string(clipOnly ? "GAME_PICKUP_CLIPONLY_AMMO" : "GAME_PICKUP_AMMO") + "\x14" + display});
        const std::string snd = weaponField(w, "ammoPickupSound");
        if (!snd.empty()) startSound(snd, nullptr);
    } else {
        giveWeapon(inv, w);
        // 0x5215c3..0x521611: the clip slot is set to the item's clip (over clipSize goes to the stock), the stock is added (0x520e40).
        int clip = std::max(ammo.clip, 0), stock = std::max(ammo.stock, 0);
        if (clip > weaponInt(w, "clipSize")) stock += clip - weaponInt(w, "clipSize"), clip = weaponInt(w, "clipSize");
        const std::string a = weaponField(w, "ammoName"), c = weaponField(w, "clipName");
        inv.clip[c] = clip;
        inv.ammo[a] += stock;
        if (weaponInt(w, "clipOnly") == 0) inv.ammo[a] = std::min(inv.ammo[a], weaponInt(w, "maxAmmo"));
        inv.clip[c] = std::min(inv.clip[c], weaponInt(w, "clipSize"));
        trimSharedAmmo(inv, w);
        const std::string snd = weaponField(w, "pickupSound");
        if (!snd.empty()) startSound(snd, nullptr);
    }
    vm_.notify(item.obj, "trigger", {Value(player_->obj)});
    vm_.notify(player_->obj, "pickup", {Value(item.obj)});
    return used;
}

} // namespace iw2::game
