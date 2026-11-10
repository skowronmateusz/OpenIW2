// The client's predicted weapon against a server on one command stream (docs/spec/net.md section 10 "Command steps",
// "Predicted weapon"), without a network or a level: a model of both ends in one process, in whole milliseconds.
//
// The server side follows the spec: it reads the waiting commands before its catch-up frames, clamps a command's time
// to the level time - 1000 .. + 200, does not run a command that is not newer than the last one run, and runs the
// others in pieces of at most 66 ms (netgame::weaponStep). After a stall the commands beyond level time + 200 are
// therefore swallowed and the next one runs the whole gap, so the server's shots fall on other commands than the
// client's. The client side is netgame::PredictedWeapon as the front ends use it: the snapshot's counters with
// PredictedAmmo, snapshot() once per snapshot, command() once per frame.
//
// Checked over many seeds, with stalls of 0.2 .. 0.9 s, jittered frames, packet and snapshot delays:
// - without stalls the prediction is exact: no rebuild, the same shots;
// - a magazine fired empty in bursts (the soak's scenario: fire while the acknowledged clip is not 0): the fire events
//   the client played are exactly the server's shots, for an automatic, a fast automatic, a semi-automatic and a
//   bolt-action weapon;
// - after the firing ends the client's weapon state, clip and stock are the server's;
// - bursts that end at any time: the client never plays fewer events than the server fired, and plays more only when
//   it predicted a shot in a stall that the server then did not fire before the trigger was released.
// Single cases with hand-written snapshots: a shot on another command, a shot the client did not predict, a commandTime
// that is no command's time, more events than the ring holds, a prediction that falls back and comes again, a repeated
// command time.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/playerweapon.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"

using namespace iw2;
using game::PlayerWeapon;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

struct Rng {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int range(int a, int b) { return a + int(next() % uint32_t(b - a + 1)); }
};

struct Ammo {
    std::map<std::string, int> clip, stock;
};

void wire(PlayerWeapon& w, Ammo& a) {
    Ammo* p = &a;
    w.readAmmo = [p](const std::string&) -> std::optional<std::pair<int, int>> {
        return std::make_pair(p->clip["clip"], p->stock["ammo"]);
    };
    w.writeAmmo = [p](const std::string&, int clip, int stock) {
        p->clip["clip"] = clip;
        p->stock["ammo"] = stock;
    };
}

constexpr uint32_t kFire = 1;

struct Cmd {
    int time = 0;
    uint32_t buttons = 0;
    int arrival = 0;
};

struct Snap {
    int number = 0;
    int arrival = 0;
    netgame::PsFields ps;
    int clip = 0, stock = 0;
};

struct Server {
    PlayerWeapon w;
    Ammo ammo;
    netgame::EventRing ring;
    int level = 1000;
    int lastTime = 0;
    int shots = 0;
    int skipped = 0;
    void think(const Cmd& c) {
        const int t = std::clamp(c.time, level - 1000, level + 200);
        if (lastTime != 0 && t <= lastTime) {
            lastTime = t;
            ++skipped;
            return;
        }
        const int msec = netgame::usercmdMsec(lastTime, t);
        lastTime = t;
        PlayerWeapon::Input in;
        in.fire = (c.buttons & kFire) != 0;
        in.weapon = "gun";
        const int n = netgame::weaponStep(w, msec, in);
        for (int i = 0; i < n; ++i) ring.push(w.clip() == 0 && i == n - 1 ? netgame::kEvFireWeaponLastShot : netgame::kEvFireWeapon, 1);
        shots += n;
        for (const auto& ev : w.takeEvents()) {
            if (ev.kind == PlayerWeapon::Event::ReloadBegin) ring.push(netgame::kEvReload, 1);
            if (ev.kind == PlayerWeapon::Event::NoAmmo) ring.push(netgame::kEvNoAmmo, 1);
        }
        (void)w.takeSounds();
    }
    Snap snapshot(int number, int arrival) const {
        Snap s;
        s.number = number;
        s.arrival = arrival;
        s.ps.commandTime = lastTime;
        s.ps.weaponTime = std::clamp(int(std::lround(std::max(0.0f, w.timer()) * 1000.0f)), 0, 32767);
        s.ps.weaponState = w.stateNumber();
        s.ps.eventSequence = ring.sequence & 0xff;
        for (int i = 0; i < 4; ++i) s.ps.events[i] = ring.events[size_t(i)];
        s.clip = ammo.clip.at("clip");
        s.stock = ammo.stock.at("ammo");
        return s;
    }
};

