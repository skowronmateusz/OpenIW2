// Loopback test of the game client (src/net/gameclient.cpp) against the server core with a game that moves players
// with the shared movement code (src/netgame/netmove.cpp): connection states, configstring updates (d, x/y/z),
// server and client commands, prediction matching the server, interpolation, connect timeout.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "game/collision.h"
#include "net/gameclient.h"
#include "net/netfields.h"
#include "net/server.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"

using namespace iw2;
using namespace iw2::net;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A game that runs every user command through the shared movement code in an empty world (noclip), and shows each player to the others as an entity.
struct MoveGame : ServerGame {
    std::mutex m;
    game::CollisionWorld world;
    int time = 0;
    struct P {
        bool active = false;
        game::PlayerState ps;
        int lastTime = 0;
    };
    P p[4];
    std::vector<std::string> commands;
    std::atomic<int> sid{12};

    std::map<int, std::string> configstrings() override {
        return {{0, "\\mapname\\mp_stub\\g_gametype\\dm\\protocol\\118"},
                {1, "\\sv_serverid\\" + std::to_string(sid.load()) + "\\sv_pure\\0"},
                {netgame::kMenuConfigstrings, "team_britishgerman"}};
    }
    bool clientConnect(int slot, const std::string&) override {
        std::lock_guard<std::mutex> l(m);
        p[slot] = P{};
        p[slot].ps.origin = {100.0f * static_cast<float>(slot), 0, 0};
        p[slot].ps.pmType = 2;  // noclip: the empty world has nothing to stand on
        return true;
    }
    void clientBegin(int slot) override {
        std::lock_guard<std::mutex> l(m);
        p[slot].active = true;
    }
    void clientCommand(int, const std::string& text) override {
        std::lock_guard<std::mutex> l(m);
        commands.push_back(text);
    }
    void clientThink(int slot, const UserCmd& cmd) override {
        std::lock_guard<std::mutex> l(m);
        netgame::runUsercmd(world, p[slot].ps, cmd, p[slot].lastTime);
        p[slot].lastTime = cmd.serverTime;
    }
    void clientDisconnect(int slot) override {
        std::lock_guard<std::mutex> l(m);
        p[slot].active = false;
    }
    std::atomic<int> stallMs{0};  // one frame that takes this long (a round restart rebuilding the world)
    std::atomic<int> holdFrames{0};  // frames that do not advance the time (a server that falls behind the real time)
    void runFrame(int ms) override {
        if (const int st = stallMs.exchange(0)) std::this_thread::sleep_for(std::chrono::milliseconds(st));
        if (holdFrames > 0) {
            --holdFrames;
            return;
        }
        time += ms;
    }
    int serverTime() const override { return time; }
    SnapshotView snapshot(int slot) override {
        std::lock_guard<std::mutex> l(m);
        SnapshotView v;
        netgame::PsFields f;
        f.commandTime = p[slot].lastTime;
        f.pmType = p[slot].ps.pmType;
        f.clientNum = slot;
        for (int i = 0; i < 3; ++i) {
            f.origin[i] = p[slot].ps.origin[size_t(i)];
            f.velocity[i] = p[slot].ps.velocity[size_t(i)];
        }
        netgame::writePs(v.ps, f);
        for (int s = 0; s < 4; ++s) {
            if (!p[s].active) continue;
            std::vector<uint8_t> cs(kClientStateSize, 0);
            std::memcpy(cs.data() + 60, "bob", 3);
            v.clients[s] = cs;
            if (s == slot) continue;
            netgame::RemoteEntity e;
            e.eType = 1;
            e.clientNum = s;
            for (int i = 0; i < 3; ++i) e.origin[i] = p[s].ps.origin[size_t(i)];
            netgame::writeEntity(v.entities[s], e);
        }
        return v;
    }
};

void unitTests() {
    // Movement input from a command.
    UserCmd c;
    c.forwardmove = 127;
    c.rightmove = -127;
    c.angles[1] = 16384;  // 90 degrees
    c.buttons = 0x400 | 0x200;
    const game::PlayerInput in = netgame::inputFromUsercmd(c);
    CHECK(in.forward == 1.0f && in.right == -1.0f);
    CHECK(std::fabs(in.yaw - 90.0f) < 0.01f);
    CHECK(in.jump && in.stance == game::Stance::Crouch);
    CHECK(!in.leanLeft && !in.leanRight && !in.adsHeld && !in.holdBreathHeld);
    c.buttons = 0x40 | 0x1000 | 0x8000;  // lean left, aim, hold breath (net.md section 6)
    const game::PlayerInput in2 = netgame::inputFromUsercmd(c);
    CHECK(in2.leanLeft && !in2.leanRight && in2.adsHeld && in2.holdBreathHeld);
    c.buttons = 0x80;
    CHECK(netgame::inputFromUsercmd(c).leanRight);
    {
        // Hold breath through runUsercmd (the server's pmove and the client's prediction): fully aimed with a scoped
        // weapon and 0x8000 held, the breath flag rises and the sway scale falls below 1 (MP 0x4f2aa0 gates on the
        // weapon's reticle and class, so the weapon side must say the weapon holds breath).
        game::CollisionWorld empty;
        netgame::WeaponMove wm;
        wm.adsAllowed = true;
        wm.adsFrac = 1.0f;
        wm.breathWeapon = true;
        game::PlayerState ps;
        ps.breathScale = 1.0f;
        int prev = 0;
        for (int t = 50; t <= 1000; t += 50) {
            UserCmd hc;
            hc.serverTime = t;
            hc.buttons = 0x1000 | 0x8000;
            netgame::runUsercmd(empty, ps, hc, prev, &wm);
            prev = t;
        }
        CHECK(ps.holdingBreath);
        CHECK(ps.breathScale < 0.5f);
        wm.breathWeapon = false; // no reticle: no hold breath
        game::PlayerState ps2;
        ps2.breathScale = 1.0f;
        prev = 0;
        for (int t = 50; t <= 500; t += 50) {
            UserCmd hc;
            hc.serverTime = t;
            hc.buttons = 0x1000 | 0x8000;
            netgame::runUsercmd(empty, ps2, hc, prev, &wm);
            prev = t;
        }
        CHECK(!ps2.holdingBreath && ps2.breathScale == 1.0f);
    }
    // The entity event ring keeps the last four events (net.md section 13).
    netgame::EventRing ring;
    for (int i = 0; i < 6; ++i) ring.push(netgame::kEvFireWeapon + (i & 1), i + 1);
    CHECK(ring.sequence == 6 && ring.events[(5) & 3] == netgame::kEvFireWeapon + 1 && ring.parms[(4) & 3] == 5);
    CHECK(netgame::usercmdStep(0, 1234) == 0.05f);
    CHECK(std::fabs(netgame::usercmdStep(1000, 1016) - 0.016f) < 1e-6f);
    CHECK(netgame::usercmdStep(1000, 990) == 0.0f);
    CHECK(netgame::usercmdStep(1000, 5000) == 1.0f);  // the whole gap, at most 1000 ms
    {
        int pieces = 0, longest = 0, sum = 0;
        netgame::forEachUsercmdPiece(netgame::usercmdMsec(1000, 1200), [&](float dt) {
            ++pieces;
            const int ms = int(std::lround(dt * 1000.0f));
            longest = std::max(longest, ms);
            sum += ms;
        });
        CHECK(pieces == 4 && longest == 66 && sum == 200);  // pieces of at most 66 ms
    }

    // Player-state fields round trip.
    std::vector<uint8_t> ps;
    netgame::PsFields f;
    f.commandTime = 777;
    f.pmType = 2;
    f.origin[2] = -5.5f;
    f.velocity[0] = 3;
    netgame::writePs(ps, f);
    CHECK(ps.size() == size_t(kPlayerStateSize));
    const netgame::PsFields g = netgame::readPs(ps);
    CHECK(g.commandTime == 777 && g.pmType == 2 && g.origin[2] == -5.5f && g.velocity[0] == 3);

    // Interpolation: halfway between two snapshots; angles take the short way; a new entity is not interpolated.
    ClientSnapshot a, b;
    netgame::RemoteEntity e;
    e.origin[0] = 0;
    e.angles[1] = 350;
    netgame::writeEntity(a.entities[5], e);
    e.origin[0] = 100;
    e.angles[1] = 10;
    netgame::writeEntity(b.entities[5], e);
    e.origin[0] = 42;
    netgame::writeEntity(b.entities[6], e);
    const auto out = netgame::interpolateEntities(a, b, 0.5f);
    CHECK(out.size() == 2);
    CHECK(std::fabs(out[0].origin[0] - 50) < 1e-3f);
    CHECK(std::fabs(std::remainder(out[0].angles[1], 360.0f)) < 1e-3f);
    CHECK(out[1].origin[0] == 42);
}

