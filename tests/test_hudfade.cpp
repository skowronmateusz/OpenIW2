// HUD fade helpers (exe 0x4b0900, 0x4a8790).
#include "game/hudfade.h"

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
    CHECK(hudFadeAlpha(0, 500, 100, 100) < 0.0f);
    CHECK(hudFadeAlpha(1000, 1000, 100, 100) == 1.0f);
    CHECK(hudFadeAlpha(1000, 1050, 100, 100) == 0.5f);
    CHECK(hudFadeAlpha(1000, 1100, 100, 100) < 0.0f);
    CHECK(hudFadeAlpha(1000, 1500, 2000, 500) == 1.0f);
    CHECK(hudFadeAlpha(1000, 2750, 2000, 500) == 0.5f);
    CHECK(hudHealthFraction(0, 100) == 0.0f);
    CHECK(hudHealthFraction(50, 0) == 0.0f);
    CHECK(hudHealthFraction(50, 100) == 0.5f);
    CHECK(hudHealthFraction(150, 100) == 1.0f);
    // MP never fades the ammo display, compass or offhand by default; SP keeps its timed fades.
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::AmmoDisplay, true) == 0.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::Compass, true) == 0.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::Offhand, true) == 0.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::HealthBar, true) == 2.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::Stance, true) == 1.7f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::AmmoDisplay, false) == 3.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::Compass, false) == 8.0f);
    CHECK(hudFadeDefaultSeconds(HudFadeDvar::Offhand, false) == 3.0f);
    {
        const std::string cur = "mp40", sel = "kar98k", none;
        auto owns = [](const std::string& w) { return w == "kar98k" || w == "mp40"; };
        CHECK(hudShownWeapon(sel, cur, owns) == "kar98k");      // owned selected weapon: the display follows the key press
        CHECK(hudShownWeapon(none, cur, owns) == "mp40");       // nothing selected
        CHECK(hudShownWeapon("bar", cur, owns) == "mp40");      // not owned: the current weapon
    }
    return failures ? 1 : 0;
}
