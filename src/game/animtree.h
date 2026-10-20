// Animation trees (animtrees/<name>.atr) and per-entity blending state (docs/spec/animtree.md).
#pragma once

#include "assets/xanim.h"
#include "fs/vfs.h"

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
};

class AnimTree {
public:
    // Parses the .atr text. Node 0 is the implicit "root".
    AnimTree(std::string name, std::string_view text);

    const std::string& name() const { return name_; }
    const std::vector<AnimNode>& nodes() const { return nodes_; }
    int find(const std::string& name) const; // -1 if absent

private:
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

struct AnimNodeState {
    float weight = 0, goalWeight = 0, weightRate = 0; // weight change per second
    float time = 0;                                   // normalized 0..1
    float rate = 1;
    bool ended = false;
    std::string notify;                               // flagged: notify name for notetracks
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

    // Advances time; `onNote(notifyName, noteName)` is called for crossed notetracks and "end".
    void update(float dt, const std::function<void(const std::string&, const std::string&)>& onNote);

    float time(int n) const { return nodes_[size_t(n)].time; }
    const AnimTree& tree() const { return tree_; }

    // Horizontal root-motion speed (units/s) of the currently weighted animations with delta tracks.
    float moveSpeed() const;

    // Blended local pose for a skeleton.
    assets::XPose pose(const assets::XModelParts& parts) const;

private:
    void setWeight(int n, float goal, float blendTime);
    float effectiveWeight(int n) const;

    const AnimTree& tree_;
    AnimLibrary& lib_;
    std::vector<AnimNodeState> nodes_;
};

} // namespace iw2::game
