// Follower trail ring (exe 0x505520 / 0x504f60 / 0x504e70 / 0x504de0).
#include "game/actortrail.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
}  // namespace

int main() {
    using iw2::game::ActorTrail;
    ActorTrail t;
    t.add(110, 1, 0);
    t.add(150, 1, 0);
    CHECK(std::fabs(t.v[0][0] - 0.2f) < 1e-6f && t.idx == 0);
    // A boundary (time / 100 divisible by 10) advances and starts the new entry.
    t.add(1000, 0, 1);
    CHECK(t.idx == 1 && std::fabs(t.v[1][1] - 0.1f) < 1e-6f);
    // The finished entry keeps its weight; the others (empty here) are weighted.
    CHECK(std::fabs(t.v[0][0] - 0.2f) < 1e-6f);
    const auto s = t.sum();
    // With empty older entries the zero crosses count as a reversal, so entry 8 also took the finished entry (0.2, 0): then weighted 8/9.
    CHECK(std::fabs(s[0] - (0.2f + 0.2f * 8.0f / 9.0f)) < 1e-6f && std::fabs(s[1] - 0.1f) < 1e-6f);
    t.clear();
    CHECK(t.idx == 0 && t.sum()[0] == 0.0f);
    t.idx = 9;
    t.advance();
    CHECK(t.idx == 0);
    // A direction reversal with non-negative dots replaces the entry before the previous one by the current.
    ActorTrail r;
    r.idx = 3;
    r.v[1] = {1, 1};
    r.v[2] = {1, 0};
    r.v[3] = {1, -1};
    r.advance();
    CHECK(r.v[1][1] < 0.0f);
    // The oldest entry (idx + 1) is weighted 1/2, the next 2/3.
    ActorTrail w;
    for (auto& e : w.v) e = {1, 0};
    w.idx = 0;
    w.advance();
    CHECK(std::fabs(w.v[1][0] - 0.5f) < 1e-6f && std::fabs(w.v[2][0] - 2.0f / 3.0f) < 1e-6f && std::fabs(w.v[9][0] - 0.9f) < 1e-6f);
    if (failures == 0) std::cout << "trail ok\n";
    return failures == 0 ? 0 : 1;
}
