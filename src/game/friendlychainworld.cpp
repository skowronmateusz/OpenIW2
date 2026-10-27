// Friendly chains in the world (docs/spec/ai.md "Friendly chain"): the load-time build (0x52d630), setfriendlychain
// (0x4eeb10), the trigger_friendlychain touch (0x529140 → 0x518bf0) and the follower update of an actor whose goal
// entity is a friendly sentient (0x505ed0 → 0x506250).
#include "game/world.h"

#include <cstdio>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

std::string strField(const ObjectPtr& o, const char* key) {
    const Value v = o->get(key);
    return v.isString() ? v.asString() : std::string();
}

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

int teamIdx(const Entity& e) {
    const std::string s = strField(e.obj, "team");
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

// 0x550f10: the team table {0, 2, 1, 0, 0}.
bool enemyTeams(const Entity& me, const Entity& other) {
    static constexpr int kEnemy[5] = {0, 2, 1, 0, 0};
    return teamIdx(other) == kEnemy[teamIdx(me)];
}

std::string fmtPos(const Vec3& p, const char* f) {
    char b[160];
    std::snprintf(b, sizeof b, f, double(p[0]), double(p[1]), double(p[2]));
    return b;
}

std::string nameOf(const Entity& e) {
    const std::string t = strField(e.obj, "targetname");
    return t.empty() ? e.classname : t;
}

} // namespace

void World::buildFriendlyChains() {
    std::vector<ChainNodeIn> in;
    for (const PathNode& n : nodes_) {
        ChainNodeIn c;
        c.name = strField(n.obj, "targetname");
        c.target = strField(n.obj, "target");
        c.spawnflags = n.spawnflags;
        c.typeNum = n.typeNum;
        c.origin = originOf(n.obj);
        in.push_back(std::move(c));
    }
    chains_.build(in);
}

// setfriendlychain: the node must be on a chain (+0x34 >= 1).
void World::setFriendlyChain(Entity& sentient, int node) {
    if (chains_.chainId(node) < 1)
        throw std::runtime_error("Node " + std::to_string(node) +
                                 fmtPos(originOf(nodes_[size_t(node)].obj), " (%.2f, %.2f, %.2f) is not a friendly chain node.\n"));
    sentient.friendlyChain = node;
}

// Touch 0x529140 → 0x518bf0: the chain node named by the trigger's target becomes the toucher's chain node (when
// g_spawnai is on; default 1, TODO(spec): the dvar is not modelled). TODO(spec): the toucher filter of 0x529140.
// The exe's Com_Error cases are logged and ignored (the touch runs outside the VM).
void World::touchFriendlyChain(Entity& trigger, Entity& other) {
    if (!isSentient(other)) return;
    const std::string id = strField(trigger.obj, "target");
    if (id.empty()) return print("ERROR: trigger_friendlychain must target a friendly chain node");
    if (other.friendlyChain >= 0 && strField(nodes_[size_t(other.friendlyChain)].obj, "targetname") == id) return;
    const int node = chains_.findByName(id);
    if (node >= 0) {
        other.friendlyChain = node;
        return;
    }
    for (const PathNode& n : nodes_)
        if (strField(n.obj, "targetname") == id) return print("ERROR: Node '" + id + "' is not part of a friendly chain");
    print("ERROR: Friendly chain node '" + id + "' does not exist");
}

// 0x505ed0: an actor with a goal entity that is a sentient follows it along the friendly chain.
void World::followFriendlyChain(Entity& e) {
    Entity* S = entityOf(e.obj->get("$goalent"));
    if (!S || !isSentient(*S)) return;
    const Value minV = e.obj->get("followmin"), maxV = e.obj->get("followmax");
    const int minOff = minV.isNumber() ? minV.asInt() : 0, maxOff = maxV.isNumber() ? maxV.asInt() : 0;
    if (enemyTeams(e, *S)) {
        SentientInfo& in = e.sentientInfo[S->obj.get()]; // 0x500c50
        in.lastKnownMs = timeMs_;
        in.lastKnownPos = originOf(S->obj);
        in.ownPosThen = originOf(e.obj);
        e.chainNode = -1;
    } else if (minOff > maxOff) {
        e.chainNode = -1;
    } else {
        // 0x506250
        if (e.chainNode >= 0 && e.claimedNode < 0 && !nodeUsable(e, e.chainNode)) e.chainNode = -1;
        if (S->friendlyChainMs != timeMs_) { // the followed sentient moves along its chain once a frame
            S->friendlyChainMs = timeMs_;
            const int near = chains_.nearest(S->friendlyChain, originOf(S->obj));
            if (near >= 0) S->friendlyChain = near;
        }
        const int L = S->friendlyChain;
        if (L < 0) {
            // Com_Error(drop) in the exe. The port logs it and keeps going: these run outside the VM, where an exception
            // would end the whole session, and the log line points at the script-flow defect that caused it.
            print("ERROR: Sentient " + nameOf(e) + fmtPos(originOf(e.obj), " at (%.0f %.0f %.0f) is following the sentient ") +
                  nameOf(*S) + fmtPos(originOf(S->obj), " at (%.0f %.0f %.0f) that is not on a friendly chain"));
            return;
        }
        bool keep = false;
        if (e.chainNode >= 0 && chains_.chainId(e.chainNode) == chains_.chainId(L)) {
            const int rel = chains_.pos(e.chainNode) - chains_.pos(L);
            const Vec3 d = originOf(e.obj), o = originOf(nodes_[size_t(e.chainNode)].obj);
            const float dx = d[0] - o[0], dy = d[1] - o[1], dz = d[2] - o[2];
            keep = rel >= minOff && rel <= maxOff && dx * dx + dy * dy + dz * dz <= 4096.0f;
        }
        if (!keep) {
            ChainEnv env;
            env.now = timeMs_;
            env.hold = [this](int n) { return nodes_[size_t(n)].ownerUntilMs; };
            env.ok = [this, &e](int n) {
                if (!nodeUsable(e, n)) return false;
                for (const auto& a : entities_) // 0x518510: no other actor of a non-enemy team stands on this chain node
                    if (a.get() != &e && a->isActor && !a->obj->removed && !enemyTeams(e, *a) && a->chainNode == n) return false;
                return true;
            };
            const Value fb = e.obj->get("chainfallback");
            e.chainNode = chains_.choose(L, minOff, maxOff, e.chainNode, fb.truthy(), env);
        }
    }
    if (e.chainNode >= 0 && e.claimedNode < 0 && nodeUsable(e, e.chainNode)) claimNode(e, e.chainNode);
}

} // namespace iw2::game
