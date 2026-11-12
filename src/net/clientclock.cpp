// docs/spec/net.md section 10, "Client time" and "Client time: the even clock".
#include "net/clientclock.h"

#include <algorithm>
#include <cstdint>
#include <vector>

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

bool ClientClock::evenSnapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead) {
    const int offset = snapTime - static_cast<int>(nowMs);
    const int period = snapTime - oldSnapTime;
    if (first) {
        arrivals_.clear();
        arrivals_.push_back({nowMs, offset, 0});
        evenDelta_ = target_ = static_cast<double>(offset - 50 - safetyMs); // the interval is not known yet
        movedAt_ = nowMs;
        cover_ = 0;
        time_ = static_cast<double>(snapTime);
        floor_ = snapTime;
        extrapolated_ = false;
        return true;
    }
    // A snapshot the client itself read late (a long frame) says nothing about the path.
    if (lateRead) return false;
    // A reset is decided with this snapshot's own arrival, as the 1.3 rule does (a new map, a stall of the server:
    // more than 500 ms from where this snapshot alone would put the clock): start again from this snapshot.
    {
        const double own = static_cast<double>(offset - (static_cast<unsigned>(period) <= 500 ? period : 50) - safetyMs);
        if (own - evenDelta_ > 500.0 || own - evenDelta_ < -500.0) {
            arrivals_.clear();
            arrivals_.push_back({nowMs, offset, 0});
            evenDelta_ = target_ = own;
            movedAt_ = nowMs;
            cover_ = 0;
            time_ = static_cast<double>(snapTime);
            floor_ = snapTime;
            extrapolated_ = false;
            return true;
        }
    }
    while (!arrivals_.empty() && arrivals_.front().atMs < nowMs - windowMs) arrivals_.pop_front();
    arrivals_.push_back({nowMs, offset, static_cast<unsigned>(period) <= 500 ? period : 0});
    // The least delayed snapshot of the window, and the snapshot interval: the second largest of the last five known
    // (a single missing snapshot does not change it; a server that leaves out every other snapshot, or another rate,
    // is followed within two snapshots, and by the longer interval).
    int best = offset;
    for (const Arrival& a : arrivals_) best = std::max(best, a.offset);
    int nominal = 50;
    {
        std::vector<int> last;
        for (auto it = arrivals_.rbegin(); it != arrivals_.rend() && last.size() < 5; ++it)
            if (it->interval > 0) last.push_back(it->interval);
        std::sort(last.begin(), last.end());
        if (!last.empty()) nominal = last[last.size() >= 2 ? last.size() - 2 : 0];
    }
    // How much later than that the others came. The clock runs behind by the delay that coverPercent of them stayed
    // within (ordinary jitter; the few beyond it are drawn past the newest snapshot for a moment). Delays over
    // spreadMs count only on a path that keeps having them (two stretches 200 ms or more apart in the window): then
    // the largest one, up to maxBehindMs.
    std::vector<int> small;
    int large = 0, stretches = 0;
    int64_t lastLargeAt = -1;
    for (const Arrival& a : arrivals_) {
        const int late = best - a.offset;
        if (late <= spreadMs) {
            small.push_back(late);
            continue;
        }
        large = std::max(large, late);
        if (lastLargeAt < 0 || a.atMs - lastLargeAt >= 200) ++stretches;
        lastLargeAt = a.atMs;
    }
    std::sort(small.begin(), small.end());
    int cover = small.empty() ? 0 : small[std::min(small.size() - 1, small.size() * static_cast<size_t>(coverPercent) / 100)];
    if (stretches >= 2) cover = std::max(cover, std::min(large, maxBehindMs));
    cover_ = cover;
    interval_ = nominal;
    target_ = static_cast<double>(best - nominal - cover - safetyMs);
    // Another snapshot rate (a killcam's one snapshot per game frame, and back): the clock goes to the new target at
    // once; slewed, it would run past the newest snapshot for a third of a second. Only when the last three intervals
    // all have the new length: lost snapshots also lengthen intervals, but not three in a row, and what they change
    // is slewed like everything else.
    {
        int n = 0;
        bool all = true;
        for (auto it = arrivals_.rbegin(); it != arrivals_.rend() && n < 3; ++it)
            if (it->interval > 0) {
                all = all && it->interval - nominal < 10 && nominal - it->interval < 10;
                ++n;
            }
        if (stepInterval_ == 0) stepInterval_ = nominal;
        if (n == 3 && all && (nominal - stepInterval_ >= 10 || stepInterval_ - nominal >= 10)) {
            evenDelta_ = target_;
            stepInterval_ = nominal;
        }
    }
    return false;
}

int ClientClock::evenTime(int64_t nowMs, int newestSnapTime) {
    const double dt = movedAt_ < 0 ? 0.0 : static_cast<double>(std::clamp<int64_t>(nowMs - movedAt_, 0, 1000));
    movedAt_ = nowMs;
    const double diff = target_ - evenDelta_;
    const double want = diff * std::min(1.0, dt / static_cast<double>(settleMs));
    evenDelta_ += std::clamp(want, -slowRate * dt, fastRate * dt);
    double t = static_cast<double>(nowMs) + evenDelta_;
    if (t < time_ && t > time_ - 1000.0) t = time_; // never backwards (a reset sets time_)
    time_ = t;
    floor_ = static_cast<int>(t);
    extrapolated_ = newestSnapTime >= 0 && floor_ >= newestSnapTime;
    return floor_;
}

bool ClientClock::snapshot(int snapTime, int oldSnapTime, bool first, int64_t nowMs, bool lateRead) {
    if (windowMs > 0) return evenSnapshot(snapTime, oldSnapTime, first, nowMs, lateRead);
    if (first) {
        delta_ = snapTime - static_cast<int>(nowMs);
        floor_ = snapTime;
        extrapolated_ = false;
        return true;
    }
    return adjust(delta_, extrapolated_, floor_, snapTime, oldSnapTime, nowMs);
}

int ClientClock::time(int64_t nowMs, int newestSnapTime) {
    if (windowMs > 0) return evenTime(nowMs, newestSnapTime);
    int t = static_cast<int>(nowMs) + delta_;
    if (t < floor_) t = floor_;
    floor_ = t;
    if (newestSnapTime >= 0 && t >= newestSnapTime - 5) extrapolated_ = true;
    return t;
}

} // namespace iw2::net