// The client clock on a path that delivers in bursts (docs/spec/net.md section 10 "Client time: the even clock"):
// snapshots every 50 ms of server time, 5 ms on the way, but the path is dark for darkMs out of every periodMs and
// delivers what it held at the end of the stretch (a radio that leaves its channel). Frames every 16 ms.
struct ClockRun {
    int holds = 0, leaps = 0, starved = 0, frames = 0; // frames the clock stood, leapt, lay past the newest snapshot
    int lagMin = 1 << 30, lagMax = -(1 << 30);          // the newest sent snapshot's time minus the clock
};
ClockRun runClock(ClientClock clock, int periodMs, int darkMs, int fromMs, int toMs, int oneDarkAt = -1, int clientStallAt = -1) {
    ClockRun r;
    int nextSnap = 0, newest = -1, prevSnap = 0, prevT = 0, prevNow = 0;
    bool first = true;
    for (int now = 0; now < toMs; now += 16) {
        if (clientStallAt >= 0 && now >= clientStallAt && now < clientStallAt + 16) now += 1000; // one frame of a second
        const bool lateRead = now - prevNow > 100;
        prevNow = now;
        for (;; ++nextSnap) {
            const int sent = nextSnap * 50 + 5; // real time the snapshot of server time 1000 + nextSnap * 50 leaves
            int at = sent;
            if (periodMs > 0 && sent % periodMs < darkMs) at = sent - sent % periodMs + darkMs;
            if (oneDarkAt >= 0 && sent >= oneDarkAt && sent < oneDarkAt + darkMs) at = oneDarkAt + darkMs;
            if (at > now) break;
            const int st = 1000 + nextSnap * 50;
            clock.snapshot(st, prevSnap, first, now, lateRead);
            first = false;
            prevSnap = newest = st;
        }
        if (newest < 0) continue;
        const int t = clock.time(now, newest);
        if (now >= fromMs) {
            ++r.frames;
            if (t - prevT < 8) ++r.holds;
            if (t - prevT > 24) ++r.leaps;
            if (t > newest) ++r.starved;
            const int lag = 1000 + (now - 5) / 50 * 50 - t;
            r.lagMin = std::min(r.lagMin, lag), r.lagMax = std::max(r.lagMax, lag);
        }
        prevT = t;
    }
    return r;
}

// The even clock (docs/spec/net.md section 10 "Client time: the even clock"). Snapshots of server time 1000 + n *
// interval leave at n * interval + 5 and arrive `late(n)` ms later; the client draws every frameMs and either knows
// the arrival times or only the times of its reads.
struct EvenRun {
    double stepMin = 1e9, stepMax = -1e9; // the clock's step per frame, with the fraction
    double behindMin = 1e9, behindMax = -1e9; // the newest arrived snapshot's time minus the clock
    int past = 0, frames = 0;
};
template <typename Late>
EvenRun runEven(ClientClock clock, int intervalMs, int frameMs, bool arrivalKnown, Late late, int fromMs = 4000, int toMs = 12000) {
    EvenRun r;
    int next = 0, newest = -1, prevSnap = 0;
    double prevT = 0;
    for (int now = 0; now < toMs; now += frameMs) {
        for (;; ++next) {
            const int at = next * intervalMs + 5 + late(next);
            if (at > now) break;
            const int st = 1000 + next * intervalMs;
            clock.snapshot(st, prevSnap, next == 0, arrivalKnown ? at : now);
            prevSnap = newest = st;
        }
        if (newest < 0) continue;
        const double t = double(clock.time(now, newest)) + clock.fraction();
        if (now >= fromMs) {
            ++r.frames;
            r.stepMin = std::min(r.stepMin, t - prevT), r.stepMax = std::max(r.stepMax, t - prevT);
            r.behindMin = std::min(r.behindMin, newest - t), r.behindMax = std::max(r.behindMax, newest - t);
            if (t > newest) ++r.past;
        }
        prevT = t;
    }
    return r;
}

