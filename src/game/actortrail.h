// The follower trail ring of an actor (+0x214c index, +0x2150 ten (x, y) entries). Exe 0x505520 (add), 0x504f60 (advance),
// 0x504e70 (sum), 0x504de0 (clear). Only the facing blend 0x505630 reads the sum (TODO(spec): not modelled).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace iw2::game {

struct ActorTrail {
    std::array<std::array<float, 2>, 10> v{};
    int idx = 0;

    // 0x504de0.
    void clear() {
        v = {};
        idx = 0;
    }

    // 0x504f60: the entry idx is finished. When the last turn (entry before the previous -> previous, previous -> current)
    // changes direction (product of the two crosses <= 0) with both dots non-negative, the entry before the previous is
    // replaced by the current one. Then every other entry is weighted by k / (k + 1), k = 1, 2, ... in the order
    // idx + 1 .. 9, 0 .. idx - 1 (the oldest the least), and the index moves on.
    void advance() {
        const int cur = idx;
        const int prev = (cur == 0 ? 10 : cur) - 1;
        const int pp = (prev == 0 ? 10 : prev) - 1;
        const auto c = v[size_t(cur)];
        const auto p = v[size_t(prev)];
        const auto q = v[size_t(pp)];
        const float c1 = -p[0] * q[1] + p[1] * q[0];
        const float c2 = -p[0] * c[1] + p[1] * c[0];
        if (!(c1 * c2 > 0.0f) && !(p[1] * c[1] + p[0] * c[0] < 0.0f) && !(p[1] * q[1] + p[0] * q[0] < 0.0f)) v[size_t(pp)] = c;
        double f = 1.0;
        auto weigh = [&](int i) {
            const double m = f / (f + 1.0);
            f += 1.0;
            v[size_t(i)][0] = float(double(v[size_t(i)][0]) * m);
            v[size_t(i)][1] = float(double(v[size_t(i)][1]) * m);
        };
        for (int i = cur + 1; i < 10; ++i) weigh(i);
        for (int i = 0; i < cur; ++i) weigh(i);
        idx = (cur + 1) % 10;
    }

    // 0x504e70: the sum of the entries, in order.
    std::array<float, 2> sum() const {
        float x = v[0][0], y = v[0][1];
        for (int i = 1; i < 10; ++i) {
            x += v[size_t(i)][0];
            y += v[size_t(i)][1];
        }
        return {x, y};
    }

    // 0x505520, every think of a followed actor with a path: (dx, dy) is the lookahead direction. At the level time / 100
    // boundary (a multiple of 10) the ring advances and the new entry is set to the direction * 0.1; otherwise it is added.
    void add(int64_t timeMs, float dx, float dy) {
        const int64_t t = timeMs / 100;
        if (t % 10 != 0) {
            v[size_t(idx)][0] += dx * 0.1f;
            v[size_t(idx)][1] += dy * 0.1f;
            return;
        }
        advance();
        v[size_t(idx)][0] = dx * 0.1f;
        v[size_t(idx)][1] = dy * 0.1f;
    }
};

}  // namespace iw2::game
