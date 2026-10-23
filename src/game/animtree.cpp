#include "game/animtree.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace iw2::game {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

AnimTree::AnimTree(std::string name, std::string_view text) : name_(std::move(name)) {
    nodes_.push_back({"root", -1, {}, {}, false});
    index_["root"] = 0;
    std::vector<int> stack = {0};
    int last = -1;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '{') {
            if (last < 0) throw std::runtime_error("animtree " + name_ + ": '{' without a node");
            stack.push_back(last);
            ++i;
        } else if (c == '}') {
            if (stack.size() <= 1) throw std::runtime_error("animtree " + name_ + ": unbalanced '}'");
            stack.pop_back();
            ++i;
        } else if (c == ':') {
            // Modifiers until end of line: "complete", "loopsync", "nonloopsync".
            size_t e = i + 1;
            while (e < text.size() && text[e] != '\n' && text[e] != '{') ++e;
            const std::string mods = lower(std::string(text.substr(i + 1, e - i - 1)));
            if (last >= 0) {
                if (mods.find("nonloopsync") != std::string::npos) nodes_[size_t(last)].sync = "nonloopsync";
                else if (mods.find("loopsync") != std::string::npos) nodes_[size_t(last)].sync = "loopsync";
                if (mods.find("complete") != std::string::npos) nodes_[size_t(last)].complete = true;
            }
            i = e;
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            size_t e = i;
            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '_' || text[e] == '-')) ++e;
            const std::string n = lower(std::string(text.substr(i, e - i)));
            AnimNode node;
            node.name = n;
            node.parent = stack.back();
            last = int(nodes_.size());
            nodes_[size_t(node.parent)].children.push_back(last);
            if (!index_.count(n)) index_[n] = last;
            nodes_.push_back(std::move(node));
            i = e;
        } else {
            ++i;
        }
    }
}

int AnimTree::find(const std::string& name) const {
    auto it = index_.find(lower(name));
    return it == index_.end() ? -1 : it->second;
}

const AnimTree* AnimLibrary::tree(const std::string& name) {
    const std::string n = lower(name);
    auto it = trees_.find(n);
    if (it != trees_.end()) return it->second.get();
    auto d = vfs_.read("animtrees/" + n + ".atr");
    if (!d) return nullptr;
    auto t = std::make_unique<AnimTree>(n, std::string_view(reinterpret_cast<const char*>(d->data()), d->size()));
    return (trees_[n] = std::move(t)).get();
}

const assets::XAnim* AnimLibrary::anim(const std::string& name) {
    const std::string n = lower(name);
    if (auto it = anims_.find(n); it != anims_.end()) return it->second.get();
    if (missing_.count(n)) return nullptr;
    auto d = vfs_.read("xanim/" + n);
    if (!d) {
        missing_[n] = true;
        return nullptr;
    }
    try {
        auto a = std::make_unique<assets::XAnim>(assets::parseXAnim(*d));
        return (anims_[n] = std::move(a)).get();
    } catch (const std::exception&) {
        missing_[n] = true;
        return nullptr;
    }
}

AnimState::AnimState(const AnimTree& tree, AnimLibrary& lib) : tree_(tree), lib_(lib), nodes_(tree.nodes().size()) {
    nodes_[0].weight = nodes_[0].goalWeight = 1.0f; // the root is always on
}

// 0x478f10: a blend time of 0 jumps to the goal; otherwise a node at weight 0 starts from goal × 0.001 and the time
// left is the blend time scaled by the fraction of the way still to go.
void AnimState::setWeight(int n, float goal, float blendTime) {
    if (n == 0) return; // the root stays at weight 1
    AnimNodeState& s = nodes_[size_t(n)];
    if (blendTime == 0) s.weight = goal;
    else if (s.weight == 0) s.weight = goal * 0.001f;
    if (goal == 0) {
        if (s.goalWeight != 0) s.blendLeft = blendTime;
        else if (blendTime < s.blendLeft) s.blendLeft = blendTime;
    } else if (goal < s.weight) {
        s.blendLeft = (s.weight - goal) / s.weight * blendTime;
    } else {
        s.blendLeft = (goal - s.weight) / goal * blendTime;
    }
    s.goalWeight = goal;
}

// Knob (0x4789e0): the node's siblings go to goal 0 over the blend time scaled by the largest change in the
// family, i.e. the largest sibling weight or the node's own |goal − weight| (clear 0x478890).
void AnimState::knobSiblings(int n, float goal, float blendTime) {
    const auto& nodes = tree_.nodes();
    const int p = nodes[size_t(n)].parent;
    if (p < 0) return;
    float most = 0;
    for (int sib : nodes[size_t(p)].children) {
        const AnimNodeState& s = nodes_[size_t(sib)];
        most = std::max(most, sib == n ? std::fabs(goal - s.weight) : s.weight);
    }
    for (int sib : nodes[size_t(p)].children)
        if (sib != n) clearNode(sib, most * blendTime);
}

