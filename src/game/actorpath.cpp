// Path planning over the node graph (docs/spec/ai.md "Path planner"): the node query behind a sentient's node and a
// path's goal node (0x517f90), the A* search (0x545840 / 0x545ca0) and building the path points (0x541da0).
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

} // namespace

// 0x516950 / 0x516820: nodes within `radius` (2D) of `center` whose height difference is at most `height`, of a type
// in `typeMask` (spawnflag 1 nodes only when bit 0 is set), at most `max`, with their 2D distance². TODO(spec): the
// original walks a node tree, so which nodes are taken when more than `max` qualify can differ.
std::vector<std::pair<int, float>> World::nodesInRange(const Vec3& center, float radius, float height, size_t max,
                                                       uint32_t typeMask) const {
    std::vector<std::pair<int, float>> out;
    for (size_t i = 0; i < nodes_.size() && out.size() < max; ++i) {
        const PathNode& n = nodes_[i];
        const Vec3 p = originOf(n.obj);
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float d2 = dx * dx + dy * dy;
        if (d2 > radius * radius || !(typeMask >> n.typeNum & 1)) continue;
        if ((n.spawnflags & 1) && !(typeMask & 1)) continue;
        if (dz * dz > height * height) continue;
        out.push_back({int(i), d2});
    }
    return out;
}

// The node query (0x517f90): candidates within `radius` (`lowered`: of the point 120 units down, within 184 units of
// height; else any height), linked nodes first (comparator 0x516640), nearest first. A linked node is taken when the
// point stands on it (box −16…16, −1…73 around the node) or when an actor box (−15,−15,17)…(15,15,72) moves from the
// point to it unblocked (0x44c3c0, mask 0x820011); nodes beyond one of the planes and unlinked nodes are tried last,
// by the trace alone. `first` gets the first candidate (−1 when none).
int World::queryNode(const Vec3& pos, float radius, bool lowered, uint32_t typeMask, const std::vector<PathPlane>& planes,
                     int* first) {
    auto list = lowered ? nodesInRange({pos[0], pos[1], pos[2] - 120.0f}, radius, 184.0f, 64, typeMask)
                        : nodesInRange(pos, radius, 1e9f, 64, typeMask);
    std::stable_sort(list.begin(), list.end(), [this](const auto& a, const auto& b) {
        const bool la = !paths_.links(size_t(a.first)).empty(), lb = !paths_.links(size_t(b.first)).empty();
        if (la != lb) return la;
        return a.second < b.second;
    });
    if (first) *first = list.empty() ? -1 : list.front().first;
    const Vec3 mins{-15, -15, 17}, maxs{15, 15, 72};
    std::vector<int> later;
    for (const auto& [node, d2] : list) {
        if (paths_.links(size_t(node)).empty()) {
            later.push_back(node);
            continue;
        }
        const Vec3 p = originOf(nodes_[size_t(node)].obj);
        if (p[0] - 16.0f < pos[0] && pos[0] < p[0] + 16.0f && p[1] - 16.0f < pos[1] && pos[1] < p[1] + 16.0f &&
            p[2] - 1.0f < pos[2] && pos[2] < p[2] + 73.0f)
            return node;
        bool behind = false;
        for (const PathPlane& pl : planes) behind = behind || pl.dist < pl.normal[0] * p[0] + pl.normal[1] * p[1];
        if (behind) {
            later.push_back(node);
            continue;
        }
        if (!collision_->sightBlocked(pos, p, mins, maxs, 0x820011)) return node;
    }
    for (int node : later)
        if (!collision_->sightBlocked(pos, originOf(nodes_[size_t(node)].obj), mins, maxs, 0x820011)) return node;
    return -1;
}

// The sentient's node (0x515040): the node query around its origin (192 units, lowered, every type but spawnflag 1
// nodes), cached until the planners reset the cache (+0x3c) before they plan; when no node is reachable the first
// candidate is kept with the "unreachable" flag (+0x3d), and without candidates the previous node stays.
// TODO(spec): where else the original clears the cache flag is not read; it is also cleared every frame here.
int World::sentientNode(Entity& e) {
    if (e.nodeCacheMs != timeMs_) {
        e.nodeCacheMs = timeMs_;
        e.nodeCached = false;
    }
    if (e.nodeCached) return e.cachedNode;
    int first = -1;
    const int node = queryNode(originOf(e.obj), 192.0f, true, 0xfffffffe, {}, &first);
    if (node >= 0) {
        e.cachedNode = node;
        e.nodeUnreachable = false;
    } else {
        if (first >= 0) e.cachedNode = first;
        e.nodeUnreachable = true;
    }
    e.nodeCached = true;
    return e.cachedNode;
}

