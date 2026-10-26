// Path planning over the node graph (docs/spec/ai.md "Path planner"): the node query behind a sentient's node and a
// path's goal node (0x517f90), the A* search (0x545840 / 0x545ca0) and building the path points (0x541da0).
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iw2::game {

using namespace script;

namespace {

// 0x421160: normalizes (x, y) in place unless it is zero; returns the length.
float normalize2(float& x, float& y) {
    const float l = std::sqrt(x * x + y * y);
    if (l != 0.0f) {
        const float k = 1.0f / l;
        x *= k;
        y *= k;
    }
    return l;
}

// Distance of `o` from the line through `at` along `dir` (2D).
float lineDist(const Vec3& at, const std::array<float, 2>& dir, const Vec3& o) {
    return std::fabs((at[1] - o[1]) * dir[0] - (at[0] - o[0]) * dir[1]);
}

// Points the lookahead at `pt` (0x5419e0 / 0x544170 / 0x544ca0): 2D unit direction, its length, z per unit.
void aimLook(ActorPath& p, const Vec3& o, const Vec3& pt) {
    p.lookDir = {pt[0] - o[0], pt[1] - o[1], pt[2] - o[2]};
    p.lookDist = normalize2(p.lookDir[0], p.lookDir[1]);
    p.lookDir[2] = p.lookDist != 0.0f ? p.lookDir[2] / p.lookDist : 0.0f;
}

// 0x543b70: the budget becomes a part of `area` (3/4 after a clear lookahead, else 9/16), at least 0.001.
void shrinkBudget(ActorPath& p, float area) {
    p.budget = area * ((p.flags & 2) ? 0.75f : 0.5625f);
    if (p.budget < 0.001f) p.budget = 0.001f;
    p.flags &= ~0x103u;
}

// 0x543be0: a clear lookahead grows the budget (· 1.17647 + 6.4, at most 65536) and sets flag 2, then 0x100.
void growBudget(ActorPath& p) {
    p.budget = p.budget * 1.1764705f + 6.4f;
    if (p.budget > 65536.0f) p.budget = 65536.0f;
    p.flags |= (p.flags & 2) ? 0x100u : 2u;
}

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

// 0x550f10's table: which team each team fights ({0, 2, 1, 0, 0}).
bool isEnemyTeam(const Entity& me, const Entity& other) {
    static constexpr int kEnemy[5] = {0, 2, 1, 0, 0};
    return teamOf(other) == kEnemy[teamOf(me)];
}

} // namespace

// 0x54b3b0 (slot choice 0x54b370): the fire of `source` near the actor marks the half plane beyond the line a–b. Only
// an enemy team's fire counts (0x54b3c9…0x54b3f2: the source's team must be the enemy of the actor's). The slot is
// the one the source already owns, else the oldest; it stores the time, the source and the plane. The plane is the
// perpendicular of b − a through a — (a.y − b.y, b.x − a.x), n·p = c — flipped so the actor's own origin is on the
// kept side (n·origin ≤ c). A same-team source (0x54b484: only reachable through the unidentified entity +0xfc
// exception, TODO(spec)) marks the slot short-lived and drops the actor's path.
void World::addDangerPlane(Entity& e, const Entity& source, const Vec3& a, const Vec3& b) {
    if (!isEnemyTeam(e, source)) return;
    int slot = -1;
    int64_t best = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < 4; ++i) {
        if (e.dangerPlanes[size_t(i)].sourceEnt == source.number) {
            slot = i;
            break;
        }
        if (e.dangerPlanes[size_t(i)].timeMs < best) {
            best = e.dangerPlanes[size_t(i)].timeMs;
            slot = i;
        }
    }
    DangerPlane& p = e.dangerPlanes[size_t(slot)];
    p.timeMs = timeMs_;
    p.sourceEnt = source.number;
    float nx = a[1] - b[1], ny = b[0] - a[0];
    float c = nx * a[0] + ny * a[1];
    const Vec3 o = originOf(e.obj);
    if (nx * o[0] + ny * o[1] > c) {
        nx = -nx;
        ny = -ny;
        c = -c;
    }
    p.normal[0] = nx;
    p.normal[1] = ny;
    p.dist = c;
    p.shortLived = teamOf(source) == teamOf(e);
    if (p.shortLived) clearActorPath(e);
}

