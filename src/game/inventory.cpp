// Player inventory built-ins. Behaviour follows CoD2SP_s.exe 1.3 (docs/spec/inventory.md):
// giveweapon 0x5376d0, takeweapon 0x537850, takeallweapons 0x537960, givemaxammo 0x538020,
// getfractionstartammo 0x538130, getfractionmaxammo 0x538270, getweaponslotweapon 0x539df0,
// setweaponslotweapon 0x539f40, give 0x531e30, add ammo 0x520e40, fill clip 0x520de0.
#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* const kSlotNames[3] = {"none", "primary", "primaryb"};

int slotIndex(const std::string& name) {
    const std::string n = lowerCopy(name);
    for (int i = 1; i < 3; ++i)
        if (n == kSlotNames[i]) return i;
    throw std::runtime_error("Unknown weaponslot name " + name + ". Valid weaponslots are \"primary\" and \"primaryb\"");
}

} // namespace

std::string World::weaponField(const std::string& weaponName, const char* key) {
    const auto* def = weapon(weaponName);
    if (!def) return {};
    auto it = def->find(lowerCopy(key));
    return it == def->end() ? std::string() : it->second;
}

int World::weaponInt(const std::string& weaponName, const char* key) { return std::atoi(weaponField(weaponName, key).c_str()); }

Inventory& World::inventory(Entity& player) {
    if (!player.isPlayer) throw std::runtime_error("entity " + std::to_string(player.number) + " is not a player");
    return inventories_[player.number];
}

// Loaded rounds move from stock into the clip up to clipSize (0x520de0).
void World::fillClip(Inventory& inv, const std::string& w) {
    const std::string a = weaponField(w, "ammoName"), c = weaponField(w, "clipName");
    const int room = weaponInt(w, "clipSize") - inv.clip[c];
    const int take = std::min(room, inv.ammo[a]);
    if (take <= 0) return;
    inv.ammo[a] -= take;
    inv.clip[c] += take;
}

// Adds stock ammo (0x520e40): clip-only weapons are given as well; the clip is filled when asked or when the
// weapon was given here; stock is clamped to maxAmmo and the clip to clipSize.
void World::addAmmo(Inventory& inv, const std::string& w, int amount, bool fillClipToo) {
    const std::string a = weaponField(w, "ammoName"), c = weaponField(w, "clipName");
    inv.ammo[a] += amount;
    bool given = false;
    if (weaponInt(w, "clipOnly")) given = giveWeapon(inv, w);
    if (fillClipToo || given) fillClip(inv, w);
    inv.ammo[a] = std::min(inv.ammo[a], weaponInt(w, "maxAmmo"));
    inv.clip[c] = std::min(inv.clip[c], weaponInt(w, "clipSize"));
}

// Gives a weapon (0x531e30). Returns false if already owned or a turret / non-player weapon. Items are only
// marked owned; offhands become the current offhand when there is none (TODO: the replacement rule when one
// is already selected, 0x4dd580/0x4dc6d0); primaries take the first empty slot. The altWeapon chain is given too.
bool World::giveWeapon(Inventory& inv, const std::string& name) {
    const std::string w = lowerCopy(name);
    if (inv.owned.count(w)) return false;
    const std::string cls = lowerCopy(weaponField(w, "weaponClass"));
    if (cls == "turret" || cls == "non-player") return false;
    inv.owned.insert(w);
    if (cls == "item") return true;
    const std::string offhandClass = lowerCopy(weaponField(w, "offhandClass"));
    if (!offhandClass.empty() && offhandClass != "none") {
        if (inv.offhand.empty()) inv.offhand = w;
        return true;
    }
    const std::string slot = lowerCopy(weaponField(w, "weaponSlot"));
    if (slot == "primary" || slot == "primaryb") {
        if (inv.slot[1].empty()) inv.slot[1] = w;
        else if (inv.slot[2].empty()) inv.slot[2] = w;
    }
    for (std::string alt = lowerCopy(weaponField(w, "altWeapon")); !alt.empty() && !inv.owned.count(alt);
         alt = lowerCopy(weaponField(alt, "altWeapon")))
        inv.owned.insert(alt);
    return true;
}

