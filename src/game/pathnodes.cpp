#include "game/pathnodes.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace iw2::game {
namespace {

template <typename T>
T rd(std::span<const uint8_t> d, size_t& pos) {
    if (pos + sizeof(T) > d.size()) throw std::runtime_error("path data: truncated");
    T v;
    std::memcpy(&v, d.data() + pos, sizeof v);
    pos += sizeof v;
    return v;
}

} // namespace

PathGraph::PathGraph(std::span<const uint8_t> lump, std::vector<std::array<float, 3>> positions) : positions_(std::move(positions)) {
    if (lump.empty()) {
        links_.resize(positions_.size());
        return;
    }
    size_t pos = 0;
    const int32_t version = rd<int32_t>(lump, pos);
    if (version != 8) throw std::runtime_error("path data: version " + std::to_string(version));
    const int16_t count = rd<int16_t>(lump, pos);
    if (size_t(count) != positions_.size())
        throw std::runtime_error("Path node count mismatch in bsp file (" + std::to_string(count) + " vs " +
                                 std::to_string(positions_.size()) + " node entities)");
    links_.resize(size_t(count));
    for (int i = 0; i < count; ++i) {
        const int16_t n = rd<int16_t>(lump, pos);
        for (int k = 0; k < n; ++k) {
            PathLink l;
            l.target = rd<uint16_t>(lump, pos);
            l.distance = rd<float>(lump, pos);
            if (l.target >= count) throw std::runtime_error("path data: link target out of range");
            links_[size_t(i)].push_back(l);
        }
    }
    // Run-length coded bit matrix: a byte b < 0x80 means b zero bytes then one literal byte;
    // b >= 0x80 means ~b literal bytes.
    const size_t size = (size_t(count - 1) * size_t(count) + 7) / 8;
    vis_.assign(size, 0);
    size_t out = 0;
    while (out < size) {
        const uint8_t b = rd<uint8_t>(lump, pos);
        if (b & 0x80) {
            for (int k = 0; k < uint8_t(~b) && out < size; ++k) vis_[out++] = rd<uint8_t>(lump, pos);
        } else {
            out += b;
            if (out < size) vis_[out++] = rd<uint8_t>(lump, pos);
        }
    }
}

std::vector<int> PathGraph::findPath(int from, int to) const {
    const size_t n = positions_.size();
    if (from < 0 || to < 0 || size_t(from) >= n || size_t(to) >= n) return {};
    auto h = [&](int a) {
        const auto& p = positions_[size_t(a)];
        const auto& q = positions_[size_t(to)];
        return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    };
    std::vector<float> g(n, std::numeric_limits<float>::infinity());
    std::vector<int> prev(n, -1);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    g[size_t(from)] = 0;
    open.push({h(from), from});
    while (!open.empty()) {
        const auto [f, cur] = open.top();
        open.pop();
        if (cur == to) break;
        if (f - h(cur) > g[size_t(cur)] + 1e-3f) continue;
        for (const PathLink& l : links_[size_t(cur)]) {
            const float ng = g[size_t(cur)] + l.distance;
            if (ng < g[l.target]) {
                g[l.target] = ng;
                prev[l.target] = cur;
                open.push({ng + h(l.target), l.target});
            }
        }
    }
    if (from != to && prev[size_t(to)] < 0) return {};
    std::vector<int> path;
    for (int c = to; c >= 0; c = prev[size_t(c)]) {
        path.push_back(c);
        if (c == from) break;
    }
    return {path.rbegin(), path.rend()};
}

namespace {
const char* const kNodeClasses[] = {"node_pathnode", "node_cover_stand", "node_cover_crouch", "node_cover_crouch_window",
                                    "node_cover_prone", "node_cover_right", "node_cover_left", "node_cover_wide_right",
                                    "node_cover_wide_left", "node_concealment_stand", "node_concealment_crouch",
                                    "node_concealment_prone", "node_reacquire", "node_balcony", "node_scripted",
                                    "node_negotiation_begin", "node_negotiation_end", "node_turret"};
} // namespace

int pathNodeTypeNum(const std::string& classname) {
    for (size_t i = 0; i < std::size(kNodeClasses); ++i)
        if (classname == kNodeClasses[i]) return int(i) + 1;
    return 0;
}

std::string pathNodeTypeName(const std::string& classname) {
    static const char* const names[] = {"BAD NODE", "Path", "Cover Stand", "Cover Crouch", "Cover Crouch Window", "Cover Prone",
                                        "Cover Right", "Cover Left", "Cover Wide Right", "Cover Wide Left", "Conceal Stand",
                                        "Conceal Crouch", "Conceal Prone", "Reacquire", "Balcony", "Scripted", "Begin", "End",
                                        "Turret"};
    const int t = pathNodeTypeNum(classname);
    return t ? names[t] : std::string();
}

} // namespace iw2::game