// 0x54b4d0 (each actor think, 0x4ff030): a short-lived plane expires after 300 ms, the others after the actor's
// `suppressionwait` (field +0x20b0, ms; default 5000).
void World::expireDangerPlanes(Entity& e) {
    const Value w = e.obj->get("suppressionwait");
    const int64_t wait = int64_t((w.isNumber() ? w.asFloat() : 5.0f) * 1000.0f);
    for (DangerPlane& p : e.dangerPlanes) {
        if (p.timeMs == 0) continue;
        const int64_t since = timeMs_ - p.timeMs;
        if (since >= (p.shortLived ? 300 : wait)) p = {};
    }
}

// 0x54b5a0 (from the per-sentient release 0x4fe170, on the sentient's death/removal): the planes it added go.
void World::removeDangerPlanesFrom(int sourceNumber) {
    for (const auto& ep : entities_) {
        Entity& a = *ep;
        if (!a.isActor) continue;
        for (DangerPlane& p : a.dangerPlanes)
            if (p.sourceEnt == sourceNumber) p = {};
    }
}

// 0x54b670: the active planes, in slot order.
std::vector<PathPlane> World::dangerPlanesOf(const Entity& e) const {
    std::vector<PathPlane> out;
    for (const DangerPlane& p : e.dangerPlanes)
        if (p.timeMs != 0) out.push_back({{p.normal[0], p.normal[1]}, p.dist});
    return out;
}

// 0x54b610: whether a plane can be added without evicting a short-lived one (a free slot, or one not short-lived).
bool World::hasFreeDangerPlane(const Entity& e) const {
    for (const DangerPlane& p : e.dangerPlanes)
        if (p.timeMs == 0 || !p.shortLived) return true;
    return false;
}

// 0x542710: whether the node is on the excluded side of one of the planes (n·p > dist).
bool World::nodeBeyondPlanes(int node, const std::vector<PathPlane>& planes) const {
    const Vec3& p = paths_.position(size_t(node));
    for (const PathPlane& pl : planes)
        if (pl.dist < pl.normal[0] * p[0] + pl.normal[1] * p[1]) return true;
    return false;
}

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

