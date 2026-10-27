// Path nodes as cover (docs/spec/ai.md "Cover nodes"): node claiming, the cover node search and score, the
// animscript a claimed node selects, the path update that walks actors to cover (0x503490) and the cover /
// reacquire built-ins.
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace iw2::game {

using namespace script;

namespace {

constexpr int64_t kNodeClaimed = std::numeric_limits<int64_t>::max(); // +0x44 = 0x7fffffff
constexpr uint32_t kCoverTypes = 0x41ffc;                             // node types 2…12 and 18 (0x516950 masks)
constexpr int kSubCombat = 100, kSubNoncombat = 0x65, kSubReacquire = 0x66;

// 0x54ba00: how a node of each type covers a point. Closer than sqrt(distSq) (2D) never counts; then either the
// point is in front of the node (useFacing) or its yaw from the node, relative to the node's yaw and normalised to
// [0, 360), lies in [arcMin, arcMax] (wrapping when arcMin > arcMax).
struct CoverArc {
    float distSq, arcMin, arcMax;
    bool useFacing;
};

CoverArc coverArc(int type) {
    switch (type) {
    case 2: case 3: case 10: case 11: return {81225, 315, 45, true};
    case 4: return {81225, 350, 10, true};
    case 5: return {640000, 330, 30, true};
    case 6: case 8: return {81225, 305, 30, false};
    case 7: case 9: return {81225, 330, 55, false};
    case 12: return {81225, 330, 30, true};
    case 18: return {81225, 315, 45, false}; // TODO(spec): arcs of the node's turret (0x5159f0) when it has one
    default: return {0, 305, 55, true};
    }
}

// Animscripts by node type (0x54bb30; script slots 0x1c7e098… filled from the names at 0x4eb233).
const char* coverScriptName(int type) {
    switch (type) {
    case 2: return "cover_stand";
    case 3: case 4: case 18: return "cover_crouch";
    case 5: return "cover_prone";
    case 6: return "cover_right";
    case 7: return "cover_left";
    case 8: return "cover_wide_right";
    case 9: return "cover_wide_left";
    case 10: return "concealment_stand";
    case 11: return "concealment_crouch";
    case 12: return "concealment_prone";
    default: return nullptr;
    }
}

int teamIndex(const Value& team) {
    const std::string t = team.isString() ? team.asString() : std::string();
    return t == "axis" ? 1 : t == "allies" ? 2 : t == "neutral" ? 3 : 0;
}

Vec3 vecField(const ObjectPtr& o, const char* key) {
    const Value v = o->get(key);
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

float floatField(const ObjectPtr& o, const char* key, float def) {
    const Value v = o->get(key);
    return v.isNumber() ? v.asFloat() : def;
}

float dist2d(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

// AngleNormalize360 as the original computes it: through a 16-bit angle (× 65536/360, & 0xffff, × 360/65536).
float angleNormalize360(float a) {
    return float(int32_t(a * 182.04445f) & 0xffff) * 0.0054931641f;
}

// 0x502660 (nearnode): within 80 units vertically and 30 units horizontally of the node.
bool nearPoint(const Vec3& node, const Vec3& p) {
    const float dz = p[2] - node[2];
    return dz * dz < 6400.0f && dist2d(node, p) < 900.0f;
}

// 0x5027e0: the same point for the path code: within 80 units vertically and 2 units horizontally.
bool samePoint(const Vec3& a, const Vec3& b) {
    const float dz = b[2] - a[2];
    return dz * dz < 6400.0f && dist2d(a, b) <= 4.0f;
}

} // namespace

bool World::inActorGoal(Entity& e, const Vec3& p) {
    const Value saved = e.obj->get("origin");
    e.obj->set("origin", Value(p));
    const bool in = actorInGoal(e, 0.0f);
    e.obj->set("origin", saved);
    return in;
}

// Node usable by this sentient (0x518f20): its own node; not while the node is blocked for its team; free when the
// owner's hold has expired. TODO(spec): taking a claimed node from another actor (0x518e80 / 0x518d80) and the
// distance rule of priority nodes (0x518dc0: 0x420f00 against 225) are not implemented.
bool World::nodeUsable(Entity& e, int node) {
    PathNode& n = nodes_[size_t(node)];
    if (n.owner == &e) return true;
    const int team = teamIndex(e.obj->get("team"));
    if (team > 0 && timeMs_ <= n.teamBlockedMs[size_t(team)]) return false;
    if (n.ownerUntilMs < timeMs_) return true;
    if ((n.spawnflags & 0x40) && n.owner && n.owner->isActor && n.owner->ai.stack[size_t(n.owner->ai.depth)] == AiDeath)
        return true; // 0x518dc0: the owner of a priority node is dying
    return false;
}

// Release the claimed node (0x5191a0): owner and hold cleared. TODO(spec): the linked nodes (+0x30 / +0x32).
void World::releaseNode(Entity& e) {
    if (e.claimedNode < 0) return;
    PathNode& n = nodes_[size_t(e.claimedNode)];
    n.owner = nullptr;
    n.ownerUntilMs = 0;
    e.claimedNode = -1;
    e.obj->set("node", Value());
}

// 0x519300: the node failed for the sentient's team (no path, given up): its owner of that team leaves it with a
// 5 s hold (0x519200), and the team may not use it for 5 s.
void World::blockNodeForTeam(int node, Entity& e) {
    PathNode& n = nodes_[size_t(node)];
    const int team = teamIndex(e.obj->get("team"));
    if (n.owner && teamIndex(n.owner->obj->get("team")) == team) {
        Entity* owner = n.owner;
        if (owner->claimedNode == node) {
            owner->claimedNode = -1;
            owner->obj->set("node", Value());
            n.ownerUntilMs = timeMs_ + 5000;
        }
        n.owner = nullptr;
    }
    if (team > 0) n.teamBlockedMs[size_t(team)] = timeMs_ + 5000;
    // TODO(spec): with no claims on its linked nodes (+0x56 = 0) +0x44 and those nodes are cleared too.
}

// Claim a node (0x5152f0): the previous one is released; a normal node (or a priority one whose hold expired) is
// taken (0x518fa0); a held priority node is taken over (0x519050), evicting a claiming owner (0x515330: its path is
// dropped when it ends at the node, then 0x519300).
void World::claimNode(Entity& e, int node) {
    if (e.claimedNode == node) return;
    if (e.claimedNode >= 0) releaseNode(e);
    if (node < 0) return;
    PathNode& n = nodes_[size_t(node)];
    if ((n.spawnflags & 0x40) && n.ownerUntilMs >= timeMs_ && n.owner && n.owner != &e && n.ownerUntilMs == kNodeClaimed) {
        Entity* owner = n.owner;
        if (owner->isActor && owner->claimedNode == node) {
            if (actorHasPath(*owner) && samePoint(vecField(n.obj, "origin"), pathEnd(*owner))) clearActorPath(*owner);
            blockNodeForTeam(node, *owner);
        }
    }
    n.owner = &e;
    n.ownerUntilMs = kNodeClaimed;
    e.claimedNode = node;
    e.obj->set("node", Value(n.obj));
    // TODO(spec): 0x515390 (every 4950 ms, flagged 0x8000 nodes within 80 units that face it are blocked).
}

// 0x4fe670: give up the claimed node: drop a path that ends at it and block it for the team (0x519300).
void World::giveUpClaimedNode(Entity& e) {
    if (e.claimedNode < 0) return;
    const int node = e.claimedNode;
    if (actorHasPath(e) && samePoint(vecField(nodes_[size_t(node)].obj, "origin"), pathEnd(e))) clearActorPath(e);
    blockNodeForTeam(node, e);
}

// keepclaimednode, or keepclaimednodeingoal while the claimed node is inside the goal (0x4fe6f0; the flag is
// cleared once it is not).
bool World::keepClaimedNode(Entity& e) {
    if (e.obj->get("keepclaimednode").truthy()) return true;
    if (e.obj->get("keepclaimednodeingoal").truthy()) {
        if (e.claimedNode >= 0 && inActorGoal(e, vecField(nodes_[size_t(e.claimedNode)].obj, "origin"))) return true;
        e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
    }
    return false;
}

// 0x502910: keeping the claimed node, or standing near it (nearnode).
bool World::atClaimedNode(Entity& e) {
    if (keepClaimedNode(e)) return true;
    return nearPoint(vecField(nodes_[size_t(e.claimedNode)].obj, "origin"), vecField(e.obj, "origin"));
}

// 0x502850: inside the goal, and either without a path at the claimed node (if any), or with a path that ends
// inside the goal while the claimed node (if any) is kept or the actor stands on it (0x5027e0).
bool World::holdsPosition(Entity& e) {
    if (!actorInGoal(e, 0.0f)) return false;
    if (e.path.count <= 0) return e.claimedNode < 0 || atClaimedNode(e);
    if (!inActorGoal(e, e.path.goal)) return false;
    if (e.claimedNode < 0 || keepClaimedNode(e)) return true;
    return samePoint(vecField(nodes_[size_t(e.claimedNode)].obj, "origin"), vecField(e.obj, "origin"));
}

// 0x54bb30: the animscript of a node, or none with true when the node does not cover the actor from its enemy
// (or, without an enemy, from its grenade). Returns false for nodes without a cover script and without flag 0x8000.
bool World::coverScriptAt(Entity& e, int node, bool checkEnemy, std::string& script) {
    const PathNode& n = nodes_[size_t(node)];
    CoverArc arc = coverArc(n.typeNum);
    const char* name = coverScriptName(n.typeNum);
    script.clear();
    if (!name && !(n.spawnflags & 0x8000)) return false;
    const Vec3 no = vecField(n.obj, "origin");
    const float yaw = vecField(n.obj, "angles")[1];
    const float fx = std::cos(yaw * float(M_PI) / 180.0f), fy = std::sin(yaw * float(M_PI) / 180.0f);
    // 0x54b960 / 0x54b8c0.
    auto covers = [&](const Vec3& p) {
        if (dist2d(no, p) < arc.distSq) return false;
        if (arc.useFacing) return (p[0] - no[0]) * fx + (p[1] - no[1]) * fy >= 0.0f;
        const float d = angleNormalize360(std::atan2(p[1] - no[1], p[0] - no[0]) * 57.29578f - yaw);
        if (arc.arcMin <= arc.arcMax) return arc.arcMin <= d && d <= arc.arcMax;
        return d >= arc.arcMin || d <= arc.arcMax;
    };
    Entity* enemy = entityOf(e.obj->get("enemy"));
    if (!enemy) {
        if (checkEnemy) {
            Entity* g = e.ai.grenade;
            if (g && !covers(vecField(g->obj, "origin"))) return true;
        }
    } else if (!enemy->obj->get("ignoreme").truthy()) { // TODO(spec): also not with notarget (+0x128 & 4)
        auto it = e.sentientInfo.find(enemy->obj.get());
        if (it != e.sentientInfo.end() && it->second.lastKnownMs > 0) {
            bool check = true;
            if (checkEnemy) {
                if (arc.useFacing && (it->second.lastSeenMs == 0 || timeMs_ - it->second.lastSeenMs >= 5000)) arc.useFacing = false;
            } else {
                check = seesEnemyRecently(e, 250);
            }
            if (check && !covers(vecField(enemy->obj, "origin")) && !covers(it->second.lastKnownPos)) return true;
        }
    }
    if (name) script = name;
    return name != nullptr;
}

// 0x54c000: without an enemy any node; else the enemy must be in front of the node, and the actor not farther from
// the node than the enemy on the enemy's side.
bool World::nodeFacesEnemy(Entity& e, int node) {
    Entity* enemy = entityOf(e.obj->get("enemy"));
    if (!enemy) return true;
    const PathNode& n = nodes_[size_t(node)];
    const Vec3 no = vecField(n.obj, "origin"), eo = vecField(enemy->obj, "origin"), ao = vecField(e.obj, "origin");
    const float yaw = vecField(n.obj, "angles")[1] * float(M_PI) / 180.0f;
    const float ex = eo[0] - no[0], ey = eo[1] - no[1];
    if (ex * std::cos(yaw) + ey * std::sin(yaw) < 0.0f) return false;
    const float ax = ao[0] - no[0], ay = ao[1] - no[1];
    if (ax * ex + ay * ey <= 0.0f) return true;
    return ax * ax + ay * ay <= ex * ex + ey * ey;
}

// 0x54c0c0: usable, facing the enemy and covering from it.
bool World::claimedNodeGood(Entity& e, int node) {
    std::string script;
    return nodeUsable(e, node) && nodeFacesEnemy(e, node) && (!coverScriptAt(e, node, true, script) || !script.empty());
}

// Cover score (0x54bf10): nearness (1 − min(d², 1200²) / 1200²) + 1 when the node sees the enemy's node (0x517ed0)
// + 2 unless concealment + 4 when the enemy is inside the node's arcs (0x54bdd0) + 8 for priority nodes.
float World::coverScore(Entity& e, int node) {
    const PathNode& n = nodes_[size_t(node)];
    const Vec3 no = vecField(n.obj, "origin"), ao = vecField(e.obj, "origin");
    const float d2 = (ao[0] - no[0]) * (ao[0] - no[0]) + (ao[1] - no[1]) * (ao[1] - no[1]) + (ao[2] - no[2]) * (ao[2] - no[2]);
    const float near = 1.0f - (d2 > 0.0f ? std::min(d2, 1440000.0f) : 0.0f) / 1440000.0f;
    float sees = 1.0f;
    Entity* enemy = entityOf(e.obj->get("enemy"));
    if (enemy) {
        const int en = sentientNode(*enemy);
        if (en >= 0 && en != node) {
            const size_t cnt = paths_.size(), hi = size_t(std::max(en, node)), lo = size_t(std::min(en, node));
            const size_t bit = (hi - 1) * cnt + lo; // 0x517ed0 indexing
            const auto& bits = paths_.visibilityBits();
            if (!(bit / 8 < bits.size() && (bits[bit / 8] >> (bit % 8) & 1))) sees = 0.0f;
        }
    }
    const float open = (n.typeNum >= 10 && n.typeNum <= 12) ? 0.0f : 1.0f;
    // 0x54bdd0: whether the node covers the point of interest (0x54b8c0: beyond the node's distance, in front of it or inside
    // its arc, as in coverScriptAt). The point is the enemy's origin, unless the actor has a potential threat
    // (setpotentialthreat, +0x20b4 / +0x20b8 / +0x20bc) and has not seen the enemy within 250 ms (0x54ce10); with no enemy it
    // is the node's origin plus the threat's (cos, sin) offset at the node's height. Without either: not covered.
    float inArc = 0.0f;
    {
        const ActorAi& ai = e.ai;
        bool have = false;
        Vec3 p{};
        if (enemy && (seesEnemyRecently(e, 250) || !ai.potentialThreat)) {
            p = vecField(enemy->obj, "origin");
            have = true;
        } else if (ai.potentialThreat) {
            p = {no[0] + ai.potentialThreatDir[0], no[1] + ai.potentialThreatDir[1], no[2]};
            have = true;
        }
        if (have) {
            const CoverArc arc = coverArc(n.typeNum);
            const float yaw = vecField(n.obj, "angles")[1];
            bool in;
            if (dist2d(no, p) < arc.distSq) in = false;
            else if (arc.useFacing) {
                const float fx = std::cos(yaw * float(M_PI) / 180.0f), fy = std::sin(yaw * float(M_PI) / 180.0f);
                in = (p[0] - no[0]) * fx + (p[1] - no[1]) * fy >= 0.0f;
            } else {
                const float d = angleNormalize360(std::atan2(p[1] - no[1], p[0] - no[0]) * 57.29578f - yaw);
                in = arc.arcMin <= arc.arcMax ? (arc.arcMin <= d && d <= arc.arcMax) : (d >= arc.arcMin || d <= arc.arcMax);
            }
            inArc = in ? 1.0f : 0.0f;
        }
    }
    return near + sees + open + open + inArc * 4.0f + ((n.spawnflags & 0x40) ? 8.0f : 0.0f);
}

// Cover node search (0x54c3f0): cover nodes (types 2…12, 18; not spawnflag 1) within the goal radius (2D) and goal
// height of the goal position (0x516950, at most 256), kept when usable, facing the enemy and covering from it and
// not `ignore` (0x54c2a0); `prefer` scores 16, others need a score of at least that of `better` (+ 1.25e-5) when
// `better` itself is a good node inside the goal. Sorted by score; the best `max` are returned, best last.
std::vector<int> World::findCoverNodes(Entity& e, int better, int ignore, int prefer, size_t max) {
    float minScore = 0.0f;
    std::string script;
    if (better >= 0 && nodeUsable(e, better) && nodeFacesEnemy(e, better) &&
        (!coverScriptAt(e, better, true, script) || !script.empty()) &&
        inActorGoal(e, vecField(nodes_[size_t(better)].obj, "origin")))
        minScore = coverScore(e, better) + 1.25e-5f;
    Vec3 goal = vecField(e.obj, "origin");
    float r = floatField(e.obj, "goalradius", 0.0f), h = floatField(e.obj, "goalheight", 0.0f);
    actorGoal(e, goal, r, h);
    std::vector<std::pair<int, float>> list;
    // TODO(spec): the original walks a node tree (0x516820), so which 256 nodes are taken when more qualify differs.
    for (size_t i = 0; i < nodes_.size() && list.size() < 256; ++i) {
        const PathNode& n = nodes_[i];
        if (!(kCoverTypes >> n.typeNum & 1) || (n.spawnflags & 1)) continue;
        const Vec3 no = vecField(n.obj, "origin");
        const float dz = no[2] - goal[2];
        if (dist2d(no, goal) > r * r || dz * dz > h * h) continue;
        list.push_back({int(i), 0.0f});
    }
    std::vector<std::pair<int, float>> kept;
    for (auto [node, score] : list) {
        if (node == ignore || !nodeUsable(e, node) || !nodeFacesEnemy(e, node)) continue;
        if (coverScriptAt(e, node, true, script) && script.empty()) continue;
        // TODO(spec): nodes outside the goal volume (+0x75c, 0x447830) are dropped too.
        if (node == prefer) {
            score = 16.0f;
        } else {
            score = coverScore(e, node);
            if (score < minScore) continue;
        }
        kept.push_back({node, score});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<int> out;
    for (size_t i = kept.size() > max ? kept.size() - max : 0; i < kept.size(); ++i) out.push_back(kept[i].first);
    return out;
}

// Node update after the exposed / pain / death think (0x4fe740): keeping the claimed node while the actor (or its
// path end) is near it or the plan time is not reached; else release it and claim the node the actor stands at
// when it is a cover node (or flagged 0x8000), near and usable.
void World::updateClaimedNode(Entity& e) {
    if (!e.ai.alive || e.obj->get("keepclaimednode").truthy()) return;
    if (e.obj->get("keepclaimednodeingoal").truthy()) {
        if (e.claimedNode >= 0 && inActorGoal(e, vecField(nodes_[size_t(e.claimedNode)].obj, "origin"))) return;
        e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
    }
    const Vec3 pos = actorHasPath(e) ? pathEnd(e) : vecField(e.obj, "origin");
    if (e.claimedNode >= 0) {
        if (nearPoint(vecField(nodes_[size_t(e.claimedNode)].obj, "origin"), pos)) return;
        if (timeMs_ < e.ai.planTimeMs) return;
        releaseNode(e);
    }
    const int node = sentientNode(e);
    if (node < 0) return;
    const PathNode& n = nodes_[size_t(node)];
    if (!(kCoverTypes >> n.typeNum & 1) && !(n.spawnflags & 0x8000)) return;
    if (!nearPoint(vecField(n.obj, "origin"), pos)) return;
    if (nodeUsable(e, node)) claimNode(e, node);
}

// 0x503650 / 0x502d00: a path to the node. A path that already ends at the node is kept; standing at it needs none.
// When there was no path, a node that is not a corner (types 6…9) and that the actor is near inside the goal needs
// no path either. A failure sets the plan time (0x502c70).
bool World::pathToNode(Entity& e, int node) {
    const Vec3 no = vecField(nodes_[size_t(node)].obj, "origin"), o = vecField(e.obj, "origin");
    const bool hadPath = actorHasPath(e);
    bool ok;
    // A truncated path is kept only while more than a quarter of its planned points are left (+0x6b2 < count · 4).
    const bool keep = !(e.path.flags & 4) || e.path.planned < e.path.count * 4;
    if (hadPath && keep && samePoint(no, pathEnd(e))) {
        ok = true;
    } else if (!hadPath && samePoint(no, o)) {
        ok = true;
    } else {
        ok = pathToPoint(e, no);
        if (!ok) clearActorPath(e);
    }
    if (!actorHasPath(e)) {
        if (!ok) e.ai.planTimeMs = timeMs_ + 500;
        return ok;
    }
    const int type = nodes_[size_t(node)].typeNum;
    if (!hadPath && !(0x3c0 >> type & 1) && nearPoint(no, o) && inActorGoal(e, o)) clearActorPath(e);
    return true;
}

// 0x502cd0 / 0x502960: a path to the goal unless the actor (or its path end) is already inside it.
// TODO(spec): the goal offset of 0x502960 (+0x73c) and its node-based target choice are not read.
bool World::pathToGoal(Entity& e) {
    if (!actorHasPath(e)) {
        if (actorInGoal(e, 0.0f)) return true;
    } else {
        if (inActorGoal(e, pathEnd(e))) return true;
        clearActorPath(e);
    }
    Vec3 gp{};
    float gr = 0, gh = 0;
    const bool ok = actorGoal(e, gp, gr, gh) && pathToPoint(e, gp);
    if (!ok) {
        clearActorPath(e);
        e.ai.planTimeMs = timeMs_ + 500;
    }
    return ok;
}

// The path update (0x503490): with a path, or once the plan time has passed. Keeping the claimed node stops all
// movement for 500 ms. Otherwise the cover node is chosen (0x502da0: the claimed node while it is inside the goal
// and still good, else the best cover node, else the claimed node inside the goal) and walked to; a node that
// cannot be reached is blocked for the team for 5 s; without a node the actor walks into the goal. The chosen node
// is claimed.
void World::updateActorPath(Entity& e) {
    ActorAi& ai = e.ai;
    const bool hadPath = actorHasPath(e);
    if (!hadPath && timeMs_ < ai.planTimeMs) return;
    if (keepClaimedNode(e)) {
        clearActorPath(e);
        ai.planTimeMs = timeMs_ + 500;
        return;
    }
    followFriendlyChain(e); // 0x505ed0
    // 0x503080: fighting = an enemy known within 5 s, or (without a path) one whose node the claimed node sees.
    bool fighting = false;
    if (Entity* enemy = entityOf(e.obj->get("enemy"))) {
        auto it = e.sentientInfo.find(enemy->obj.get());
        fighting = it != e.sentientInfo.end() && it->second.lastKnownMs != 0 && timeMs_ - it->second.lastKnownMs < 5000;
    }
    if (!fighting) ai.pathToEnemy = false;
    updateWorkingGoal(e); // 0x505f70
    int node = -1;
    const int claimed = e.claimedNode;
    const bool claimedInGoal = claimed >= 0 && inActorGoal(e, vecField(nodes_[size_t(claimed)].obj, "origin"));
    if (claimedInGoal && claimedNodeGood(e, claimed)) {
        node = claimed;
    } else {
        const auto best = findCoverNodes(e, -1, -1, -1, 1);
        node = !best.empty() ? best.back() : claimedInGoal ? claimed : -1;
    }
    if (node < 0) {
        if (!ai.pathToEnemy) {
            pathToGoal(e);
        } else {
            ai.pathToEnemy = false;
            clearActorPath(e);
            ai.planTimeMs = timeMs_ + 500;
        }
    } else if (!pathToNode(e, node)) {
        blockNodeForTeam(node, e);
        ai.planTimeMs = timeMs_ + 500;
        node = -1;
    } else if (!actorHasPath(e) && ai.pathToEnemy) {
        ai.pathToEnemy = false;
        ai.planTimeMs = timeMs_ + 500;
    }
    // 0x503200: stop for the enemy. Only its first test is implemented: never without an enemy and a path, or
    // when the enemy is farther (2D) than pathenemyfightdist. TODO(spec): the rest (0x503000, 0x502e80, 0x543ae0).
    if (node >= 0) claimNode(e, node);
}

// Move check towards a point (0x543d30, src/game/actorfollow.cpp) from the actor's origin.
bool World::aiMoveCheck(Entity& e, const Vec3& target, float step, Vec3& out) {
    return aiReach(vecField(e.obj, "origin"), target, e.number, Contents::ActorClipMask | 4, out, step);
}

void World::registerCoverBuiltins() {
    auto actor = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        return *e;
    };
    auto nodeIndex = [this](const Value& v) -> int {
        if (v.isObject())
            for (size_t i = 0; i < nodes_.size(); ++i)
                if (nodes_[i].obj == v.asObject()) return int(i);
        throw std::runtime_error("not a pathnode");
    };
    auto exposed = [](Entity& e) { return e.ai.stack[size_t(e.ai.depth)] == AiExposed; };
    auto hasEnemy = [this](Entity& e) { return entityOf(e.obj->get("enemy")) != nullptr; };
    auto nodeValue = [this](int n) { return n >= 0 ? Value(nodes_[size_t(n)].obj) : Value(); };

    // findcovernode (0x54c510): with an enemy in the exposed state, the 1000 best cover nodes for getcovernode.
    vm_.registerBuiltin("findcovernode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        e.ai.coverNodes.clear();
        if (hasEnemy(e) && exposed(e)) e.ai.coverNodes = findCoverNodes(e, -1, -1, -1, 1000);
        if (std::getenv("IW2_COVER_LOG")) std::fprintf(stderr, "[cover %lld] #%d findcovernode enemy %d exposed %d -> %zu nodes\n", (long long)timeMs_, e.number, int(hasEnemy(e)), int(exposed(e)), e.ai.coverNodes.size());
        return Value();
    });
    // findbestcovernode(["prefer" | "ignore" | "better", node]) (0x53c730): the best cover node, or undefined.
    vm_.registerBuiltin("findbestcovernode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        int better = -1, ignore = -1, prefer = -1;
        if (c.argc() == 2) {
            const std::string kind = c.arg(0).toString();
            if (kind == "prefer") prefer = nodeIndex(c.arg(1));
            else if (kind == "ignore") ignore = nodeIndex(c.arg(1));
            else if (kind == "better") better = nodeIndex(c.arg(1));
            else throw std::runtime_error("findbestcovernode: unknown option " + kind);
        } else if (c.argc() != 0) {
            throw std::runtime_error("findbestcovernode: wrong number of parameters");
        }
        const auto best = findCoverNodes(e, better, ignore, prefer, 1);
        if (std::getenv("IW2_COVER_LOG")) std::fprintf(stderr, "[cover %lld] #%d findbestcovernode(prefer %d ignore %d better %d) -> %d\n", (long long)timeMs_, e.number, prefer, ignore, better, best.empty() ? -1 : best.back());
        return best.empty() ? Value() : nodeValue(best.back());
    });
    // getcovernode (0x54c580): the next node of findcovernode while there is a path or the plan time has passed.
    vm_.registerBuiltin("getcovernode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        auto& list = e.ai.coverNodes;
        if (hasEnemy(e) && exposed(e) && !list.empty() && (actorHasPath(e) || e.ai.planTimeMs <= timeMs_)) {
            const int n = list.back();
            list.pop_back();
            return nodeValue(n);
        }
        list.clear();
        return Value();
    });
    // usecovernode(node) (0x53c900 / 0x54c5e0): in the exposed or turret state claim a usable node that faces the
    // enemy; the turret state is replaced by exposed; substate combat.
    vm_.registerBuiltin("usecovernode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        if (e.obj->get("keepclaimednode").truthy()) throw std::runtime_error("cannot change node when keepclaimednode is set");
        if (e.obj->get("keepclaimednodeingoal").truthy()) {
            if (e.claimedNode >= 0 && inActorGoal(e, vecField(nodes_[size_t(e.claimedNode)].obj, "origin")))
                throw std::runtime_error("cannot change node when keepclaimednodeingoal is set and the node is in the goal");
            e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
        }
        const int node = nodeIndex(c.arg(0));
        const int state = e.ai.stack[size_t(e.ai.depth)];
        if (std::getenv("IW2_COVER_LOG")) std::fprintf(stderr, "[cover %lld] #%d usecovernode(%d) state %d usable %d facesEnemy %d\n", (long long)timeMs_, e.number, node, state, node >= 0 ? int(nodeUsable(e, node)) : -1, node >= 0 ? int(nodeFacesEnemy(e, node)) : -1);
        if (state != AiExposed && state != AiTurret) return Value(0);
        if (!nodeUsable(e, node) || !nodeFacesEnemy(e, node)) return Value(0);
        claimNode(e, node);
        e.ai.coverNodes.clear();
        if (state != AiExposed) aiSetState(e, AiExposed);
        e.ai.substate = kSubCombat;
        return Value(1);
    });
    // findreacquirenode (0x53ca30): the 10 best cover nodes for getreacquirenode.
    vm_.registerBuiltin("findreacquirenode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        if (hasEnemy(e) && exposed(e)) e.ai.reacquireNodes = findCoverNodes(e, -1, -1, -1, 10);
        return Value();
    });
    vm_.registerBuiltin("getreacquirenode", [=, this](CallContext& c) { // 0x548e00
        Entity& e = actor(c);
        auto& list = e.ai.reacquireNodes;
        if (hasEnemy(e) && exposed(e) && !list.empty() && (actorHasPath(e) || e.ai.planTimeMs <= timeMs_)) {
            const int n = list.back();
            list.pop_back();
            return nodeValue(n);
        }
        list.clear();
        return Value();
    });
    // usereacquirenode(node) (0x548e60): a usable node from which the enemy's eye is visible and that has a path
    // is claimed; substate reacquire. TODO(spec): the path comes from 0x503890 (node-to-node planner).
    vm_.registerBuiltin("usereacquirenode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        if (e.obj->get("keepclaimednode").truthy()) throw std::runtime_error("cannot change node when keepclaimednode is set");
        const int node = nodeIndex(c.arg(0));
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy || !exposed(e) || !nodeUsable(e, node)) return Value(0);
        // 0x54c7d0 with no distance limit: a sight trace (0x54c760) from the node origin raised by 64 (0x56f6e0)
        // to the enemy's eye.
        const Vec3 no = vecField(nodes_[size_t(node)].obj, "origin");
        if (collision_->sightBlocked({no[0], no[1], no[2] + 64.0f}, sightEye(*enemy, nullptr), {0, 0, 0}, {0, 0, 0},
                                     Contents::SightMask, e.number, enemy->number))
            return Value(0);
        if (!pathToNode(e, node) || !actorHasPath(e)) return Value(0);
        claimNode(e, node);
        e.ai.reacquireNodes.clear();
        e.ai.substate = kSubReacquire;
        return Value(1);
    });
    // reacquirestep(dist) (0x548f10): try a side step of `dist` units (both sides, random order) from which the
    // enemy's eye is visible; with a path there inside the goal: substate reacquire, facing 3.
    vm_.registerBuiltin("reacquirestep", [=, this](CallContext& c) {
        Entity& e = actor(c);
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy || !exposed(e)) return Value(0);
        const float dist = c.arg(0).asFloat();
        const Vec3 eo = vecField(enemy->obj, "origin"), o = vecField(e.obj, "origin");
        float dx = eo[0] - o[0], dy = eo[1] - o[1];
        const float l2 = dx * dx + dy * dy;
        if (!(l2 > 1.0f)) return Value(0);
        const float l = std::sqrt(l2);
        dx /= l;
        dy /= l;
        const Vec3 enemyEye = sightEye(*enemy, nullptr), eye = sightEye(e, nullptr);
        static std::minstd_rand rng(1);
        const int first = int(rng() & 1);
        const float maxd = floatField(e.obj, "maxsightdistsqrd", 0.0f);
        for (int i = 0; i < 2; ++i) {
            const float side = ((i ^ first) == 0) ? -1.0f : 1.0f; // table 0x5918a8 {−1, 1}
            const float ox = dy * dist * side, oy = -dx * dist * side;
            const Vec3 from{eye[0] + ox, eye[1] + oy, eye[2]};
            const float ex = enemyEye[0] - from[0], ey = enemyEye[1] - from[1], ez = enemyEye[2] - from[2];
            if (maxd != 0.0f && ex * ex + ey * ey + ez * ez >= maxd) continue;
            if (collision_->sightBlocked(from, enemyEye, {0, 0, 0}, {0, 0, 0}, Contents::SightMask, e.number, enemy->number)) continue;
            Vec3 at;
            const float step = e.obj->get("anim_pose").toString() == "crouch" ? 10.0f : 18.0f; // TODO(spec): actor +0x288
            if (!aiMoveCheck(e, {o[0] + ox, o[1] + oy, o[2]}, step, at) || !inActorGoal(e, at)) continue;
            if (pathToPoint(e, at) && actorHasPath(e)) {
                e.ai.substate = kSubReacquire;
                e.ai.facing = 3;
                return Value(1);
            }
        }
        return Value(0);
    });
    // findreacquiredirectpath (0x549280) / findreacquireproximatepath (0x549340): a path towards the enemy, cut
    // to the goal (0x5047c0); without an enemy in the exposed state the path is cleared.
    // TODO(spec): 0x503a50 / 0x503be0 (direct and proximate planners) and the trim fields +0x70c / +0x710.
    auto reacquirePath = [=, this](CallContext& c) {
        Entity& e = actor(c);
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy || !exposed(e)) {
            clearActorPath(e);
            return Value();
        }
        pathToPoint(e, vecField(enemy->obj, "origin"));
        // 0x5434e0: drop the path when its start is outside the goal, else cut it after the last point in the goal.
        if (actorHasPath(e)) {
            ActorPath& p = e.path;
            if (!inActorGoal(e, p.pts[size_t(p.count - 1)].pos)) {
                clearActorPath(e);
            } else {
                int keep = p.count - 2; // lowest index still kept, walking from the start end
                while (keep >= 0 && inActorGoal(e, p.pts[size_t(keep)].pos)) --keep;
                dropPathGoalPoints(p, keep + 1);
            }
        }
        return Value();
    };
    vm_.registerBuiltin("findreacquiredirectpath", reacquirePath);
    vm_.registerBuiltin("findreacquireproximatepath", reacquirePath);
    // trimpathtoattack (0x504830 → 0x543580): shorten the path by one point per call until the enemy's eye is
    // visible from its end. TODO(spec): the original's trim state (+0x70c / +0x710) and 0x5433e0.
    vm_.registerBuiltin("trimpathtoattack", [=, this](CallContext& c) {
        Entity& e = actor(c);
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy || e.path.count < 2) return Value(0);
        const Vec3 eye = sightEye(*enemy, nullptr);
        const Vec3 end = e.path.pts[1].pos;
        if (!collision_->sightBlocked({end[0], end[1], end[2] + 64.0f}, eye, {0, 0, 0}, {0, 0, 0}, Contents::SightMask, e.number, enemy->number)) {
            dropPathGoalPoints(e.path, 1);
            return Value(0);
        }
        dropPathGoalPoints(e.path, 1);
        return Value(1);
    });
    // reacquiremove (0x53ccb0): with an enemy, in the exposed state and with a path: substate reacquire.
    vm_.registerBuiltin("reacquiremove", [=, this](CallContext& c) {
        Entity& e = actor(c);
        if (hasEnemy(e) && exposed(e) && actorHasPath(e)) {
            e.ai.substate = kSubReacquire;
            return Value(1);
        }
        return Value(0);
    });
    // flagenemyunattackable (0x53cd90): ignore the enemy for good (ignore-until 0x7fffffff), substate noncombat.
    vm_.registerBuiltin("flagenemyunattackable", [=, this](CallContext& c) {
        Entity& e = actor(c);
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (enemy && exposed(e)) {
            e.sentientInfo[enemy->obj.get()].ignoreUntilMs = std::numeric_limits<int64_t>::max();
            e.ai.substate = kSubNoncombat;
        }
        return Value();
    });
    // withinapproxpathdist(dist) (0x545030): no path, or the rest of the path shorter than dist.
    vm_.registerBuiltin("withinapproxpathdist", [=, this](CallContext& c) {
        Entity& e = actor(c);
        if (!actorHasPath(e)) return Value(1);
        const float limit = c.arg(0).asFloat();
        Vec3 p = vecField(e.obj, "origin");
        float len = 0.0f;
        for (int i = e.path.count > 1 ? e.path.count - 2 : 0; i >= 0; --i) {
            const Vec3& q = e.path.pts[size_t(i)].pos;
            len += std::sqrt(dist2d(p, q));
            if (len >= limit) return Value(0);
            p = q;
        }
        return Value(1);
    });
    // nearnode(node) (0x540ca0 → 0x502660).
    vm_.registerBuiltin("nearnode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        return Value(nearPoint(vecField(nodes_[size_t(nodeIndex(c.arg(0)))].obj, "origin"), vecField(e.obj, "origin")) ? 1 : 0);
    });
    // canattackenemynode (0x540500): the actor's node sees its enemy's node (0x517e40 indexing).
    vm_.registerBuiltin("canattackenemynode", [=, this](CallContext& c) {
        Entity& e = actor(c);
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (!enemy) return Value(0);
        const int a = sentientNode(e), b = sentientNode(*enemy);
        if (a < 0 || b < 0) return Value(0);
        if (a == b) return Value(1);
        const size_t bit = paths_.size() * size_t(std::min(a, b)) + size_t(std::max(a, b));
        const auto& bits = paths_.visibilityBits();
        return Value(bit / 8 < bits.size() && (bits[bit / 8] >> (bit % 8) & 1) ? 1 : 0);
    });
    // isnodeoccupied(node) (0x5158c0): held or claimed (now ≤ +0x44).
    vm_.registerBuiltin("isnodeoccupied", [=, this](CallContext& c) {
        return Value(timeMs_ <= nodes_[size_t(nodeIndex(c.arg(0)))].ownerUntilMs ? 1 : 0);
    });
    // setnodepriority(node, on) (0x515810): spawnflag 0x40, for the node types listed at 0x5a9468 (18, 2, 3, 5,
    // 10, 11, 12, 6, 7).
    vm_.registerBuiltin("setnodepriority", [=, this](CallContext& c) {
        PathNode& n = nodes_[size_t(nodeIndex(c.arg(0)))];
        if (!((1u << 18 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 6 | 1u << 7) >> n.typeNum & 1))
            throw std::runtime_error("setnodepriority: node type " + n.obj->get("type").toString() + " cannot have a priority");
        if (c.arg(1).truthy()) n.spawnflags |= 0x40;
        else n.spawnflags &= ~0x40;
        return Value();
    });
}

} // namespace iw2::game