struct Client {
    PlayerWeapon w;
    Ammo ammo;
    netgame::PredictedWeapon predicted;
    int lastCmd = 0;
    int played = 0;       // fire events played
    int plainPlayed = 0;  // what the comparison with the previous frame alone (0x4e0340) would have played
    int prevPredicted = 0;
    int snapNumber = -1;
    int ackClip = -1;     // the clip of the newest snapshot
    // One frame: the newest snapshot (nullptr none yet), then the frame's command with these buttons.
    Cmd frame(int clock, uint32_t buttons, const Snap* snap) {
        if (snap) {
            netgame::PredictedAmmo::Counters c{{{"clip", snap->clip}}, {{"ammo", snap->stock}}};
            predicted.ammo.adjust(snap->ps.commandTime, c);
            ammo.clip = c.clip;
            ammo.stock = c.stock;
            ackClip = snap->clip;
            if (snap->number != snapNumber) {
                snapNumber = snap->number;
                predicted.snapshot(snap->ps, "gun", w, ammo.clip, ammo.stock);
            }
        }
        Cmd c;
        c.time = std::max(lastCmd, clock);
        c.buttons = buttons;
        lastCmd = c.time;
        PlayerWeapon::Input in;
        in.fire = (buttons & kFire) != 0;
        in.weapon = "gun";
        played += predicted.command(c.time, in, w, ammo.clip, ammo.stock);
        plainPlayed += std::max(0, predicted.predictedFires() - prevPredicted);
        prevPredicted = predicted.predictedFires();
        (void)w.takeSounds();
        (void)w.takeEvents();
        return c;
    }
};

enum class Scenario { EmptyMagazine, AnyBursts };

struct Result {
    int serverShots = 0, played = 0, plainPlayed = 0, rebuilds = 0, skipped = 0;
    bool emptied = false;
    bool stateEqual = false;
};

Result run(const PlayerWeapon::Def& def, uint32_t seed, bool stalls, Scenario scenario, int clockOffset) {
    Rng rng{seed * 2654435761u + 12345u};
    Server sv;
    Client cl;
    const int clipSize = std::atoi(def.at("clipsize").c_str());
    for (Ammo* a : {&sv.ammo, &cl.ammo}) {
        a->clip["clip"] = clipSize;
        a->stock["ammo"] = 10 * clipSize;
    }
    wire(sv.w, sv.ammo);
    wire(cl.w, cl.ammo);
    sv.w.equip("gun", def);
    cl.w.equip("gun", def);

    std::deque<Cmd> toServer;
    std::deque<Snap> toClient;
    std::optional<Snap> latest;
    int now = 1000, nextFrame = 1050, nextClient = 1003, nextPoll = 1005, snapNumber = 0;
    int stallFrom = stalls ? now + rng.range(1200, 2500) : 1 << 30, stallTo = stallFrom + rng.range(200, 500);
    const int fireStart = 2000;
    int quietFrom = -1;         // the firing has ended: the run goes on for the pending times
    bool emptied = false;
    uint32_t pattern = kFire;   // AnyBursts: the buttons until patternUntil
    int patternUntil = fireStart;
    Result r;
    for (; now < 60000; ++now) {
        if (now == nextPoll) {
            nextPoll += 5;
            if (now >= stallFrom && now < stallTo) {
                // stalled: no packet is read, no frame runs
            } else {
                if (now >= stallTo) {
                    stallFrom = now + rng.range(600, 2500);
                    stallTo = stallFrom + (rng.range(0, 5) == 0 ? rng.range(500, 900) : rng.range(200, 500));
                    if (!stalls) stallFrom = stallTo = 1 << 30;
                }
                while (!toServer.empty() && toServer.front().arrival <= now) {
                    sv.think(toServer.front());
                    toServer.pop_front();
                }
                bool ran = false;
                while (now >= nextFrame) {
                    sv.level += 50;
                    nextFrame += 50;
                    ran = true;
                }
                if (ran) toClient.push_back(sv.snapshot(++snapNumber, now + rng.range(1, 8)));
            }
        }
        if (now == nextClient) {
            nextClient += rng.range(6, 22);
            while (!toClient.empty() && toClient.front().arrival <= now) {
                latest = toClient.front();
                toClient.pop_front();
            }
            uint32_t buttons = 0;
            if (quietFrom < 0 && now >= fireStart) {
                if (scenario == Scenario::EmptyMagazine) {
                    // The soak's fire step: bursts of 0.3 s while the acknowledged clip is not empty.
                    if (cl.ackClip == 0) emptied = true;
                    if (emptied) quietFrom = now;
                    else if ((now - fireStart) / 100 % 4 != 3) buttons |= kFire;
                } else {
                    if (now >= patternUntil) {
                        pattern ^= kFire;
                        patternUntil = now + rng.range(40, 450);
                    }
                    buttons |= pattern;
                    if (now >= fireStart + 9000) quietFrom = now, buttons = 0;
                }
            }
            Cmd c = cl.frame(now + clockOffset, buttons, latest ? &*latest : nullptr);
            c.arrival = now + rng.range(1, 8);
            toServer.push_back(c);
        }
        if (quietFrom >= 0 && now - quietFrom > 6000) break;
    }
    r.serverShots = sv.shots;
    r.played = cl.played;
    r.plainPlayed = cl.plainPlayed;
    r.rebuilds = cl.predicted.rebuilds();
    r.skipped = sv.skipped;
    r.emptied = emptied;
    r.stateEqual = cl.w.stateNumber() == sv.w.stateNumber() && cl.ammo.clip == sv.ammo.clip && cl.ammo.stock == sv.ammo.stock &&
                   std::fabs(std::max(0.0f, cl.w.timer()) - std::max(0.0f, sv.w.timer())) < 0.003f;
    return r;
}

