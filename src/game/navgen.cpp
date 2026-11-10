// Navigation graph generated from map collision (docs/spec/mp-bots.md, "Navigation graph"). OpenIW2 extension.
#include "game/navgen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace iw2::game {

namespace {

using V = std::array<float, 3>;

// The player's clip mask without the bodies of players and actors: the graph describes the static world.
constexpr uint32_t kNavMask = Contents::PlayerClipMask & ~(Contents::PlayerBody | 0x4000u);
const V kBoxMins{-15, -15, 0};
const V kBoxMaxs{15, 15, 70};
const V kCrouchMaxs{15, 15, 50}; // the crouched box top (docs/spec/pmove.md)
const V kZero{0, 0, 0};

TraceResult trace(const CollisionWorld& w, const V& a, const V& b, const V& mins, const V& maxs) {
    return w.trace(a, b, mins, maxs, kNavMask, -1, true, false);
}

bool pointSolid(const CollisionWorld& w, const V& p) {
    return trace(w, p, {p[0], p[1], p[2] - 0.5f}, kZero, kZero).startSolid;
}

// Ladder surfaces (surface flag 0x8, PM_CheckLadderMove 0x50bd90) and mantle brushes (contents 0x1000000 with the
// MANTLE_ON / MANTLE_OVER surface flags 0x2000000 / 0x4000000, 0x5257f0), docs/spec/pmove.md.
constexpr uint32_t kLadderSurf = 0x8;
constexpr uint32_t kMantleContents = 0x1000000, kMantleSurf = 0x2000000 | 0x4000000;
constexpr float kRad = 3.14159265358979f / 180.0f;

} // namespace

PlayerInput navTraverseInput(const NavLink& link, float fromZ, float toZ, const PlayerState& ps, bool first) {
    PlayerInput in;
    in.yaw = link.yaw;
    in.pitch = -60.0f; // looking up: the ladder climb speed follows the view pitch (0x50c070)
    in.forward = 1.0f;
    if ((link.flags & NavLinkFlag::Ladder) && toZ < fromZ)
        // Down: backward off the top facing the ladder; falling past its top edge forward again (the airborne grab sweeps
        // 30 units along the facing, 0x50bd90); on the ladder backward with the view up climbs down.
        in.forward = (!ps.onGround && !ps.onLadder) ? 1.0f : -1.0f;
    if (link.flags & NavLinkFlag::Leap) in.jump = first || !ps.onGround || ps.mantling; // pressed, held in the air
    else if (link.flags & NavLinkFlag::Mantle) in.jump = (ps.mantleFlags & 8u) != 0;   // once the mantle check found a ledge
    return in;
}

