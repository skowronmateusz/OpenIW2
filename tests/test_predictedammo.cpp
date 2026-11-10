// The client's predicted ammunition (docs/spec/net.md section 10, "Prediction and interpolation"): the counters the
// HUD draws are the server's with the shots and reloads of the commands it has not run yet applied.
// netgame::PredictedAmmo keeps those changes per command time; checked here without a server:
// - a snapshot older than a predicted shot keeps the shot subtracted, and one that has seen it does not subtract again;
// - a predicted reload (clip up, stock down) stays applied until the server has seen it;
// - a counter the server changes on its own (a spawn's full magazine, a scripted refill) is taken as sent;
// - a snapshot's correction is not replayed by the next snapshot, which is still older than the corrected frame;
// - a command without a change (or a time nobody recorded) adds nothing, and clear() forgets everything.

#include <cmath>
#include <iostream>

#include "netgame/netmove.h"

using namespace iw2::netgame;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using C = PredictedAmmo::Counters;

C counters(int clip, int stock) { return {{{"clip", clip}}, {{"ammo", stock}}}; }

// The clip of a snapshot with these counters, as the HUD shows it.
C shown(PredictedAmmo& p, int commandTime, int clip, int stock) {
    C c = counters(clip, stock);
    p.adjust(commandTime, c);
    return c;
}

} // namespace

int main() {
    // A shot the snapshot has not seen, then one that has seen it.
    {
        PredictedAmmo p;
        p.record(1015, counters(30, 90), counters(29, 90));
        CHECK(shown(p, 1000, 30, 90).clip.at("clip") == 29);
        CHECK(shown(p, 1015, 29, 90).clip.at("clip") == 29);
        CHECK(shown(p, 1030, 29, 90).clip.at("clip") == 29);
    }
    // Two pending shots: each snapshot takes one off the pending list.
    {
        PredictedAmmo p;
        p.record(1015, counters(30, 90), counters(29, 90));
        p.record(1030, counters(29, 90), counters(28, 90));
        CHECK(shown(p, 1000, 30, 90).clip.at("clip") == 28);
        CHECK(shown(p, 1015, 29, 90).clip.at("clip") == 28);
    }
    // A predicted reload.
    {
        PredictedAmmo p;
        p.record(1015, counters(5, 90), counters(30, 65));
        const C r = shown(p, 1000, 5, 90);
        CHECK(r.clip.at("clip") == 30 && r.stock.at("ammo") == 65);
        const C seen = shown(p, 1015, 30, 65);
        CHECK(seen.clip.at("clip") == 30 && seen.stock.at("ammo") == 65);
    }
    // The server's own change with nothing pending is taken as sent.
    {
        PredictedAmmo p;
        p.record(1015, counters(30, 90), counters(30, 90));
        CHECK(shown(p, 1000, 12, 40).clip.at("clip") == 12);
    }
    // A correction: the server's magazine is one lower than the client's. The next frame's counters start from the
    // corrected value, so the record holds no change for it and the next snapshot draws the same number.
    {
        PredictedAmmo p;
        p.record(1015, counters(30, 90), counters(30, 90));  // no shot predicted
        CHECK(shown(p, 1000, 29, 90).clip.at("clip") == 29);
        p.record(1030, counters(29, 90), counters(29, 90));
        CHECK(shown(p, 1015, 29, 90).clip.at("clip") == 29);
    }
    // Never below zero; a counter the snapshot does not carry stays absent.
    {
        PredictedAmmo p;
        p.record(1015, {{{"clip", 1}, {"other", 5}}, {}}, {{{"clip", 0}, {"other", 4}}, {}});
        p.record(1030, {{{"clip", 1}}, {}}, {{{"clip", 0}}, {}});
        const C r = shown(p, 1000, 1, 0);
        CHECK(r.clip.at("clip") == 0);
        CHECK(!r.clip.count("other"));
    }
    // clear() forgets the pending changes.
    {
        PredictedAmmo p;
        p.record(1015, counters(30, 90), counters(29, 90));
        p.clear();
        CHECK(shown(p, 1000, 30, 90).clip.at("clip") == 30);
    }
    // The counters as the server sent them stay readable, and the kept changes can be restarted (a rebuilt weapon).
    {
        PredictedAmmo p;
        p.record(1010, counters(30, 90), counters(29, 90));
        CHECK(shown(p, 1000, 30, 90).clip.at("clip") == 29);
        CHECK(p.server().clip.at("clip") == 30);
        p.clearPending();
        CHECK(shown(p, 1000, 30, 90).clip.at("clip") == 30);
    }
    if (failures == 0) std::cout << "predictedammo: ok\n";
    return failures ? 1 : 0;
}