// Single cases on the client side alone, with snapshots written by hand.
void singleCases() {
    const PlayerWeapon::Def def{{"firetime", "0.1"}, {"clipsize", "30"}, {"raisetime", "0.1"}, {"reloadtime", "2.0"}, {"reloademptytime", "2.4"}};
    auto start = [&](Client& cl) {
        cl.ammo.clip["clip"] = 30;
        cl.ammo.stock["ammo"] = 90;
        wire(cl.w, cl.ammo);
        cl.w.equip("gun", def);
        int t = 1000;
        for (; t < 1400; t += 20) cl.frame(t, 0, nullptr);  // raised and ready
        return t;
    };
    auto snap = [](int number, int commandTime, int state, int weaponTime, int clip, int sequence, std::initializer_list<int> events) {
        Snap s;
        s.number = number;
        s.ps.commandTime = commandTime;
        s.ps.weaponState = state;
        s.ps.weaponTime = weaponTime;
        s.ps.eventSequence = sequence;
        int i = sequence - int(events.size());
        for (int ev : events) s.ps.events[i++ & 3] = ev;
        s.clip = clip;
        s.stock = 90;
        return s;
    };
    const int fire = netgame::kEvFireWeapon;
    {
        // The server fired on another command than the client: one event, the same rounds, and the client's next
        // shot comes one fire time after the server's.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        t += 20;
        cl.frame(t, kFire, &s0);  // the client fires on this command (time 1420)
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29);
        t += 20;
        cl.frame(t, kFire, &s0);
        // The server ran 1420 without the shot (its weapon was 30 ms behind) and fired on 1440.
        Snap s1 = snap(2, 1420, 0, 30, 30, 0, {});
        t += 20;
        cl.frame(t, kFire, &s1);
        CHECK(cl.predicted.rebuilds() == 1);
        CHECK(cl.played == 1);                 // the shot is not played again
        CHECK(cl.ammo.clip["clip"] == 29);     // the rebuilt weapon fired on a later kept command
        Snap s2 = snap(3, 1460, 3, 80, 29, 1, {fire});
        t += 20;
        cl.frame(t, kFire, &s2);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29);
        for (int i = 0; i < 5; ++i) cl.frame(t += 20, kFire, &s2);
        CHECK(cl.played == 2 && cl.ammo.clip["clip"] == 28);  // the second shot follows the server's weapon time
    }
    {
        // A shot the client did not predict at all (the server's command ran longer): played from the snapshot's ring,
        // the weapon takes the firing state and does not fire a second time at once.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        cl.frame(t += 20, 0, &s0);
        Snap s1 = snap(2, t, 3, 100, 29, 1, {fire});
        cl.frame(t += 20, kFire, &s1);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29 && cl.w.stateNumber() == 3);
        cl.frame(t += 20, kFire, &s1);
        CHECK(cl.played == 1);
    }
    {
        // A commandTime that is no command's time (the server's clamp): the weapon is rebuilt from the newest kept
        // command before it and the kept commands after it run from that time.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        for (int i = 0; i < 5; ++i) cl.frame(t += 20, kFire, &s0);  // the first shot (command 1420) and 80 ms of its fire time
        CHECK(cl.played == 1);
        // The server stopped at 1430 (between the commands 1420 and 1440), 5 ms into its own shot: 95 ms are left, so its
        // next shot is due at 1525. The kept commands 1440 .. 1500 run again from 1430.
        Snap s1 = snap(2, 1430, 3, 95, 29, 1, {fire});
        cl.frame(t += 20, kFire, &s1);  // command 1520
        CHECK(cl.predicted.rebuilds() == 1);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29);  // the client's own timer would have fired here
        cl.frame(t += 20, kFire, &s1);  // command 1540
        CHECK(cl.played == 2 && cl.ammo.clip["clip"] == 28);
    }
    {
        // More events than the ring holds: the ones that left it are fire events as far as the clip fell.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        Snap s1 = snap(2, t, 3, 100, 24, 6, {fire, fire, fire, fire});
        cl.frame(t += 20, 0, &s1);
        CHECK(cl.played == 6);
        // Without the clip's evidence (a reload in between raised it) only the ring's four count.
        Snap s2 = snap(3, t, 0, 0, 30, 12, {fire, fire, fire, fire});
        cl.frame(t += 20, 0, &s2);
        CHECK(cl.played == 10);
    }
    {
        // A prediction ahead of the server falls back with the snapshot and comes again: the number is played once.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        cl.frame(t += 20, kFire, &s0);
        cl.frame(t += 20, 0, &s0);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29);
        // The server ran both commands without a shot (it never saw the press).
        Snap s1 = snap(2, t, 0, 0, 30, 0, {});
        cl.frame(t += 20, 0, &s1);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 30);  // the round is back, the event stays played
        cl.frame(t += 20, kFire, &s1);
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 29);  // the next shot takes the number already played
        Snap s2 = snap(3, t, 3, 100, 29, 1, {fire});
        cl.frame(t += 20, 0, &s2);
        CHECK(cl.played == 1);
    }
    {
        // A shot the server never fires (its press fell into commands the server did not run) is played once and stays
        // ahead for a second at most: a shot long after it is played like any other.
        Client cl;
        int t = start(cl);
        Snap s0 = snap(1, t - 20, 0, 0, 30, 0, {});
        cl.frame(t, 0, &s0);
        cl.frame(t += 20, kFire, &s0);
        CHECK(cl.played == 1);
        Snap s1 = snap(2, t, 0, 0, 30, 0, {});
        for (int i = 0; i < 250; ++i) cl.frame(t += 20, 0, &s1);  // five seconds without a shot on either side
        CHECK(cl.played == 1 && cl.ammo.clip["clip"] == 30);
        cl.frame(t += 20, kFire, &s1);
        CHECK(cl.played == 2 && cl.ammo.clip["clip"] == 29);
    }
    {
        // A command that repeats the last time is not run (the server does not run it either).
        Client cl;
        int t = start(cl);
        cl.lastCmd = t + 30;  // the clock is held behind the last command
        cl.frame(t, kFire, nullptr);
        CHECK(cl.played == 1);
        const float timer = cl.w.timer();
        cl.frame(t + 10, kFire, nullptr);
        cl.frame(t + 20, kFire, nullptr);
        CHECK(cl.played == 1 && cl.w.timer() == timer);  // the repeated time ran nothing
    }
    {
        // Commands with time 0 (before the level's first snapshot) run nothing.
        Client cl;
        cl.ammo.clip["clip"] = 30;
        cl.ammo.stock["ammo"] = 90;
        wire(cl.w, cl.ammo);
        cl.w.equip("gun", def);
        const float timer = cl.w.timer();
        PlayerWeapon::Input in;
        in.weapon = "gun";
        for (int i = 0; i < 10; ++i) CHECK(cl.predicted.command(0, in, cl.w, cl.ammo.clip, cl.ammo.stock) == 0);
        CHECK(cl.w.timer() == timer);
    }
    {
        // Commands the weapon does not run in (on a map turret): their time is passed, the next step is the usual one.
        Client cl;
        const int t = start(cl);
        for (int i = 0; i < 100; ++i) CHECK(cl.predicted.idle(t + 20 * i) == 0);
        cl.lastCmd = t + 2000;
        cl.frame(t + 2020, kFire, nullptr);
        CHECK(cl.played == 1);  // one 40 ms step from the last passed time, not two seconds in pieces
    }
}

}  // namespace