std::optional<V> navSimulateTraverse(const CollisionWorld& w, const V& start, uint8_t flags, float yaw, bool down,
                                     const MantleAnims* mantle, float maxSecs, bool* mantledOut) {
    PlayerState ps;
    ps.origin = start;
    ps.timeMs = 10000; // no jump in the last 300 ms (the ladder grab gate)
    NavLink l;
    l.flags = flags;
    l.yaw = yaw;
    MoveParams mp = multiplayerMoveParams(); // the bots move with the MP dvar defaults (docs/spec/mp-pmove.md)
    mp.traceMask = kNavMask;
    mp.mantle = (flags & (NavLinkFlag::Mantle | NavLinkFlag::Leap)) ? mantle : nullptr;
    bool mantled = false;
    bool used = false;
    int after = 0;
    const int frames = int(maxSecs / 0.05f);
    for (int f = 0; f < frames; ++f) {
        PlayerInput in = navTraverseInput(l, down ? 1.0f : 0.0f, down ? 0.0f : 1.0f, ps, f == 0);
        if (after) in.jump = false;
        if (after >= 4) in.forward = 0;
        playerMove(w, ps, in, 0.05f, mp);
        const bool special = (flags & NavLinkFlag::Ladder) ? ps.onLadder : (flags & NavLinkFlag::Leap) ? (ps.mantling || !ps.onGround) : ps.mantling;
        if (ps.mantling) mantled = true;
        if (special) {
            used = true;
            after = 0;
        } else if (used && ps.onGround) {
            // Off the ladder or the mantle: a few more steps forward onto the floor, then stand still.
            if (++after >= 12) {
                if ((flags & NavLinkFlag::Leap) && start[2] - ps.origin[2] > 190.0f) return std::nullopt; // fall damage
                if (mantledOut) *mantledOut = mantled;
                return ps.origin;
            }
        } else if (!used && f >= ((flags & NavLinkFlag::Leap) ? 4 : 40)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

V navDropToFloor(const CollisionWorld& w, const V& p) {
    const TraceResult tr = trace(w, {p[0], p[1], p[2] + 1.0f}, {p[0], p[1], p[2] - 256.0f}, kBoxMins, kBoxMaxs);
    return tr.startSolid || tr.fraction >= 1.0f ? p : tr.endPos;
}

namespace {
bool walkTestBox(const CollisionWorld& w, const V& a, const V& b, const NavParams& p, uint8_t& flags, const V& kBoxMaxs) {
    flags = 0;
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float dist = std::sqrt(dx * dx + dy * dy);
    const int n = std::max(1, int(std::ceil(dist / p.walkStep)));
    V cur = a;
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) / float(n);
        const float nx = a[0] + dx * t, ny = a[1] + dy * t;
        bool moved = false;
        for (const float lift : {p.stepHeight, p.jumpHeight}) {
            const TraceResult up = trace(w, cur, {cur[0], cur[1], cur[2] + lift}, kBoxMins, kBoxMaxs);
            if (up.startSolid) return false;
            const V top = up.endPos;
            const TraceResult h = trace(w, top, {nx, ny, top[2]}, kBoxMins, kBoxMaxs);
            if (h.startSolid || h.fraction < 1.0f) continue;
            const TraceResult down = trace(w, h.endPos, {nx, ny, h.endPos[2] - (top[2] - cur[2]) - p.maxDrop}, kBoxMins, kBoxMaxs);
            if (down.startSolid || down.fraction >= 1.0f || down.normal[2] < p.minNormalZ) return false;
            const float rise = down.endPos[2] - cur[2];
            if (rise > p.stepHeight + 0.5f) flags |= NavLinkFlag::Jump;
            if (-rise > p.stepHeight + 0.5f) flags |= NavLinkFlag::Drop;
            cur = down.endPos;
            moved = true;
            break;
        }
        if (!moved) return false;
    }
    return std::fabs(cur[2] - b[2]) <= 8.0f;
}
} // namespace

bool navWalkTest(const CollisionWorld& w, const V& a, const V& b, const NavParams& p, uint8_t& flags) {
    if (walkTestBox(w, a, b, p, flags, kBoxMaxs)) return true;
    // A way only the crouched box fits through (a low door or passage): a Crouch link.
    if (!walkTestBox(w, a, b, p, flags, kCrouchMaxs)) return false;
    if (flags & NavLinkFlag::Jump) return false; // a crouched player does not jump: the ledge stays out of reach
    flags |= NavLinkFlag::Crouch;
    return true;
}

// The node of a seed (a spawn point): the nearest node within 160 units that the body reaches from the dropped seed in
// a straight line (not one behind the wall of the room the spawn is in), else the nearest one.
int seedNode(const CollisionWorld& w, const NavGraph& g, const V& seed) {
    const V s = navDropToFloor(w, seed);
    std::vector<int> close = g.within(s, 160.0f);
    for (int k : close) {
        const V& q = g.nodes[size_t(k)];
        if (std::fabs(q[2] - s[2]) > 40.0f) continue;
        const TraceResult tr = trace(w, {s[0], s[1], s[2] + 1.0f}, {q[0], q[1], std::max(q[2], s[2]) + 1.0f}, {-15, -15, 18}, {15, 15, 40});
        if (tr.fraction >= 1.0f && !tr.startSolid) return k;
    }
    return g.nearest(s, 160.0f);
}

NavGraph generateNavGraph(const CollisionWorld& w, const V& mins, const V& maxs, const std::vector<V>& seeds, const NavParams& p) {
    NavGraph g;
    g.cell = p.cell;
    g.gridOrigin = {std::floor(mins[0] / p.cell) * p.cell, std::floor(mins[1] / p.cell) * p.cell};
    const int nx = int(std::ceil((maxs[0] - g.gridOrigin[0]) / p.cell)) + 1;
    const int ny = int(std::ceil((maxs[1] - g.gridOrigin[1]) / p.cell)) + 1;
    std::vector<V> nodes;
    std::vector<std::array<int, 2>> cellOf;
    // 1. Floors of every column: point traces down from the top; each walkable hit (slope within minNormalZ) places the
    //    standing box on it when the box fits; below a hit the scan goes on from the first free point under the floor.
    for (int cy = 0; cy < ny; ++cy)
        for (int cx = 0; cx < nx; ++cx) {
            const float x = g.gridOrigin[0] + float(cx) * p.cell, y = g.gridOrigin[1] + float(cy) * p.cell;
            float z = maxs[2] - 1.0f;
            std::vector<float> floors;
            for (int guard = 0; guard < 512 && z > mins[2] && int(floors.size()) < p.maxFloorsPerColumn; ++guard) {
                const TraceResult tr = trace(w, {x, y, z}, {x, y, mins[2]}, kZero, kZero);
                if (tr.startSolid) {
                    z -= 16.0f;
                    continue;
                }
                if (tr.fraction >= 1.0f) break;
                const float hz = tr.endPos[2];
                if (tr.normal[2] >= p.minNormalZ) {
                    // The box comes down from a step above the hit; under a low ceiling (a room barely taller than
                    // the box, as the inside of a goods wagon) from just above it.
                    TraceResult b = trace(w, {x, y, hz + p.stepHeight}, {x, y, hz - 4.0f}, kBoxMins, kBoxMaxs);
                    if (b.startSolid) b = trace(w, {x, y, hz + 1.0f}, {x, y, hz - 4.0f}, kBoxMins, kBoxMaxs);
                    // Under a lower ceiling the crouched box (a floor reached crouched, linked with Crouch links).
                    if (b.startSolid) b = trace(w, {x, y, hz + 1.0f}, {x, y, hz - 4.0f}, kBoxMins, kCrouchMaxs);
                    if (!b.startSolid && b.fraction < 1.0f && b.normal[2] >= p.minNormalZ) {
                        const float fz = b.endPos[2];
                        if (std::none_of(floors.begin(), floors.end(), [&](float f) { return std::fabs(f - fz) < 16.0f; })) {
                            floors.push_back(fz);
                            nodes.push_back({x, y, fz});
                            cellOf.push_back({cx, cy});
                        }
                    }
                }
                z = hz - 2.0f;
                for (int k = 0; k < 64 && z > mins[2] && pointSolid(w, {x, y, z}); ++k) z -= 8.0f;
            }
        }
    // 2. Links to the nodes of the 8 neighbouring columns that pass the walk test (both directions tested separately:
    //    drops are one-way).
    std::unordered_map<int64_t, std::vector<int>> byCell;
    auto key = [](int cx, int cy) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cy)); };
    for (size_t i = 0; i < nodes.size(); ++i) byCell[key(cellOf[i][0], cellOf[i][1])].push_back(int(i));
    std::vector<std::vector<NavLink>> links(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                auto it = byCell.find(key(cellOf[i][0] + dx, cellOf[i][1] + dy));
                if (it == byCell.end()) continue;
                for (int j : it->second) {
                    const float dz = nodes[size_t(j)][2] - nodes[i][2];
                    if (dz > p.jumpHeight || dz < -p.maxDrop) continue;
                    uint8_t flags = 0;
                    if (!navWalkTest(w, nodes[i], nodes[size_t(j)], p, flags)) continue;
                    const float ddx = nodes[size_t(j)][0] - nodes[i][0], ddy = nodes[size_t(j)][1] - nodes[i][1];
                    links[i].push_back({uint32_t(j), std::sqrt(ddx * ddx + ddy * ddy + dz * dz), flags});
                }
            }
    // 2a. Over a column without a floor node (a door sill or a thin beam the point trace finds no standing place on): a
    //     walk link to the node two columns on in that direction when there is no link into the column between.
    for (size_t i = 0; i < nodes.size(); ++i)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                if (std::any_of(links[i].begin(), links[i].end(), [&](const NavLink& l) {
                        return cellOf[l.target][0] == cellOf[i][0] + dx && cellOf[l.target][1] == cellOf[i][1] + dy;
                    }))
                    continue;
                auto it = byCell.find(key(cellOf[i][0] + 2 * dx, cellOf[i][1] + 2 * dy));
                if (it == byCell.end()) continue;
                for (int j : it->second) {
                    const float dz = nodes[size_t(j)][2] - nodes[i][2];
                    if (dz > p.jumpHeight || dz < -p.maxDrop) continue;
                    uint8_t flags = 0;
                    if (!navWalkTest(w, nodes[i], nodes[size_t(j)], p, flags)) continue;
                    const float ddx = nodes[size_t(j)][0] - nodes[i][0], ddy = nodes[size_t(j)][1] - nodes[i][1];
                    links[i].push_back({uint32_t(j), std::sqrt(ddx * ddx + ddy * ddy + dz * dz), flags});
                }
            }
    // 2b. Ladder and mantle links: from every node, the walls in 8 directions are probed for a ladder surface (the
    //     player box 6 narrower, feet at +8, as the ladder grab) or a mantle brush (a thin box as the mantle check); for each
    //     new face pmove is run with the traversal input and a link is made to the node where the player ends.
    if (p.special) {
        NavGraph idx;
        idx.nodes = nodes;
        idx.cell = g.cell;
        idx.gridOrigin = g.gridOrigin;
        idx.buildIndex();
        auto addLink = [&](size_t from, const V& end, uint8_t flag, float yaw) {
            const int t = idx.nearest(end, 32.0f);
            if (t < 0 || size_t(t) == from) return;
            const V& q = nodes[size_t(t)];
            if (std::fabs(q[2] - nodes[from][2]) <= p.stepHeight) return; // a plain walk does as well
            for (const NavLink& l : links[from])
                if (l.target == uint32_t(t)) return;
            const float ddx = q[0] - nodes[from][0], ddy = q[1] - nodes[from][1], ddz = q[2] - nodes[from][2];
            links[from].push_back({uint32_t(t), std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz), flag, yaw});
        };
        const V ladMins{-9, -9, 8}, ladMaxs{9, 9, 70};
        const V manMins{-0.1f, -0.1f, 0}, manMaxs{0.1f, 0.1f, 70};
        for (size_t i = 0; i < nodes.size(); ++i) {
            const V& o = nodes[i];
            std::vector<float> yaws[2];
            for (int k = 0; k < 8; ++k) {
                const float a = float(k) * 45.0f * kRad;
                const V d{std::cos(a), std::sin(a), 0};
                TraceResult tr = trace(w, o, {o[0] + d[0] * 24, o[1] + d[1] * 24, o[2]}, ladMins, ladMaxs);
                if (!tr.startSolid && tr.fraction < 1.0f && (tr.surfaceFlags & kLadderSurf) && std::fabs(tr.normal[2]) < 0.7f)
                    yaws[0].push_back(std::atan2(-tr.normal[1], -tr.normal[0]) / kRad);
                tr = w.trace({o[0] - d[0] * 14.9f, o[1] - d[1] * 14.9f, o[2]}, {o[0] + d[0] * 40, o[1] + d[1] * 40, o[2]}, manMins,
                             manMaxs, kMantleContents, -1, true, false);
                if (!tr.startSolid && tr.fraction < 1.0f && (tr.surfaceFlags & kMantleSurf) && std::fabs(tr.normal[2]) < 0.7f)
                    yaws[1].push_back(std::atan2(-tr.normal[1], -tr.normal[0]) / kRad);
            }
            for (int kind = 0; kind < 2; ++kind) {
                if (kind == 1 && !(p.mantle && p.mantle->loaded())) continue;
                std::vector<float> done;
                for (float y : yaws[kind]) {
                    if (std::any_of(done.begin(), done.end(), [&](float e) { return std::fabs(std::remainder(e - y, 360.0f)) < 10.0f; }))
                        continue;
                    done.push_back(y);
                    const uint8_t flag = kind == 0 ? NavLinkFlag::Ladder : NavLinkFlag::Mantle;
                    const auto end = navSimulateTraverse(w, o, flag, y, false, p.mantle);
                    if (!end) continue;
                    const size_t before = links[i].size();
                    addLink(i, *end, flag, y);
                    if (kind == 0 && links[i].size() > before) {
                        // The way down: from the top node, backward off the edge with the same view yaw.
                        const size_t top = links[i].back().target;
                        if (const auto back = navSimulateTraverse(w, nodes[top], flag, y, true, nullptr)) addLink(top, *back, flag, y);
                    }
                }
            }
            // Leaps: towards every neighbouring column without a plain (flagless) link, a standing jump forward.
            if (p.leaps) {
                for (int k = 0; k < 8; ++k) {
                    static constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                    bool plain = false;
                    for (const NavLink& l : links[i])
                        if (!l.flags && cellOf[l.target][0] == cellOf[i][0] + kDx[k] && cellOf[l.target][1] == cellOf[i][1] + kDy[k]) plain = true;
                    if (plain) continue;
                    bool mantled = false;
                    const float y = float(k) * 45.0f;
                    const auto end = navSimulateTraverse(w, o, NavLinkFlag::Leap, y, false, p.mantle, 2.0f, &mantled);
                    if (!end) continue;
                    const int t = idx.nearest(*end, 24.0f);
                    if (t < 0 || size_t(t) == i) continue;
                    const V& q = nodes[size_t(t)];
                    const float ddx = q[0] - o[0], ddy = q[1] - o[1], ddz = q[2] - o[2];
                    if (ddz < p.stepHeight + 0.5f && ddx * ddx + ddy * ddy < 40.0f * 40.0f) continue; // the walk links do
                    if (std::any_of(links[i].begin(), links[i].end(), [&](const NavLink& l) { return l.target == uint32_t(t); })) continue;
                    links[i].push_back({uint32_t(t), std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz),
                                        uint8_t(NavLinkFlag::Leap | (mantled ? NavLinkFlag::Mantle : 0)), y});
                }
            }
        }
    }
    // 3. Keep the strongly connected component that holds the most seed nodes: every kept node reaches every other one
    //    (islands, roofs reached only by a drop and other one-way dead ends are removed).
    std::vector<char> keep(nodes.size(), 1);
    if (!seeds.empty() && !nodes.empty()) {
        NavGraph tmp;
        tmp.nodes = nodes;
        tmp.cell = g.cell;
        tmp.gridOrigin = g.gridOrigin;
        tmp.buildIndex();
        const size_t n = nodes.size();
        std::vector<std::vector<uint32_t>> back(n);
        for (size_t i = 0; i < n; ++i)
            for (const NavLink& l : links[i]) back[l.target].push_back(uint32_t(i));
        // Kosaraju: finish order on the forward graph (iterative), components on the reversed graph.
        std::vector<uint32_t> order;
        order.reserve(n);
        std::vector<char> vis(n, 0);
        std::vector<std::pair<uint32_t, size_t>> stack;
        for (size_t s0 = 0; s0 < n; ++s0) {
            if (vis[s0]) continue;
            vis[s0] = 1;
            stack.push_back({uint32_t(s0), 0});
            while (!stack.empty()) {
                auto& [v, k] = stack.back();
                if (k < links[v].size()) {
                    const uint32_t t = links[v][k++].target;
                    if (!vis[t]) vis[t] = 1, stack.push_back({t, 0});
                } else {
                    order.push_back(v);
                    stack.pop_back();
                }
            }
        }
        std::vector<int> comp(n, -1);
        int comps = 0;
        for (size_t oi = n; oi-- > 0;) {
            const uint32_t r = order[oi];
            if (comp[r] >= 0) continue;
            std::vector<uint32_t> st{r};
            comp[r] = comps;
            while (!st.empty()) {
                const uint32_t v = st.back();
                st.pop_back();
                for (uint32_t u : back[v])
                    if (comp[u] < 0) comp[u] = comps, st.push_back(u);
            }
            ++comps;
        }
        std::vector<int> seedCount(size_t(comps), 0), sizeOf(size_t(comps), 0);
        for (size_t i = 0; i < n; ++i) ++sizeOf[size_t(comp[i])];
        for (const V& s : seeds) {
            const int k = seedNode(w, tmp, s);
            if (k >= 0) ++seedCount[size_t(comp[size_t(k)])];
        }
        int best = 0;
        for (int c = 1; c < comps; ++c)
            if (seedCount[size_t(c)] > seedCount[size_t(best)] ||
                (seedCount[size_t(c)] == seedCount[size_t(best)] && sizeOf[size_t(c)] > sizeOf[size_t(best)]))
                best = c;
        for (size_t i = 0; i < n; ++i) keep[i] = comp[i] == best;
        // Exit-only nodes (docs/spec/mp-bots.md "Off-graph spawn points"): the nodes outside the main part that a seed's
        // node off the main part reaches and that lead into the main part (the way out of a spawn spot, nothing else).
        std::vector<char> leads(n, 0);
        std::vector<uint32_t> st;
        for (size_t i = 0; i < n; ++i)
            if (keep[i] == 1) st.push_back(uint32_t(i));
        while (!st.empty()) {
            const uint32_t v = st.back();
            st.pop_back();
            for (uint32_t u : back[v])
                if (!keep[u] && !leads[u]) leads[u] = 1, st.push_back(u);
        }
        // A seed in a room the walk, jump and special links lead into but not out of (a spawn room entered over a
        // window sill by a standing jump, mp_farmhouse): the shortest way to the main part over links in either
        // direction; a link used backwards is added reversed as a Jump link (a running jump where the standing one
        // came in), so the room gets a way out.
        for (const V& s : seeds) {
            const int k = seedNode(w, tmp, s);
            if (k < 0 || keep[size_t(k)] || leads[size_t(k)]) continue;
            std::vector<int> prev(n, -2);
            std::vector<char> rev(n, 0);
            std::vector<uint32_t> q{uint32_t(k)};
            prev[size_t(k)] = -1;
            int hit = -1;
            for (size_t qi = 0; qi < q.size() && hit < 0 && qi < 20000; ++qi) {
                const uint32_t v = q[qi];
                auto visit = [&](uint32_t u, bool backward) {
                    if (prev[u] != -2 || hit >= 0) return;
                    prev[u] = int(v), rev[u] = backward;
                    if (keep[u] == 1 || leads[u]) hit = int(u);
                    q.push_back(u);
                };
                for (const NavLink& l : links[v]) visit(l.target, false);
                for (uint32_t u : back[v]) visit(u, true);
            }
            if (hit < 0) continue;
            for (int u = hit; prev[size_t(u)] >= 0; u = prev[size_t(u)]) {
                const uint32_t v = uint32_t(prev[size_t(u)]);
                if (rev[size_t(u)]) {
                    const V& a = nodes[v];
                    const V& b2 = nodes[size_t(u)];
                    const float ddx = b2[0] - a[0], ddy = b2[1] - a[1], ddz = b2[2] - a[2];
                    links[v].push_back({uint32_t(u), std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz), NavLinkFlag::Jump});
                    back[size_t(u)].push_back(v);
                }
                leads[v] = 1;
            }
        }
        for (const V& s : seeds) {
            const int k = seedNode(w, tmp, s);
            if (k >= 0 && leads[size_t(k)] && !keep[size_t(k)]) keep[size_t(k)] = 2, st.push_back(uint32_t(k));
        }
        while (!st.empty()) {
            const uint32_t v = st.back();
            st.pop_back();
            for (const NavLink& l : links[v])
                if (leads[l.target] && !keep[l.target]) keep[l.target] = 2, st.push_back(l.target);
        }
    }
    std::vector<int> remap(nodes.size(), -1);
    for (char pass = 1; pass <= 2; ++pass) { // the main nodes first, then the exit-only ones
        for (size_t i = 0; i < nodes.size(); ++i)
            if (keep[i] == pass) {
                remap[i] = int(g.nodes.size());
                g.nodes.push_back(nodes[i]);
            }
        if (pass == 1) g.mainCount = g.nodes.size();
    }
    g.links.resize(g.nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (remap[i] < 0) continue;
        for (const NavLink& l : links[i])
            if (remap[l.target] >= 0) g.links[size_t(remap[i])].push_back({uint32_t(remap[l.target]), l.distance, l.flags, l.yaw});
    }
    g.buildIndex();
    return g;
}