void evenClockTests() {
    const auto even = [](int) { return 0; };
    // An even path, 20 snapshots a second, a client at 125 frames a second that only knows when it read them (up to a
    // frame late): the clock never passes the newest snapshot and moves by the frame's length in every frame (the plain
    // rule's whole milliseconds move by 1 ms now and then, an eighth of such a frame).
    {
        const EvenRun r = runEven(ClientClock{}, 50, 8, false, even);
        std::printf("even clock, 20 Hz: step %.3f..%.3f ms per 8 ms frame, %.1f..%.1f ms before the newest snapshot, past it in %d of %d\n",
                    r.stepMin, r.stepMax, r.behindMin, r.behindMax, r.past, r.frames);
        CHECK(r.frames > 500 && r.past == 0);
        CHECK(r.stepMin > 7.9 && r.stepMax < 8.1);
        CHECK(r.behindMin >= 0 && r.behindMax <= 50 + 8 + 3 + 1);
    }
    // 60 snapshots a second (16 / 17 / 17 ms apart): the clock follows the snapshots' own spacing.
    {
        ClientClock c;
        EvenRun r;
        int next = 0, newest = -1, prevSnap = 0;
        for (int now = 0; now < 12000; now += 8) {
            for (;; ++next) {
                if (next * 50 / 3 + 5 > now) break;
                const int st = 1000 + next * 50 / 3;
                c.snapshot(st, prevSnap, next == 0, next * 50 / 3 + 5);
                prevSnap = newest = st;
            }
            if (newest < 0) continue;
            const double t = double(c.time(now, newest)) + c.fraction();
            if (now >= 4000) {
                ++r.frames;
                r.behindMin = std::min(r.behindMin, newest - t), r.behindMax = std::max(r.behindMax, newest - t);
                if (t > newest) ++r.past;
            }
        }
        std::printf("even clock, 60 Hz with arrival times: %.1f..%.1f ms before the newest snapshot, past it in %d of %d\n", r.behindMin,
                    r.behindMax, r.past, r.frames);
        CHECK(r.frames > 500 && r.past == 0 && r.behindMax <= 17 + 8 + 3 + 2);
    }
    // A client at 30 frames a second: with the read times alone the snapshots look up to 33 ms late and the clock runs
    // that much further behind; with the arrival times it stays one interval + safetyMs behind the arrivals.
    {
        const EvenRun read = runEven(ClientClock{}, 50, 33, false, even);
        const EvenRun arrival = runEven(ClientClock{}, 50, 33, true, even);
        std::printf("even clock at 30 frames a second: %.1f..%.1f ms before the newest snapshot by read times, %.1f..%.1f by arrival times\n",
                    read.behindMin, read.behindMax, arrival.behindMin, arrival.behindMax);
        CHECK(read.past == 0 && arrival.past == 0);
        CHECK(arrival.behindMax <= 50 + 33 + 3 + 1 && arrival.behindMax < read.behindMax - 15);
    }
    // Jitter (every snapshot 0..12 ms late, in order): the clock runs behind the delay nine snapshots in ten stay
    // within, moves evenly, and is past the newest snapshot only for the few beyond that, never by more than the jitter.
    {
        uint32_t seed = 12345;
        std::vector<int> lates;
        for (int i = 0; i < 400; ++i) {
            seed = seed * 1664525u + 1013904223u;
            lates.push_back(int((seed >> 16) % 13));
        }
        const auto jitter = [&](int n) { return lates[size_t(n) % lates.size()]; };
        const EvenRun r = runEven(ClientClock{}, 50, 8, true, jitter);
        const EvenRun plain = runEven(ClientClock{0, 0}, 50, 8, true, jitter);
        std::printf("even clock, 0..12 ms jitter: step %.3f..%.3f ms, %.1f..%.1f ms before the newest snapshot, past it in %d of %d (plain rule: %d, step %.0f..%.0f)\n",
                    r.stepMin, r.stepMax, r.behindMin, r.behindMax, r.past, r.frames, plain.past, plain.stepMin, plain.stepMax);
        CHECK(r.stepMin > 7.1 && r.stepMax < 8.5);  // within the slew limits (10 % slower, 5 % faster)
        CHECK(r.past * 20 < r.frames && r.behindMin > -13);
        CHECK(r.behindMax <= 50 + 12 + 8 + 3 + 1);
    }
    // One lost snapshot does not move the clock (the interval is the second largest of the last five).
    {
        ClientClock c;
        int prevSnap = 0, newest = 0;
        double worst = 0, before = 0;
        for (int now = 0, n = 0; now < 9000; now += 8)
            for (; n * 50 + 5 <= now; ++n) {
                if (n == 100) continue; // lost
                const int st = 1000 + n * 50;
                c.snapshot(st, prevSnap, n == 0, n * 50 + 5);
                prevSnap = newest = st;
                const double t = double(c.time(now, newest)) + c.fraction() - now;
                if (n == 99) before = t;
                if (n > 100 && n < 130) worst = std::max(worst, std::fabs(t - before));
            }
        std::printf("even clock, one lost snapshot: the clock moves by %.2f ms at most\n", worst);
        CHECK(worst < 1.0);
    }
    // Two snapshots lost within five (at 60 and at 20 a second): the interval grows for a while, but that is no change
    // of the rate; the clock slews (never slower than 10 % or faster than 5 % against real time) and does not stand
    // or leap.
    for (int interval : {17, 50}) {
        ClientClock c;
        int prevSnap = 0, newest = -1;
        double prevT = 0, lo = 1e9, hi = -1e9;
        for (int now = 0, n = 0; now < 9000; now += 8) {
            for (; n * interval + 5 <= now; ++n) {
                if (n % 100 == 50 || n % 100 == 52) continue; // lost
                const int st = 1000 + n * interval;
                c.snapshot(st, prevSnap, newest < 0, n * interval + 5);
                prevSnap = newest = st;
            }
            if (newest < 0) continue;
            const double t = double(c.time(now, newest)) + c.fraction();
            if (now >= 3000) lo = std::min(lo, t - prevT), hi = std::max(hi, t - prevT);
            prevT = t;
        }
        std::printf("even clock, two snapshots lost within five at %d ms: step %.3f..%.3f ms per 8 ms frame\n", interval, lo, hi);
        CHECK(lo > 7.19 && hi < 8.41);
    }
    // Snapshots 17 and 33 ms apart in turn (a remote client whose rate limit leaves out steps), then one per 50 ms
    // (a killcam): the clock keeps the longer interval between itself and the arrivals, and takes the change of the
    // rate at once, with the third snapshot of the new rate.
    {
        ClientClock c;
        int prevSnap = 0, newest = -1, st = 1000, at = 5, n = 0, past = 0, pastSlow = 0, frames = 0;
        for (int now = 0; now < 12000; now += 8) {
            for (; at <= now; ++n) {
                c.snapshot(st, prevSnap, n == 0, at);
                prevSnap = newest = st;
                const int step = now < 8000 ? (n % 2 ? 33 : 17) : 50;
                st += step, at += step;
            }
            if (newest < 0) continue;
            const double t = double(c.time(now, newest)) + c.fraction();
            if (now >= 3000 && now < 8000) ++frames, past += t > newest ? 1 : 0;
            if (now >= 8200) pastSlow += t > newest ? 1 : 0;
        }
        std::printf("even clock, 17 / 33 ms in turn: past the newest snapshot in %d of %d frames; after the change to 50 ms: %d\n", past, frames, pastSlow);
        CHECK(past == 0 && pastSlow == 0);
    }
}

