// docs/spec/net.md section 10 "Player positions in a snapshot": the place of a network client in the other clients'
// snapshots is his walk sampled at an even time (src/netgame/eventrack.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "netgame/eventrack.h"

using iw2::netgame::EvenTrack;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

// A client walking along x at `speed` units/s whose commands (one per frameMs of his own time) reach the server
// `delayMs` + a per-command extra late; a snapshot every 50 ms takes what has arrived. Returns, per snapshot, the plain
// place (after the last command) and the sampled one.
struct Run {
    std::vector<float> plain, even;
    std::vector<int> lags;
};
template <class Extra> Run walk(EvenTrack& track, double frameMs, int delayMs, int snapshots, Extra extra, float speed = 200.0f) {
    Run r;
    double next = 0;  // the client's time of his next command
    int n = 0;
    float last = 0;
    bool have = false;
    for (int s = 1; s <= snapshots; ++s) {
        const int snapshotTime = 1000 + s * 50;
        // the client's clock runs 60 ms behind the server's time, as a 1.3 client's does
        for (;; ++n, next += frameMs) {
            const int commandTime = 940 + static_cast<int>(next);
            const double arrival = 1000 + next + delayMs + extra(n);
            if (arrival > snapshotTime) break;
            last = speed * static_cast<float>(commandTime - 940) / 1000.0f;
            track.add(commandTime, {last, 0, 0});
            have = true;
        }
        EvenTrack::Vec at{};
        if (!have || !track.sample(snapshotTime, at)) continue;
        r.plain.push_back(last);
        r.even.push_back(at[0]);
        r.lags.push_back(track.lagMs());
    }
    return r;
}
// Largest and smallest step between consecutive entries, from index `from`.
std::pair<float, float> steps(const std::vector<float>& v, size_t from) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = from + 1; i < v.size(); ++i) {
        lo = std::min(lo, v[i] - v[i - 1]);
        hi = std::max(hi, v[i] - v[i - 1]);
    }
    return {lo, hi};
}
}  // namespace

