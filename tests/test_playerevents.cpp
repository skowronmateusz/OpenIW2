// Tests for the landing outcome (PM_CrashLand 0x508fe0) and the player-state event sounds / fall damage.

#include "game/playerevents.h"
#include "game/textmessage.h"
#include "game/pmove.h"

#include <cmath>
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
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    using namespace iw2::game;
    const uint32_t dirt = 6u << 20; // surface type 6

    // Small falls: nothing up to 4, the walk step below 8, the run step below 12.
    CHECK(landingOutcome(3.0f, dirt, false).event == 0);
    CHECK(landingOutcome(5.0f, dirt, false).event == 0x18 + 6);
    CHECK(landingOutcome(10.0f, dirt, false).event == 1 + 6);
    CHECK(landingOutcome(10.0f, 0, false).event == 0); // surface 0: no footstep event
    CHECK(landingOutcome(10.0f, dirt, false).velScale == 1.0f);
    // A hard landing: velocity x0.67, landing event 0x5d + surface with the dip as parameter.
    {
        const LandingOutcome o = landingOutcome(20.0f, dirt, false);
        CHECK(o.event == 0x5d + 6);
        CHECK(o.eventParm == 5);
        CHECK(near(o.velScale, 0.67f));
        CHECK(o.damage == 0);
    }
    CHECK(landingOutcome(20.0f, 0x2000u | dirt, false).event == 0x5d); // flag 0x2000: surface 0
    // Fall damage: 33 percent at h 250; pm_time 500 + 35 * 33, speed x0.2.
    {
        const LandingOutcome o = landingOutcome(250.0f, dirt, false);
        CHECK(o.damage == 33);
        CHECK(o.event == 0x74 + 6 && o.eventParm == 33);
        CHECK(o.pmTimeMs == 1655 && o.landTimer);
        CHECK(near(o.velScale, 0.2f));
    }
    {
        const LandingOutcome o = landingOutcome(220.0f, dirt, false); // 13 percent: t 955
        CHECK(o.damage == 13 && o.pmTimeMs == 955);
        CHECK(near(o.velScale, 0.5f - 455.0f * 0.001f * 0.3f));
    }
    {
        const LandingOutcome o = landingOutcome(400.0f, dirt, false); // lethal: speed x0.67, no timer
        CHECK(o.damage == 100 && o.pmTimeMs == 0 && !o.landTimer);
        CHECK(near(o.velScale, 0.67f));
    }
    CHECK(landingOutcome(400.0f, dirt | 1u, false).damage == 0); // no-damage surface
    CHECK(landingOutcome(400.0f, dirt | 1u, false).eventParm == 24); // the dip is capped at 24
    CHECK(landingOutcome(400.0f, dirt, true).damage == 0);         // dead
    CHECK(landingOutcome(250.0f, dirt | 2u, false).pmTimeMs == 0);  // slick: only the speed loss
    CHECK(near(landingOutcome(250.0f, dirt | 2u, false).velScale, 0.67f));

    // The event ring.
    {
        PlayerState ps;
        addPlayerEvent(ps, 0);
        CHECK(ps.eventSeq == 0);
        for (int i = 1; i <= 5; ++i) addPlayerEvent(ps, i, i * 10);
        CHECK(ps.eventSeq == 5);
        CHECK(ps.events[0] == 5 && ps.eventParms[0] == 50); // the fifth overwrote the first
        CHECK(ps.events[3] == 4);
    }

    // Sounds.
    {
        using V = std::vector<std::string>;
        CHECK(playerEventSounds(0x1e, true) == (V{"step_walk_plr_dirt", "gear_rattle_plr_walk"}));
        CHECK(playerEventSounds(0x1e, false) == (V{"gear_rattle_plr_walk"}));
        CHECK(playerEventSounds(0x8b, false) == (V{"movement_foliage"}));
        CHECK(playerEventSounds(1 + 13, true) == (V{"step_run_plr_metal", "gear_rattle_plr_run"}));
        CHECK(playerEventSounds(0x2f + 10, true) == (V{"step_prone_plr_grass", "gear_rattle_plr_walk"}));
        CHECK(playerEventSounds(0x46, false) == (V{"step_run_plr_default", "gear_rattle_plr_run"}));
        CHECK(playerEventSounds(0x5d + 6, false) == (V{"land_plr_dirt"}));
        CHECK(playerEventSounds(0x74 + 22, false) == (V{"land_plr_asphalt", "land_damage"}));
        CHECK(playerEventSounds(0x8c, true).empty());
        CHECK(playerEventSounds(0, true).empty());
    }

    // Fall damage: percent * 0.01f in single precision, 1.1 from 100 percent.
    CHECK(fallDamageAmount(100, 100) == 110);
    CHECK(fallDamageAmount(100, 50) == 49);
    CHECK(fallDamageAmount(100, 0) == 0);
    CHECK(fallDamageAmount(200, 25) == 49);

    if (failures == 0) std::cout << "playerevents: ok\n";
        {
        // Text message localization (0x496e80): the item pickup message `GAME_PICKUP_AMMO\x14<displayName>` (0x58e080).
        auto find = [](const std::string& k) -> const std::string* {
            static const std::string a = "Picked up ammo for the &&1", g = "Stielhandgranate", o = "Objective: &&1";
            if (k == "GAME_PICKUP_AMMO") return &a;
            if (k == "WEAPON_GERMANGRENADE") return &g;
            if (k == "OBJ") return &o;
            return nullptr;
        };
        CHECK(iw2::game::localizeTextMessage("GAME_PICKUP_AMMO\x14WEAPON_GERMANGRENADE", find) == "Picked up ammo for the Stielhandgranate");
        CHECK(iw2::game::localizeTextMessage("GAME_PICKUP_AMMO\x15WEAPON_GERMANGRENADE", find) == "Picked up ammo for the WEAPON_GERMANGRENADE");
        CHECK(iw2::game::localizeTextMessage("^2\x14OBJ\x1f" "5", find) == "^2Objective: 5");
        CHECK(iw2::game::localizeTextMessage("plain text", find) == "plain text");
    }
    return failures == 0 ? 0 : 1;
}
