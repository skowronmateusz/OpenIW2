// The arrival delays of a stream of time-stamped messages over the last stretch of real time (docs/spec/net.md
// section 10, "Client time on a bursty path" and "Commands on a bursty path"): snapshots on the client, command
// packets on the server. openiw2, no 1.3 counterpart.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <utility>

namespace iw2::net {

class DelayWindow {
public:
    int windowMs = 1500;
    int spreadMs = 40; // a message this much later than the window's least delayed one is late
    int gapMs = 200;   // late messages this far apart are separate stretches

    void clear() { offsets_.clear(); }
    // offset = the message's own time - its receive time: only differences count, a lower value is a later message.
    void add(int64_t nowMs, int offset) {
        expire(nowMs);
        offsets_.emplace_back(nowMs, offset);
    }
    void expire(int64_t nowMs) {
        while (!offsets_.empty() && offsets_.front().first < nowMs - windowMs) offsets_.pop_front();
    }
    bool empty() const { return offsets_.empty(); }
    // The least and the most delayed message of the window.
    int best() const {
        int b = INT32_MIN;
        for (const auto& o : offsets_) b = std::max(b, o.second);
        return b;
    }
    int worst() const {
        int w = INT32_MAX;
        for (const auto& o : offsets_) w = std::min(w, o.second);
        return w;
    }
    // A path that keeps delivering late: the window holds two separate stretches of late messages. A single late
    // burst (or ordinary jitter below spreadMs) is not one.
    bool bursty() const {
        if (windowMs <= 0 || offsets_.empty()) return false;
        const int limit = best() - spreadMs;
        int stretches = 0;
        int64_t lastLate = INT64_MIN / 2;
        for (const auto& o : offsets_)
            if (o.second < limit) {
                if (o.first - lastLate >= gapMs) ++stretches;
                lastLate = o.first;
            }
        return stretches >= 2;
    }

    // The same against a moving reference, for a sender whose own clock is adjusted while it runs (a client's command
    // times follow its clock, which slides by tens of milliseconds over seconds): a message is compared with the
    // least delayed one of the referenceMs before it, so a slow slide of the offsets is not a delay.
    struct Stats {
        bool bursty = false; // two separate stretches of late messages
        int lateness = 0;    // the largest delay of a message against its reference
        int reference = 0;   // the least delayed offset of the last referenceMs before atMs
    };
    Stats stats(int64_t atMs, int referenceMs) const {
        Stats st;
        if (offsets_.empty()) return st;
        std::deque<std::pair<int64_t, int>> lead; // candidates for the reference: offsets falling towards the back
        int stretches = 0;
        int64_t lastLate = INT64_MIN / 2;
        for (const auto& o : offsets_) {
            while (!lead.empty() && lead.front().first <= o.first - referenceMs) lead.pop_front();
            while (!lead.empty() && lead.back().second <= o.second) lead.pop_back();
            lead.push_back(o);
            const int late = lead.front().second - o.second;
            st.lateness = std::max(st.lateness, late);
            if (late > spreadMs) {
                if (o.first - lastLate >= gapMs) ++stretches;
                lastLate = o.first;
            }
        }
        while (lead.size() > 1 && lead.front().first <= atMs - referenceMs) lead.pop_front();
        st.reference = lead.front().second;
        st.bursty = windowMs > 0 && stretches >= 2;
        return st;
    }

private:
    std::deque<std::pair<int64_t, int>> offsets_; // (real time, offset)
};

} // namespace iw2::net