int main() {
    // 120 commands a second against 20 snapshots: 6 per snapshot, and 5 or 7 where the two rates beat.
    {
        EvenTrack t;
        const Run r = walk(t, 1000.0 / 119.0, 3, 400, [](int) { return 0.0; });
        const auto [plo, phi] = steps(r.plain, 70);
        const auto [elo, ehi] = steps(r.even, 70);
        std::printf("120/s: plain steps %.2f..%.2f, even steps %.2f..%.2f units per snapshot (10 = 50 ms), lag %d ms\n", double(plo), double(phi), double(elo),
                    double(ehi), r.lags.back());
        check(phi - plo > 1.5f, "the plain places of a 120/s client differ by a command per snapshot");
        check(std::fabs(elo - 10.0f) < 0.01f && std::fabs(ehi - 10.0f) < 0.01f, "the sampled places are 50 ms of the walk apart");
        // never ahead of his commands, and at most one command interval + rounding behind the plain place
        for (size_t i = 70; i < r.even.size(); ++i) {
            check(r.even[i] <= r.plain[i] + 0.001f, "the sampled place is not ahead of the last command");
            check(r.plain[i] - r.even[i] < 200.0f * 0.012f, "the sampled place is at most 12 ms of the walk back");
        }
    }
    // 60 commands a second with a path that jitters by up to 6 ms: 2 to 4 commands per snapshot.
    {
        EvenTrack t;
        unsigned seed = 12345;
        const Run r = walk(t, 1000.0 / 59.0, 8, 400, [&](int) {
            seed = seed * 1664525u + 1013904223u;
            return double((seed >> 16) % 7);
        });
        const auto [plo, phi] = steps(r.plain, 70);
        const auto [elo, ehi] = steps(r.even, 70);
        std::printf("60/s, jitter: plain steps %.2f..%.2f, even steps %.2f..%.2f, lag %d ms\n", double(plo), double(phi), double(elo), double(ehi),
                    r.lags.back());
        check(phi - plo > 3.0f, "the plain places of a 60/s client on a jittering path differ by commands");
        check(ehi - elo < 0.8f, "the sampled places stay within 4 ms of the walk of each other"); // the lag follows the window
    }
    // One packet 120 ms late: the newest place is held (nothing is drawn ahead), the lag rises by the 50 ms limit only
    // and then falls by 1 ms per snapshot.
    {
        EvenTrack t;
        const Run r = walk(t, 1000.0 / 119.0, 3, 300, [](int n) { return n >= 1190 && n < 1204 ? 120.0 - (n - 1190) * 8.4 : 0.0; });
        int top = 0, topAt = 0;
        for (size_t i = 0; i < r.lags.size(); ++i)
            if (r.lags[i] > top) top = r.lags[i], topAt = int(i);
        const int usual = r.lags[150];
        std::printf("late packet: lag %d ms, %d at snapshot %d, %d 70 snapshots later, %d at the end\n", usual, top, topAt, r.lags[size_t(topAt) + 70],
                    r.lags.back());
        check(top > usual + 20 && top <= usual + EvenTrack::kMaxSpreadMs + 9, "a late packet raises the lag, by the limit at most");
        check(r.lags[size_t(topAt) + 70] < top, "the lag falls again once the late packet has left the window");
        for (size_t i = size_t(topAt) + 1; i < r.lags.size(); ++i)
            check(r.lags[i] >= r.lags[i - 1] - 1, "the lag falls by 1 ms per snapshot at most");
        for (size_t i = 70; i < r.even.size(); ++i) {
            check(r.even[i] <= r.plain[i] + 0.001f, "late packet: never ahead of the last command");
            check(i == 0 || r.even[i] >= r.even[i - 1] - 0.001f, "late packet: the sampled place never goes back");
        }
    }
    // A restart forgets the track: the place after it is the new one, not one between the old and the new.
    {
        EvenTrack t;
        for (int i = 0; i < 40; ++i) t.add(1000 + i * 8, {float(i), 0, 0});
        EvenTrack::Vec at{};
        check(t.sample(1380, at), "a track with commands gives a place");
        t.restart();
        check(!t.sample(1430, at), "no place after a restart without a command");
        t.add(1400, {5000, 0, 0});
        check(t.sample(1480, at) && at[0] == 5000.0f, "the place after a restart is the new one");
        check(t.lagMs() == 80, "the lag starts again");
    }
    // Command times that start again earlier (the client's clock was set back): the server restarts the track, and the
    // walk is sampled evenly again from the new times.
    {
        EvenTrack t;
        const Run before = walk(t, 1000.0 / 119.0, 3, 100, [](int) { return 0.0; });
        t.restart();
        EvenTrack::Vec at{};
        float last = 0;
        std::vector<float> even;
        for (int s = 1; s <= 100; ++s) {
            for (int k = 0; k < 6; ++k) {
                const int commandTime = 400 + (s - 1) * 50 + k * 8;  // 5 s earlier than the old times
                last = 9000.0f + 0.2f * float(commandTime - 400);
                t.add(commandTime, {last, 0, 0});
            }
            check(t.sample(6000 + s * 50, at), "a place after the times started again");
            even.push_back(at[0]);
        }
        const auto [lo, hi] = steps(even, 5);
        check(!before.even.empty() && std::fabs(lo - 10.0f) < 0.01f && std::fabs(hi - 10.0f) < 0.01f, "even again after the times started again");
    }
    // Two frames for one arrival of commands (the server caught up): the second sample is 50 ms further from the last
    // command; the place does not go back and the lag stays within the limit.
    {
        EvenTrack t;
        for (int i = 0; i < 60; ++i) t.add(1000 + i * 8, {float(i) * 1.6f, 0, 0});
        EvenTrack::Vec a{}, b{};
        check(t.sample(1540, a) && t.sample(1590, b), "two samples");
        check(b[0] >= a[0] && t.lagMs() <= 1540 - 1472 + EvenTrack::kMaxSpreadMs, "a second frame without new commands");
    }
    // A standing client and a command that is not newer.
    {
        EvenTrack t;
        t.add(1000, {1, 2, 3});
        t.add(1000, {9, 9, 9});
        t.add(990, {9, 9, 9});
        EvenTrack::Vec at{};
        check(t.sample(1050, at) && at[0] == 1 && at[1] == 2 && at[2] == 3, "a command not newer than the last one is ignored");
    }
    std::printf(failures ? "eventrack: FAILED\n" : "eventrack: OK\n");
    return failures ? 1 : 0;
}
