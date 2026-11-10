// docs/spec/net.md section 10, "Client time" and "Client time on a bursty path".
#include "net/clientclock.h"

#include <algorithm>
#include <cstdint>

namespace iw2::net {

bool ClientClock::adjust(int& delta, bool& extrapolated, int& floor, int snapTime, int oldSnapTime, int64_t nowMs) {
    const int period = snapTime - oldSnapTime;
    // openiw2: an interval over 500 ms (no valid snapshot for that long: lost or undecodable ones) counts as one
    // snapshot interval of 50 ms; taken in full, the target fell back by the whole gap and the commands went out that
    // far behind the level time. TODO(spec): whether the 1.3 parser's previous serverTime also advances on snapshots
    // it does not keep.
    const int interval = static_cast<unsigned>(period) <= 500 ? period : 50;
    const int target = snapTime - static_cast<int>(nowMs) - interval - 5;
    int diff = target - delta;
    bool thresholds = true;
    if (diff > 0) {
        // Behind the target: the snapshot interval itself is not a drift (a late snapshot moves the target by it).
        if (static_cast<unsigned>(period) <= 500) {
            diff -= period;
            if (diff < 0) thresholds = false;
        }
    } else {
        diff = -diff;
    }
    if (thresholds && diff > 500) {  // a new map, a stall: start again from this snapshot
        delta = target;
        floor = snapTime;
        return true;
    }
    if (thresholds && diff > 100) {  // far off: half way
        delta = (delta + target) >> 1;
        return false;
    }
    if (extrapolated) {  // the clock ran into the newest snapshot: back off
        extrapolated = false;
        delta -= 2;
    } else if (delta < target) {
        ++delta;
    } else if (delta > target) {
        --delta;
    }
    return false;
}

bool ClientClock::snapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead) {
    const int offset = snapTime - static_cast<int>(nowMs);
    if (first) {
        delta_ = offset;
        pending_ = 0;
        floor_ = snapTime;
        extrapolated_ = false;
        window_.clear();
        window_.windowMs = windowMs, window_.spreadMs = spreadMs;
        window_.add(nowMs, offset);
        return true;
    }
    window_.windowMs = windowMs, window_.spreadMs = spreadMs;
    window_.expire(nowMs);
    // A snapshot the client itself read late (a long frame; several wait in the socket) says nothing about the path:
    // it stays out of the window, and one that looks more than 100 ms late is not used at all (the older ones of the
    // batch). The others go through the plain rule, steps at once.
    if (lateRead && windowMs > 0) {
        const int period = snapTime - oldSnapTime;
        if (offset - (static_cast<unsigned>(period) <= 500 ? period : 50) - 5 < delta_ - 100) return false;
        if (!adjust(delta_, extrapolated_, floor_, snapTime, oldSnapTime, nowMs)) return false;
        pending_ = 0;
        window_.clear();
        return true;
    }
    // A reset is decided and made with this snapshot's own receive time, as the 1.3 rule does: the window is for the
    // steps below it.
    {
        int d = delta_, f = floor_;
        bool e = extrapolated_;
        if (adjust(d, e, f, snapTime, oldSnapTime, nowMs)) {
            delta_ = d, floor_ = f, extrapolated_ = e;
            pending_ = 0;
            window_.clear();
            window_.add(nowMs, offset);
            return true;
        }
    }
    // The most delayed snapshot of the window stands for this one: the rule sees the receive time this snapshot would
    // have had with that delay.
    window_.add(nowMs, offset);
    // Only for a path that keeps doing it (DelayWindow::bursty): ordinary jitter runs evenly with the plain rule, and
    // a single late burst (or the first snapshots of a level) is left to the plain rule and the slew: the window
    // would only keep the clock behind for its whole length.
    const int worst = window_.bursty() ? window_.worst() : offset;
    const int before = delta_;
    int keepFloor = floor_;
    if (adjust(delta_, extrapolated_, keepFloor, snapTime, oldSnapTime, static_cast<int64_t>(snapTime) - worst)) {
        // Over 500 ms from the window's target only (a path that got much better or worse within the window): the
        // delta goes there without the floor of a reset, the clock takes the step as a slew.
        window_.clear();
        window_.add(nowMs, offset);
    }
    // A step (the half-way rule) is taken by the clock over time; the 1 or 2 ms of the slow rule at once.
    const int step = delta_ - before;
    if (slewDivisor > 0 && (step > 2 || step < -2)) {
        if (pending_ == 0) slewAt_ = nowMs, slewRest_ = 0;
        pending_ += step;
    }
    return false;
}

int ClientClock::time(int64_t nowMs, int newestSnapTime) {
    if (pending_ != 0) {
        slewRest_ += static_cast<int>(std::clamp<int64_t>(nowMs - slewAt_, 0, 1000));
        slewAt_ = nowMs;
        const int can = slewRest_ / slewDivisor;
        slewRest_ -= can * slewDivisor;
        pending_ -= std::clamp(pending_, -can, can);
    }
    int t = static_cast<int>(nowMs) + delta_ - pending_;
    if (t < floor_) t = floor_;
    floor_ = t;
    if (newestSnapTime >= 0 && t >= newestSnapTime - 5) extrapolated_ = true;
    return t;
}

} // namespace iw2::net
