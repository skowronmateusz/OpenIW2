// The network client's clock (docs/spec/net.md section 10, "Client time" and "Client time: the even clock"): the
// client's estimate of the server time, real time + a time delta that every received snapshot adjusts.
#pragma once

#include <cstdint>
#include <deque>

namespace iw2::net {

class ClientClock {
public:
    // The time delta rule of the 1.3 client for one received snapshot: the target delta puts the client time one
    // snapshot interval + 5 ms behind the newest snapshot; a difference over 500 ms resets (returns true), over
    // 100 ms averages, otherwise 1 ms per snapshot (2 ms back when extrapolating).
    static bool adjust(int& delta, bool& extrapolated, int& floor, int snapTime, int oldSnapTime, int64_t nowMs);

    // openiw2, the even clock (windowMs 0 gives the plain 1.3 rule above; IW2_CLOCK_PLAIN=1 does so in the game
    // client): the delta follows, at a bounded rate and in fractions of a millisecond, the value that puts the clock
    // one snapshot interval + safetyMs + the path's usual delay spread behind the least delayed snapshot of the last
    // windowMs of real time.
    int windowMs = 1500;
    // A snapshot this much later than the window's least delayed one is a late stretch, not jitter: it counts only on
    // a path that keeps having them (two stretches 200 ms or more apart in the window). Above a frame's read delay.
    int spreadMs = 40;
    // The late stretches never put the clock more than this far behind the window's least delayed snapshot: every
    // drawn thing the client does not predict (other players, hits, deaths, a turret) is that much later as well.
    int maxBehindMs = 100;
    int safetyMs = 3;
    int coverPercent = 90;                   // the share of the window's snapshots whose delay the clock stays behind
    double slowRate = 0.10, fastRate = 0.05; // the clock runs at most this much slower / faster than real time
    int settleMs = 250;                      // a difference to the target is taken at difference / settleMs per ms

    // A gamestate: no server time yet.
    void restart() {
        ClientClock fresh = *this;
        fresh.arrivals_.clear();
        fresh.delta_ = 0, fresh.floor_ = 0, fresh.extrapolated_ = false;
        fresh.evenDelta_ = fresh.target_ = fresh.time_ = 0;
        fresh.movedAt_ = -1, fresh.cover_ = 0, fresh.interval_ = 0, fresh.stepInterval_ = 0;
        *this = fresh;
    }
    ClientClock() = default;
    // window 0: the plain 1.3 rule. (The second value was the slew of an earlier clock and is not used.)
    ClientClock(int window, int) : windowMs(window) {}

    // The first snapshot of a level sets the delta; every later one adjusts it. Returns true when the clock was set
    // (first snapshot or a reset): the floor is then the snapshot's time and may lie below the last time given out.
    // nowMs is the time the snapshot reached this host. lateRead: only a read time is known and the client read the
    // socket late (a long frame), so it says nothing about the path.
    bool snapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead = false);
    // The client time now: never below the last value given. newestSnapTime marks the extrapolation (pass a negative
    // value when there is no snapshot).
    int time(int64_t nowMs, int newestSnapTime);
    // The part of that time below the millisecond, for drawing (0 with the plain rule).
    double fraction() const { return windowMs > 0 ? time_ - static_cast<double>(floor_) : 0.0; }

    int delta() const { return windowMs > 0 ? static_cast<int>(evenDelta_) : delta_; }
    int floor() const { return floor_; }
    bool extrapolating() const { return extrapolated_; }
    int coverMs() const { return cover_; }
    int intervalMs() const { return interval_; } // the snapshot interval the even clock runs behind by (0: none yet) // what the window's delays add to one snapshot interval + safetyMs

private:
    bool evenSnapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead);
    int evenTime(int64_t nowMs, int newestSnapTime);

    int delta_ = 0;             // the plain rule's time delta
    int floor_ = 0;             // the last client time given out
    bool extrapolated_ = false; // the client time reached the newest snapshot (plain rule: consumed by the next adjust)
    struct Arrival {
        int64_t atMs;  // arrival time
        int offset;    // snapshot time - arrival time
        int interval;  // snapshot time - the previous snapshot's time (0: not known)
    };
    std::deque<Arrival> arrivals_; // the snapshots of the last windowMs
    double evenDelta_ = 0, target_ = 0, time_ = 0;
    int64_t movedAt_ = -1;         // real time the delta was last moved at
    int cover_ = 0;
    int interval_ = 0;
    int stepInterval_ = 0; // the interval the delta was last stepped for (a change of the snapshot rate)
};

} // namespace iw2::net
