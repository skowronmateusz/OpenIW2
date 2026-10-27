// Tests for the shell shock record parsing and the per-frame effects (cgame 0x4b1950, 0x4b22c0, 0x4b2380, 0x4b1f90).

#include "game/shellshock.h"

#include <algorithm>
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
bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    using namespace iw2::game;
    CHECK(shockMs(0.4f) == 400);
    CHECK(shockMs(2.5f) == 2500);
    CHECK(shockMs(-0.75f) == -750);

    const ShockRecord r = parseShockFile(
        "cg_shock_screenBlendTime \"0.5\"\n"
        "cg_shock_screenBlendFadeTime \"2\"\n"
        "cg_shock_viewKickPeriod \"0.5\"\n"
        "cg_shock_viewKickRadius \"0.1\"\n"
        "cg_shock_viewKickFadeTime \"4\"\n"
        "cg_shock_sound \"1\"\n"
        "cg_shock_soundFadeInTime \"1\"\n"
        "cg_shock_soundFadeOutTime \"2\"\n"
        "cg_shock_soundLoopFadeTime \"1\"\n"
        "cg_shock_soundLoopEndDelay \"-1\"\n"
        "cg_shock_soundModEndDelay \"-0.5\"\n"
        "cg_shock_soundRoomType \"underwater\"\n"
        "cg_shock_volume_weapon \"0.25\"\n"
        "cg_shock_volume_voice \"7\"\n"
        "cg_shock_mouse \"1\"\n"
        "cg_shock_mouse_maxpitchspeed \"60\"\n"
        "cg_shock_mouse_maxyawspeed \"80\"\n"
        "cg_shock_mouse_sensitivityscale \"0.5\"\n"
        "cg_shock_mouse_fadeTime \"2\"\n"
        "cg_shock_unknownKey \"9\"\n");
    CHECK(r.blendMs == 500 && r.blendFadeMs == 2000);
    CHECK(near(r.viewKickCyclesPerMs, 0.002f) && near(r.viewKickRadius, 0.1f) && r.viewKickFadeMs == 4000);
    CHECK(r.room == "underwater");
    CHECK(near(r.volume[5], 0.25f)); // weapon
    CHECK(near(r.volume[6], 1.0f));  // voice: clamped to 1
    CHECK(r.loopEndDelayMs == -1000 && r.soundModEndDelayMs == -500);
    // Times are clamped to the dvars' 0.001..10 range.
    CHECK(parseShockFile("cg_shock_viewKickFadeTime \"99\"\n").viewKickFadeMs == 10000);
    CHECK(parseShockFile("cg_shock_viewKickFadeTime \"0\"\n").viewKickFadeMs == 1);

    // View kick: nothing at the end, bounded by the radius before.
    {
        const int dur = 6000;
        CHECK(shockViewKick(r, dur, dur).x == 0 && shockViewKick(r, dur + 5, dur).y == 0);
        float maxAbs = 0;
        for (int t = 0; t < dur; t += 7) {
            const ShockKick k = shockViewKick(r, t, dur);
            maxAbs = std::max({maxAbs, std::fabs(k.x), std::fabs(k.y)});
        }
        CHECK(maxAbs > 0.0f && maxAbs < 0.1f * 1.2f);
        const ShockKick a = shockViewKick(r, 1000, dur), b = shockViewKick(r, 1000, dur); // deterministic
        CHECK(a.x == b.x && a.y == b.y);
    }
    // The axis rotation: no kick leaves the axis alone.
    {
        const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float out[9];
        applyShockKick(id, {0, 0}, out);
        for (int i = 0; i < 9; ++i) CHECK(near(out[i], id[i]));
        applyShockKick(id, {0.1f, 0}, out);
        CHECK(out[1] > 0.09f && out[0] > 0.99f); // forward leans to the left
        CHECK(near(out[0] * out[0] + out[1] * out[1] + out[2] * out[2], 1.0f));
    }
    // Mouse: full effect until the fade starts, back to normal at the end.
    {
        ShockMouse m = shockMouse(r, 1000, 6000);
        CHECK(near(m.sensitivityScale, 0.5f) && near(m.maxPitchSpeed, 60) && near(m.maxYawSpeed, 80));
        m = shockMouse(r, 5000, 6000); // remaining 1000 of a 2000 fade: t = 0.5
        CHECK(near(m.sensitivityScale, 0.75f) && near(m.maxPitchSpeed, 120) && near(m.maxYawSpeed, 160));
        m = shockMouse(r, 6000, 6000);
        CHECK(m.sensitivityScale == 1.0f && m.maxPitchSpeed == 0 && m.maxYawSpeed == 0);
    }
    // Sound filter: fade in over 1000 ms, held, fading out to zero at duration - 500 + 2000.
    {
        const int dur = 6000;
        CHECK(near(shockSoundFraction(r, 0, dur), 0.0f));
        CHECK(near(shockSoundFraction(r, 500, dur), 0.5f));
        CHECK(near(shockSoundFraction(r, 3000, dur), 1.0f));
        CHECK(near(shockSoundFraction(r, 5500, dur), 1.0f)); // x = -500 - 5500 + 2000 + 6000 = 2000 = fadeOut
        CHECK(near(shockSoundFraction(r, 6500, dur), 0.5f)); // x = 1000
        CHECK(shockSoundFraction(r, 7500, dur) == 0.0f);
        CHECK(near(shockChannelVolume(r, 5, 1.0f), 0.25f) && near(shockChannelVolume(r, 5, 0.0f), 1.0f));
        CHECK(near(shockChannelVolume(r, 5, 0.5f), 0.625f));
    }
    // Loop: until duration + loopEndDelay + loopFade; fades to its silent twin over the last loopFade ms.
    {
        const int dur = 6000;
        CHECK(shockLoopBlend(r, 0, dur) == 0.0f);
        CHECK(shockLoopBlend(r, 4500, dur) == 0.0f);
        CHECK(near(shockLoopBlend(r, 5500, dur), 0.5f)); // z = -1000 - 5500 + 6000 + 1000 = 500
        CHECK(shockLoopBlend(r, 6000, dur) < 0.0f);
        CHECK(shockEndSoundMs(r, dur) == 5000);
    }
    // Screen blend: the full time until the fade, then proportional.
    CHECK(shockBlendMs(r, 5000) == 500 && shockBlendMs(r, 1000) == 250 && shockBlendMs(r, 0) == 0);

    if (failures == 0) std::cout << "shellshock: ok\n";
    return failures == 0 ? 0 : 1;
}