int main() {
    singleCases();
    const std::vector<std::pair<const char*, PlayerWeapon::Def>> weapons = {
        {"automatic", {{"firetime", "0.1"}, {"clipsize", "30"}, {"raisetime", "0.3"}, {"reloadtime", "2.0"}, {"reloademptytime", "2.4"}, {"reloadaddtime", "1.4"}}},
        {"fast automatic", {{"firetime", "0.07"}, {"clipsize", "71"}, {"raisetime", "0.3"}, {"reloadtime", "2.5"}, {"reloademptytime", "2.9"}}},
        {"semi-automatic", {{"firetime", "0.15"}, {"semiauto", "1"}, {"clipsize", "8"}, {"raisetime", "0.3"}, {"reloadtime", "1.6"}, {"reloademptytime", "1.9"}}},
        {"bolt-action", {{"firetime", "0.3"}, {"semiauto", "1"}, {"boltaction", "1"}, {"rechambertime", "1.0"}, {"rechamberbolttime", "0.4"}, {"clipsize", "5"}, {"raisetime", "0.3"}, {"reloadtime", "2.5"}, {"reloademptytime", "2.8"}}},
    };
    const int kSeeds = 150;
    for (const auto& [name, def] : weapons) {
        // Without stalls: exact.
        int rebuilds = 0, different = 0, unequal = 0;
        for (int seed = 1; seed <= 40; ++seed)
            for (Scenario sc : {Scenario::EmptyMagazine, Scenario::AnyBursts}) {
                const Result r = run(def, uint32_t(seed), false, sc, -60);
                rebuilds += r.rebuilds;
                different += r.played != r.serverShots;
                unequal += !r.stateEqual;
                if (sc == Scenario::EmptyMagazine) CHECK(r.emptied);
                CHECK(r.serverShots > 0);
            }
        CHECK(rebuilds == 0);
        CHECK(different == 0);
        CHECK(unequal == 0);
        // With stalls, a magazine fired empty: the events played are the server's shots.
        int stallDifferent = 0, stallUnequal = 0, stallRebuilds = 0, stallSkipped = 0, notEmptied = 0, plainDifferent = 0;
        for (int seed = 1; seed <= kSeeds; ++seed)
            for (int offset : {-60, -20, 30}) {
                const Result r = run(def, uint32_t(seed), true, Scenario::EmptyMagazine, offset);
                if (r.played != r.serverShots) {
                    if (++stallDifferent <= 5)
                        std::cerr << name << " seed " << seed << " offset " << offset << ": played " << r.played << ", server shots " << r.serverShots << "\n";
                }
                plainDifferent += r.plainPlayed != r.serverShots;
                stallUnequal += !r.stateEqual;
                stallRebuilds += r.rebuilds;
                stallSkipped += r.skipped;
                notEmptied += !r.emptied;
            }
        CHECK(stallDifferent == 0);
        CHECK(stallUnequal == 0);
        CHECK(notEmptied == 0);
        CHECK(stallRebuilds > 0);  // the stalls did swallow commands and the weapon was rebuilt
        CHECK(stallSkipped > 0);
        // With stalls, bursts ending at any time: never fewer events than shots; more only by a shot predicted in a stall.
        int fewer = 0, more = 0, moreShots = 0, runs = 0, anyUnequal = 0, plainMore = 0, plainMoreShots = 0;
        for (int seed = 1; seed <= kSeeds; ++seed) {
            const Result r = run(def, uint32_t(seed), true, Scenario::AnyBursts, -60);
            ++runs;
            fewer += r.played < r.serverShots;
            more += r.played > r.serverShots;
            moreShots += std::max(0, r.played - r.serverShots);
            plainMore += r.plainPlayed > r.serverShots;
            plainMoreShots += std::max(0, r.plainPlayed - r.serverShots);
            anyUnequal += !r.stateEqual;
        }
        CHECK(fewer == 0);
        CHECK(anyUnequal == 0);
        // The numbers behind the choice of comparing with the highest number played (docs/spec/net.md section 10): the
        // comparison with the previous frame alone plays a shot again whenever a prediction falls back and returns.
        std::cout << "weaponpredict " << name << ": magazine runs " << 3 * kSeeds << " (rebuilds " << stallRebuilds << ", commands not run "
                  << stallSkipped << "; previous-frame rule: " << plainDifferent << " runs differ), burst runs " << runs << ": " << more
                  << " with events ahead of the server (" << moreShots << " shots; previous-frame rule: " << plainMore << " runs, "
                  << plainMoreShots << " shots)\n";
    }
    if (failures == 0) std::cout << "weaponpredict: ok\n";
    return failures ? 1 : 0;
}