// A* over the node graph (0x545840; 0x545ca0 with planes). Links blocked for the team by bad places are skipped (link
// byte +8 + team), and with planes the nodes beyond one of them. The open list is kept sorted by g + h (h: the 2D
// distance to the goal position); a node is inserted before the first one whose f is not lower. A visited node is
// re-queued when reached cheaper. The search ends when the goal node is taken from the head of the list.
bool World::searchNodes(int team, int start, int goal, const Vec3& goalPos, const std::vector<PathPlane>& planes,
                        std::vector<int>& chain) {
    const size_t n = nodes_.size();
    if (start < 0 || goal < 0 || size_t(start) >= n || size_t(goal) >= n || paths_.size() != n) return false;
    static constexpr int kHead = -2; // the list head (the original's fake node whose "next" is the head variable)
    if (search_.size() != n) search_.assign(n, {});
    auto& s = search_;
    const int id = ++searchId_;
    int head = start;
    s[size_t(start)] = {id, -1, kHead, -1, 0.0f, 0.0f, true};
    auto setNext = [&](int node, int next) { (node == kHead ? head : s[size_t(node)].next) = next; };
    while (true) {
        const int cur = head;
        if (cur == goal) break;
        head = s[size_t(cur)].next;
        if (head >= 0) s[size_t(head)].prev = kHead;
        const auto& links = paths_.links(size_t(cur));
        for (size_t li = 0; li < links.size(); ++li) {
            if (linkBlocked(size_t(cur), li, team)) continue;
            const int nb = links[li].target;
            const Vec3& p = paths_.position(size_t(nb));
            bool beyond = false;
            for (const PathPlane& pl : planes) beyond = beyond || pl.dist < pl.normal[0] * p[0] + pl.normal[1] * p[1];
            if (beyond) continue;
            auto& sn = s[size_t(nb)];
            float g;
            if (sn.id == id) {
                g = s[size_t(cur)].g + links[li].distance;
                if (sn.g <= g) continue;
                if (sn.queued) {
                    setNext(sn.prev, sn.next);
                    if (sn.next >= 0) s[size_t(sn.next)].prev = sn.prev;
                }
            } else {
                sn.id = id;
                const float dx = goalPos[0] - p[0], dy = goalPos[1] - p[1];
                sn.h = std::sqrt(dx * dx + dy * dy);
                g = links[li].distance + s[size_t(cur)].g;
            }
            sn.parent = cur;
            sn.g = g;
            int after = kHead;
            for (int it = head; it >= 0 && s[size_t(it)].h + s[size_t(it)].g < g + sn.h; it = s[size_t(it)].next) after = it;
            sn.prev = after;
            sn.next = after == kHead ? head : s[size_t(after)].next;
            setNext(after, nb);
            if (sn.next >= 0) s[size_t(sn.next)].prev = nb;
            sn.queued = true;
        }
        s[size_t(cur)].queued = false;
        if (head < 0) return false;
    }
    chain.clear();
    for (int c = goal; c >= 0; c = c == start ? -1 : s[size_t(c)].parent) chain.push_back(c);
    std::reverse(chain.begin(), chain.end());
    return true;
}

// The path points (0x541da0): the nodes from the start node to the goal node, then the goal position when it differs
// (2D) from the goal node. More than 32 points keep the first 32 and mark the path truncated (+0x6e8 |= 4).
// TODO(spec): the negotiation pairs (node types 16 → 17 with matching link ids) that set the traversal index +0x386
// and fail the path when traversals are not allowed; +0x3b0 / 0x541a70 / 0x544ca0 (the path's lookahead state).
void World::buildActorPath(Entity& e, const std::vector<int>& chain, const Vec3& goalPos) {
    clearActorPath(e);
    const Vec3 last = originOf(nodes_[size_t(chain.back())].obj);
    const bool extra = goalPos[0] != last[0] || goalPos[1] != last[1];
    const size_t count = chain.size() + (extra ? 1 : 0);
    e.pathTruncated = count > 32;
    for (size_t i = 0; i < chain.size() && e.path.size() < 32; ++i) {
        e.path.push_back(originOf(nodes_[size_t(chain[i])].obj));
        e.pathNodes.push_back(chain[i]);
    }
    if (extra && !e.pathTruncated) {
        e.path.push_back(goalPos);
        e.pathNodes.push_back(-1);
    }
    e.pathPlannedCount = e.path.size();
    e.pathGoal = e.path.back();
}

// A path to a point (0x503650 core → 0x5424d0): from the sentient's node to the node query around the point (192
// units, lowered); the node cache is reset first. TODO(spec): with danger planes (+0x2050, 0x54b670) the plane
// variants 0x5150d0 / 0x542810 / 0x5428a0 are used; the planes are not modelled, so they are always empty here.
bool World::pathToPoint(Entity& e, const Vec3& target) {
    clearActorPath(e);
    e.nodeCached = false;
    const int start = sentientNode(e);
    if (start < 0) return false;
    const int goal = queryNode(target, 192.0f, true, 0xfffffffe, {}, nullptr);
    if (goal < 0) return false;
    std::vector<int> chain;
    if (!searchNodes(teamOf(e), start, goal, target, {}, chain)) return false;
    buildActorPath(e, chain, target);
    return true;
}

// A path to a node (0x503890 with plain planning): from the sentient's node to `node`, ending at its origin; a node
// with spawnflag 1 is replaced by the node query around it (192 units, lowered).
bool World::pathToNodeDirect(Entity& e, int node) {
    clearActorPath(e);
    e.nodeCached = false;
    const int start = sentientNode(e);
    if (start < 0) return false;
    int goal = node;
    if (nodes_[size_t(node)].spawnflags & 1) {
        goal = queryNode(originOf(nodes_[size_t(node)].obj), 192.0f, true, 0xfffffffe, {}, nullptr);
        if (goal < 0) return false;
    }
    const Vec3 target = originOf(nodes_[size_t(goal)].obj);
    std::vector<int> chain;
    if (!searchNodes(teamOf(e), start, goal, target, {}, chain)) return false;
    buildActorPath(e, chain, target);
    return true;
}

bool World::linkBlocked(size_t node, size_t link, int team) const {
    if (team < 0 || team > 3 || node >= linkBlock_.size() || link >= linkBlock_[node].size()) return false;
    return linkBlock_[node][link][size_t(team)] != 0;
}

} // namespace iw2::game
