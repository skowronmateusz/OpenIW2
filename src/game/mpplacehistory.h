// The places the clients had in the snapshots sent between the game frames (docs/spec/net.md section 10 "Snapshot
// rate"; openiw2, no 1.3 counterpart): one entry per snapshot time, kept for the antilag rewind of a shooter who drew
// those snapshots. The rewind takes a client's place between the two entries around the shooter's time, as his client
// drew it between the two snapshots.
#pragma once

#include <array>
#include <deque>
#include <utility>
#include <vector>

namespace iw2::game {

class PlaceHistory {
public:
    using Vec = std::array<float, 3>;
    static constexpr int kKeepMs = 1200; // the antilag time is at most 1000 ms back

    // The places of one snapshot time (client number, place), in rising time order; an earlier time starts again.
    void record(int timeMs, std::vector<std::pair<int, Vec>> places) {
        if (!entries_.empty() && timeMs <= entries_.back().timeMs) {
            if (timeMs == entries_.back().timeMs) {
                entries_.back().places = std::move(places);
                return;
            }
            entries_.clear();
        }
        entries_.push_back({timeMs, std::move(places)});
        while (entries_.front().timeMs < timeMs - kKeepMs) entries_.pop_front();
    }
    void clear() { entries_.clear(); }
    bool empty() const { return entries_.empty(); }
    int newestMs() const { return entries_.empty() ? -1 : entries_.back().timeMs; }

    // The client's place at timeMs: between the two entries around it that hold him; before the oldest or after the
    // newest one, that entry's place. False when no entry holds him.
    bool at(int client, int timeMs, Vec& out) const {
        const Vec *older = nullptr, *newer = nullptr;
        int tOlder = 0, tNewer = 0;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            const Vec* p = nullptr;
            for (const auto& [c, place] : it->places)
                if (c == client) p = &place;
            if (!p) {
                if (newer) break; // he was not there before: no place older than his first one
                continue;
            }
            if (it->timeMs >= timeMs) {
                newer = p, tNewer = it->timeMs;
            } else {
                older = p, tOlder = it->timeMs;
                break;
            }
        }
        if (!older && !newer) return false;
        if (!older || !newer) {
            out = older ? *older : *newer;
            return true;
        }
        const float f = static_cast<float>(timeMs - tOlder) / static_cast<float>(tNewer - tOlder);
        for (size_t k = 0; k < 3; ++k) out[k] = (*older)[k] + ((*newer)[k] - (*older)[k]) * f;
        return true;
    }

private:
    struct Entry {
        int timeMs;
        std::vector<std::pair<int, Vec>> places;
    };
    std::deque<Entry> entries_;
};

} // namespace iw2::game
