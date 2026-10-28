// Tests for the nearest-reachable-node search (exe 0x542340 / 0x545670 / 0x5422c0, docs/spec/ai.md "Path planner").

#include "game/pathnodes.h"

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
using namespace iw2::game;
using P = std::array<float, 3>;

PathGraph graph(std::vector<P> pos, std::vector<std::vector<int>> adj) {
    std::vector<std::vector<PathLink>> links(pos.size());
    for (size_t i = 0; i < adj.size(); ++i)
        for (int t : adj[i]) links[i].push_back({uint16_t(t), 1.0f});
    return PathGraph(std::move(pos), std::move(links));
}
const auto none = [](size_t, size_t) { return false; };
} // namespace

int main() {
    // A chain towards the point is followed to its end.
    {
        const PathGraph g = graph({{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}}, {{1}, {0, 2}, {1, 3}, {2}});
        CHECK(nearestReachableNode(g, 0, {40, 0, 0}, none) == 3);
        CHECK(nearestReachableNode(g, 3, {-5, 0, 0}, none) == 0);
    }
    // A neighbour no closer than the best so far is never entered, so a nearer node behind it is not found.
    {
        const PathGraph g = graph({{0, 0, 0}, {-10, 0, 0}, {5, 0, 0}}, {{1}, {2}, {}});
        CHECK(nearestReachableNode(g, 0, {5, 0, 0}, none) == 0);
    }
    // The height gate: (z − point z)² must not exceed 6400.
    {
        const PathGraph g = graph({{0, 0, 0}, {10, 0, 80}, {10, 0, 81}}, {{1}, {}, {}});
        CHECK(nearestReachableNode(g, 0, {10, 0, 0}, none) == 1);
        const PathGraph h = graph({{0, 0, 0}, {10, 0, 81}}, {{1}, {}});
        CHECK(nearestReachableNode(h, 0, {10, 0, 0}, none) == 0);
    }
    // A blocked link is skipped.
    {
        const PathGraph g = graph({{0, 0, 0}, {10, 0, 0}}, {{1}, {}});
        CHECK(nearestReachableNode(g, 0, {10, 0, 0}, [](size_t n, size_t l) { return n == 0 && l == 0; }) == 0);
    }
    // Link order matters: a neighbour accepted first raises the bar for the next ones of the same node.
    {
        // 0 → A (1, d² 100) and B (2, d² 49); A → C (3, d² 1).
        const std::vector<P> pos{{0, 20, 0}, {10, 0, 0}, {0, 7, 0}, {1, 0, 0}};
        const PathGraph ab = graph(pos, {{1, 2}, {3}, {}, {}});
        CHECK(nearestReachableNode(ab, 0, {0, 0, 0}, none) == 3);
        const PathGraph ba = graph(pos, {{2, 1}, {3}, {}, {}});
        CHECK(nearestReachableNode(ba, 0, {0, 0, 0}, none) == 2);
    }
    CHECK(nearestReachableNode(graph({{0, 0, 0}}, {{}}), 1, {0, 0, 0}, none) == -1);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