void NavGraph::buildIndex() {
    columns_.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int cx = int(std::lround((nodes[i][0] - gridOrigin[0]) / cell));
        const int cy = int(std::lround((nodes[i][1] - gridOrigin[1]) / cell));
        columns_[key(cx, cy)].push_back(int(i));
    }
}

int NavGraph::nearest(const V& pos, float maxDist, bool mainOnly) const {
    const int cx = int(std::lround((pos[0] - gridOrigin[0]) / cell));
    const int cy = int(std::lround((pos[1] - gridOrigin[1]) / cell));
    const int r = std::max(1, int(std::ceil(maxDist / cell)));
    int best = -1;
    float bestD = maxDist * maxDist;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            auto it = columns_.find(key(cx + dx, cy + dy));
            if (it == columns_.end()) continue;
            for (int n : it->second) {
                if (mainOnly && size_t(n) >= mainCount) continue;
                const auto& q = nodes[size_t(n)];
                const float ex = q[0] - pos[0], ey = q[1] - pos[1], ez = (q[2] - pos[2]) * 2.0f;
                const float d = ex * ex + ey * ey + ez * ez;
                if (d < bestD) bestD = d, best = n;
            }
        }
    return best;
}

std::vector<int> NavGraph::within(const V& pos, float maxDist) const {
    const int cx = int(std::lround((pos[0] - gridOrigin[0]) / cell));
    const int cy = int(std::lround((pos[1] - gridOrigin[1]) / cell));
    const int r = std::max(1, int(std::ceil(maxDist / cell)));
    std::vector<std::pair<float, int>> found;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            auto it = columns_.find(key(cx + dx, cy + dy));
            if (it == columns_.end()) continue;
            for (int n : it->second) {
                const auto& q = nodes[size_t(n)];
                const float ex = q[0] - pos[0], ey = q[1] - pos[1], ez = (q[2] - pos[2]) * 2.0f;
                const float d = ex * ex + ey * ey + ez * ez;
                if (d < maxDist * maxDist) found.push_back({d, n});
            }
        }
    std::sort(found.begin(), found.end());
    std::vector<int> out;
    out.reserve(found.size());
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

