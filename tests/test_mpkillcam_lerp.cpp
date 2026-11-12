// The followed view between two records (docs/spec/mp-hud.md "Interpolation of the followed view"): a killcam
// playback sampled at 16 ms render steps over 50 ms records gives a view-weapon animation time that rises with every
// rendered frame (no 20 Hz steps), continuous across record boundaries and animation changes; the bob cycle wraps,
// the aim fraction, velocity, view height and lean are interpolated, the discrete fields are the shown record's.
#include <cmath>
#include <iostream>
#include <vector>

#include "game/mpkillcam.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
using iw2::game::MpViewState;
using iw2::game::lerpMpViewState;
} // namespace

int main() {
    // Records every 50 ms: the fire animation from 0 ms, then at record 6 a reload starts (its time 20 ms at that record).
    std::vector<MpViewState> rec;
    for (int n = 0; n < 14; ++n) {
        MpViewState v;
        v.weapon = v.viewWeapon = "kar98k_mp";
        v.animKey = n < 6 ? "fireAnim" : "reloadAnim";
        v.animMs = n < 6 ? n * 50 : 20 + (n - 6) * 50;
        v.adsFrac = std::min(1.0f, float(n) * 0.1f);
        v.bobCycle = (200 + n * 20) & 0xff;
        rec.push_back(v);
    }
    int prevAnim = -1;
    std::string prevKey;
    int steps = 0, stalls = 0;
    float prevAds = -1;
    for (int t = 50; t <= 13 * 50; t += 16) { // render time; record n shown over (50(n-1), 50n]
        const int n = (t + 49) / 50;
        const float frac = float(t - 50 * (n - 1)) / 50.0f;
        const MpViewState v = lerpMpViewState(rec[size_t(n - 1)], rec[size_t(n)], frac, 50);
        CHECK(v.animKey == rec[size_t(n)].animKey);
        if (v.animKey == prevKey) {
            const int d = v.animMs - prevAnim;
            CHECK(d >= 0);              // monotonic
            if (d == 0 && v.animMs > 0) ++stalls; // a held frame (not the wait before a start) is a 20 Hz step
            if (d > 17) ++steps;        // a jump larger than the render step
        }
        CHECK(v.adsFrac >= prevAds);
        prevAds = v.adsFrac;
        prevAnim = v.animMs;
        prevKey = v.animKey;
    }
    CHECK(stalls == 0);
    CHECK(steps == 0);
    // The reload's start: within its first interval the time is counted back from the record, not below 0.
    CHECK(lerpMpViewState(rec[5], rec[6], 0.2f, 50).animMs == 0);
    CHECK(lerpMpViewState(rec[5], rec[6], 0.8f, 50).animMs == 10);
    CHECK(lerpMpViewState(rec[5], rec[6], 1.0f, 50).animMs == 20);
    // The shown record at frac 1, the previous at frac 0 (same animation).
    CHECK(lerpMpViewState(rec[2], rec[3], 1.0f, 50) == rec[3]);
    CHECK(lerpMpViewState(rec[2], rec[3], 0.0f, 50).animMs == rec[2].animMs);
    // The bob cycle wraps at 256: 240 -> 4 (+20) at half way is 250.
    MpViewState a, b;
    a.bobCycle = 240;
    b.bobCycle = 4;
    a.viewHeight = 60;
    b.viewHeight = 40;
    a.velocity = {0, 0, 0};
    b.velocity = {100, 0, 0};
    a.lean = 0;
    b.lean = 0.5f;
    b.weaponState = 3;
    const MpViewState h = lerpMpViewState(a, b, 0.5f, 50);
    CHECK(h.bobCycle == 250);
    CHECK(std::fabs(h.viewHeight - 50) < 1e-4f);
    CHECK(std::fabs(h.velocity[0] - 50) < 1e-4f);
    CHECK(std::fabs(h.lean - 0.25f) < 1e-4f);
    CHECK(h.weaponState == 3);
    CHECK(lerpMpViewState(a, b, 1.0f, 50).bobCycle == 4);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "mpkillcam_lerp ok\n";
    return failures ? 1 : 0;
}
