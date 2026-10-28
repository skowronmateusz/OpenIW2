// Friendly chains: the ordered chains of path nodes allies follow behind the player (docs/spec/ai.md "Friendly chain").
// Load-time builder 0x52d630 (assign 0x52d4c0, sort comparator 0x52d580), nearest 0x518270, selection 0x518a70 and
// its helpers 0x518580 / 0x5186f0 / 0x5187e0 / 0x518860 / 0x518960, reservoir 0x5184c0.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iw2::game {

// What the builder reads of each path node, in node order.
struct ChainNodeIn {
    std::string name;    // targetname (+6)
    std::string target;  // target (+0xc)
    int spawnflags = 0;  // +4; bit 2 excludes the node from chains
    int typeNum = 0;     // +0; 0 is a bad node
    std::array<float, 3> origin{};
};

// Per-query hooks of the follower (the actor-dependent parts of the selection).
struct ChainEnv {
    std::function<bool(int)> ok;      // usable by this actor (0x518f20) and not taken by another (0x518510)
    std::function<int64_t(int)> hold; // node +0x44, the claim hold time
    int64_t now = 0;                  // level time
};

class FriendlyChains {
public:
    // Throws std::runtime_error with the exe's fatal messages (multiple nodes target a node, a node targets itself,
    // a chain loop). A bad node (type 0) in a chain is not fatal: its messages go to `errors` and the chains are cleared.
    void build(const std::vector<ChainNodeIn>& nodes);

    int count() const { return int(chain_.size()); }
    int chainId(int n) const { return chain_[size_t(n)]; }  // +0x34: 0 none, -1 marker
    int pos(int n) const { return pos_[size_t(n)]; }        // +0x36
    int prev(int n) const { return prev_[size_t(n)]; }      // +0x38
    int inChainCount() const { return inChain_; }           // 0x1c8da08
    const std::vector<int>& sorted() const { return sorted_; }
    int sortedIndex(int n) const { return inv_[size_t(n)]; }

    // 0x518270: the chain node nearest to `p` on the chain of `node` (-1 = chain 0), -1 when there is none.
    int nearest(int node, const std::array<float, 3>& p) const;
    // The first node in sorted order whose targetname is `name` (0x518bf0), -1 when none.
    int findByName(const std::string& name) const;
    // 0x518a70: the node a follower of leader node `L` stands at; `cur` is its current chain node (-1 none).
    int choose(int L, int minOff, int maxOff, int cur, bool fallback, const ChainEnv& env);

    std::vector<std::string> errors; // the non-fatal bad-node messages of the last build (0x52d8e9, 0x52d90d)
    uint32_t seed = 1; // the exe's own LCG (0x5a56f4); the stream is not reproducible.

private:
    struct Res {
        int best = -1;
        int count = 0;
    };
    void reservoir(int cand, Res& r);
    int sub580(int lo, int hi, int n, const ChainEnv& env);
    int f5186f0(int n, int lo, int hi, const ChainEnv& env);
    int f5187e0(int hi, int n, int lo, const ChainEnv& env) const;
    int f860(int lo, int hi, int n, const ChainEnv& env) const;
    int f960(int lo, int hi, int n, const ChainEnv& env) const;

    std::vector<ChainNodeIn> in_;
    std::vector<int> chain_, pos_, prev_;
    std::vector<int> sorted_, inv_;
    int inChain_ = 0;
};

} // namespace iw2::game
