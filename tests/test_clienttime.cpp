// The client's per-frame clock (docs/spec/net.md section 10 "Frame clock"), and one check per class of effect read on it. The
// frame loop below repeats the accumulate / tick / fraction steps of the viewer's main loop with the same helpers.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

#include "game/clienttime.h"
#include "game/compassspring.h"
#include "game/fog.h"
#include "game/grenadeindicator.h"
#include "game/hudfade.h"
#include "game/screen_blur.h"
#include "game/viewweapon.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"

using namespace iw2;
using namespace iw2::game;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

struct Loop {
    int64_t world = 1000;
    float gameClock = 0, lerpFrac = 0;
    int64_t clock() const { return clientRenderTimeMs(world, lerpFrac); }
    void frame(float dt) {
        gameClock += dt;
        while (gameClock >= kServerFrameSeconds) {
            world += kServerFrameMs;
            gameClock -= kServerFrameSeconds;
        }
        lerpFrac = serverFrameLerp(gameClock);
    }
};

void clockRules() {
    CHECK(clientRenderTimeMs(1500, 1.0f) == 1500);
    CHECK(clientRenderTimeMs(1500, 0.0f) == 1500 - kServerFrameMs);
    CHECK(clientRenderTimeMs(0, 0.0f) == 0 && clientRenderTimeMs(10, 0.5f) == 0); // never negative at the level start
    // Monotonic over every frame length, from far above the tick rate to several ticks per frame, and within one frame of the world.
    for (float dt : {0.0003f, 0.0041f, 0.0083f, 0.0167f, 0.0333f, 0.05f, 0.051f, 0.12f, 0.31f}) {
        Loop l;
        int64_t last = l.clock();
        for (int i = 0; i < 20000; ++i) {
            // The camera's read before the tick loop is the pair of the end of the last frame.
            CHECK(l.clock() == last);
            l.frame(dt * (1.0f + 0.3f * std::sin(float(i))));
            const int64_t now = l.clock();
            CHECK(now >= last);
            CHECK(now <= l.world && now >= l.world - kServerFrameMs);
            last = now;
        }
    }
    // Between two ticks it moves, which is the point: the frames of one server frame see different times.
    Loop l;
    l.frame(0.051f);
    const int64_t a = l.clock();
    l.frame(0.008f);
    CHECK(l.clock() > a);
    CHECK(elapsedMs(1000, 1030) == 0 && elapsedMs(1030, 1000) == 30);
}

// The landing dip stamped and read on the clock: never cancelled by a tick, and it moves on frames without a tick.
void impulses() {
    Loop l;
    ViewImpulses v;
    l.frame(0.01f);
    v.land(38.0f, 0, l.clock(), 200.0f, 350.0f);
    float last = v.landOffset(l.clock());
    int moved = 0;
    for (int i = 0; i < 12; ++i) { // 96 ms, inside the 150 ms way down
        l.frame(0.008f);
        const float o = v.landOffset(l.clock());
        CHECK(o <= last);
        moved += o < last;
        last = o;
    }
    CHECK(last < -3.0f);
    CHECK(moved >= 10);
    // Damage kick: on the clock it starts at 0 and grows; a stamp ahead of the clock (world time) would flip it.
    const V3 fwd{1, 0, 0}, left{0, 1, 0};
    ViewImpulses d;
    d.damage(0xff, 0xff, 50, fwd, left, 1000);
    CHECK(d.damageKick(1000, 0, false)[0] == 0.0f);
    CHECK(d.damageKick(1008, 0, false)[0] < 0.0f); // a world hit kicks the pitch back
    CHECK(d.damageKick(970, 0, false)[0] > 0.0f);  // negative control
}

// A spring fed per rendered frame moves on every frame; fed the 50 ms time it stands still between ticks.
void spring() {
    Loop l;
    CompassSpring perFrame, perTick;
    l.frame(0.01f);
    (void)perFrame.update(int(l.clock()), 0.0f);
    (void)perTick.update(int(l.world), 0.0f);
    float a = 0, b = 0;
    int frameMoves = 0, tickMoves = 0;
    for (int i = 0; i < 24; ++i) {
        l.frame(0.008f);
        const float na = perFrame.update(int(l.clock()), 90.0f), nb = perTick.update(int(l.world), 90.0f);
        frameMoves += na != a;
        tickMoves += nb != b;
        a = na;
        b = nb;
    }
    CHECK(frameMoves >= 20);
    CHECK(tickMoves <= 5); // negative control: 192 ms hold about four ticks
}