void burstyClockTests() {
    // A steady path: both clocks sit one interval + 5 ms behind the newest snapshot and never stand or leap.
    for (const ClientClock& c : {ClientClock{0, 0}, ClientClock{}}) {
        const ClockRun r = runClock(c, 0, 0, 3000, 12000);
        CHECK(r.frames > 500 && r.holds == 0 && r.leaps == 0 && r.starved == 0);
        CHECK(r.lagMin >= 0 && r.lagMax <= 70);
    }
    // Dark for 220 ms out of every 520 ms (the measured case): the plain 1.3 rule stands and leaps with the bursts and
    // runs past the newest snapshot; the windowed clock settles behind the most delayed snapshots and runs evenly.
    const ClockRun plain = runClock(ClientClock{0, 0}, 520, 220, 4000, 16000);
    std::printf("bursty path, plain 1.3 rule: %d frames, %d held, %d leaps, %d past the newest snapshot, lag %d..%d ms\n", plain.frames,
                plain.holds, plain.leaps, plain.starved, plain.lagMin, plain.lagMax);
    CHECK(plain.holds + plain.leaps > 10);
    CHECK(plain.starved > 50);
    // Without the limit on how far the window puts the clock behind (maxBehindMs), it settles behind the most delayed
    // snapshots and never runs past the newest one. With the default limit of 100 ms the clock stays at most that far
    // behind the window's least delayed snapshot: it runs evenly, and past the newest snapshot only at the ends of the
    // dark stretches (everything drawn from snapshots would be that much later otherwise).
    ClientClock unlimited;
    unlimited.maxBehindMs = 100000;
    const ClockRun full = runClock(unlimited, 520, 220, 4000, 16000);
    std::printf("bursty path, openiw2 clock without the limit: %d frames, %d held, %d leaps, %d past the newest snapshot, lag %d..%d ms\n",
                full.frames, full.holds, full.leaps, full.starved, full.lagMin, full.lagMax);
    CHECK(full.frames > 500 && full.holds == 0 && full.leaps == 0 && full.starved == 0);
    CHECK(full.lagMax <= 220 + 50 + 70);
    const ClockRun ours = runClock(ClientClock{}, 520, 220, 4000, 16000);
    std::printf("bursty path, openiw2 clock: %d frames, %d held, %d leaps, %d past the newest snapshot, lag %d..%d ms\n", ours.frames,
                ours.holds, ours.leaps, ours.starved, ours.lagMin, ours.lagMax);
    CHECK(ours.frames > 500 && ours.holds == 0 && ours.leaps == 0); // past the newest snapshot about as often as the plain rule
    CHECK(ours.lagMax <= 55 + 100 + 16);
    // One dark stretch of 300 ms on a steady path: the plain rule stands still (the half-way step), ours slows down
    // and comes back without a frame that stands or leaps.
    const ClockRun plainOne = runClock(ClientClock{0, 0}, 0, 300, 3000, 12000, 6000);
    CHECK(plainOne.holds > 0);
    const ClockRun oursOne = runClock(ClientClock{}, 0, 300, 3000, 12000, 6000);
    std::printf("one dark stretch: plain %d held / %d leaps, openiw2 %d held / %d leaps, lag at the end of the run within %d..%d\n",
                plainOne.holds, plainOne.leaps, oursOne.holds, oursOne.leaps, oursOne.lagMin, oursOne.lagMax);
    CHECK(oursOne.holds == 0 && oursOne.leaps == 0);
    // Back on the steady path the clock returns to one interval behind (measured over the last 2 s).
    const ClockRun back = runClock(ClientClock{}, 0, 300, 18000, 20000, 6000);
    CHECK(back.lagMin >= 0 && back.lagMax <= 75);
    // A frame of the client that takes a second (bursty path or not): the snapshots it then reads at once are not a
    // delay of the path. Half a second later the clock is where it was before, not a second behind.
    const ClockRun stallSteady = runClock(ClientClock{}, 0, 0, 6500, 9000, -1, 5000);
    CHECK(stallSteady.lagMin >= 0 && stallSteady.lagMax <= 75 && stallSteady.holds == 0 && stallSteady.leaps == 0);
    const ClockRun stallBursty = runClock(ClientClock{}, 520, 220, 8000, 16000, -1, 5000);
    std::printf("a client frame of 1 s on the bursty path: %d held, %d leaps, lag %d..%d ms\n", stallBursty.holds, stallBursty.leaps,
                stallBursty.lagMin, stallBursty.lagMax);
    CHECK(stallBursty.holds == 0 && stallBursty.leaps == 0 && stallBursty.lagMax <= 220 + 50 + 70);
    // Spikes below the 100 ms at which the 1.3 rule steps (85 ms dark out of every 500 ms): the plain rule runs past
    // the newest snapshot with every spike (the other players are held, then moved at once). A stretch over spreadMs
    // (40 ms) that comes again puts the even clock that much further behind, and it stays before the newest snapshot.
    const ClockRun plainSpike = runClock(ClientClock{0, 0}, 500, 85, 4000, 16000);
    const ClockRun oursSpike = runClock(ClientClock{}, 500, 85, 4000, 16000);
    std::printf("spikes of 85 ms: past the newest snapshot in %d frames (plain), %d (openiw2) of %d; openiw2 %d held / %d leaps, lag %d..%d ms\n",
                plainSpike.starved, oursSpike.starved, oursSpike.frames, oursSpike.holds, oursSpike.leaps, oursSpike.lagMin, oursSpike.lagMax);
    CHECK(plainSpike.starved > 20);
    CHECK(oursSpike.starved == 0 && oursSpike.holds == 0 && oursSpike.leaps == 0);
    CHECK(oursSpike.lagMax <= 85 + 50 + 70);
    evenClockTests();
}

