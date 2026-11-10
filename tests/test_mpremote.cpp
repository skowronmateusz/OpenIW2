// Network client view of a multiplayer map (src/game/mpremote.cpp, docs/spec/net.md section 10): the map without the
// gametype, the local view entity, puppets for the other players placed, drawn, moved and removed from snapshots.
// Needs the game files (IW2_MAIN).

#include "game/mphud.h"
#include "game/world.h"

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
using namespace iw2::game;
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "mp_toujane", true);
    w.mpStartRemote();
    CHECK(w.mpRemote());
    CHECK(w.player() != nullptr);
    if (w.player()) CHECK(w.player()->obj->get("health").toString() == "100");
    CHECK(w.mpState().gametype.empty());

    World::RemotePlayer a, b;
    a.client = 1;
    a.name = "alpha";
    a.origin = {100, 200, 30};
    a.velocity = {150, 0, 0};
    a.yaw = 90;
    b.client = 3;
    b.name = "bravo";
    b.origin = {-50, 0, 0};
    w.mpSetRemotePlayers({a, b});
    w.frame();
    Entity* ea = w.mpClientEntity(1);
    Entity* eb = w.mpClientEntity(3);
    CHECK(ea && eb);
    if (ea && eb) {
        CHECK(w.mpPlayerDrawn(*ea) && w.mpPlayerDrawn(*eb));
        CHECK(ea->obj->get("name").toString() == "alpha");
        CHECK(ea->obj->get("origin").asVector()[1] == 200.0f);
        CHECK(ea->obj->get("angles").asVector()[1] == 90.0f);
        CHECK(!w.mpPlayerDrawn(*w.player()));
    }
    // The local slot is never a puppet.
    World::RemotePlayer self;
    self.client = 0;
    a.origin = {110, 200, 30};
    w.mpSetRemotePlayers({a, self});
    w.frame();
    CHECK(w.mpClientEntity(1) == ea);  // the same puppet moves
    if (ea) CHECK(ea->obj->get("origin").asVector()[0] == 110.0f);
    CHECK(w.mpClientEntity(3) == nullptr);  // bravo left the snapshot
    if (eb) CHECK(eb->obj->removed);
    CHECK(w.mpClientEntity(0) == nullptr);

    w.mpSetMenuNames({"ingame", "team_britishgerman"});
    CHECK(w.menuNames().size() == 2 && w.menuNames()[1] == "team_britishgerman");
    // The cursor hint comes only from the server's player state, never from the map copy's triggers.
    w.mpSetRemoteUseHint(0, "");
    w.frame();
    CHECK(w.playerUseHint().hint == 0 && w.playerUseHint().text.empty());
    w.mpSetRemoteUseHint(2, "PLATFORM_HOLD_TO_USE");
    w.frame();
    CHECK(w.playerUseHint().hint == 2 && w.playerUseHint().text == "PLATFORM_HOLD_TO_USE");
    w.mpSetRemoteUseHint(0, "");
    w.frame();
    CHECK(w.playerUseHint().hint == 0);
    // The pickup prompt of a weapon item hint follows the player state's weapon of the newest snapshot (mpSetRemoteWeapon) and
    // the inventory of the moment, not the view weapon's animation state or the inventory of an older snapshot: swapping for
    // the weapon on the ground, the hint of the dropped weapon shows the swap text from the first frame after the swap.
    if (w.player() && w.weapon("kar98k_mp") && w.weapon("mp40_mp") && w.weapon("m1garand_mp")) {
        Inventory& inv = w.inventory(*w.player());
        inv.owned = {"mp40_mp", "m1garand_mp"};
        inv.slot[1] = "mp40_mp";
        inv.slot[2] = "m1garand_mp";
        w.mpSetRemoteWeapon("mp40_mp");
        w.mpSetRemoteUseHint(5, "", "kar98k_mp");  // the weapon item in front of the player
        w.frame();
        CHECK(w.playerUseHint().weapon == "kar98k_mp" && w.playerUseHint().text == "PLATFORM_SWAPWEAPONS");
        // The swap: the snapshot's weapon is the new one, the item on the ground is the dropped mp40. The view weapon is still
        // lowering the mp40 (the front end writes it to $weapon); the prompt does not read that.
        inv.owned = {"kar98k_mp", "m1garand_mp"};
        inv.slot[1] = "kar98k_mp";
        w.mpSetRemoteWeapon("kar98k_mp");
        w.player()->obj->set("$weapon", iw2::script::Value(std::string("mp40_mp")));
        w.mpSetRemoteUseHint(5, "", "mp40_mp");
        w.frame();
        CHECK(w.playerUseHint().weapon == "mp40_mp" && w.playerUseHint().text == "PLATFORM_SWAPWEAPONS");
        // The hint arrives before the inventory of its snapshot is applied: the text is chosen per tick, so the next tick has the
        // text for the inventory it sees (it was frozen against the older inventory before).
        w.mpSetRemoteUseHint(5, "", "kar98k_mp");
        inv.slot[1] = "mp40_mp";
        inv.owned = {"mp40_mp", "m1garand_mp"};
        w.mpSetRemoteWeapon("mp40_mp");
        w.frame();
        CHECK(w.playerUseHint().text == "PLATFORM_SWAPWEAPONS");
        // The hinted weapon is the current one: no prompt, the icon stays alone (the original's icon-only path).
        w.mpSetRemoteWeapon("kar98k_mp");
        inv.slot[1] = "kar98k_mp";
        w.frame();
        CHECK(w.playerUseHint().weapon == "kar98k_mp" && w.playerUseHint().text.empty());
        // A hint string from the server is used as it is.
        w.mpSetRemoteUseHint(5, "PLATFORM_USEMG42", "kar98k_mp");
        w.frame();
        CHECK(w.playerUseHint().text == "PLATFORM_USEMG42");
        w.mpSetRemoteUseHint(0, "");
        w.frame();
    } else {
        std::cerr << "weapons for the pickup prompt test missing\n";
        ++failures;
    }
    // Obituary names from the client info carried by the event, even with no puppet for the victim.
    MpObituary ob;
    ob.victim = 7;
    ob.attacker = 0;
    ob.weapon = "kar98k_mp";
    ob.hasClientInfo = true;
    ob.victimName = "bot0";
    ob.attackerName = "me";
    const ObituaryLine line = w.mpObituaryLine(ob);
    CHECK(line.centerPrint == "CGAME_YOUKILLED");
    CHECK(line.centerName == "bot0^7");
    CHECK(line.victim == "bot0^7");
    CHECK(w.vm().errors().empty());
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpremote_world: ok\n";
    return failures ? 1 : 0;
}
