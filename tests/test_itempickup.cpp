// Tests for the weapon item pickup helpers (BG_PlayerTouchesItem 0x4facc0, BG_CanItemBeGrabbed 0x4fad50, Pickup_Weapon 0x521170).

#include "game/itempickup.h"
#include "game/world.h"
#include "fs/vfs.h"

#include <cstdlib>

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    using namespace iw2::game;
    CHECK(playerTouchesItem({0, 0, 0}, {0, 0, 0}));
    CHECK(playerTouchesItem({36, -36, 0}, {0, 0, 0}));
    CHECK(!playerTouchesItem({37, 0, 0}, {0, 0, 0}));
    CHECK(playerTouchesItem({0, 0, -52}, {0, 0, 0}));  // the broad phase (+-52) is tighter than the narrow -88
    CHECK(!playerTouchesItem({0, 0, -53}, {0, 0, 0}));
    CHECK(playerTouchesItem({0, 0, 18}, {0, 0, 0}));
    CHECK(!playerTouchesItem({0, 0, 19}, {0, 0, 0}));

    CHECK(canGrabWeaponItem(true, false, true, 4));
    CHECK(!canGrabWeaponItem(true, false, true, 0));
    CHECK(!canGrabWeaponItem(false, false, true, 30)); // a rifle is taken by use only
    CHECK(canGrabWeaponItem(false, false, false, 30));
    CHECK(canGrabWeaponItem(false, true, true, 5));

    uint32_t seed = 1;
    ItemAmmo a = resolveItemAmmo({}, 0, 0, 4, 4, seed);
    CHECK(a.clip >= 2 && a.clip <= 4 && a.stock == 0);
    seed = 1;
    a = resolveItemAmmo({0, 0}, 7, 7, 8, 32, seed);
    CHECK(a.clip == 7 && a.stock == 0);
    a = resolveItemAmmo({20, 0}, 0, 0, 8, 32, seed);
    CHECK(a.clip == 8 && a.stock == 12);
    a = resolveItemAmmo({50, 0}, 0, 0, 8, 32, seed);
    CHECK(a.clip == 8 && a.stock == 24);

    ItemAmmo r = itemAmmoAfterPartial({10, 8}, 12);
    CHECK(r.stock == 0 && r.clip == 6);
    r = itemAmmoAfterPartial({10, 8}, 4);
    CHECK(r.stock == 6 && r.clip == 8);

    if (failures) return 1;
    std::cout << "ok\n";
        // Actor drop ammo (0x522185..): ranges, the default formula and the clip draw stay within the total.
    {
        uint32_t seed = 1;
        for (int i = 0; i < 50; ++i) {
            const ItemAmmo d = actorDropAmmo(0, 0, 32, seed);
            CHECK(d.clip >= 0 && d.stock >= 0 && d.clip + d.stock >= 16 && d.clip + d.stock <= 33);
            const ItemAmmo r = actorDropAmmo(10, 20, 8, seed);
            CHECK(r.clip >= 0 && r.clip < 8 && r.clip + r.stock >= 10 && r.clip + r.stock < 20);
        }
        CHECK(actorDropAmmo(-5, -1, 8, seed).clip == 0 && actorDropAmmo(-5, -1, 8, seed).stock == 0);
        const ItemAmmo eq = actorDropAmmo(3, 3, 0, seed);
        CHECK(eq.clip == 0 && eq.stock == 3);
    }
    // World (IW2_MAIN): an emptied MP40 swapped for a kar98k keeps its (empty) counters on the dropped item; an NPC's MP40 picked up
    // afterwards brings its own clip (Pickup_Weapon sets the clip slot to the item's clip, 0x521611), not the old empty one.
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        World world(vfs, "toujane");
        world.start();
        for (int i = 0; i < 5; ++i) world.frame();
        Inventory& inv = world.inventory(*world.player());
        inv = Inventory{};
        world.debugUseWeaponItem("mp40", 0, 0);
        world.debugUseWeaponItem("enfield", 0, 0); // second slot
        CHECK(inv.owned.count("mp40") && inv.owned.count("enfield"));
        const auto mp = world.debugAmmo("mp40");
        std::cout << "mp40 from an unset item: stock " << mp.first << " clip " << mp.second << "\n";
        CHECK(mp.second > 0);
        world.debugSetClip("mp40", 0);
        inv.ammo[world.weapon("mp40")->at("ammoname")] = 0;
        world.player()->obj->set("$weapon", iw2::script::Value("mp40"));
        Entity* dropped = world.debugUseWeaponItem("kar98k", 0, 0); // full slots: the held (empty) MP40 is dropped
        CHECK(dropped && dropped->classname == "weapon_mp40");
        if (dropped) CHECK(dropped->obj->get("$ammoStock").asInt() == -1 && dropped->obj->get("$ammoClip").asInt() == -1);
        CHECK(!inv.owned.count("mp40") && inv.owned.count("kar98k"));
        world.player()->obj->set("$weapon", iw2::script::Value("kar98k"));
        world.debugUseWeaponItem("mp40", 40, 32); // an NPC's MP40 with its own rounds
        const auto mp2 = world.debugAmmo("mp40");
        std::cout << "npc mp40: stock " << mp2.first << " clip " << mp2.second << "\n";
        CHECK(mp2.second == 32 && mp2.first == 40);
        // Shared ammo cap (0x520f3d): frag grenades share WEAPON_FRAGGRENADE, cap 4. With 3 frags a touched stick grenade item holding 3
        // is trimmed back to the one that fits.
        inv.owned.insert("fraggrenade");
        inv.offhand = "fraggrenade";
        world.debugSetClip("fraggrenade", 3);
        world.debugSpawnItem("stielhandgranate");
        for (const auto& e : world.entities())
            if (!e->obj->removed && e->classname == "weapon_stielhandgranate") e->obj->set("$ammoClip", iw2::script::Value(int32_t(3)));
        world.frame();
        const auto st = world.debugAmmo("stielhandgranate");
        std::cout << "stielhandgranate after the shared cap: clip " << st.second << " owned " << inv.owned.count("stielhandgranate") << "\n";
        CHECK(world.debugAmmo("fraggrenade").second + st.second <= 4);
    }
    return failures ? 1 : 0;
}