// The time delta rule of the 1.3 client (docs/spec/net.md section 10 "Client time").
void timeDeltaTests() {
    int delta = 0, floor = 0;
    bool ex = false;
    // Snapshots 50 ms apart, received 1000 ms after their server time: the target is 1000 + 50 + 5 behind.
    GameClient::adjustTimeDelta(delta, ex, floor, 1050, 1000, 2050);  // target 1050-2050-50-5 = -1055: reset
    CHECK(delta == -1055 && floor == 1050);
    GameClient::adjustTimeDelta(delta, ex, floor, 1100, 1050, 2100);  // on target: unchanged
    CHECK(delta == -1055);
    delta = -1000;  // 55 ahead of the target (clock too far forward): 1 ms per snapshot
    GameClient::adjustTimeDelta(delta, ex, floor, 1150, 1100, 2150);
    CHECK(delta == -1001);
    delta = -1200;  // 145 behind the target: the interval (50) is taken off first, 95 is the slow rule
    GameClient::adjustTimeDelta(delta, ex, floor, 1200, 1150, 2200);
    CHECK(delta == -1199);
    delta = -1300;  // 245 behind: 195 after the interval, over 100: half way (floored)
    GameClient::adjustTimeDelta(delta, ex, floor, 1250, 1200, 2250);
    CHECK(delta == -1178);
    delta = -1055;
    ex = true;  // an extrapolating frame: 2 ms back, the mark cleared
    GameClient::adjustTimeDelta(delta, ex, floor, 1300, 1250, 2300);
    CHECK(delta == -1057 && !ex);
    delta = -300;  // 755 ahead (a stall on the client): reset
    GameClient::adjustTimeDelta(delta, ex, floor, 1350, 1300, 2350);
    CHECK(delta == -1055 && floor == 1350);
    delta = 5000;  // a new map: reset
    GameClient::adjustTimeDelta(delta, ex, floor, 20050, 20000, 21050);
    CHECK(delta == -1055 && floor == 20050);
    // 120 s without a valid snapshot, then one: the clock goes to one interval behind it, not 120 s behind.
    delta = -1055;
    GameClient::adjustTimeDelta(delta, ex, floor, 140050, 20050, 141050);
    CHECK(delta == -1055);                 // one interval + 5 behind the new snapshot (before: -121005)
    CHECK(141050 + delta >= 140050 - 60);  // the command time at that real time
}

// A game whose one other player walks along +x at a constant speed in server time.
struct ConstGame : ServerGame {
    int time = 0;
    static constexpr float kSpeed = 200.0f;  // units per second
    std::map<int, std::string> configstrings() override {
        return {{0, "\\mapname\\mp_stub"}, {1, "\\sv_serverid\\3"}};
    }
    bool clientConnect(int, const std::string&) override { return true; }
    void clientBegin(int) override {}
    void clientCommand(int, const std::string&) override {}
    void clientThink(int, const UserCmd&) override {}
    void clientDisconnect(int) override {}
    std::atomic<int> stallMs{0};  // one frame that takes this long (a round restart rebuilding the world)
    void runFrame(int ms) override {
        if (const int st = stallMs.exchange(0)) std::this_thread::sleep_for(std::chrono::milliseconds(st));
        time += ms;
    }
    int serverTime() const override { return time; }
    SnapshotView snapshot(int slot) override {
        SnapshotView v;
        netgame::PsFields f;
        f.commandTime = time;
        f.pmType = 2;
        f.clientNum = slot;
        netgame::writePs(v.ps, f);
        netgame::RemoteEntity e;
        e.eType = 1;
        e.clientNum = 1;
        e.origin[0] = kSpeed * 0.001f * static_cast<float>(time);
        netgame::writeEntity(v.entities[1], e);
        v.clients[1] = std::vector<uint8_t>(kClientStateSize, 0);
        return v;
    }
};