// The sentient's node under danger planes (0x5150d0): the same query as 0x515040 around the origin (192 units,
// lowered), but uncached and with the plane exclusion (nodes beyond a plane are only tried last, by the trace alone).
int World::sentientNodePlanes(Entity& e, const std::vector<PathPlane>& planes) {
    int first = -1;
    const int node = queryNode(originOf(e.obj), 192.0f, true, 0xfffffffe, planes, &first);
    return node; // a failed query is not cached or kept here (0x5150d0 returns the query's result directly)
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

// The path points (0x541da0): goal first, then the nodes back to the start node, which is the last point. The goal
// position is the first point when it differs (2D) from the goal node. More than 32 points keep the 32 nearest the
// start and mark the path truncated (+0x6e8 |= 4). Each point gets the unit 2D direction and length of the segment
// from the next point to it; the start (+0x6f8) is the last point. The budget (from 0x541780) carries over, and the
// lookahead is re-aimed (0x541a70; with flag 0x80 reset instead). TODO(spec): +0x3d4, cleared when the goal moves.
// TODO(spec): the negotiation pairs (node types 16 → 17 with matching link ids) that set the traversal index +0x386
// and fail the path when traversals are not allowed, and the plan argument +0x3c0.
void World::buildActorPath(Entity& e, const std::vector<int>& chain, const Vec3& goalPos) {
    ActorPath& p = e.path;
    const Vec3 o = originOf(e.obj);
    pathClearBudget(p, o);
    const Vec3 last = originOf(nodes_[size_t(chain.back())].obj);
    const bool extra = goalPos[0] != last[0] || goalPos[1] != last[1];
    p.pts[0] = extra ? PathPoint{goalPos, {}, 0, -1} : PathPoint{last, {}, 0, chain.back()};
    const uint32_t oldFlags = p.flags;
    p.flags = 0;
    if (p.count == 0 || p.pts[0].pos != p.goal) p.goal = p.pts[0].pos;
    p.count = p.planned = p.detour = 0;
    int n = int(chain.size()) + (extra ? 1 : 0);
    const bool truncated = n > 32;
    if (truncated) {
        n = 32;
        p.flags |= 4;
    }
    for (int i = n - 1, c = 0; i >= 1; --i, ++c) {
        const int node = chain[size_t(c)];
        p.pts[size_t(i)] = {originOf(nodes_[size_t(node)].obj), {}, 0, node};
    }
    if (truncated) p.pts[0] = {originOf(nodes_[size_t(chain[size_t(n - 1)])].obj), {}, 0, chain[size_t(n - 1)]};
    p.traversal = 0;
    PathPoint& end = p.pts[size_t(n - 1)];
    end.dir = {0, 0};
    end.len = 0;
    p.start = end.pos;
    for (int i = 0; i < n - 1; ++i) {
        PathPoint& q = p.pts[size_t(i)];
        const PathPoint& nx = p.pts[size_t(i + 1)];
        float dx = q.pos[0] - nx.pos[0], dy = q.pos[1] - nx.pos[1];
        q.len = std::sqrt(dx * dx + dy * dy);
        // The original divides by the length unguarded (points at one place give NaN directions).
        const float k = q.len != 0.0f ? 1.0f / q.len : 0.0f;
        q.dir = {dx * k, dy * k};
    }
    p.startLen = n > 1 ? p.pts[size_t(n - 2)].len : 0.0f;
    p.count = p.planned = n;
    p.planMs = timeMs_;
    if (p.budget != 0.0f) {
        if (oldFlags & 0x80) {
            p.budget = 32768.0f;
            p.lookDir = {0, 0, 0};
            updatePathLook(e, o, false, false);
        } else {
            pathLookNew(e, o);
        }
    }
}

// A path to a point (0x503650 core): from the sentient's node to the node query around the point (192 units,
// lowered); the node cache is reset first. Without danger planes this is 0x5424d0 (plain 0x545840); with them
// (0x54b670) the start comes from the uncached plane query 0x5150d0, the goal query keeps the planes (0x542810) and
// 0x5428a0 requires both end nodes to be on the actor's side of every plane (0x542710) before the plane-limited
// search 0x545ca0. TODO(spec): the planes only appear through the AI events (0x535770 → 0x54b3b0), not implemented.
bool World::pathToPoint(Entity& e, const Vec3& target) {
    clearActorPath(e);
    const std::vector<PathPlane> planes = dangerPlanesOf(e);
    e.nodeCached = false;
    const int start = planes.empty() ? sentientNode(e) : sentientNodePlanes(e, planes);
    if (start < 0) return false;
    const int goal = queryNode(target, 192.0f, true, 0xfffffffe, planes, nullptr);
    if (goal < 0) return false;
    if (!planes.empty() && (nodeBeyondPlanes(start, planes) || nodeBeyondPlanes(goal, planes))) return false;
    std::vector<int> chain;
    if (!searchNodes(teamOf(e), start, goal, target, planes, chain)) return false;
    buildActorPath(e, chain, target);
    return true;
}

// A path to a node (0x503890; `withPlanes` its third argument): from the sentient's node (0x515040, or 0x5150d0 with
// the planes) to `node`, ending at its origin; a node with spawnflag 1 is replaced by the node query around it (192
// units, lowered, without planes — 0x517f90 is called with none there). With planes both end nodes must be on the
// actor's side of every plane (0x5428a0 → 0x542710) and the search is 0x545ca0.
bool World::pathToNodeDirect(Entity& e, int node, bool withPlanes) {
    clearActorPath(e);
    const std::vector<PathPlane> planes = withPlanes ? dangerPlanesOf(e) : std::vector<PathPlane>{};
    e.nodeCached = false;
    const int start = planes.empty() ? sentientNode(e) : sentientNodePlanes(e, planes);
    if (start < 0) return false;
    int goal = node;
    if (nodes_[size_t(node)].spawnflags & 1) {
        goal = queryNode(originOf(nodes_[size_t(node)].obj), 192.0f, true, 0xfffffffe, {}, nullptr);
        if (goal < 0) return false;
    }
    if (!planes.empty() && (nodeBeyondPlanes(start, planes) || nodeBeyondPlanes(goal, planes))) return false;
    const Vec3 target = originOf(nodes_[size_t(goal)].obj);
    std::vector<int> chain;
    if (!searchNodes(teamOf(e), start, goal, target, planes, chain)) return false;
    buildActorPath(e, chain, target);
    return true;
}

bool World::linkBlocked(size_t node, size_t link, int team) const {
    if (team < 0 || team > 3 || node >= linkBlock_.size() || link >= linkBlock_[node].size()) return false;
    return linkBlock_[node][link][size_t(team)] != 0;
}

// 0x5433e0: drops `n` points from the goal end (keeping the start end); with fewer than two left the start becomes
// the only point. The goal is the new first point.
void World::dropPathGoalPoints(ActorPath& p, int n) {
    if (n == 0) return;
    p.count -= n;
    p.planned -= n;
    if (p.detour >= 0) p.detour = std::max(p.detour - n, 0);
    p.lookIndex -= n;
    if (p.lookIndex < 0) {
        p.lookIndex = 0;
        p.lookSegLen = 0;
    }
    p.traversal = std::max(p.traversal - n, 0);
    if (p.count < 2) {
        p.pts[0] = {p.start, {0, 0}, 0, -1};
        p.planned = 1;
        p.startLen = 0;
    } else {
        std::copy(p.pts.begin() + n, p.pts.begin() + n + p.planned, p.pts.begin());
    }
    p.goal = p.pts[0].pos;
    p.flags &= ~4u;
}

// 0x541780 (when a path is cleared or replaced): the area between the actor and the old path, up to the point of it
// nearest the actor (walking from the start end over the planned points, at most 32768), plus the part of the first
// segment already covered, is added to the budget.
void World::pathClearBudget(ActorPath& p, const Vec3& o) {
    int j = p.count - 1;
    if (j <= p.traversal) return;
    float best = std::sqrt((o[0] - p.pts[size_t(j - 1)].pos[0]) * (o[0] - p.pts[size_t(j - 1)].pos[0]) +
                           (o[1] - p.pts[size_t(j - 1)].pos[1]) * (o[1] - p.pts[size_t(j - 1)].pos[1]));
    float acc = 0, bestAcc = 0;
    while (true) {
        const PathPoint& q = p.pts[size_t(j)];
        const float dx = q.pos[0] - o[0], dy = q.pos[1] - o[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= best) {
            best = d;
            bestAcc = acc;
        }
        if (++j >= p.planned) break;
        acc += std::fabs(dy * q.dir[0] - dx * q.dir[1]) * q.len;
        if (acc > 32768.0f) break;
    }
    const PathPoint& f = p.pts[size_t(p.count - 2)];
    p.budget += lineDist(f.pos, f.dir, o) * (f.len - p.startLen) + bestAcc;
}

// 0x5419e0: the lookahead at the start, on the last segment; then the update (0x544ca0, `correct` → 0x541930).
void World::pathLookReset(Entity& e, const Vec3& o, bool correct) {
    ActorPath& p = e.path;
    aimLook(p, o, p.start);
    p.lookIndex = p.count - 1;
    updatePathLook(e, o, false, correct);
}

// 0x541a70 (after a new path): walk the new path from the start, summing the area between the actor and it, to the
// budget. The old lookahead direction is kept when no point along the way lines up with it better than the start
// (else the lookahead restarts and the budget shrinks to the area up to the best one, at least 64); a path that
// crosses the old direction re-runs the update instead.
void World::pathLookNew(Entity& e, const Vec3& o) {
    ActorPath& p = e.path;
    if (p.lookDist == 0.0f) {
        pathLookReset(e, o, false);
        return;
    }
    float sx = p.start[0] - o[0], sy = p.start[1] - o[1];
    const float budget = p.budget, lx = p.lookDir[0], ly = p.lookDir[1];
    const float sl = normalize2(sx, sy);
    float crossPrev = (sy * lx - sx * ly) * sl;
    float bestDot = ly * sy + lx * sx;
    float acc = 0, best = 0;
    auto restart = [&] {
        pathLookReset(e, o, true);
        if (!(p.flags & 2) && p.budget > best) p.budget = best < 64.0f ? 64.0f : best;
    };
    bool first = true;
    for (int i = p.count - 2; i >= p.traversal; --i, first = false) {
        const PathPoint& q = p.pts[size_t(i)];
        float qx = q.pos[0] - o[0], qy = q.pos[1] - o[1];
        const float len = first ? p.startLen : q.len;
        const float c1 = qy * q.dir[0] - qx * q.dir[1];
        const bool crossing = (ly * q.dir[0] - lx * q.dir[1]) * c1 > 0.0f;
        const float dist = std::fabs(c1);
        const float crossQ = qy * lx - qx * ly;
        if (crossing && crossQ * crossPrev < 0.0f) {
            updatePathLook(e, o, false, true);
            return;
        }
        acc += dist * len;
        if (acc >= budget) {
            const float t = (acc - budget) / dist;
            float px = q.pos[0] - t * q.dir[0] - o[0], py = q.pos[1] - t * q.dir[1] - o[1];
            normalize2(px, py);
            if (py * ly + px * lx >= bestDot) pathLookReset(e, o, false);
            else restart();
            return;
        }
        crossPrev = crossQ;
        normalize2(qx, qy);
        const float d = qy * ly + qx * lx;
        if (d >= bestDot) {
            bestDot = d;
            best = acc;
        }
    }
    if (p.count - 2 < p.traversal || acc == best) pathLookReset(e, o, false);
    else restart();
}

// 0x544520 (the start is ahead of the actor): walk back over the planned points from the start end while they stay
// within 256 units, summing the area to 0.17647 · budget, and move the start (and the point count) back to where the
// actor is level with the path; a point more than 256 units away sets flag 0x20. The covered area (· 3/4 unless the
// lookahead was clear) is added to the budget (at most 65536).
void World::pathBackUp(ActorPath& p, const Vec3& o) {
    const float lim = p.budget * 0.17647055f;
    int i = p.count - 1;
    float c = lineDist(p.pts[size_t(i - 1)].pos, p.pts[size_t(i - 1)].dir, o);
    float acc = -(c * p.startLen);
    const float lx = p.lookDir[0], ly = p.lookDir[1];
    auto toPoint = [&](int at, float accIn) { // 0x544730
        const PathPoint& q = p.pts[size_t(at - 1)];
        p.start = q.pos;
        p.startLen = at > p.traversal + 1 ? p.pts[size_t(at - 2)].len : 0.0f;
        p.count = at;
        return accIn + c * q.len;
    };
    float f = 0;
    while (true) {
        const PathPoint& cur = p.pts[size_t(i)];
        const PathPoint& prev = p.pts[size_t(i - 1)];
        const float qx = cur.pos[0] - o[0], qy = cur.pos[1] - o[1];
        if (qx * qx + qy * qy > 65536.0f) {
            p.flags |= 0x20;
            acc = toPoint(i, acc);
            goto done;
        }
        const float d = qy * ly + qx * lx;
        if (d <= 0.0f) {
            const float dp = (prev.pos[0] - o[0]) * lx + (prev.pos[1] - o[1]) * ly;
            if (dp <= 0.0f) {
                acc = toPoint(i, acc);
                goto done;
            }
            f = d / (d - dp);
            break;
        }
        const float seg = c * prev.len;
        acc += seg;
        if (acc >= lim) {
            f = (acc - lim) / seg;
            acc -= seg;
            break;
        }
        if (++i >= p.planned) {
            p.startLen = prev.len;
            p.start = cur.pos;
            p.count = p.planned;
            goto done;
        }
        c = std::fabs(qy * cur.dir[0] - qx * cur.dir[1]);
    }
    {
        const PathPoint& cur = p.pts[size_t(i)];
        const PathPoint& prev = p.pts[size_t(i - 1)];
        const float t = f * prev.len;
        if (t < prev.len) {
            const float r = prev.len - t;
            p.startLen = r;
            acc += r * c;
            p.start = {t * prev.dir[0] + cur.pos[0], t * prev.dir[1] + cur.pos[1],
                       (prev.pos[2] - cur.pos[2]) * f + cur.pos[2]};
            p.count = i + 1;
        } else {
            if (i - 1 <= p.traversal) return;
            p.start = prev.pos;
            p.startLen = p.pts[size_t(i - 2)].len;
            p.count = i;
        }
    }
done:
    if (acc > 0.0f) {
        if (!(p.flags & 2)) acc *= 0.75f;
        p.budget += acc;
        if (p.budget > 65536.0f) p.budget = 65536.0f;
    }
}

// 0x544310 (the start is level with or behind the actor): the first point ahead of the actor along the lookahead
// direction; the start moves to where the actor is level with the path before it, dropping the passed points.
void World::pathAdvance(ActorPath& p, const Vec3& o) {
    const float lx = p.lookDir[0], ly = p.lookDir[1];
    int i = p.count - 2;
    bool first = true;
    float d = 0;
    for (; i >= p.traversal; --i, first = false) {
        d = (p.pts[size_t(i)].pos[0] - o[0]) * lx + (p.pts[size_t(i)].pos[1] - o[1]) * ly;
        if (d > 0.0f) break;
    }
    if (i < p.traversal) return;
    const PathPoint& q = p.pts[size_t(i)];
    const Vec3 prevPos = first ? p.start : p.pts[size_t(i + 1)].pos;
    const float segLen = first ? p.startLen : q.len;
    const float dp = (prevPos[0] - o[0]) * lx + (prevPos[1] - o[1]) * ly;
    const float frac = dp / (dp - d);
    const float along = segLen * frac;
    if (along >= segLen) {
        if (i <= p.traversal) return;
        p.start = q.pos;
        --i;
        p.startLen = p.pts[size_t(i)].len;
    } else {
        p.start = {along * q.dir[0] + prevPos[0], along * q.dir[1] + prevPos[1],
                   (q.pos[2] - prevPos[2]) * frac + prevPos[2]};
        p.startLen = segLen - along;
    }
    if (p.detour >= 0) p.detour = std::max(p.detour - p.count + i + 2, 0);
    p.count = i + 2;
}

// 0x541930 (after the start moved): the area of the planned points beyond the current ones is taken off the budget
// (at least 64 left).
void World::pathCorrectBudget(ActorPath& p, const Vec3& o) {
    if (p.count == p.planned) return;
    const PathPoint& f = p.pts[size_t(p.count - 2)];
    float v = lineDist(f.pos, f.dir, o) * (f.len - p.startLen);
    for (int j = p.count - 1; j < p.planned - 1; ++j) {
        const PathPoint& q = p.pts[size_t(j)];
        v += lineDist(q.pos, q.dir, o) * q.len;
    }
    p.budget -= v;
    if (p.budget < 64.0f) p.budget = 64.0f;
}

// 0x5447d0: the lookahead point is where the area between the actor and the path, summed from the start, reaches the
// budget (flags 1|0x40 cleared); when the whole path is within the budget it is the last usable point (flags set) and
// the budget is capped by that area (or 32768).
void World::pathLookAhead(Entity& e, const Vec3& o, bool shrink) {
    ActorPath& p = e.path;
    const float budget = p.budget;
    float acc = 0;
    bool first = true;
    for (int i = p.count - 2; i >= p.traversal; --i, first = false) {
        const PathPoint& q = p.pts[size_t(i)];
        const float dist = lineDist(q.pos, q.dir, o);
        const float len = first ? p.startLen : q.len;
        acc += dist * len;
        if (acc >= budget) {
            float t = (acc - budget) / dist;
            if (t > len) t = len;
            const float prevZ = first ? p.start[2] : p.pts[size_t(i + 1)].pos[2];
            const Vec3 pt{q.pos[0] - t * q.dir[0], q.pos[1] - t * q.dir[1], q.pos[2] - (q.pos[2] - prevZ) / len * t};
            p.flags &= ~0x41u;
            pathLookPoint(e, o, pt, shrink, t, i, budget);
            return;
        }
    }
    p.flags |= 0x41;
    const Vec3 pt = p.count - 1 == p.traversal ? p.start : p.pts[size_t(p.traversal)].pos;
    pathLookPoint(e, o, pt, shrink, 0.0f, p.traversal, acc);
    if (acc > 32768.0f) {
        if (p.budget > acc) p.budget = acc;
    } else if (p.budget > 32768.0f) {
        p.budget = 32768.0f;
    }
}

// 0x544170: aim at the candidate point. With `shrink`, or when the move there is blocked (0x5440e0), the budget
// shrinks (0x543b70) — a blocked point is still taken unless the lookahead was clear before and its segment is no
// shorter than the old one; a clear move grows the budget (0x543be0).
void World::pathLookPoint(Entity& e, const Vec3& o, const Vec3& pt, bool shrink, float segLen, int index, float area) {
    ActorPath& p = e.path;
    if (p.lookIndex >= p.count - 1) {
        p.lookSegLen = 0;
        p.lookIndex = p.count - 1;
    }
    if (shrink) {
        shrinkBudget(p, area);
    } else if (!pathLookClear(p, o, pt)) {
        const float len = p.lookIndex == p.count - 2 ? p.startLen : p.pts[size_t(p.lookIndex)].len;
        const bool keep = (p.flags & 2) && p.lookIndex < p.count && !(len < p.lookSegLen);
        shrinkBudget(p, area);
        if (keep) return;
    } else {
        growBudget(p);
    }
    aimLook(p, o, pt);
    p.lookSegLen = segLen;
    p.lookIndex = index;
}

// 0x544a10: the far direction (+0x6d4) is the lookahead direction, except with flag 0x100 and a lookahead point
// (0x5449a0: at most 60 along) behind which the path goes on: then it points at where the area from that point
// reaches the budget (or at the last usable point).
void World::pathFarDir(ActorPath& p, const Vec3& o) {
    p.farDir = {p.lookDir[0], p.lookDir[1]};
    if (!(p.flags & 0x100) || p.count <= 1 || (p.flags & 1)) return;
    const float l = p.lookDist > 60.0f ? 60.0f : p.lookDist;
    const Vec3 q{p.lookDir[0] * l + o[0], p.lookDir[1] * l + o[1], 0};
    const float lx = p.lookDir[0], ly = p.lookDir[1];
    if ((p.start[0] - q[0]) * lx + (p.start[1] - q[1]) * ly > 0.0f) return;
    int i = p.count - 2;
    bool first = true;
    float d = 0;
    for (; i >= p.traversal; --i, first = false) {
        d = (p.pts[size_t(i)].pos[0] - q[0]) * lx + (p.pts[size_t(i)].pos[1] - q[1]) * ly;
        if (d > 0.0f) break;
    }
    if (i < p.traversal) return;
    const PathPoint& pi = p.pts[size_t(i)];
    const Vec3 prevPos = first ? p.start : p.pts[size_t(i + 1)].pos;
    const float segLen = first ? p.startLen : pi.len;
    const float dp = (prevPos[0] - q[0]) * lx + (prevPos[1] - q[1]) * ly;
    const float budget = p.budget;
    const float frac = dp / (dp - d);
    float c = lineDist(pi.pos, pi.dir, q);
    float acc = (segLen - frac * segLen) * c;
    auto aim = [&](const PathPoint& at, float t) {
        p.farDir = {at.pos[0] - t * at.dir[0] - q[0], at.pos[1] - t * at.dir[1] - q[1]};
        normalize2(p.farDir[0], p.farDir[1]);
    };
    if (acc >= budget) {
        aim(pi, (acc - budget) / c);
        return;
    }
    while (i > p.traversal) {
        --i;
        const PathPoint& pj = p.pts[size_t(i)];
        c = lineDist(pj.pos, pj.dir, q);
        acc += c * pj.len;
        if (!(acc < budget)) {
            aim(pj, (acc - budget) / c);
            return;
        }
    }
    const PathPoint& pt = p.pts[size_t(p.traversal)];
    p.farDir = {pt.pos[0] - q[0], pt.pos[1] - q[1]};
    normalize2(p.farDir[0], p.farDir[1]);
}

// 0x544ca0: the lookahead update. With one usable point left the lookahead is the start (flags 1|0x40). Otherwise
// the start is moved back (0x544520, budget ≥ 64) when it is ahead of the actor, or forward (0x544310, while the
// lookahead was clear or the budget ≤ 16384; `correct` → 0x541930) — a truncated path with no more than a quarter
// of its planned points left is replanned instead (0x542410); then the lookahead point (0x5447d0) and the far
// direction (0x544a10).
void World::updatePathLook(Entity& e, const Vec3& o, bool shrink, bool correct) {
    ActorPath& p = e.path;
    p.flags &= ~0x20u;
    if (p.traversal == p.count - 1) {
        aimLook(p, o, p.start);
        p.lookSegLen = 0;
        p.lookIndex = p.traversal;
        p.flags |= 0x41;
        pathFarDir(p, o);
        return;
    }
    const float d = (p.start[0] - o[0]) * p.lookDir[0] + (p.start[1] - o[1]) * p.lookDir[1];
    if (d > 0.0f) {
        if (p.budget >= 64.0f) pathBackUp(p, o);
    } else {
        if ((p.flags & 4) && p.count * 4 <= p.planned) {
            replanActorPath(e, o);
            return;
        }
        if ((p.flags & 2) || p.budget <= 16384.0f) pathAdvance(p, o);
        if (correct) pathCorrectBudget(p, o);
    }
    pathLookAhead(e, o, shrink);
    pathFarDir(p, o);
}

// 0x542410: a truncated path is planned again from the node query around the actor to the one around the goal (192
// units, lowered); when either query or the search fails the path stays as it is.
bool World::replanActorPath(Entity& e, const Vec3& o) {
    const Vec3 goal = e.path.goal;
    const int g = queryNode(goal, 192.0f, true, 0xfffffffe, {}, nullptr);
    if (g < 0) return false;
    const int s = queryNode(o, 192.0f, true, 0xfffffffe, {}, nullptr);
    if (s < 0) return false;
    std::vector<int> chain;
    if (!searchNodes(teamOf(e), s, g, goal, {}, chain)) return false;
    buildActorPath(e, chain, goal);
    return true;
}

} // namespace iw2::game
