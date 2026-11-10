// The player's projectile weapon (panzerschreck) fires a rocket (FireWeapon 0x531c40, weaponType 2 -> 0x531a40 ->
// G_FireRocket 0x527810) instead of a bullet trace, and the missile's projTrailEffect is queued bolted on its tag_origin
// (CG_Missile 0x4ab3af). A network client hears another player's shot as fireSound at the shooter (MP 0x4d70b0). Needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/world.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "moscow");
    w.prepareStart();
    CHECK(w.player() != nullptr);
    if (!w.player()) return 1;
    (void)w.takeFxEvents();
    w.player()->obj->set("$weapon", iw2::script::Value(std::string("panzerschreck")));
    const iw2::script::Vec3 o = w.player()->obj->get("origin").asVector();
    std::vector<std::pair<std::string, bool>> sounds; // alias, positional
    w.playSound = [&](const std::string& a, const iw2::game::Vec3* pos, float*, int) { sounds.emplace_back(a, pos != nullptr); return 0; };
    const int impactsBefore = w.stats.bulletImpacts;
    const auto kick = w.playerFire({o[0], o[1], o[2] + 60}, {1, 0, 0});
    CHECK(kick[0] == 64.0f && kick[1] == 0.0f && kick[2] == 0.0f); // 0x531b1b: forward * 64
    CHECK(w.stats.rockets == 1);
    CHECK(w.stats.bulletImpacts == impactsBefore); // no bullet trace
    bool trail = false;
    for (const auto& e : w.takeFxEvents())
        if (e.path == "fx/smoke/emitter_panzerschreck.efx" && e.tag == "tag_origin" && e.entity >= 0) {
            trail = true;
            iw2::game::Vec3 p{};
            std::array<iw2::game::Vec3, 3> ax{};
            bool hasAxes = false;
            CHECK(w.entityTagOrientation(e.entity, "tag_origin", p, ax, hasAxes)); // the bolt resolves on the missile model
        }
    CHECK(trail);
    // EV_FIRE_WEAPON (0x4b7800) plays the fire sound for projectile weapons too; the panzerschreck has no fireSoundPlayer, so the
    // player hears its fireSound (non-positional for the player).
    CHECK(sounds.size() == 1 && sounds[0].first == "weap_panzerfaust_fire" && !sounds[0].second);
    // A bullet weapon still traces.
    w.player()->obj->set("$weapon", iw2::script::Value(std::string("kar98k")));
    const auto bulletKick = w.playerFire({o[0], o[1], o[2] + 60}, {1, 0, 0}, false, 5.0f); // spread draws two rand()s
    CHECK(bulletKick[0] == 0.0f);
    CHECK(w.stats.rockets == 1);
    CHECK(sounds.size() >= 2 && sounds[1].first == "weap_kar98k_fire_plr" || (sounds.size() >= 3 && sounds[2].first == "weap_kar98k_fire_plr")); // bullet weapons keep their player alias
    // Another player's shot (not the local view) plays the weapon's fireSound at its position, never the local "_plr"
    // alias (MP 0x4d70b0: the player variant only for the player state's own client in first person).
    {
        iw2::game::World mp(vfs, "mp_carentan", true);
        mp.mpStartRemote();
        iw2::game::World::RemotePlayer rp;
        rp.client = 3;
        rp.name = "far";
        rp.origin = {2000, 0, 0};
        mp.mpSetRemotePlayers({rp});
        std::vector<std::pair<std::string, bool>> ms;
        std::vector<iw2::game::Vec3> at;
        mp.playSound = [&](const std::string& a, const iw2::game::Vec3* pos, float*, int) {
            ms.emplace_back(a, pos != nullptr);
            at.push_back(pos ? *pos : iw2::game::Vec3{0, 0, 0});
            return 0;
        };
        (void)mp.takeFxEvents();
        mp.mpRemotePlayerEvent(3, 0x9e, "kar98k_mp", 0.0f);
        // The shooter's muzzle flash on its world weapon's tag_flash (MP 0x4d70fa flash flag).
        const iw2::game::Entity* shooter = mp.mpClientEntity(3);
        bool flash = false;
        for (const auto& fx : mp.takeFxEvents())
            flash = flash || (shooter && fx.entity == shooter->number && fx.tag == "tag_flash" && !fx.path.empty());
        CHECK(flash);
        // The third-person fire animation (event "fireweapon" of the player anim script) on the next frame.
        mp.frame();
        CHECK(mp.mpState().clients[3].anim.torsoActive);
        bool fire = false;
        for (size_t i = 0; i < ms.size(); ++i)
            if (ms[i].first == "weap_kar98k_fire" && ms[i].second && at[i][0] > 1900) fire = true;
        CHECK(fire);
        for (const auto& s2 : ms) CHECK(s2.first != "weap_kar98k_fire_plr");
        // The followed (killcam) client's shot is this view's own: the player alias without a position.
        ms.clear();
        if (mp.player()) {
            mp.player()->obj->set("health", iw2::script::Value(int32_t(100)));
            mp.player()->obj->set("$weapon", iw2::script::Value(std::string("kar98k_mp"))); // the followed ps weapon (RemoteView::apply)
            const iw2::script::Vec3 po = mp.player()->obj->get("origin").asVector();
            mp.mpRemoteFollowShot("kar98k_mp", {po[0], po[1], po[2] + 60}, {1, 0, 0}, false);
            bool plr = false;
            for (const auto& s2 : ms) plr = plr || (s2.first == "weap_kar98k_fire_plr" && !s2.second);
            CHECK(plr);
        }
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "playerrocket ok\n";
    return failures ? 1 : 0;
}