// 0x478890: goal 0 over the time (keeping a shorter time already running to 0), a time of 0 zeroes the weight;
// the notify name is dropped.
void AnimState::clearNode(int n, float blendTime) {
    if (n == 0) return; // the root stays at weight 1
    AnimNodeState& s = nodes_[size_t(n)];
    if (s.goalWeight != 0 || blendTime < s.blendLeft) s.blendLeft = blendTime;
    s.goalWeight = 0;
    if (blendTime == 0) s.weight = 0;
    s.notify.clear();
}

// The script calls (0x4f6930 setanim, 0x4f63d0 setanimknob, 0x4f6660 setanimknoball). A weight below 0.001 counts
// as 0. Setting a node (0x478f10) also replaces its notify name (none for the unflagged calls).
// - setanim (0x4795e0): the node is set, then every ancestor whose goal weight is 0 goes to goal 1 over the same
//   blend time at rate 1.
// - setanimknob: the siblings are knobbed (0x4789e0) first, then as setanim.
// - setanimknoball (0x478c40): knob, set the node, then each ancestor below the given root is knobbed and set to
//   goal 1 over the blend time at rate 1.
// The restart variants restart the node's time after it is set (0x4793c0).
void AnimState::set(int n, const SetArgs& a) {
    if (n < 0 || size_t(n) >= nodes_.size()) return;
    const auto& nodes = tree_.nodes();
    const float weight = a.weight < 0.001f ? 0.0f : a.weight;
    auto setNode = [&](int node, float w, float rate, const std::string& notify) {
        setWeight(node, w, a.blendTime);
        AnimNodeState& s = nodes_[size_t(node)];
        s.rate = rate;
        s.notify = notify;
    };
    if (a.knob || a.knobAllRoot >= 0) knobSiblings(n, weight, a.blendTime);
    setNode(n, weight, a.rate, a.notify);
    if (a.restart) restart(n);
    if (a.knobAllRoot >= 0) {
        for (int p = nodes[size_t(n)].parent; p >= 0 && p != a.knobAllRoot; p = nodes[size_t(p)].parent) {
            knobSiblings(p, 1.0f, a.blendTime);
            setNode(p, 1.0f, 1.0f, {});
        }
        return;
    }
    for (int p = nodes[size_t(n)].parent; p > 0; p = nodes[size_t(p)].parent)
        if (nodes_[size_t(p)].goalWeight == 0) setNode(p, 1.0f, 1.0f, {});
}

// clearanim (0x4f6300 → 0x478940): the node and every descendant go to goal 0 (0x478890).
void AnimState::clear(int n, float blendTime) {
    if (n < 0 || size_t(n) >= nodes_.size()) return;
    clearNode(n, blendTime);
    for (int c : tree_.nodes()[size_t(n)].children) clear(c, blendTime);
}

// 0x4793c0: TODO(spec): a node under a synced blend node (tree flag 4) restarts that whole synced subtree; only
// the node itself is restarted here.
void AnimState::restart(int n) {
    AnimNodeState& s = nodes_[size_t(n)];
    s.time = 0;
    s.ended = false;
}

// 0x476960: weights move linearly to the goal over the time left; under an inactive parent they jump to the goal.
// A node at weight and goal 0 with no live children is released, so its time starts again at 0 when it is next set.
bool AnimState::updateWeight(int n, float dt, bool parentActive) {
    AnimNodeState& s = nodes_[size_t(n)];
    const bool active = parentActive && s.weight != 0;
    if (n != 0) {
        if (!parentActive || s.blendLeft <= dt) {
            s.weight = s.goalWeight;
            s.blendLeft = 0;
        } else {
            s.weight += (s.goalWeight - s.weight) / s.blendLeft * dt;
            if (s.weight < 1e-6f) s.weight = s.goalWeight * 0.001f;
            s.blendLeft -= dt;
        }
    }
    bool childAlive = false;
    for (int c : tree_.nodes()[size_t(n)].children) childAlive |= updateWeight(c, dt, active);
    const bool alive = n == 0 || active || s.goalWeight != 0 || childAlive;
    if (!alive) {
        s.time = 0;
        s.ended = false;
    }
    return alive;
}

