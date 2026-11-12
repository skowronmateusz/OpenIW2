// The multiplayer low-health overlay (docs/spec/mp-hud.md "Low-health overlay", src/game/healthoverlay.h): no overlay at
// full health or above the threshold; a drop below it pulses (1, 0.7, 0.6 of the level's alpha over 150 / 320 / 400 ms,
// the levels 1, 0.8, 0.6, 0.3 one after the other across the regeneration pause), then fades to 0 over 700 ms; a new
// drop starts the pulses again; dead (pm_type 6) draws nothing. The pulse does not care how the health was lost, so a
// fall and a bullet show it alike. With IW2_MAIN: the MP hud menu "Low Health" has the ownerdraw 109 item with the
// overlay material.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "fs/vfs.h"
#include "game/healthoverlay.h"
#include "ui/menu.h"

using namespace iw2;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.02f; }
} // namespace

int main() {
    const game::HealthOverlayParams p;
    check(game::healthOverlayFraction(100, 100, 0) == 1.0f, "full health is fraction 1");
    check(game::healthOverlayFraction(20, 100, 6) == 0.0f, "dead is fraction 0");
    check(game::healthOverlayFraction(0, 100, 0) == 0.0f, "no health is fraction 0");
    {
        game::HealthOverlay o;
        o.reset(p);
        int64_t t = 1000;
        float maxA = 0;
        for (; t < 3000; t += 16) maxA = std::max(maxA, o.frame(1.0f, t, p));
        check(maxA == 0.0f, "full health: no overlay");
        for (; t < 5000; t += 16) maxA = std::max(maxA, o.frame(0.5f, t, p));
        check(maxA == 0.0f, "half health (above 0.35): no overlay");
        // A hit to 20 health (a fall or a bullet).
        const int64_t hit = t;
        float peak = 0, at300 = -1;
        for (; t < hit + 800; t += 16) {
            const float a = o.frame(0.2f, t, p);
            peak = std::max(peak, a);
            if (at300 < 0 && t >= hit + 300) at300 = a;
        }
        check(near(peak, 1.0f), "the first pulse reaches alpha 1");
        check(at300 > 0.7f && at300 < 1.0f, "then falls towards 0.7");
        check(o.active(), "pulsing while low");
        // The pulses run their levels across the regeneration pause, then the overlay fades out.
        for (; t < hit + 9000; t += 16) o.frame(0.2f, t, p);
        check(!o.active() && near(o.frame(0.2f, t, p), 0.0f), "after the pause the overlay is gone");
        // A new drop below the threshold starts again.
        const int64_t hit2 = t;
        peak = 0;
        for (; t < hit2 + 1500; t += 16) peak = std::max(peak, o.frame(0.1f, t, p));
        check(near(peak, 1.0f), "a new drop pulses again (after the running fade step of up to 700 ms)");
        // Death: nothing drawn.
        check(o.frame(0.0f, t, p) == 0.0f, "dead: nothing drawn");
        // A spawn resets.
        o.reset(p);
        check(o.frame(1.0f, t + 16, p) == 0.0f, "after a spawn at full health: nothing");
    }
    if (const char* main = std::getenv("IW2_MAIN")) {
        fs::Vfs vfs;
        vfs.mountDirectory(main);
        ui::MenuLibrary lib(vfs);
        check(lib.loadList("ui_mp/hud.txt"), "ui_mp/hud.txt loads");
        const ui::Menu* m = lib.find("Low Health");
        check(m != nullptr, "the MP hud has the Low Health menu");
        bool item = false;
        if (m)
            for (const auto& it : m->items)
                if (it->ownerDraw == 109 && it->background == "overlay_low_health") item = true;
        check(item, "its item is ownerdraw 109 with the overlay material");
    }
    if (failures) return 1;
    std::printf("healthoverlay: ok\n");
    return 0;
}
