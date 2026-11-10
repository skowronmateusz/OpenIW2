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

// The server ran a command with a longer step than the client (its clamp after a stall): the client's timer is moved to
// the server's, once, and the next shot falls where the server's does.
void weaponTimeFollowsTheServer() {
    using iw2::game::PlayerWeapon;
    PlayerWeapon w;
    w.equip("smg", {{"firetime", "0.1"}, {"clipsize", "30"}, {"startammo", "60"}, {"raisetime", "0.1"}});
    PlayerWeapon::Input fire;
    fire.fire = true;
    fire.weapon = "smg";
    PlayerWeapon::Input idle;
    idle.weapon = "smg";
    w.update(1.0f, idle);  // raised and ready
    int shots = w.update(0.02f, fire);
    CHECK(shots == 1);
    const float afterShot = w.timer();
    PredictedWeaponTime t;
    t.record(1000, w);
    w.update(0.02f, fire);
    t.record(1020, w);
    w.update(0.02f, fire);
    t.record(1040, w);
    const float predicted = w.timer();
    PsFields s;
    s.commandTime = 1000;
    s.weaponState = w.stateNumber();
    s.weaponTime = int(std::lround(afterShot * 1000.0f)) + 50;
    t.reconcile(s, w);
    CHECK(std::fabs(w.timer() - (predicted + 0.05f)) < 0.001f);
    t.reconcile(s, w);  // the same snapshot again: nothing more
    CHECK(std::fabs(w.timer() - (predicted + 0.05f)) < 0.001f);
    // The newer command's kept value moved too: a server that agrees with the corrected timer changes nothing.
    PsFields s2 = s;
    s2.commandTime = 1020;
    s2.weaponTime = int(std::lround((afterShot - 0.02f + 0.05f) * 1000.0f));
    const float corrected = w.timer();
    t.reconcile(s2, w);
    CHECK(std::fabs(w.timer() - corrected) < 0.003f);
    // The shot the uncorrected timer would have fired does not happen.
    int later = 0;
    for (int i = 0; i < int(predicted / 0.02f); ++i) later += w.update(0.02f, fire);
    CHECK(later == 0);
    // Another weapon state on the server, or a command the client never recorded: no change.
    PsFields other = s;
    other.commandTime = 1040;
    other.weaponState = w.stateNumber() + 1;
    other.weaponTime = 900;
    const float kept = w.timer();
    t.reconcile(other, w);
    CHECK(w.timer() == kept);
    other.commandTime = 2000;
    other.weaponState = w.stateNumber();
    t.reconcile(other, w);
    CHECK(w.timer() == kept);
}

int main() {
    weaponTimeFollowsTheServer();
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
    // The weapon step is the command's step; without a new command the frame time.
    {
        PredictedAmmo p;
        CHECK(p.weaponStep(1000, 0.02f) == 0.05f);  // first command, as usercmdStep
        CHECK(p.weaponStep(1000, 0.02f) == 0.0f);  // held clock: the server runs no command
        const float s = p.weaponStep(1016, 0.02f);
        CHECK(s > 0.0159f && s < 0.0161f);
    }
    if (failures == 0) std::cout << "predictedammo: ok\n";
    return failures ? 1 : 0;
}