// Smoothness of the drawn motion (docs/spec/net.md section 10): server and client driven by one injected clock at
// 16 ms render frames; the client's packets are held back now and then (network jitter) and the server is polled
// late now and then (load: it catches up several frames at once). After the clock settled, the entity's drawn
// displacement per frame must stay close to speed * 16 ms and never go backwards.
void smoothnessTest() {
    ConstGame game;
    Server server(game, 3);
    server.maxClients = 2;
    server.serverId = 3;
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    NetAddress addr;
    NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), addr, 28960);
    GameClient c;
    c.snaps = 20; // one snapshot per game frame, as a 1.3 client gets (the game here has one place per frame)
    int64_t now = 5000;
    CHECK(c.connect(addr, now));
    for (int i = 0; i < 400 && c.state() != ClientState::Active; ++i) {
        c.poll(now, 1);
        if (c.state() >= ClientState::Primed) {
            UserCmd u;
            u.serverTime = c.commandTime(now);
            c.sendCommand(u, now);
        }
        server.poll(now, 1);
        now += 16;
    }
    CHECK(c.state() == ClientState::Active);
    const float nominal = ConstGame::kSpeed * 0.016f;
    int samples = 0, good = 0, backwards = 0, big = 0, subMsChecks = 0;
    float lastX = -1;
    int holdClient = 0, holdServer = 0;
    for (int step = 0; step < 1500; ++step) {
        // Jitter: every 9th frame the client's packets wait one frame; load: every 37th frame the server misses 2 polls.
        if (step % 9 == 0) holdClient = 1;
        if (step % 37 == 0) holdServer = 2;
        if (holdClient > 0) --holdClient;
        else c.poll(now, 1);
        UserCmd u;
        u.serverTime = c.commandTime(now);
        c.sendCommand(u, now);
        if (holdServer > 0) --holdServer;
        else server.poll(now, 1);
        const ClientSnapshot *s0 = nullptr, *s1 = nullptr;
        float frac = 0;
        if (c.interpolation(c.renderTime(now), s0, s1, frac)) {
            // A time between two milliseconds gives the fraction between theirs (docs/spec/net.md section 10 "Client
            // time": the drawn frame's sub-millisecond time).
            if (s0 != s1 && c.renderTime(now) + 1 < s1->serverTime) {
                const ClientSnapshot *h0 = nullptr, *h1 = nullptr;
                float half = 0;
                CHECK(c.interpolation(double(c.renderTime(now)) + 0.5, h0, h1, half));
                CHECK(h0 == s0 && h1 == s1);
                CHECK(std::fabs(half - (frac + 0.5f / float(s1->serverTime - s0->serverTime))) < 1e-4f);
                ++subMsChecks;
            }
            const auto ents = netgame::interpolateEntities(*s0, *s1, frac);
            if (!ents.empty() && step > 200) {  // after the clock settled
                const float x = ents[0].origin[0];
                if (lastX >= 0) {
                    const float d = x - lastX;
                    ++samples;
                    if (d < -0.01f) ++backwards;
                    if (d > 2.5f * nominal) ++big;
                    if (d >= 0.5f * nominal && d <= 1.5f * nominal) ++good;
                }
                lastX = x;
            }
        }
        now += 16;
    }
    std::cout << "smoothness: " << samples << " frames, " << good << " within 50% of " << nominal << " units, " << backwards
              << " backwards, " << big << " over 2.5x, time delta " << c.timeDelta() << "\n";
    CHECK(samples > 1000);
    CHECK(subMsChecks > 500);
    CHECK(backwards == 0);
    CHECK(big <= samples / 100);  // a held frame (the clock ran into a late snapshot) is caught up at once
    CHECK(good >= samples * 9 / 10);
    // pastNewestTest (docs/spec/net.md section 10 "Past the newest snapshot"): no snapshot arrives any more; the time
    // past the newest one grows with the clock up to 30 ms, and the players move on along their velocity that long.
    {
        const ClientSnapshot* newest = c.latest();
        CHECK(newest != nullptr);
        float last = 0;
        bool grew = false, capped = false;
        for (int i = 0; i < 20 && newest; ++i) {
            now += 8;
            const double t = double(c.renderTime(now)) + c.timeFraction();
            const float past = c.pastNewestMs(t);
            CHECK(past >= last && past <= float(GameClient::kExtrapolateMs));
            if (t <= double(newest->serverTime)) CHECK(past == 0);
            else CHECK(std::fabs(past - float(std::min(t - double(newest->serverTime), 30.0))) < 1e-3f);
            grew = grew || (past > 0 && past < 30);
            capped = capped || (past == 30 && t > double(newest->serverTime) + 40);
            last = past;
        }
        CHECK(grew && capped);
        std::vector<game::World::RemotePlayer> players(2);
        players[0].origin = {10, 20, 30};
        players[0].velocity = {200, -100, 0};
        players[1] = players[0];
        players[1].dead = true;
        netgame::RemoteView::extrapolate(players, 30.0f);
        CHECK(std::fabs(players[0].origin[0] - 16.0f) < 1e-3f && std::fabs(players[0].origin[1] - 17.0f) < 1e-3f && players[0].origin[2] == 30.0f);
        CHECK(players[1].origin[0] == 10.0f);
    }
    c.disconnect();
}

// A remote client (IW2_NET_WAN=1: the loopback peer counts as remote, so the rate and snaps limits apply) with the
// default snaps 20 and small snapshots: one snapshot per 50 ms server frame, whatever the real time of the server's
// polls (docs/spec/net.md section 4a "Cadence": the delay counts in server time).
void remoteCadenceTest() {
    ConstGame game;
    Server server(game, 3);
    server.maxClients = 2;
    server.serverId = 3;
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    NetAddress addr;
    NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), addr, 28960);
    GameClient c;
    c.snaps = 20; // as a 1.3 client asks
    int64_t now = 5000;
    CHECK(c.connect(addr, now));
    for (int i = 0; i < 2000 && c.state() != ClientState::Active; ++i) {
        c.poll(now, 1);
        if (c.state() >= ClientState::Primed) {
            UserCmd u;
            u.serverTime = c.commandTime(now);
            c.sendCommand(u, now);
        }
        server.poll(now, 1);
        now += 16;
    }
    CHECK(c.state() == ClientState::Active);
    setenv("IW2_NET_WAN", "1", 1);  // from here on (the handshake of a remote client is not what this checks)
    int snaps = 0, longGaps = 0, lastTime = -1, lastMsg = -1;
    const int startTime = game.serverTime();
    while (game.serverTime() - startTime < 3000) {
        c.poll(now, 1);
        UserCmd u;
        u.serverTime = c.commandTime(now);
        c.sendCommand(u, now);
        server.poll(now, 1);
        if (const ClientSnapshot* s = c.latest(); s && s->messageNum != lastMsg) {
            lastMsg = s->messageNum;
            if (lastTime >= 0) {
                ++snaps;
                if (s->serverTime - lastTime > 50) ++longGaps;
            }
            lastTime = s->serverTime;
        }
        now += 7;  // the polls fall at other real times than the frames
    }
    std::cout << "remote cadence: " << snaps << " snapshots in 3 s of server time, " << longGaps << " more than one frame apart\n";
    CHECK(snaps >= 58);
    CHECK(longGaps == 0);
    c.disconnect();
    unsetenv("IW2_NET_WAN");
}

} // namespace

