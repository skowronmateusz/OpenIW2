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
    static constexpr int kWindow = 60;      // snapshots whose distance to the last command sets the lag (3 s)
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
    }
    // Once per snapshot time: the place for it. False when there is nothing to sample (no command yet).
    bool sample(int snapshotTime, Vec& out) {
        if (track_.empty()) return false;
        const int distance = snapshotTime - track_.back().time;
        lags_.push_back(distance);
        if (lags_.size() > size_t(kWindow)) lags_.pop_front();
        const auto [lo, hi] = std::minmax_element(lags_.begin(), lags_.end());
        // The distance swings by one command interval as his frame rate beats against the snapshots, also while the
        // window happens to hold only one end of the swing: the lag covers the whole swing from the start.
        const int interval = track_.size() > 1 ? (track_.back().time - track_.front().time + static_cast<int>(track_.size()) - 2) /
                                                     (static_cast<int>(track_.size()) - 1)
                                               : 0;
        const int want = std::min(std::max(*hi, *lo + interval), *lo + kMaxSpreadMs);
        lag_ = lags_.size() == 1 || want > lag_ ? want : std::max(want, lag_ - 1);
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
    std::deque<int> lags_;
    int lag_ = 0;
};

} // namespace iw2::netgame
