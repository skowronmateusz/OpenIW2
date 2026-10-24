// Animation trees (animtrees/<name>.atr) and per-entity blending state (docs/spec/animtree.md).
#pragma once

#include "assets/xanim.h"
#include "fs/vfs.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

struct AnimNode {
    std::string name;          // lower case
    int parent = -1;
    std::vector<int> children;
    std::string sync;          // "loopsync", "nonloopsync" or empty
    bool complete = false;     // ": complete …"
    // Runtime sync flags (0x479260 / 0x479120): 1 loop synced, 2 nonloop synced (a blend node with the modifier and
    // every blend node below it), 4 the synced node that carries the modifier (the top of the synced subtree).
    int syncFlags = 0;
};

class AnimTree {
public:
    // Parses the .atr text. Node 0 is the implicit "root".
    AnimTree(std::string name, std::string_view text);

    const std::string& name() const { return name_; }
    const std::vector<AnimNode>& nodes() const { return nodes_; }
    int find(const std::string& name) const; // -1 if absent

private:
    void markSync(int n, int type);

    std::string name_;
    std::vector<AnimNode> nodes_;
    std::unordered_map<std::string, int> index_;
};

// Loads trees and animations from the game data, with caching.
class AnimLibrary {
public:
    explicit AnimLibrary(const fs::Vfs& vfs) : vfs_(vfs) {}
    const AnimTree* tree(const std::string& name);
    const assets::XAnim* anim(const std::string& name); // nullptr for blend nodes without an xanim file

private:
    const fs::Vfs& vfs_;
    std::map<std::string, std::unique_ptr<AnimTree>> trees_;
    std::map<std::string, std::unique_ptr<assets::XAnim>> anims_;
    std::map<std::string, bool> missing_;
};

// One node's state (the exe's 0x28-byte entries at 0xa0a200: +0xc time, +0x10 old time, +0x14 / +0x16 cycle and
// old cycle, +0x18 blend time left, +0x1c goal weight, +0x20 weight, +0x24 rate, +0x4 notify name).
struct AnimNodeState {
    float weight = 0, goalWeight = 0, blendLeft = 0; // blendLeft: seconds left to reach goalWeight
    float time = 0;                                   // normalized 0..1
    float oldTime = 0;                                // the time at the start of this frame's time update
    int cycle = 0, oldCycle = 0;                      // loops completed (time wraps)
    float rate = 1;
    bool ended = false;
    std::string notify;                               // flagged: notify name for notetracks
};

// Root motion of the weighted animations over the last time update (0x478550 → 0x477b80).
struct AnimDelta {
    float weight = 0;              // summed weight of the animations with delta tracks
    std::array<float, 3> trans{};  // translation (length preserved; TODO(spec): the rotation into the start frame, 0x4758f0)
};

// Per-entity animation state on one tree.
class AnimState {
public:
    AnimState(const AnimTree& tree, AnimLibrary& lib);

    struct SetArgs {
        float weight = 1.0f;
        float blendTime = 0.2f;
        float rate = 1.0f;
        bool knob = false;    // zero the node's siblings
        int knobAllRoot = -1; // also knob every ancestor up to this node
        bool restart = false;
        std::string notify;   // flagged variants
    };
    // Applies one of the setanim* calls to node `n`.
    void set(int n, const SetArgs& a);
    void clear(int n, float blendTime);
    // 0x4789a0: clear (0x478940) every child of the node.
    void clearChildren(int n, float blendTime);
    // The looping flag of a leaf's animation (xanim +2). TODO(spec): a blend node's flag (entry +0x10 bit 1).
    bool looping(int n) const;
    void restart(int n);

    // Advances time; `onNote(notifyName, noteName)` is called for crossed notetracks and "end".
    void update(float dt, const std::function<void(const std::string&, const std::string&)>& onNote);

    float time(int n) const { return nodes_[size_t(n)].time; }
    const AnimTree& tree() const { return tree_; }

    // Debug: active nodes with weight, goal, time, rate and notify name.
    std::string debugString() const;

    // Translation of the delta tracks between the old and new time of every weighted leaf (0x478550). With
    // `goalWeights` the goal weights pick and weigh the animations instead of the current ones (actor byte +0x220).
    AnimDelta delta(bool goalWeights) const;
    // Delta of one animation at its current time (0x4785d0 → 0x477b80 with +6 set → 0x475aa0): the yaw quaternion
    // (z, w), (0, 1) without rotation, and the translation. TODO(spec): blend nodes (0x477b80's child walk) give none.
    void nodeDelta(int n, float& qz, float& qw, std::array<float, 3>& trans) const;
    // 0x478020: the node has played through (its time reached 1, went back or completed a cycle since the last
    // update), or has no state.
    bool playedThrough(int n) const;

    // Blended local pose for a skeleton.
    assets::XPose pose(const assets::XModelParts& parts) const;

private:
    void setWeight(int n, float goal, float blendTime);
    void clearNode(int n, float blendTime);
    void knobSiblings(int n, float goal, float blendTime);
    using NoteFn = std::function<void(const std::string&, const std::string&)>;
    void updateWeight(int n, float inheritedTime, int inheritedCycle, float dt, bool parentActive, bool& alive, bool& playing);
    void resetSubtree(int n);
    float syncFreq(int n) const;
    void updateTime(int n, float dt, bool notes, const NoteFn& onNote);
    void syncTo(int n, const AnimNodeState& top, bool notes, const NoteFn& onNote);
    void advanceLeaf(int n, float to, int toCycle, bool notes, const NoteFn& onNote);
    void deltaNode(int n, float weight, bool first, bool nested, bool goalWeights, AnimDelta& out) const;
    void calc(int n, float weight, assets::AnimAccum& acc, bool top, const assets::XModelParts& parts) const;

    const AnimTree& tree_;
    AnimLibrary& lib_;
    std::vector<AnimNodeState> nodes_;
};

} // namespace iw2::game
