// A network client's place for the snapshots of the other clients (docs/spec/net.md section 10 "Player positions in a
// snapshot"; openiw2, no 1.3 counterpart): his commands run when they arrive, so the place after the last one is a
// varying time behind the snapshot. The track keeps his last places with their command times and gives the place he
// had a fixed lag before the snapshot's time, so the snapshots sample his walk evenly.
#pragma once

#include <algorithm>
#include <array>
#include <deque>

namespace iw2::netgame {

class EvenTrack {
public:
    using Vec = std::array<float, 3>;
    static constexpr int kWindowMs = 3000;  // the snapshots of this long set the lag by their distance to the last command
    static constexpr int kFallMs = 50;      // the lag falls by 1 ms per this much snapshot time
    static constexpr int kMaxSpreadMs = 35; // the lag never exceeds the window's smallest distance by more than this
    static constexpr int kKeepMs = 400;     // places older than this before the newest one are dropped

    // The place after the command with that time. A command not newer than the last one is ignored.
    void add(int commandTime, const Vec& origin) {
        if (!track_.empty() && commandTime <= track_.back().time) return;
        track_.push_back({commandTime, origin});
        while (track_.size() > 2 && track_[1].time < commandTime - kKeepMs) track_.pop_front();
    }
    // The movement starts again (a spawn, a place set from outside): the old places are not part of it.
    void restart() {
        track_.clear();
        lags_.clear();
        lag_ = 0;
        fallRest_ = 0;
    }
    // The place for a snapshot time. False when there is nothing to sample (no command yet). The lag is brought up to
    // date once per snapshot time, in rising order; an earlier or repeated time is sampled with the lag as it is.
    bool sample(int snapshotTime, Vec& out) {
        if (track_.empty()) return false;
        if (lags_.empty() || snapshotTime > lags_.back().time) {
            const int distance = snapshotTime - track_.back().time;
            const int passed = lags_.empty() ? 0 : snapshotTime - lags_.back().time;
            lags_.push_back({snapshotTime, distance});
            while (lags_.front().time <= snapshotTime - kWindowMs) lags_.pop_front();
            int lo = distance, hi = distance;
            for (const Lag& l : lags_) lo = std::min(lo, l.distance), hi = std::max(hi, l.distance);
            // The distance swings by one command interval as his frame rate beats against the snapshots, also while
            // the window happens to hold only one end of the swing: the lag covers the whole swing from the start.
            const int interval = track_.size() > 1 ? (track_.back().time - track_.front().time + static_cast<int>(track_.size()) - 2) /
                                                         (static_cast<int>(track_.size()) - 1)
                                                   : 0;
            const int want = std::min(std::max(hi, lo + interval), lo + kMaxSpreadMs);
            // It rises at once and falls by 1 ms per kFallMs of snapshot time.
            fallRest_ += passed;
            const int fall = fallRest_ / kFallMs;
            fallRest_ -= fall * kFallMs;
            lag_ = lags_.size() == 1 || want > lag_ ? want : std::max(want, lag_ - fall);
        }
        const int t = snapshotTime - lag_;
        if (t >= track_.back().time) {  // nothing newer than his last command is known: his newest place
            out = track_.back().origin;
            return true;
        }
        if (t <= track_.front().time) {
            out = track_.front().origin;
            return true;
        }
        const auto next = std::upper_bound(track_.begin(), track_.end(), t, [](int time, const Point& p) { return time < p.time; });
        const Point& b = *next;
        const Point& a = *(next - 1);
        const float f = static_cast<float>(t - a.time) / static_cast<float>(b.time - a.time);
        for (size_t k = 0; k < 3; ++k) out[k] = a.origin[k] + (b.origin[k] - a.origin[k]) * f;
        return true;
    }
    int lagMs() const { return lag_; }

private:
    struct Point {
        int time;
        Vec origin;
    };
    std::deque<Point> track_;
    struct Lag {
        int time, distance;
    };
    std::deque<Lag> lags_;
    int lag_ = 0;
    int fallRest_ = 0;
};

} // namespace iw2::netgame
