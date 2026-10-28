#include "game/friendlychain.h"

#include <algorithm>
#include <cfloat>
#include <stdexcept>

namespace iw2::game {

namespace {

std::string fmt3(const std::array<float, 3>& o) {
    return std::to_string(o[0]) + " " + std::to_string(o[1]) + " " + std::to_string(o[2]);
}

} // namespace

// 0x52d630 with assign 0x52d4c0 (one pass per head, ids from 1) and the qsort of 0x52d580 (chain id descending, then
// position, predecessor ascending, type descending; the exe's qsort order for equal keys is not reproduced: stable here).
void FriendlyChains::build(const std::vector<ChainNodeIn>& nodes) {
    in_ = nodes;
    const int n = int(nodes.size());
    chain_.assign(size_t(n), 0);
    pos_.assign(size_t(n), 0);
    prev_.assign(size_t(n), -1);
    for (int i = 0; i < n; ++i) {
        const ChainNodeIn& a = nodes[size_t(i)];
        if (a.name.empty() || (a.spawnflags & 2)) continue;
        for (int j = 0; j < n; ++j) {
            const ChainNodeIn& b = nodes[size_t(j)];
            if (b.target.empty() || b.target != a.name) continue;
            if (prev_[size_t(i)] >= 0) throw std::runtime_error("multiple nodes target node with name '" + a.name + "'");
            if (j == i) throw std::runtime_error("node with name '" + a.name + "' targets itself");
            prev_[size_t(i)] = j;
            chain_[size_t(j)] = -1;
        }
    }
    for (int i = 0; i < n; ++i) {
        int c = i;
        for (int steps = 0; steps < n && c >= 0; ++steps) c = prev_[size_t(c)];
        if (c >= 0) throw std::runtime_error("found chain loop including node with name '" + nodes[size_t(i)].name + "'");
    }
    int nextId = 1;
    std::function<void(int, int, int)> assign = [&](int node, int id, int p) {
        chain_[size_t(node)] = id;
        pos_[size_t(node)] = p;
        const ChainNodeIn& a = nodes[size_t(node)];
        if (a.target.empty()) return;
        for (int k = 0; k < n; ++k) {
            const ChainNodeIn& b = nodes[size_t(k)];
            if (b.name.empty() || b.name != a.target) continue;
            if (k == node) throw std::runtime_error("node '" + a.target + "' targets itself");
            if (!(b.spawnflags & 2)) assign(k, id, p == 0 ? 0 : p + 1);
        }
    };
    for (int i = 0; i < n; ++i) {
        if (chain_[size_t(i)] != -1 || prev_[size_t(i)] != -1) continue;
        if (nodes[size_t(i)].spawnflags & 2) assign(i, 0, 0);
        else assign(i, nextId++, 1);
    }
    sorted_.resize(size_t(n));
    for (int i = 0; i < n; ++i) sorted_[size_t(i)] = i;
    std::stable_sort(sorted_.begin(), sorted_.end(), [&](int a, int b) {
        if (chain_[size_t(a)] != chain_[size_t(b)]) return chain_[size_t(a)] > chain_[size_t(b)];
        if (pos_[size_t(a)] != pos_[size_t(b)]) return pos_[size_t(a)] < pos_[size_t(b)];
        if (prev_[size_t(a)] != prev_[size_t(b)]) return prev_[size_t(a)] < prev_[size_t(b)];
        return nodes[size_t(a)].typeNum > nodes[size_t(b)].typeNum;
    });
    inv_.assign(size_t(n), 0);
    for (int p = 0; p < n; ++p) inv_[size_t(sorted_[size_t(p)])] = p;
    inChain_ = 0;
    while (inChain_ < n && chain_[size_t(sorted_[size_t(inChain_)])] != 0) ++inChain_;
    // 0x52d8b0..0x52d917: every chain node of type 0 prints "Error: bad node [%d] in friendly chain at (%g %g %g)" (0x42c7f0
    // channel 1, not fatal); if there was one, 0x52d5d0 keeps the first path error "\nBad node in friendly chain" (0x1cb2bac,
    // printed again every 100 frames by 0x4da9b9 while a dvar at 0x1c1487c is set) and clears the chain fields of every
    // node (+0x34 id and +0x36 position 0, +0x38 predecessor -1). The returned chain node count (0x1c8da08) is kept.
    errors.clear();
    for (int p = 0; p < inChain_; ++p) {
        const ChainNodeIn& a = nodes[size_t(sorted_[size_t(p)])];
        if (a.typeNum == 0)
            errors.push_back("Error: bad node [" + std::to_string(sorted_[size_t(p)]) + "] in friendly chain at (" + fmt3(a.origin) + ")");
    }
    if (!errors.empty()) {
        errors.push_back("Bad node in friendly chain");
        std::fill(chain_.begin(), chain_.end(), 0);
        std::fill(pos_.begin(), pos_.end(), 0);
        std::fill(prev_.begin(), prev_.end(), -1);
    }
}

int FriendlyChains::nearest(int node, const std::array<float, 3>& p) const {
    const int id = node >= 0 ? chain_[size_t(node)] : 0;
    int best = -1;
    double bestD = FLT_MAX; // x87 extended in the exe
    for (int s = 0; s < inChain_; ++s) {
        const int c = sorted_[size_t(s)];
        if (chain_[size_t(c)] != id) continue;
        const auto& o = in_[size_t(c)].origin;
        const double dx = double(o[0]) - p[0], dy = double(o[1]) - p[1], dz = double(o[2]) - p[2];
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    return best;
}

int FriendlyChains::findByName(const std::string& name) const {
    for (int s = 0; s < inChain_; ++s)
        if (in_[size_t(sorted_[size_t(s)])].name == name) return sorted_[size_t(s)];
    return -1;
}

// 0x5184c0: keep the candidate with the greater position; among equals a reservoir sample (the exe's LCG).
void FriendlyChains::reservoir(int cand, Res& r) {
    if (r.best < 0) {
        r.best = cand;
        r.count = 1;
    } else if (pos_[size_t(cand)] != pos_[size_t(r.best)]) {
        if (pos_[size_t(cand)] > pos_[size_t(r.best)]) {
            r.best = cand;
            r.count = 1;
        }
    } else {
        ++r.count;
        seed = seed * 0x343fdu + 0x269ec3u;
        const uint32_t v = seed >> 17;
        if (!(int64_t(v) * r.count > 0x7fff)) r.best = cand;
    }
}

// 0x518580: the descendants of `n` up to min(hi, pos+1) positions, deepest first.
int FriendlyChains::sub580(int lo, int hi, int n, const ChainEnv& env) {
    const int top = std::min(hi, pos_[size_t(n)] + 1);
    Res r;
    for (int p = inv_[size_t(n)] + 1; p < inChain_; ++p) {
        const int c = sorted_[size_t(p)];
        if (chain_[size_t(c)] != chain_[size_t(n)] || pos_[size_t(c)] > top) return r.best;
        if (prev_[size_t(c)] != n) continue;
        if (pos_[size_t(c)] < hi) {
            const int sub = sub580(lo, hi, c, env);
            if (sub >= 0) reservoir(sub, r);
        }
        if (pos_[size_t(c)] == hi || (r.best < 0 && pos_[size_t(c)] >= lo)) {
            if (env.ok(c)) reservoir(c, r);
        }
    }
    return r.best;
}

// 0x5186f0: the nodes after `n` in sorted order within [lo, hi]; a direct successor ends the search with none.
int FriendlyChains::f5186f0(int n, int lo, int hi, const ChainEnv& env) {
    Res r;
    for (int p = inv_[size_t(n)] + 1; p < count(); ++p) {
        const int c = sorted_[size_t(p)];
        if (chain_[size_t(c)] != chain_[size_t(n)] || pos_[size_t(c)] > hi) break;
        if (prev_[size_t(c)] == n) return -1;
        if (pos_[size_t(c)] >= lo && env.ok(c)) reservoir(c, r);
    }
    return r.best;
}

// 0x5187e0: walk back towards the head from `n`.
int FriendlyChains::f5187e0(int hi, int n, int lo, const ChainEnv& env) const {
    int cur = n;
    while (pos_[size_t(cur)] > hi) {
        cur = prev_[size_t(cur)];
        if (cur < 0) return -1;
    }
    if (pos_[size_t(cur)] < lo) return -1;
    while (true) {
        if (env.ok(cur)) return cur;
        cur = prev_[size_t(cur)];
        if (cur < 0 || pos_[size_t(cur)] < lo) return -1;
    }
}

// 0x518860: the usable node nearest to the window [lo, hi] (in positions), scanning forward from `n`.
int FriendlyChains::f860(int lo, int hi, int n, const ChainEnv& env) const {
    int best = -1;
    int bestDist = 0x7fffffff;
    for (int p = inv_[size_t(n)]; p < inChain_; ++p) {
        const int c = sorted_[size_t(p)];
        if (chain_[size_t(c)] != chain_[size_t(n)]) return best;
        int d;
        if (pos_[size_t(c)] < lo) d = lo - pos_[size_t(c)];
        else if (pos_[size_t(c)] <= hi) continue;
        else d = pos_[size_t(c)] - hi;
        if (d > bestDist) continue;
        if (env.ok(c)) {
            bestDist = d;
            best = c;
            if (hi < pos_[size_t(c)]) return best;
        }
    }
    return best;
}

// 0x518960: as 0x518860, but nodes whose hold time has passed are skipped and the window itself has distance 0.
int FriendlyChains::f960(int lo, int hi, int n, const ChainEnv& env) const {
    int best = -1;
    int bestDist = 0x7fffffff;
    for (int p = inv_[size_t(n)]; p < inChain_; ++p) {
        const int c = sorted_[size_t(p)];
        if (chain_[size_t(c)] != chain_[size_t(n)]) return best;
        if (env.now > env.hold(c)) continue;
        int d;
        if (pos_[size_t(c)] < lo) d = lo - pos_[size_t(c)];
        else if (pos_[size_t(c)] > hi) d = pos_[size_t(c)] - hi;
        else d = 0;
        if (d > bestDist) continue;
        if (env.ok(c)) {
            bestDist = d;
            best = c;
            if (hi < pos_[size_t(c)]) return best;
        }
    }
    return best;
}

int FriendlyChains::choose(int L, int minOff, int maxOff, int cur, bool fallback, const ChainEnv& env) {
    const int lo = pos_[size_t(L)] + minOff, hi = pos_[size_t(L)] + maxOff;
    const int chain = chain_[size_t(L)];
    const bool onChain = cur >= 0 && chain_[size_t(cur)] == chain;
    int r;
    if (onChain) {
        if (pos_[size_t(cur)] == hi) return cur;
        if (pos_[size_t(cur)] < hi) {
            if ((r = sub580(lo, hi, cur, env)) >= 0) return r;
            if ((r = f5186f0(cur, lo, hi, env)) >= 0) return r;
            if (pos_[size_t(cur)] >= lo) return cur;
        }
        if (!fallback) return cur;
        if (pos_[size_t(cur)] >= lo && (r = f5187e0(hi, cur, lo, env)) >= 0) return r;
    }
    if (maxOff > 0 && (r = sub580(lo, hi, L, env)) >= 0) return r;
    if (minOff <= 0 && (r = f5187e0(hi, L, lo, env)) >= 0) return r;
    int head = L;
    while (prev_[size_t(head)] >= 0) head = prev_[size_t(head)];
    if (pos_[size_t(head)] < hi && (r = sub580(lo, hi, head, env)) >= 0) return r;
    if (onChain) return cur;
    if ((r = f860(lo, hi, head, env)) >= 0) return r;
    if (cur >= 0) return -1;
    return f960(lo, hi, head, env);
}

} // namespace iw2::game
