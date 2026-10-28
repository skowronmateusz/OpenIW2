// Path node graph from lump 38 of .d3dbsp (docs/spec/pathnodes.md) and A* search.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace iw2::game {

// Script value of a path node's "type" field for its classname (spawn table 0x56fea8 maps node_* classes to
// types 1..18; names from the table at 0x5a9368, e.g. "Cover Crouch", "Turret"). Empty for an unknown class.
std::string pathNodeTypeName(const std::string& classname);
// The node's type number (+0x0 of the runtime node): 1 node_pathnode … 18 node_turret, 0 for an unknown class.
int pathNodeTypeNum(const std::string& classname);

struct PathLink {
    uint16_t target = 0;
    float distance = 0;
};

class PathGraph {
public:
    PathGraph() = default;
    // `positions` are the node origins in entity-string order; the lump must describe the same count.
    PathGraph(std::span<const uint8_t> lump, std::vector<std::array<float, 3>> positions);
    // A graph given directly (tests).
    PathGraph(std::vector<std::array<float, 3>> positions, std::vector<std::vector<PathLink>> links)
        : positions_(std::move(positions)), links_(std::move(links)) {}

    size_t size() const { return positions_.size(); }
    const std::array<float, 3>& position(size_t i) const { return positions_[i]; }
    const std::vector<PathLink>& links(size_t i) const { return links_[i]; }
    // Precomputed node-to-node visibility (bit matrix of n·(n−1) bits; pair indexing not specified yet).
    const std::vector<uint8_t>& visibilityBits() const { return vis_; }

    // A* from node `from` to node `to`; returns node indices including both ends, empty if unreachable.
    std::vector<int> findPath(int from, int to) const;

private:
    std::vector<std::array<float, 3>> positions_;
    std::vector<std::vector<PathLink>> links_;
    std::vector<uint8_t> vis_;
};

// The node nearest (2D) to `pos` that a search from `start` reaches through ever closer nodes (0x542340 with the
// search 0x545670 and its neighbour gate 0x5422c0; docs/spec/ai.md "Path planner"). `blocked(node, link)` is the
// team's link block byte (+8 + team). Returns `start` when no neighbour improves on it.
int nearestReachableNode(const PathGraph& g, int start, const std::array<float, 3>& pos,
                         const std::function<bool(size_t node, size_t link)>& blocked);

} // namespace iw2::game
