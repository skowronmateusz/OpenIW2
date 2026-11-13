// docs/spec/net.md section 10 "Snapshot rate": the places the clients had in the snapshots sent between the game
// frames, kept for the antilag rewind (src/game/mpplacehistory.h).
#include <cmath>
#include <cstdio>

#include "game/mpplacehistory.h"

using iw2::game::PlaceHistory;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}
bool near(const PlaceHistory::Vec& v, float x, float y, float z) {
    return std::fabs(v[0] - x) < 1e-3f && std::fabs(v[1] - y) < 1e-3f && std::fabs(v[2] - z) < 1e-3f;
}
} // namespace

int main() {
    PlaceHistory h;
    PlaceHistory::Vec o{};
    check(h.empty() && h.newestMs() == -1 && !h.at(0, 1000, o), "an empty history has no place");
    // Client 0 walks 1 unit per ms along x; client 1 joins at 1033 and stands.
    h.record(1000, {{0, {1000, 0, 0}}});
    h.record(1016, {{0, {1016, 0, 0}}});
    h.record(1033, {{0, {1033, 0, 0}}, {1, {5, 6, 7}}});
    h.record(1050, {{0, {1050, 0, 0}}, {1, {5, 6, 7}}});
    check(h.newestMs() == 1050, "the newest entry");
    check(h.at(0, 1016, o) && near(o, 1016, 0, 0), "a time on an entry: its place");
    check(h.at(0, 1025, o) && near(o, 1025, 0, 0), "between two entries: between their places");
    check(h.at(0, 1049, o) && near(o, 1049, 0, 0), "just before the newest entry");
    check(h.at(0, 1050, o) && near(o, 1050, 0, 0), "the newest entry's time");
    check(h.at(0, 2000, o) && near(o, 1050, 0, 0), "after the newest entry: its place");
    check(h.at(0, 900, o) && near(o, 1000, 0, 0), "before the oldest entry: its place");
    check(!h.at(2, 1025, o), "a client no entry holds");
    check(h.at(1, 1040, o) && near(o, 5, 6, 7), "a client that joined later, between his entries");
    check(h.at(1, 1010, o) && near(o, 5, 6, 7), "before his first entry: that entry's place");
    // The same time again replaces the entry.
    h.record(1050, {{0, {2000, 0, 0}}});
    check(h.at(0, 1050, o) && near(o, 2000, 0, 0), "a repeated time replaces the entry");
    // Entries older than kKeepMs before the newest one are dropped.
    for (int t = 1066; t <= 1000 + PlaceHistory::kKeepMs + 400; t += 17) h.record(t, {{0, {float(t), 0, 0}}});
    check(h.at(0, 1000, o) && o[0] >= float(h.newestMs() - PlaceHistory::kKeepMs) - 17.0f, "old entries are dropped");
    // A time that goes back (a new level) starts the history again.
    h.record(500, {{0, {1, 2, 3}}});
    check(h.newestMs() == 500 && h.at(0, 3000, o) && near(o, 1, 2, 3), "an earlier time starts again");
    h.clear();
    check(h.empty(), "clear");
    if (failures) std::printf("%d check(s) failed\n", failures);
    else std::printf("placehistory: OK\n");
    return failures ? 1 : 0;
}
