// The network client's clock (docs/spec/net.md section 10, "Client time" and "Client time on a bursty path"): the
// client's estimate of the server time, real time + a time delta that every received snapshot adjusts.
#pragma once

#include <cstdint>

#include "net/delaywindow.h"

namespace iw2::net {

class ClientClock {
public:
    // The time delta rule of the 1.3 client for one received snapshot: the target delta puts the client time one
    // snapshot interval + 5 ms behind the newest snapshot; a difference over 500 ms resets (returns true), over
    // 100 ms averages, otherwise 1 ms per snapshot (2 ms back when extrapolating).
    static bool adjust(int& delta, bool& extrapolated, int& floor, int snapTime, int oldSnapTime, int64_t nowMs);

    // openiw2 (both can be switched off for the plain 1.3 rule; IW2_CLOCK_PLAIN=1 does so in the game client):
    // - the rule measures against the most delayed snapshot of the last windowMs of real time, so that a path which
    //   delivers the snapshots in bursts again and again does not pull the clock back and forth with every burst and
    //   the clock stays before the newest snapshot (only while the window holds two stretches of snapshots over
    //   spreadMs late: otherwise the plain rule);
    // - a step of the delta other than a reset is spread over time: the clock runs up to 1 / slewDivisor slower or
    //   faster until the step is made, instead of standing still or leaping.
    int windowMs = 1500;
    int spreadMs = 40; // a snapshot this much later than the window's least delayed one is late (above a frame's read delay)
    int slewDivisor = 4;
    // The window never puts the clock more than this far behind the window's least delayed snapshot: every drawn
    // thing the client does not predict (other players, hits, deaths, a turret) is that much later as well.
    int maxBehindMs = 100;

    // A gamestate: no server time yet.
    void restart() {
        const int spread = spreadMs, behind = maxBehindMs;
        *this = ClientClock{windowMs, slewDivisor};
        spreadMs = spread;
        maxBehindMs = behind;
    }
    ClientClock() = default;
    ClientClock(int window, int slew) : windowMs(window), slewDivisor(slew) {}

    // The first snapshot of a level sets the delta; every later one adjusts it. Returns true when the clock was set
    // (first snapshot or a reset): the floor is then the snapshot's time and may lie below the last time given out.
    // lateRead: the client read the socket late (a long frame), so the receive time is not the arrival time.
    bool snapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead = false);
    // The client time now: never below the last value given. newestSnapTime marks the extrapolation (pass a negative
    // value when there is no snapshot).
    int time(int64_t nowMs, int newestSnapTime);

    int delta() const { return delta_; }
    int floor() const { return floor_; }
    bool extrapolating() const { return extrapolated_; }

private:
    int delta_ = 0;             // the rule's time delta
    int pending_ = 0;           // the part of delta_ the clock has not taken yet (slew)
    int64_t slewAt_ = 0;        // real time the slew was last advanced at
    int slewRest_ = 0;          // real ms not yet turned into slew
    int floor_ = 0;             // the last client time given out
    bool extrapolated_ = false; // the client time reached the newest snapshot (consumed by the next adjust)
    DelayWindow window_;        // snapshot time - receive time of the last windowMs
};

} // namespace iw2::net