PathGraph NavGraph::toPathGraph() const {
    std::vector<std::vector<PathLink>> l(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        for (const NavLink& k : links[i]) l[i].push_back({uint16_t(k.target), k.distance});
    return PathGraph(nodes, std::move(l));
}

namespace {
constexpr char kMagic[8] = {'I', 'W', '2', 'N', 'A', 'V', 0, 0};
constexpr uint32_t kVersion = 4;
template <class T> void put(std::ofstream& f, const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <class T> bool get(std::ifstream& f, T& v) { return bool(f.read(reinterpret_cast<char*>(&v), sizeof(T))); }
} // namespace

bool saveNavGraph(const std::string& path, const NavGraph& g, uint64_t key) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    // A per-process temporary name: several processes (parallel tests, a server and a client) may build the same cache
    // at once, and a shared temporary file would interleave their writes before the rename.
    static std::atomic<unsigned> serial{0};
    const std::string tmp = path + "." + std::to_string(::getpid()) + "." + std::to_string(serial++) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(kMagic, 8);
        put(f, kVersion);
        put(f, key);
        put(f, g.cell);
        put(f, g.gridOrigin[0]);
        put(f, g.gridOrigin[1]);
        put(f, uint32_t(g.nodes.size()));
        put(f, uint32_t(g.mainCount));
        for (const auto& n : g.nodes)
            for (float c : n) put(f, c);
        for (const auto& ls : g.links) {
            put(f, uint32_t(ls.size()));
            for (const NavLink& l : ls) {
                put(f, l.target);
                put(f, l.distance);
                put(f, l.flags);
                put(f, l.yaw);
            }
        }
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (!ec) return true;
    std::filesystem::remove(tmp, ec);
    return false;
}

std::optional<NavGraph> loadNavGraph(const std::string& path, uint64_t key) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return std::nullopt;
    uint32_t version = 0, n = 0, mainCount = 0;
    uint64_t k = 0;
    NavGraph g;
    if (!get(f, version) || version != kVersion || !get(f, k) || k != key) return std::nullopt;
    if (!get(f, g.cell) || !get(f, g.gridOrigin[0]) || !get(f, g.gridOrigin[1]) || !get(f, n) || n > 1000000u || !get(f, mainCount) ||
        mainCount > n)
        return std::nullopt;
    g.mainCount = mainCount;
    g.nodes.resize(n);
    for (auto& node : g.nodes)
        for (float& c : node)
            if (!get(f, c)) return std::nullopt;
    g.links.resize(n);
    for (auto& ls : g.links) {
        uint32_t c = 0;
        if (!get(f, c) || c > 64) return std::nullopt;
        ls.resize(c);
        for (NavLink& l : ls)
            if (!get(f, l.target) || !get(f, l.distance) || !get(f, l.flags) || !get(f, l.yaw) || l.target >= n) return std::nullopt;
    }
    g.buildIndex();
    return g;
}

std::string navCacheDir() {
    if (const char* e = std::getenv("IW2_NAV_CACHE"); e && *e) return e;
#ifdef __APPLE__
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/Library/Caches/openiw2/nav";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/openiw2/nav";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/openiw2/nav";
#endif
    return {};
}

} // namespace iw2::game