// Blur and fog commanded on the clock; the world time would stand ahead of the reads.
void transitions() {
    ScreenBlur onClock;
    onClock.command(100, 2.0f, false, 0, 1000, 1.0f);
    onClock.update(1000, 0);
    CHECK(onClock.cur == 1.0f);
    onClock.update(1050, 0);
    CHECK(std::fabs(onClock.cur - 1.5f) < 1e-4f);
    ScreenBlur ahead; // negative control: read 30 ms before a world-time stamp it overshoots below its start
    ahead.command(100, 2.0f, false, 0, 1030, 1.0f);
    ahead.update(1000, 0);
    CHECK(ahead.cur < 1.0f);

    FogState fog;
    fog.command(100, 1000, 0, 0, 0, 0, 1000); // the first fog is taken at once
    CHECK(fog.consts(1000).active);
    FogState early; // negative control: an immediate fog stamped ahead of the read is off for those frames
    early.command(100, 1000, 0, 0, 0, 0, 1030);
    CHECK(!early.consts(1000).active);
    // A transition moves between ticks.
    fog.command(200, 1000, 0, 0, 0, 1000, 2000);
    CHECK(fog.current(2010).nearDist < fog.current(2020).nearDist);
}

void hud() {
    // A fade read on the clock changes inside one server frame.
    CHECK(hudFadeAlpha(1000, 1920, 1000, 100) > hudFadeAlpha(1000, 1928, 1000, 100));
    // Stamped on the clock, a fade has its full length at the first read; time-only effects change per frame too.
    CHECK(hudFadeAlpha(1000, 1000, 1000, 100) == 1.0f);
    GrenadeIndicatorCfg g{};
    g.pulseMin = 0.2f;
    g.pulseMax = 1.0f;
    g.pulseFreq = 1.0f;
    CHECK(grenadeIndicatorAlpha(g, 1100) != grenadeIndicatorAlpha(g, 1108));
}

// A shell shock's time since its start on a network client: the world ticks and the snapshots arrive out of step. The server
// time moved by the world time minus the newest snapshot (the old rule) saws; through the held offset it runs with the clock.
void rebasing() {
    const int64_t shockServerStart = 5000;
    Loop l;
    std::optional<int64_t> held;
    int64_t snapshot = 5000, lastGood = -1, lastOld = -1;
    int backwardsOld = 0;
    float real = 0;
    for (int i = 0; i < 400; ++i) {
        l.frame(0.008f);
        real += 8.0f;
        while (5000.0f + real - 20.0f >= float(snapshot + kServerFrameMs)) snapshot += kServerFrameMs; // 20 ms out of step with the ticks
        const int64_t renderTime = 5000 + int64_t(real) - 55;
        const int64_t offset = heldClockOffset(held, l.clock() - renderTime);
        const int64_t good = l.clock() - (shockServerStart + offset);
        if (i) CHECK(good >= lastGood);
        lastGood = good;
        const int64_t old = l.clock() - (shockServerStart + (l.world - snapshot));
        if (i && old < lastOld) ++backwardsOld;
        lastOld = old;
    }
    CHECK(backwardsOld > 10); // negative control
    std::optional<int64_t> h;
    CHECK(heldClockOffset(h, 100) == 100 && heldClockOffset(h, 110) == 100 && heldClockOffset(h, 90) == 100);
    CHECK(heldClockOffset(h, 200) == 200);
}

// The re-based path search covers the time between the two snapshots, whatever the server's frame length.
void trajectorySpan() {
    using netgame::kPosTrajectory;
    using netgame::RemoteView;
    netgame::Trajectory pa;
    pa.type = 2;
    pa.time = 0;
    pa.delta[0] = 1000.0f; // 1 unit per ms
    netgame::Trajectory pb;
    pb.type = 0;
    pb.time = 1000;
    pb.base[0] = 900.0f; // stopped 100 ms before the newer snapshot of a 10 Hz server
    std::vector<uint8_t> ea(256, 0), eb(256, 0);
    netgame::writeTrajectory(ea, kPosTrajectory, pa);
    netgame::writeTrajectory(eb, kPosTrajectory, pb);
    float o[3];
    RemoteView::trajectoryAt(ea, &eb, 920, o, 100);
    CHECK(std::fabs(o[0] - 900.0f) < 0.01f); // holds at the base
    RemoteView::trajectoryAt(ea, &eb, 880, o, 100);
    CHECK(std::fabs(o[0] - 880.0f) < 0.01f);
    RemoteView::trajectoryAt(ea, &eb, 920, o, 50);
    CHECK(o[0] > 905.0f); // negative control: a 50 ms search on these snapshots runs past the base
}
} // namespace

int main() {
    clockRules();
    impulses();
    spring();
    transitions();
    hud();
    rebasing();
    trajectorySpan();
    if (failures == 0) std::cout << "clienttime ok\n";
    return failures ? 1 : 0;
}