// Removes a weapon from the inventory and its slots (0x4dc8a0; TODO: its stackable-slot handling).
void World::takeWeapon(Inventory& inv, const std::string& name) {
    const std::string w = lowerCopy(name);
    if (!inv.owned.erase(w)) return;
    for (auto& s : inv.slot)
        if (s == w) s.clear();
    if (inv.offhand == w) inv.offhand.clear();
}

void World::registerInventoryBuiltins() {
    auto player = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("not an entity");
        inventory(*e);
        return *e;
    };
    auto known = [this](const Value& v) {
        const std::string w = lowerCopy(v.toString());
        // "none" is weapon index 0 in the original and is accepted (its numbers are all 0).
        if (w != "none" && !weapon(w)) throw std::runtime_error("Unknown weapon " + v.toString() + ".");
        return w;
    };
    vm_.registerBuiltin("giveweapon", [this, player, known](CallContext& c) {
        Entity& p = player(c);
        Inventory& inv = inventory(p);
        const std::string w = known(c.arg(0));
        const bool had = inv.owned.count(w) != 0;
        const std::string cls = lowerCopy(weaponField(w, "weaponClass")), oc = lowerCopy(weaponField(w, "offhandClass"));
        const std::string slot = lowerCopy(weaponField(w, "weaponSlot"));
        if (cls != "item" && (oc.empty() || oc == "none") && (slot == "primary" || slot == "primaryb") && !had &&
            !inv.slot[1].empty() && !inv.slot[2].empty())
            throw std::runtime_error("Can not give player weapon without having an empty weapon slot");
        giveWeapon(inv, w);
        const int missing = weaponInt(w, "startAmmo") - inv.ammo[weaponField(w, "ammoName")];
        if (missing > 0) addAmmo(inv, w, missing, !had);
        if (p.obj->get("$weapon").toString().empty() && inv.offhand != w) p.obj->set("$weapon", Value(w));
        return Value();
    });
    vm_.registerBuiltin("takeweapon", [this, player, known](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = known(c.arg(0));
        inv.ammo[weaponField(w, "ammoName")] = 0;
        inv.clip[weaponField(w, "clipName")] = 0;
        takeWeapon(inv, w);
        return Value();
    });
    vm_.registerBuiltin("takeallweapons", [this, player](CallContext& c) {
        Entity& p = player(c);
        inventory(p) = Inventory{};
        p.obj->set("$weapon", Value("none"));
        return Value();
    });
    vm_.registerBuiltin("hasweapon", [this, player](CallContext& c) {
        return Value(inventory(player(c)).owned.count(lowerCopy(c.arg(0).toString())) != 0);
    });
    vm_.registerBuiltin("getcurrentweapon", [player](CallContext& c) {
        const Value w = player(c).obj->get("$weapon");
        return w.isString() ? w : Value("none");
    });
    vm_.registerBuiltin("switchtoweapon", [this, player](CallContext& c) {
        Entity& p = player(c);
        const std::string w = lowerCopy(c.arg(0).toString());
        if (inventory(p).owned.count(w)) p.obj->set("$weapon", Value(w)); // TODO: raise/drop timing (0x537c90)
        return Value();
    });
    vm_.registerBuiltin("switchtooffhand", [this, player](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = lowerCopy(c.arg(0).toString());
        if (inv.owned.count(w)) inv.offhand = w;
        return Value();
    });
    vm_.registerBuiltin("getcurrentoffhand", [this, player](CallContext& c) {
        const Inventory& inv = inventory(player(c));
        return Value(inv.offhand.empty() ? std::string("none") : inv.offhand);
    });
    vm_.registerBuiltin("givemaxammo", [this, player, known](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = known(c.arg(0));
        addAmmo(inv, w, weaponInt(w, "maxAmmo") - inv.ammo[weaponField(w, "ammoName")], false);
        return Value();
    });
    // Stock (or clip for clip-only weapons) over maxAmmo (clipSize); 1 when the maximum is below 1, 0 when empty.
    vm_.registerBuiltin("getfractionmaxammo", [this, player, known](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = known(c.arg(0));
        const bool clipOnly = weaponInt(w, "clipOnly") != 0;
        const int max = clipOnly ? weaponInt(w, "clipSize") : weaponInt(w, "maxAmmo");
        const int have = clipOnly ? inv.clip[weaponField(w, "clipName")] : inv.ammo[weaponField(w, "ammoName")];
        if (max < 1) return Value(1.0f);
        if (have < 1) return Value(0.0f);
        return Value(float(have) / float(max));
    });
    vm_.registerBuiltin("getfractionstartammo", [this, player, known](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = known(c.arg(0));
        const int start = weaponInt(w, "startAmmo");
        const int have = inv.ammo[weaponField(w, "ammoName")];
        if (start < 1) return Value(1.0f);
        if (have < 1) return Value(0.0f);
        return Value(float(have) / float(start));
    });
    vm_.registerBuiltin("getweaponslotweapon", [this, player](CallContext& c) {
        const Inventory& inv = inventory(player(c));
        const std::string& w = inv.slot[slotIndex(c.arg(0).toString())];
        return Value(w.empty() ? std::string("none") : w);
    });
    vm_.registerBuiltin("setweaponslotweapon", [this, player](CallContext& c) {
        Entity& p = player(c);
        Inventory& inv = inventory(p);
        const int s = slotIndex(c.arg(0).toString());
        const std::string w = lowerCopy(c.arg(1).toString());
        if (w != "none" && !weapon(w)) throw std::runtime_error("Unknown weapon " + c.arg(1).toString() + ".");
        if (w != "none") {
            const std::string ws = lowerCopy(weaponField(w, "weaponSlot"));
            const int need = ws == "primary" ? 1 : ws == "primaryb" ? 2 : 0;
            if (need != s && !(need >= 1 && s >= 1))
                throw std::runtime_error("Weapon " + w + " goes in the " + kSlotNames[need] + " weaponslot, not the " + kSlotNames[s] + " weaponslot.");
        }
        if (!inv.slot[s].empty()) takeWeapon(inv, inv.slot[s]);
        if (w != "none") {
            // With primary filled and primaryb empty, a weapon for primaryb is first placed in primary by the
            // give; the engine then moves it to primaryb.
            const bool moveToB = s == 2 && inv.slot[1].empty() == false && inv.slot[2].empty();
            giveWeapon(inv, w);
            if (moveToB && inv.slot[2] != w) inv.slot[2] = w;
        }
        return Value();
    });
    auto slotWeapon = [this, player](CallContext& c) -> std::pair<Inventory*, std::string> {
        Inventory& inv = inventory(player(c));
        return {&inv, inv.slot[slotIndex(c.arg(0).toString())]};
    };
    vm_.registerBuiltin("getweaponslotammo", [this, slotWeapon](CallContext& c) {
        auto [inv, w] = slotWeapon(c);
        return Value(int32_t(w.empty() ? 0 : inv->ammo[weaponField(w, "ammoName")]));
    });
    vm_.registerBuiltin("setweaponslotammo", [this, slotWeapon](CallContext& c) {
        auto [inv, w] = slotWeapon(c);
        if (!w.empty()) inv->ammo[weaponField(w, "ammoName")] = std::clamp(c.arg(1).asInt(), 0, weaponInt(w, "maxAmmo"));
        return Value();
    });
    vm_.registerBuiltin("getweaponslotclipammo", [this, slotWeapon](CallContext& c) {
        auto [inv, w] = slotWeapon(c);
        return Value(int32_t(w.empty() ? 0 : inv->clip[weaponField(w, "clipName")]));
    });
    vm_.registerBuiltin("setweaponslotclipammo", [this, slotWeapon](CallContext& c) {
        auto [inv, w] = slotWeapon(c);
        if (!w.empty()) inv->clip[weaponField(w, "clipName")] = std::clamp(c.arg(1).asInt(), 0, weaponInt(w, "clipSize"));
        return Value();
    });
    vm_.registerBuiltin("setweaponclipammo", [this, player, known](CallContext& c) {
        Inventory& inv = inventory(player(c));
        const std::string w = known(c.arg(0));
        inv.clip[weaponField(w, "clipName")] = std::clamp(c.arg(1).asInt(), 0, weaponInt(w, "clipSize"));
        return Value();
    });
}

} // namespace iw2::game