int main() {
    unitTests();
    timeDeltaTests();
    burstyClockTests();
    smoothnessTest();
    remoteCadenceTest();

    // Connecting to a port nobody answers gives up after the timeout.
    {
        GameClient dead;
        dead.connectTimeoutMs = 300;
        NetAddress nowhere;
        NetAddress::parse("127.0.0.1:9", nowhere, 28960);
        CHECK(dead.connect(nowhere, nowMs()));
        CHECK(dead.state() == ClientState::Challenging);
        const int64_t end = nowMs() + 1000;
        while (nowMs() < end && dead.state() != ClientState::Disconnected) dead.poll(nowMs(), 10);
        CHECK(dead.state() == ClientState::Disconnected);
        CHECK(dead.error == "connection timed out");
    }

    MoveGame game;
    Server server(game, 3);
    server.maxClients = 4;
    server.serverId = 12;
    server.checksumFeed = 0x2345;
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        return 1;
    }
    std::atomic<bool> stop{false};
    std::mutex serverMutex;
    std::thread th([&] {
        while (!stop) {
            {
                std::lock_guard<std::mutex> l(serverMutex);
                server.poll(nowMs(), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    NetAddress addr;
    NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), addr, 28960);
    GameClient a, b;
    b.snaps = 20; // b checks the per-frame cadence; a gets the snapshots between the frames (docs/spec/net.md section 10 "Snapshot rate")
    a.name = "alpha";
    b.name = "bravo";
    CHECK(a.connect(addr, nowMs()));
    CHECK(b.connect(addr, nowMs()));
    bool sawConnecting = false, sawPrimed = false;
    int64_t end = nowMs() + 15000;  // a deadline only; the loop ends as soon as both are active
    while (nowMs() < end && (a.state() != ClientState::Active || b.state() != ClientState::Active)) {
        a.poll(nowMs(), 5);
        b.poll(nowMs(), 0);
        sawConnecting |= a.state() == ClientState::Connecting;
        sawPrimed |= a.state() == ClientState::Primed;
    }
    std::cout << "a " << clientStateName(a.state()) << " " << a.error << " / b " << clientStateName(b.state()) << " " << b.error << "\n";
    CHECK(sawConnecting);
    CHECK(sawPrimed);
    CHECK(a.state() == ClientState::Active);
    CHECK(b.state() == ClientState::Active);
    CHECK(a.serverId == 12);
    CHECK(a.clientNum == 0 && b.clientNum == 1);
    CHECK(a.configstrings[netgame::kMenuConfigstrings] == "team_britishgerman");

    // Configstring updates and other server commands; reliable client commands.
    {
        std::lock_guard<std::mutex> l(serverMutex);
        server.sendServerCommand(0, "d 5 hello world");
        server.sendServerCommand(0, "x 20 abc");
        server.sendServerCommand(0, "y 20 def");
        server.sendServerCommand(0, "z 20 ghi");
        server.sendServerCommand(0, "t 0");
    }
    a.addReliableCommand("say hi there");
    a.addReliableCommand("mr 12 0 autoassign");

    // Movement: alpha runs forward for a while; every snapshot's server origin must match what alpha predicted for
    // the same command time.
    std::map<int, game::PlayerState> predicted;
    int compared = 0;
    float worst = 0;
    std::vector<std::string> received;
    end = nowMs() + 1500;
    const int64_t hardEnd = nowMs() + 15000;  // under load: keep going until enough snapshots were compared
    int64_t lastSend = 0;
    std::vector<int> snapTimes;
    while (nowMs() < hardEnd && (nowMs() < end || compared <= 10 || b.snapshots <= 10)) {
        const int64_t now = nowMs();
        a.poll(now, 1);
        b.poll(now, 0);
        for (auto& s : a.takeServerCommands()) received.push_back(s);
        if (now - lastSend >= 16) {
            lastSend = now;
            UserCmd c;
            c.serverTime = a.commandTime(now);
            c.forwardmove = 127;
            c.angles[1] = 8192;  // 45 degrees
            a.sendCommand(c, now);
            const ClientSnapshot* s = a.latest();
            if (s) {
                const netgame::PsFields f = netgame::readPs(s->ps);
                game::PlayerState ps = netgame::predictPlayer(game.world, f, a.commandsAfter(f.commandTime));
                predicted[a.lastCommand()->serverTime] = ps;
                auto it = predicted.find(f.commandTime);
                if (it != predicted.end()) {
                    float d = 0;
                    for (int i = 0; i < 3; ++i) d = std::max(d, std::fabs(it->second.origin[size_t(i)] - f.origin[i]));
                    worst = std::max(worst, d);
                    ++compared;
                }
            }
            UserCmd idle;
            idle.serverTime = b.commandTime(now);
            b.sendCommand(idle, now);
        }
        if (const ClientSnapshot* sb = b.latest(); sb && (snapTimes.empty() || snapTimes.back() != sb->serverTime))
            snapTimes.push_back(sb->serverTime);
    }
    std::cout << "predictions compared " << compared << ", worst origin error " << worst << "\n";
    // Server cadence (docs/spec/net.md section 10): one snapshot per game frame, their times spaced by whole frames.
    {
        int bad = 0;
        for (size_t i = 1; i < snapTimes.size(); ++i)
            if (snapTimes[i] - snapTimes[i - 1] <= 0 || (snapTimes[i] - snapTimes[i - 1]) % server.frameMs != 0) ++bad;
        std::cout << "snapshot times " << snapTimes.size() << ", off the frame grid " << bad << "\n";
        CHECK(snapTimes.size() > 10 && bad == 0);
    }
    CHECK(compared > 10);
    CHECK(worst < 0.01f);
    CHECK(a.configstrings[5] == "hello world");
    CHECK(a.configstrings[20] == "abcdefghi");
    CHECK(std::find(received.begin(), received.end(), "t 0") != received.end());
    CHECK(a.reliableAcknowledged() == a.reliableSequence());
    {
        std::lock_guard<std::mutex> l(game.m);
        CHECK(std::find(game.commands.begin(), game.commands.end(), "say hi there") != game.commands.end());
        CHECK(std::find(game.commands.begin(), game.commands.end(), "mr 12 0 autoassign") != game.commands.end());
    }

    // Bravo sees alpha as an entity and interpolates it between snapshots.
    {
        const ClientSnapshot *s0 = nullptr, *s1 = nullptr;
        float frac = 0;
        CHECK(b.interpolation(b.renderTime(nowMs()), s0, s1, frac));
        CHECK(s0 && s1 && frac >= 0 && frac <= 1);
        if (s0 && s1) {
            const auto ents = netgame::interpolateEntities(*s0, *s1, frac);
            CHECK(ents.size() == 1 && ents[0].clientNum == 0);
            CHECK(s1->clients.count(0) && netgame::clientStateName(s1->clients.at(0)) == "bob");
            CHECK(a.latest() != nullptr);
            const netgame::PsFields fa = a.latest() ? netgame::readPs(a.latest()->ps) : netgame::PsFields{};
            // Alpha moved along +x/+y (45 degrees) from x = 0.
            if (!ents.empty()) CHECK(ents[0].origin[0] > 3 && ents[0].origin[1] > 3);
            std::cout << "alpha " << fa.origin[0] << " " << fa.origin[1] << " " << fa.origin[2] << "\n";
            CHECK(fa.origin[0] > 3);
        }
        CHECK(b.renderTime(nowMs()) == b.commandTime(nowMs()));  // commands carry the drawn time (antilag)
        CHECK(b.snapshots > 10 && b.deltaSnapshots > 0);
    }

    // A modified client replaying old command times (each packet: a command a second old, then the current one) gets
    // no extra movement: without a jump of the server's time the 1.3 rule holds and the old commands are skipped
    // (docs/spec/net.md section 10 "Client time").
    {
        auto serverX = [&] {
            std::lock_guard<std::mutex> l(game.m);
            return game.p[0].ps.origin[0];
        };
        auto drive = [&](int ms, bool replay) {
            for (const int64_t until = nowMs() + ms; nowMs() < until;) {
                const int64_t now = nowMs();
                a.poll(now, 1);
                b.poll(now, 0);
                UserCmd c;
                c.serverTime = a.commandTime(now);
                c.forwardmove = 127;
                if (replay) {
                    UserCmd old = c;
                    old.serverTime -= 1000;
                    a.sendRawCommandsForTest({old, c}, now);
                } else {
                    a.sendCommand(c, now);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        };
        drive(3200, false);  // past the window of an earlier jump of the server's time
        float x0 = serverX();
        drive(600, false);
        const float normal = serverX() - x0;
        x0 = serverX();
        drive(600, true);
        const float replayed = serverX() - x0;
        drive(300, false);
        std::cout << "replayed old commands: moved " << replayed << " (" << normal << " normally)\n";
        CHECK(normal > 10 && replayed < 1.1f * normal);
    }

    // A server frame of 1.5 s (a round restart rebuilding the world): alpha's clock runs ahead, and the snapshot after
    // the frame (its time 1.5 s behind) resets the clock back to it (docs/spec/net.md section 10 "Client time"). Its commands follow the clock again:
    // none stays ahead of it by more than a snapshot interval once the server is back (they used to go on 1 ms apart
    // from the last command sent during the stall).
    {
        auto send = [&](int64_t now) {
            UserCmd c;
            c.serverTime = a.commandTime(now);
            c.forwardmove = 127;
            a.sendCommand(c, now);
        };
        game.stallMs = 1500;
        for (const int64_t until = nowMs() + 1500; nowMs() < until;) {
            a.poll(nowMs(), 1);
            send(nowMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        int worstAhead = 0;
        for (const int64_t until = nowMs() + 1500; nowMs() < until;) {
            const int64_t now = nowMs();
            a.poll(now, 1);
            b.poll(now, 0);
            send(now);
            worstAhead = std::max(worstAhead, a.lastCommand()->serverTime - a.commandTime(now));
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::cout << "after a stall: command time ahead of the clock by " << worstAhead << " ms at most\n";
        CHECK(worstAhead <= 60);
    }

    // The server's time falls a second behind (20 frames without time): alpha's clock runs on meanwhile, so the next
    // snapshots reset it back by a second (docs/spec/net.md section 10 "Client time") and its next commands are
    // older than the last one the server ran. They run: the server keeps moving alpha instead of skipping its
    // commands until the clock is back at the old ones.
    {
        auto drive = [&](int ms) {
            for (const int64_t until = nowMs() + ms; nowMs() < until;) {
                const int64_t now = nowMs();
                a.poll(now, 1);
                b.poll(now, 0);
                UserCmd c;
                c.serverTime = a.commandTime(now);
                c.forwardmove = 127;
                a.sendCommand(c, now);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        };
        auto serverX = [&] {
            std::lock_guard<std::mutex> l(game.m);
            return game.p[0].ps.origin[0];
        };
        drive(300);
        const float xa = serverX();
        drive(500);
        const float normal = serverX() - xa;
        const int before = a.commandTime(nowMs());
        game.holdFrames = 20;
        drive(1300);
        const int clockGain = a.commandTime(nowMs()) - before;  // about 1300 - 1000 after the reset
        const float x0 = serverX();
        drive(500);
        const float moved = serverX() - x0;
        std::cout << "server time a second behind: clock moved " << clockGain << " ms in 1300, alpha moved " << moved
                  << " in the next half second (" << normal << " before)\n";
        CHECK(clockGain < 800);
        CHECK(normal > 10 && moved > 0.8f * normal);
    }

    // Map rotations (docs/spec/net.md, "Map change"), three in a row: alpha keeps playing and sends a client command of
    // the old map at the moment of the change, bravo is idle (a spectator / dead player sends no movement) and stops
    // polling for a while after the new gamestate (a level load). Each client gets the new gamestate exactly once (no
    // resend while it is in flight) and goes back to active with the new server id.
    for (int r = 0; r < 3; ++r) {
        const int newSid = 30 + r;
        const int gsA = a.gamestates, gsB = b.gamestates;
        a.addReliableCommand("say before " + std::to_string(r));
        {
            std::lock_guard<std::mutex> l(serverMutex);
            game.sid = newSid;
            server.newMap(newSid);
        }
        bool bLoaded = false;
        int64_t lastSend = 0;
        for (end = nowMs() + 10000; nowMs() < end;) {
            const int64_t now = nowMs();
            a.poll(now, 1);
            b.poll(now, 0);
            if (b.gamestates > gsB && !bLoaded) {
                bLoaded = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            if (now - lastSend >= 16) {
                lastSend = now;
                UserCmd c;
                c.serverTime = a.commandTime(now);
                c.forwardmove = 64;
                a.sendCommand(c, now);
            }
            bool active;
            {
                std::lock_guard<std::mutex> l(serverMutex);
                active = server.client(0).state == ServerClientState::Active &&
                         server.client(1).state == ServerClientState::Active;
            }
            if (active && a.state() == ClientState::Active && b.state() == ClientState::Active && a.serverId == newSid &&
                b.serverId == newSid && now - lastSend < 16)
                break;
        }
        // Let any stray resend arrive before counting.
        for (int64_t settle = nowMs() + 300; nowMs() < settle;) {
            a.poll(nowMs(), 2);
            b.poll(nowMs(), 0);
        }
        std::cout << "rotation " << r << ": gamestates a " << a.gamestates - gsA << " b " << b.gamestates - gsB << "\n";
        CHECK(a.gamestates == gsA + 1);
        CHECK(b.gamestates == gsB + 1);
        CHECK(a.serverId == newSid && b.serverId == newSid);
        CHECK(a.state() == ClientState::Active && b.state() == ClientState::Active);
        std::lock_guard<std::mutex> l(serverMutex);
        CHECK(server.client(0).state == ServerClientState::Active && server.client(1).state == ServerClientState::Active);
    }
    {
        // The command sent across the change still arrives (the client sends it again under the new server id).
        std::lock_guard<std::mutex> l(game.m);
        CHECK(std::find(game.commands.begin(), game.commands.end(), "say before 2") != game.commands.end());
    }

    // Disconnect frees the slot on the server.
    a.disconnect();
    CHECK(a.state() == ClientState::Disconnected);
    bool freed = false;
    for (end = nowMs() + 10000; nowMs() < end && !freed;) {
        {
            std::lock_guard<std::mutex> l(serverMutex);
            freed = server.client(0).state == ServerClientState::Free;
        }
        if (!freed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(freed);
    b.disconnect();
    stop = true;
    th.join();
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "net_gameclient: ok\n";
    return failures ? 1 : 0;
}