// 0x476490: only nodes with weight advance. A leaf moves by frameRate / (keyCount − 1) × rate × dt in normalized
// time; a blend node passes dt × its rate to its children. (Synced blend nodes, 0x475bb0, are not done yet: they
// are treated like the others.)
void AnimState::updateTime(int n, float dt, const std::function<void(const std::string&, const std::string&)>& onNote) {
    AnimNodeState& s = nodes_[size_t(n)];
    if (s.weight == 0) return;
    const auto& node = tree_.nodes()[size_t(n)];
    if (!node.children.empty()) {
        if (s.rate * dt == 0) return;
        for (int c : node.children) updateTime(c, dt * s.rate, onNote);
        return;
    }
    const assets::XAnim* a = lib_.anim(node.name);
    if (!a) return;
    const int frames = a->keyCount() - 1;
    const float step = (frames > 0 ? float(a->frameRate) / float(frames) : 0.0f) * s.rate * dt;
    if (step == 0) return;
    const float before = s.time;
    float after = before + step;
    if (a->looping) {
        if (after >= 1.0f) {
            for (const auto& note : a->notes)
                if (note.time > before && !s.notify.empty()) onNote(s.notify, note.name);
            after = std::fmod(after, 1.0f);
            for (const auto& note : a->notes)
                if (note.time <= after && note.name != "end" && !s.notify.empty()) onNote(s.notify, note.name);
        } else {
            for (const auto& note : a->notes)
                if (note.time > before && note.time <= after && note.name != "end" && !s.notify.empty()) onNote(s.notify, note.name);
        }
    } else {
        after = std::min(after, 1.0f);
        for (const auto& note : a->notes) {
            if (note.name == "end") continue;
            if (note.time > before && note.time <= after && !s.notify.empty()) onNote(s.notify, note.name);
        }
        if (after >= 1.0f && !s.ended) {
            s.ended = true;
            if (!s.notify.empty()) onNote(s.notify, "end");
        }
    }
    s.time = after;
}

void AnimState::update(float dt, const std::function<void(const std::string&, const std::string&)>& onNote) {
    updateWeight(0, dt, true);
    updateTime(0, dt, onNote);
}

float AnimState::effectiveWeight(int n) const {
    const auto& nodes = tree_.nodes();
    float w = 1.0f;
    for (int c = n; c > 0; c = nodes[size_t(c)].parent) {
        const int p = nodes[size_t(c)].parent;
        float sum = 0;
        for (int sib : nodes[size_t(p)].children) sum += std::max(0.0f, nodes_[size_t(sib)].weight);
        const float own = std::max(0.0f, nodes_[size_t(c)].weight);
        if (sum <= 0 || own <= 0) return 0.0f;
        w *= own / std::max(sum, 1.0f); // weights below 1 in total fade towards the base pose (inferred)
    }
    return w;
}

std::string AnimState::debugString() const {
    std::string out;
    const auto& nodes = tree_.nodes();
    for (size_t i = 1; i < nodes.size(); ++i) {
        const AnimNodeState& s = nodes_[i];
        if (s.weight <= 0 && s.goalWeight <= 0) continue;
        char buf[256];
        std::snprintf(buf, sizeof buf, "  %s w=%.2f goal=%.2f t=%.2f rate=%.2f notify=%s leaf=%d anim=%d\n", nodes[i].name.c_str(), s.weight, s.goalWeight,
                      s.time, s.rate, s.notify.c_str(), int(nodes[i].children.empty()), lib_.anim(nodes[i].name) != nullptr);
        out += buf;
    }
    return out;
}

float AnimState::moveSpeed() const {
    const auto& nodes = tree_.nodes();
    float speed = 0;
    for (size_t i = 1; i < nodes.size(); ++i) {
        if (!nodes[i].children.empty()) continue;
        const float w = effectiveWeight(int(i));
        if (w <= 0.001f) continue;
        const assets::XAnim* a = lib_.anim(nodes[i].name);
        if (!a || !a->delta || a->deltaTrans.keys.size() < 2 || a->length() <= 0) continue;
        const auto& f = a->deltaTrans.keys.front();
        const auto& l = a->deltaTrans.keys.back();
        const float d = std::sqrt((l[0] - f[0]) * (l[0] - f[0]) + (l[1] - f[1]) * (l[1] - f[1]));
        speed += w * d / a->length() * nodes_[i].rate;
    }
    return speed;
}

// 0x4771e0: a node with one active child passes its weight down unchanged. The active children of a node with
// several are added together with their own weights: leaves add their raw quaternions, deeper blend nodes add
// their normalized result. At the top of the tree the sums are divided by the weight sum (finishPose); below it the
// node's result is normalized and added to its parent with the node's weight (mergeBlend).
void AnimState::calc(int n, float weight, assets::AnimAccum& acc, bool top, const assets::XModelParts& parts) const {
    const auto& node = tree_.nodes()[size_t(n)];
    if (node.children.empty()) {
        if (const assets::XAnim* a = lib_.anim(node.name))
            assets::accumulateAnim(parts, *a, nodes_[size_t(n)].time * float(a->keyCount() - 1), weight, acc);
        return;
    }
    std::vector<int> active;
    for (int c : node.children)
        if (nodes_[size_t(c)].weight != 0) active.push_back(c);
    if (active.empty()) return;
    if (active.size() == 1) {
        calc(active[0], weight, acc, top, parts);
        return;
    }
    if (top) {
        for (int c : active) calc(c, nodes_[size_t(c)].weight, acc, false, parts);
        return;
    }
    assets::AnimAccum sub(acc.bones.size());
    for (int c : active) calc(c, nodes_[size_t(c)].weight, sub, false, parts);
    assets::mergeBlend(sub, weight, acc);
}

assets::XPose AnimState::pose(const assets::XModelParts& parts) const {
    assets::AnimAccum acc(parts.bones.size());
    calc(0, 1.0f, acc, true, parts);
    return assets::finishPose(parts, acc);
}

} // namespace iw2::game
