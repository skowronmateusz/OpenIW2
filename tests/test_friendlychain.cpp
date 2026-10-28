// Tests for the friendly chain builder and follower selection (0x52d630, 0x518270, 0x518a70) on a synthetic map.

#include "game/friendlychain.h"

#include <iostream>
#include <stdexcept>

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

ChainNodeIn node(const char* name, const char* target, float x, int flags = 0) {
    ChainNodeIn n;
    n.name = name;
    n.target = target;
    n.spawnflags = flags;
    n.typeNum = 1;
    n.origin = {x, 0, 0};
    return n;
}

template <class F>
bool throws(F f, const char* what) {
    try {
        f();
    } catch (const std::runtime_error& e) {
        return std::string(e.what()).find(what) != std::string::npos;
    }
    return false;
}

int idx(const std::vector<ChainNodeIn>& v, const char* name) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name) return int(i);
    return -1;
}
} // namespace

int main() {
    // Array order is deliberately not chain order: a free node, chain n0..n4 (x = 0..40) and chain m0 -> m1.
    std::vector<ChainNodeIn> in = {node("", "", 500),      node("n4", "", 40),  node("n2", "n3", 20), node("m1", "", 1000),
                                   node("n0", "n1", 0),    node("n3", "n4", 30), node("m0", "m1", 900), node("n1", "n2", 10)};
    FriendlyChains fc;
    fc.build(in);
    const int n0 = idx(in, "n0"), n1 = idx(in, "n1"), n2 = idx(in, "n2"), n3 = idx(in, "n3"), n4 = idx(in, "n4");
    const int m0 = idx(in, "m0"), m1 = idx(in, "m1");

    // Numbering: heads in array order get ids 1, 2 (n0 comes before m0); positions are 1-based.
    CHECK(fc.chainId(n0) == 1 && fc.chainId(n4) == 1 && fc.chainId(m0) == 2 && fc.chainId(m1) == 2);
    CHECK(fc.chainId(0) == 0);
    CHECK(fc.pos(n0) == 1 && fc.pos(n1) == 2 && fc.pos(n2) == 3 && fc.pos(n3) == 4 && fc.pos(n4) == 5);
    CHECK(fc.pos(m0) == 1 && fc.pos(m1) == 2);
    CHECK(fc.prev(n0) == -1 && fc.prev(n3) == n2 && fc.prev(m1) == m0);
    // Sorted: chain id descending, then position; unchained nodes last.
    CHECK(fc.inChainCount() == 7);
    const std::vector<int> want = {m0, m1, n0, n1, n2, n3, n4, 0};
    CHECK(fc.sorted() == want);

    // The followed chain node advances to the nearest node of its own chain.
    CHECK(fc.nearest(n0, {31, 0, 0}) == n3);
    CHECK(fc.nearest(n0, {895, 0, 0}) == n4);  // never a node of another chain
    CHECK(fc.nearest(m0, {0, 0, 0}) == m0);
    CHECK(fc.nearest(-1, {0, 0, 0}) == -1);
    CHECK(fc.nearest(0, {0, 0, 0}) == -1);     // a node without a chain

    // Trigger set: the first chain node with that name.
    CHECK(fc.findByName("n2") == n2);
    CHECK(fc.findByName("nope") == -1);

    // Follower selection with the default -5 / -1 behind the leader at n3 (position 4): the window is [-1, 3].
    bool taken[8] = {};
    ChainEnv env;
    env.ok = [&](int n) { return !taken[n]; };
    env.hold = [](int) { return int64_t(0); };
    env.now = 1000;
    CHECK(fc.choose(n3, -5, -1, -1, false, env) == n2);     // walks back from the leader to the window
    CHECK(fc.choose(n4, -5, -1, -1, false, env) == n3);
    CHECK(fc.choose(n3, 0, 0, -1, false, env) == n3);       // min 0 max 0: the leader's own node
    // From behind the window the follower moves forward to the far end of it.
    CHECK(fc.choose(n3, -2, -1, n0, false, env) == n2);
    // The node at the far end is taken: the next one back is used.
    taken[n2] = true;
    CHECK(fc.choose(n3, -2, -1, n0, false, env) == n1);
    CHECK(fc.choose(n3, -5, -1, -1, false, env) == n1);
    // A follower already in the window keeps its node.
    taken[n2] = false;
    CHECK(fc.choose(n3, -3, -1, n1, false, env) == n2);

    // Fatal build errors.
    CHECK(throws([] { FriendlyChains f; f.build({node("a", "x", 0), node("b", "x", 0), node("x", "", 0)}); },
                 "multiple nodes target node with name 'x'"));
    CHECK(throws([] { FriendlyChains f; f.build({node("a", "a", 0)}); }, "targets itself"));
    CHECK(throws([] { FriendlyChains f; f.build({node("a", "b", 0), node("b", "a", 0)}); }, "found chain loop"));
    {
        ChainNodeIn bad = node("b", "", 0);
        bad.typeNum = 0;
        FriendlyChains f; // not fatal (0x52d5d0): the messages are kept and every chain field is cleared
        f.build({node("a", "b", 0), bad});
        CHECK(f.errors.size() == 2 && f.errors.back() == "Bad node in friendly chain");
        CHECK(f.chainId(0) == 0 && f.chainId(1) == 0 && f.prev(1) == -1 && f.inChainCount() == 2);
    }
    // A chain whose head is flagged 2 gets id 0 and is not a friendly chain at all.
    {
        FriendlyChains f;
        f.build({node("a", "b", 0, 2), node("b", "", 1)});
        CHECK(f.chainId(0) == 0 && f.chainId(1) == 0 && f.inChainCount() == 0);
    }

    if (failures) {
        std::cerr << failures << " failures\n";
        return 1;
    }
    std::cout << "friendlychain: ok\n";
    return 0;
}
