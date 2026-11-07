// The openiw2 hit marker option (src/game/hitmarker.h, docs/debug-flags.md): trigger rule, fade, off by default and archived, the
// options-menu row, and (with IW2_MAIN) the real trigger through World::damage on a level.

#include "fs/vfs.h"
#include "game/hitmarker.h"
#include "game/world.h"
#include "ui/iw2options.h"
#include "ui/menu.h"
#include "ui/menurun.h"
#include "ui/profile.h"

#include <cmath>
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
    // Trigger rule: player -> living enemy actor only.
    CHECK(hitMarkerTrigger(true, true, true, "allies", "axis", 20));
    CHECK(hitMarkerTrigger(true, true, true, "axis", "allies", 1)); // enemy of whichever side the player is on
    CHECK(!hitMarkerTrigger(true, true, true, "allies", "allies", 20));   // friendly
    CHECK(!hitMarkerTrigger(true, true, true, "allies", "neutral", 20));  // neutral actor
    CHECK(!hitMarkerTrigger(true, true, true, "allies", "", 20));
    CHECK(!hitMarkerTrigger(true, true, false, "allies", "axis", 20));    // dead body
    CHECK(!hitMarkerTrigger(true, false, true, "allies", "axis", 20));    // not an actor (player, script model, vehicle, trigger)
    CHECK(!hitMarkerTrigger(false, true, true, "allies", "axis", 20));    // not dealt by the player (an ally or the world)
    CHECK(!hitMarkerTrigger(true, true, true, "allies", "axis", 0));      // no damage
    // Fade: full at the hit, linear to nothing after the fade time, none before the first hit.
    CHECK(hitMarkerAlpha(5000, -1) == 0.0f);
    CHECK(hitMarkerAlpha(5000, 5000) == 1.0f);
    CHECK(std::fabs(hitMarkerAlpha(5500, 5000) - 0.5f) < 1e-6f);
    CHECK(hitMarkerAlpha(6000, 5000) == 0.0f && hitMarkerAlpha(9000, 5000) == 0.0f);
    CHECK(hitMarkerAlpha(4000, 5000) == 0.0f);
    const HitMarkerLook look;
    // Kill marker (iw2_hitMarker 2): red only for a kill in the frame of the last hit, white otherwise and in mode 1.
    CHECK(hitMarkerMode(0) == HitMarkerMode::Off && hitMarkerMode(1) == HitMarkerMode::On && hitMarkerMode(2) == HitMarkerMode::RedOnKill);
    CHECK(!hitMarkerIsKill(-1, -1) && !hitMarkerIsKill(5000, -1) && hitMarkerIsKill(5000, 5000) && !hitMarkerIsKill(5050, 5000));
    CHECK((hitMarkerColor(HitMarkerMode::RedOnKill, 5000, 5000) == std::array<uint8_t, 3>{255, 0, 0}));
    CHECK((hitMarkerColor(HitMarkerMode::RedOnKill, 5050, 5000) == std::array<uint8_t, 3>{255, 255, 255})); // later non-lethal hit
    CHECK((hitMarkerColor(HitMarkerMode::On, 5000, 5000) == std::array<uint8_t, 3>{255, 255, 255}));         // mode 1: always white
    CHECK(look.size == 24.0f && look.offset == -12.0f && look.fadeMs == 1000); // multiplayer: 24 x 24 at (-12, -12), fadeOverTime(1)

    // Off by default, and archived (saved in the profile).
    CHECK(iw2::ui::registeredDefault("iw2_hitMarker") == "0");
    CHECK(iw2::ui::isArchivedCvar("iw2_hitMarker"));
    const auto back = iw2::ui::parseConfig(iw2::ui::formatConfig({{"iw2_hitMarker", "1"}, {"iw2_other", "1"}}));
    CHECK(back.size() == 1 && back.at("iw2_hitMarker") == "1");

    // Options menu row, on a synthetic options_game menu (the shipped file is not in the repository).
    {
        iw2::fs::Vfs vfs;
        iw2::ui::MenuLibrary lib(vfs);
        lib.parseSource(R"({
menuDef
{
	rect 0 0 400 300
	name options_game
	itemDef
	{
		decoration
		rect 0 15 1 13
		text "CROSSHAIR ROW"
		type 1
		visible 1
		name rowlabel
	}
	itemDef
	{
		dvarFloatList { "ON" 1 "OFF" 0 }
		rect 0 15 100 13
		dvar "cg_drawcrosshair"
		text "x"
		type 12
		visible 1
		name rowvalue
	}
	itemDef
	{
		dvarFloatList { "A" 0 }
		rect 0 60 100 13
		dvar "ui_language"
		text "x"
		type 12
		visible 1
		name rowlanguage
	}
}
})", "ui/options_game.menu");
        iw2::ui::Menu* m = lib.find("options_game");
        CHECK(m && m->items.size() == 3);
        CHECK(iw2::ui::addOpenIw2Options(lib));
        CHECK(iw2::ui::addOpenIw2Options(lib)); // idempotent
        CHECK(m && m->items.size() == 5);
        if (m && m->items.size() == 5) {
            const auto& label = *m->items[3];
            const auto& value = *m->items[4];
            CHECK(label.decoration && label.text == "Hit marker" && label.rect.y == 75.0f);
            CHECK(value.type == 12 && value.dvar == "iw2_hitMarker" && value.rect.y == 75.0f && !value.decoration);
            CHECK(value.floatList.size() == 3 && value.floatList[0].second == 1.0f && value.floatList[1].second == 2.0f &&
                  value.floatList[1].first == "Red on kill" && value.floatList[2].second == 0.0f);
        }
        iw2::ui::MenuLibrary empty(vfs);
        CHECK(!iw2::ui::addOpenIw2Options(empty)); // no menu, nothing added
    }

    // A level: the real damage path. The player is on "allies".
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "hit marker level checks skipped (IW2_MAIN not set)\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 20; ++i) w.frame();
    CHECK(w.playerHitMarkerMs() == -1 && w.playerHitMarkerKillMs() == -1);
    // The level starts with allied actors only; two of them are switched to the opposing team, a third to neutral.
    Entity *axis = nullptr, *axis2 = nullptr, *ally = nullptr, *neutral = nullptr;
    for (const auto& e : w.entities()) {
        if (!e->isActor || e->obj->removed || !e->obj->get("health").isNumber() || e->obj->get("health").asInt() <= 0) continue;
        if (!axis) axis = e.get();
        else if (!axis2) axis2 = e.get();
        else if (!neutral) neutral = e.get();
        else if (!ally) ally = e.get();
    }
    CHECK(axis && axis2 && neutral && ally);
    if (!axis || !axis2 || !neutral || !ally) return 1;
    axis->obj->set("team", iw2::script::Value(std::string("axis")));
    axis2->obj->set("team", iw2::script::Value(std::string("axis")));
    neutral->obj->set("team", iw2::script::Value(std::string("neutral")));
    CHECK(w.player() && w.player()->obj->get("team").toString() == "allies");
    // World damage (no attacker) does not count; the player's damage to an enemy does, and restarts the fade at the new time.
    w.frame();
    w.debugDamage(*axis, 1);
    const int64_t first = w.playerHitMarkerMs();
    CHECK(first == w.timeMs() && w.playerHitMarkerKillMs() == -1); // a hit, not a kill
    for (int i = 0; i < 10; ++i) w.frame();
    w.debugDamage(*axis2, 1);
    CHECK(w.playerHitMarkerMs() == w.timeMs() && w.playerHitMarkerMs() > first);
    // A friendly does not count.
    for (int i = 0; i < 3; ++i) w.frame();
    const int64_t before = w.playerHitMarkerMs();
    w.debugDamage(*ally, 1);
    CHECK(w.playerHitMarkerMs() == before);
    w.debugDamage(*neutral, 1); // neither does a neutral actor
    CHECK(w.playerHitMarkerMs() == before);
    // The killing blow counts, a corpse does not.
    for (int i = 0; i < 3; ++i) w.frame();
    // These actors play a scripted animation, which holds a lethal hit back at 1 health (0x4ff360) unless allowdeath is set.
    // With allowdeath the hit kills at once: red.
    axis->obj->set("allowdeath", iw2::script::Value(int32_t(1)));
    w.debugDamage(*axis, 100000);
    CHECK(axis->obj->get("health").asInt() <= 0);
    CHECK(w.playerHitMarkerMs() == w.timeMs() && w.playerHitMarkerKillMs() == w.timeMs());
    CHECK(hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    for (int i = 0; i < 400 && axis->obj->get("health").asInt() > 0; ++i) w.frame();
    CHECK(axis->obj->get("health").asInt() <= 0);
    CHECK(w.playerHitMarkerKillMs() >= 0 && hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    for (int i = 0; i < 3; ++i) w.frame();
    const int64_t beforeDead = w.playerHitMarkerMs();
    w.debugDamage(*axis, 5);
    CHECK(w.playerHitMarkerMs() == beforeDead);
    // A later non-lethal hit on another enemy shows white again.
    w.debugDamage(*axis2, 1);
    CHECK(w.playerHitMarkerMs() == w.timeMs() && !hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    // Demigod cuts the killing blow to health - 1: a hit, no kill.
    for (int i = 0; i < 3; ++i) w.frame();
    axis2->obj->set("$demigod", iw2::script::Value(int32_t(1)));
    w.debugDamage(*axis2, 100000);
    CHECK(w.playerHitMarkerMs() == w.timeMs() && !hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    // Held back (no allowdeath): white at the hit, the pending death remembers the player's hit; the later delayed death (no
    // attacker, 0x4ff215) shows the marker again, red.
    for (int i = 0; i < 3; ++i) w.frame();
    axis2->obj->set("$demigod", iw2::script::Value());
    w.debugDamage(*axis2, 100000);
    CHECK(axis2->obj->get("health").asInt() == 1 && axis2->ai.deathPending && axis2->ai.deathPendingMarker);
    CHECK(!hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    for (int i = 0; i < 3; ++i) w.frame();
    axis2->obj->set("allowdeath", iw2::script::Value(int32_t(1))); // stands in for the scripted state ending
    w.debugDamageAt(*axis2, nullptr, 2, {0, 0, 0}, "head"); // the delayed death's shape: no attacker, more than the health
    CHECK(axis2->obj->get("health").asInt() <= 0 && !axis2->ai.deathPendingMarker);
    CHECK(w.playerHitMarkerKillMs() == w.timeMs() && hitMarkerIsKill(w.playerHitMarkerMs(), w.playerHitMarkerKillMs()));
    // Killing a friendly is no kill marker (last: it starts the friendly-fire failure).
    for (int i = 0; i < 3; ++i) w.frame();
    const int64_t killBefore = w.playerHitMarkerKillMs();
    w.debugDamage(*ally, 100000);
    CHECK(w.playerHitMarkerKillMs() == killBefore);
    return failures ? 1 : 0;
}
