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
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            // The tokenizer (0x43e4f0) also skips /* … */ (generic_human.atr comments out a block of 30cal nodes).
            const size_t e = text.find("*/", i + 2);
            i = e == std::string_view::npos ? text.size() : e + 2;
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
    markSync(0, 0);
}

// 0x479260: a blend node with loopsync / nonloopsync gets flag 4 and its type (1 / 2); 0x479120 gives every blend
// node below it the same type (a node may not carry both, nor a second sync modifier below a synced one).
void AnimTree::markSync(int n, int type) {
    AnimNode& node = nodes_[size_t(n)];
    if (node.children.empty()) return;
    if (type) {
        node.syncFlags |= type;
    } else if (!node.sync.empty()) {
        type = node.sync == "loopsync" ? 1 : 2;
        node.syncFlags |= 4 | type;
    }
    for (int c : std::vector<int>(node.children)) markSync(c, type);
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
    if (auto m = memo_.find(name); m != memo_.end()) return m->second;
    const assets::XAnim* r = animUncached(name);
    memo_.emplace(name, r);
    return r;
}

const assets::XAnim* AnimLibrary::animUncached(const std::string& name) {
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
// The restart variants restart the node's time after it is set (0x4793c0); knoball also restarts each ancestor it sets.
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
            if (a.restart) restart(p);
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

namespace {

// Normalized time per second of an animation (the xanim's frequency, XAnim +8); 0 without frames.
float animFreq(const assets::XAnim* a) {
    if (!a) return 0.0f;
    const int frames = a->keyCount() - 1;
    return frames > 0 ? float(a->frameRate) / float(frames) : 0.0f;
}

} // namespace

// 0x4793c0: the node or its nearest ancestor that tops a synced subtree (flag 4) restarts that whole subtree (time
// and cycle 0, 0x4768e0 for the descendants) unless it already stands at 0. Outside a synced subtree only a leaf
// restarts; a plain blend node keeps its time.
void AnimState::restart(int n) {
    const auto& nodes = tree_.nodes();
    auto zero = [](AnimNodeState& s) {
        s.time = s.oldTime = 0;
        s.cycle = s.oldCycle = 0;
        s.ended = false;
    };
    for (int p = n; p > 0; p = nodes[size_t(p)].parent) {
        if (nodes[size_t(p)].children.empty() || !(nodes[size_t(p)].syncFlags & 4)) continue;
        AnimNodeState& s = nodes_[size_t(p)];
        if (s.time == 0 && s.cycle == 0) return;
        zero(s);
        for (int c : nodes[size_t(p)].children) resetSubtree(c);
        return;
    }
    if (nodes[size_t(n)].children.empty()) zero(nodes_[size_t(n)]);
}

// 0x4768e0: time and cycle 0 for the node and every descendant.
void AnimState::resetSubtree(int n) {
    AnimNodeState& s = nodes_[size_t(n)];
    s.time = s.oldTime = 0;
    s.cycle = s.oldCycle = 0;
    s.ended = false;
    for (int c : tree_.nodes()[size_t(n)].children) resetSubtree(c);
}

// 0x476960: weights move linearly to the goal over the time left (below 1e-6 they restart at goal × 0.001); under an
// inactive parent they jump to the goal. A node is alive while it is active (weight under an active parent), has a
// goal weight or has a live child; it is playing when active with a leaf of nonzero frequency below. A node that is
// alive but not playing takes the time handed down (the synced top's time inside a synced subtree, else 0); a synced
// top that is not playing restarts its subtree (0x4768a0). Then the old time is the time. A node that is not alive
// is released: its state starts again from scratch when it is next set.
void AnimState::updateWeight(int n, float inheritedTime, int inheritedCycle, float dt, bool parentActive, bool& alive, bool& playing) {
    AnimNodeState& s = nodes_[size_t(n)];
    const auto& node = tree_.nodes()[size_t(n)];
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
    bool live = n == 0 || active || s.goalWeight != 0;
    bool childPlaying = false;
    const bool syncTop = !node.children.empty() && (node.syncFlags & 4);
    if (node.children.empty()) {
        childPlaying = animFreq(lib_.anim(node.name)) != 0;
    } else {
        const float t = syncTop ? s.time : inheritedTime;
        const int c = syncTop ? s.cycle : inheritedCycle;
        for (int ch : node.children) updateWeight(ch, t, c, dt, active, live, childPlaying);
    }
    if (!live) {
        s = AnimNodeState{};
        return;
    }
    if (!active || !childPlaying) {
        if (!syncTop) {
            if (s.time != inheritedTime || s.cycle != inheritedCycle) {
                s.time = inheritedTime;
                s.cycle = inheritedCycle;
                s.ended = false;
            }
        } else if (s.time != 0 || s.cycle != 0) {
            s.time = s.oldTime = 0;
            s.cycle = s.oldCycle = 0;
            for (int ch : node.children) resetSubtree(ch);
        }
    } else {
        playing = true;
    }
    s.oldTime = s.time;
    s.oldCycle = s.cycle;
    alive = true;
}

// 0x475bb0: a leaf's frequency, or the average over the weighted children (with nonzero frequency) of frequency ×
// rate, weighted by the child weights.
float AnimState::syncFreq(int n) const {
    const auto& node = tree_.nodes()[size_t(n)];
    if (node.children.empty()) return animFreq(lib_.anim(node.name));
    float sum = 0, acc = 0;
    for (int c : node.children) {
        const AnimNodeState& s = nodes_[size_t(c)];
        if (s.weight == 0) continue;
        const float f = syncFreq(c);
        if (f == 0) continue;
        sum += s.weight;
        acc += f * s.rate * s.weight;
    }
    return sum != 0 ? acc / sum : 0.0f;
}

// Moves a leaf from its time to `to` (cycle `toCycle`) and calls `onNote` for the notetracks crossed (0x476f10).
void AnimState::advanceLeaf(int n, float to, int toCycle, bool notes, const NoteFn& onNote) {
    AnimNodeState& s = nodes_[size_t(n)];
    const assets::XAnim* a = lib_.anim(tree_.nodes()[size_t(n)].name);
    const float before = s.time;
    const bool wrapped = toCycle > s.cycle;
    if (notes && a && !s.notify.empty()) {
        if (a->looping) {
            if (wrapped) {
                for (const auto& note : a->notes)
                    if (note.time > before) onNote(s.notify, note.name);
                for (const auto& note : a->notes)
                    if (note.time <= to && note.name != "end") onNote(s.notify, note.name);
            } else {
                for (const auto& note : a->notes)
                    if (note.time > before && note.time <= to && note.name != "end") onNote(s.notify, note.name);
            }
        } else {
            for (const auto& note : a->notes)
                if (note.name != "end" && note.time > before && note.time <= to) onNote(s.notify, note.name);
        }
    }
    // 0x476f10: a node that already stands at time 1.0 sends the last notetrack ("end") again on every call, so
    // a non-looping flagged node that sits at its end keeps ending (a pain anim reused without a restart ends at once).
    if (a && !a->looping && to >= 1.0f) {
        if (notes && !s.notify.empty()) onNote(s.notify, "end");
    }
    s.time = to;
    s.cycle = toCycle;
}

// 0x476490: only weighted nodes advance; notetracks are off below a node with goal weight 0. A leaf moves from its old
// time by frequency × rate × dt (looping anims wrap and count a cycle, others stop at 1). A plain blend node passes
// dt × rate to its children. A synced blend node advances by its weighted frequency (0x475bb0) × rate × dt (wrapping
// unless nonloopsync) and sets every weighted descendant to its time (0x476370). A time is only taken when it does
// not go back.
void AnimState::updateTime(int n, float dt, bool notes, const NoteFn& onNote) {
    AnimNodeState& s = nodes_[size_t(n)];
    if (s.weight == 0) return;
    if (s.goalWeight == 0) notes = false;
    const auto& node = tree_.nodes()[size_t(n)];
    float step;
    bool wrap;
    if (node.children.empty()) {
        const assets::XAnim* a = lib_.anim(node.name);
        step = animFreq(a) * s.rate * dt;
        wrap = a && a->looping;
    } else if (!(node.syncFlags & 3)) {
        if (s.rate * dt == 0) return;
        for (int c : node.children) updateTime(c, dt * s.rate, notes, onNote);
        return;
    } else {
        step = syncFreq(n) * s.rate * dt;
        wrap = !(node.syncFlags & 2);
    }
    if (step == 0) return;
    float to = s.oldTime + step;
    int cycle = s.oldCycle;
    if (to >= 1.0f) {
        if (wrap) {
            while (to >= 1.0f) {
                to -= 1.0f;
                ++cycle;
            }
        } else {
            to = 1.0f;
        }
    }
    if (s.time - to > float(cycle - s.cycle)) return;
    if (node.children.empty()) {
        advanceLeaf(n, to, cycle, notes, onNote);
        return;
    }
    s.time = to;
    s.cycle = cycle;
    for (int c : node.children) syncTo(c, s, notes, onNote);
}

// 0x476370: a weighted node under a synced top restarts from the top's old time if it is out of step, then moves to
// the top's time and cycle (notetracks off below a goal weight of 0).
void AnimState::syncTo(int n, const AnimNodeState& top, bool notes, const NoteFn& onNote) {
    AnimNodeState& s = nodes_[size_t(n)];
    if (s.weight == 0) return;
    if (s.goalWeight == 0) notes = false;
    if (s.oldTime != top.oldTime || s.oldCycle != top.oldCycle) {
        s.time = s.oldTime = top.oldTime;
        s.cycle = s.oldCycle = top.oldCycle;
        s.ended = false;
    }
    const auto& node = tree_.nodes()[size_t(n)];
    if (node.children.empty()) {
        advanceLeaf(n, top.time, top.cycle, notes, onNote);
        return;
    }
    s.time = top.time;
    s.cycle = top.cycle;
    for (int c : node.children) syncTo(c, top, notes, onNote);
}

void AnimState::update(float dt, const NoteFn& onNote) {
    bool alive = false, playing = false;
    updateWeight(0, 0.0f, 0, dt, true, alive, playing);
    updateTime(0, dt, true, onNote);
}

std::string AnimState::debugString() const {
    std::string out;
    const auto& nodes = tree_.nodes();
    for (size_t i = 1; i < nodes.size(); ++i) {
        const AnimNodeState& s = nodes_[i];
        if (s.weight <= 0 && s.goalWeight <= 0) continue;
        char buf[256];
        std::snprintf(buf, sizeof buf, "  %s(p%d,f%d) w=%.2f goal=%.2f t=%.2f rate=%.2f notify=%s leaf=%d anim=%d\n", nodes[i].name.c_str(), nodes[i].parent, nodes[i].syncFlags, s.weight, s.goalWeight,
                      s.time, s.rate, s.notify.c_str(), int(nodes[i].children.empty()), lib_.anim(nodes[i].name) != nullptr);
        out += buf;
    }
    return out;
}

void AnimState::clearChildren(int n, float blendTime) {
    if (n < 0 || size_t(n) >= nodes_.size()) return;
    for (int c : tree_.nodes()[size_t(n)].children) clear(c, blendTime);
}

bool AnimState::looping(int n) const {
    if (n < 0 || size_t(n) >= nodes_.size() || !tree_.nodes()[size_t(n)].children.empty()) return false;
    const assets::XAnim* a = lib_.anim(tree_.nodes()[size_t(n)].name);
    return a && a->looping;
}

// 0x477b80 (called by 0x478550 with weight 1, first, not nested). A leaf with a delta track adds weight × the
// translation from its old to its new time (0x475960: a looping anim whose time went back adds one whole cycle) and
// adds its weight. A node with one weighted child hands it its own weight. Several weighted children are summed with
// their own weights; nested, the sum is normalized and added with the node's weight, at the top it is divided by the
// weight sum. The weights are the current ones, or the goal weights with `goalWeights`. With `absolute` (tree byte +6,
// 0x4785d0) a leaf adds its delta tracks at the current time instead (0x475aa0).
void AnimState::deltaNode(int n, float weight, bool first, bool nested, bool goalWeights, bool absolute, AnimDelta& out) const {
    const auto& node = tree_.nodes()[size_t(n)];
    if (first) out = AnimDelta{};
    if (node.children.empty()) {
        const assets::XAnim* a = lib_.anim(node.name);
        if (!a || !a->delta) return;
        const AnimNodeState& s = nodes_[size_t(n)];
        const int keys = a->keyCount();
        const float span = float(keys - 1);
        if (absolute) {
            // 0x475aa0 (tree byte +6 set): the delta tracks at the current time, the raw yaw quaternion (z, w) (no track:
            // (0, 1)) and the translation, each times the weight, and the weight.
            const float frame = s.time * span;
            float qz = 0.0f, qw = 1.0f;
            if (!a->deltaRot.keys.empty()) {
                const assets::Quat q = assets::sampleRotRaw(a->deltaRot, frame, keys);
                qz = q.z;
                qw = q.w;
            }
            std::array<float, 3> t{0, 0, 0};
            if (!a->deltaTrans.keys.empty()) t = assets::sampleTrans(a->deltaTrans, frame, keys);
            out.weight += weight;
            for (int k = 0; k < 3; ++k) out.trans[size_t(k)] += t[size_t(k)] * weight;
            out.rot[0] += qz * weight;
            out.rot[1] += qw * weight;
            return;
        }
        const auto from = assets::sampleTrans(a->deltaTrans, s.oldTime * span, keys);
        auto to = assets::sampleTrans(a->deltaTrans, s.time * span, keys);
        if (a->looping && s.time < s.oldTime && !a->deltaTrans.keys.empty()) {
            const auto& f = a->deltaTrans.keys.front();
            const auto& l = a->deltaTrans.keys.back();
            for (int k = 0; k < 3; ++k) to[size_t(k)] += l[size_t(k)] - f[size_t(k)];
        }
        // 0x475960: the rotation track is sampled at both times (no track: identity). The relative quaternion
        // conj(old) * new is added with the weight (0x475a0b..0x475a35) and the translation difference is turned by
        // minus the yaw of the new rotation (0x4758f0: x' = (1 - 2 z^2 / s) x + (2 z w / s) y, y' = (1 - 2 z^2 / s) y - (2 z w / s) x).
        float z0 = 0.0f, w0 = 1.0f, z1 = 0.0f, w1 = 1.0f;
        if (!a->deltaRot.keys.empty()) {
            const assets::Quat q0 = assets::sampleRotRaw(a->deltaRot, s.oldTime * span, keys);
            const assets::Quat q1 = assets::sampleRotRaw(a->deltaRot, s.time * span, keys);
            z0 = q0.z; w0 = q0.w; z1 = q1.z; w1 = q1.w;
        }
        float dx = to[0] - from[0], dy = to[1] - from[1];
        const float sq = z1 * z1 + w1 * w1;
        if (sq > 0.0f) {
            const float k = 2.0f / sq, q = k * z1 * z1, r = k * w1 * z1;
            const float nx = (1.0f - q) * dx + r * dy;
            dy = (1.0f - q) * dy - r * dx;
            dx = nx;
        }
        out.weight += weight;
        out.trans[0] += dx * weight;
        out.trans[1] += dy * weight;
        out.trans[2] += (to[2] - from[2]) * weight;
        out.rot[0] += (w0 * z1 - z0 * w1) * weight;
        out.rot[1] += (w0 * w1 + z0 * z1) * weight;
        return;
    }
    auto w = [&](int c) { return goalWeights ? nodes_[size_t(c)].goalWeight : nodes_[size_t(c)].weight; };
    std::vector<int> active;
    for (int c : node.children)
        if (w(c) != 0) active.push_back(c);
    if (active.empty()) return;
    if (active.size() == 1) {
        deltaNode(active[0], weight, first, nested, goalWeights, absolute, out);
        return;
    }
    AnimDelta local;
    AnimDelta& sum = first ? out : local;
    for (size_t i = 0; i < active.size(); ++i) deltaNode(active[i], w(active[i]), i == 0, true, goalWeights, absolute, sum);
    if (!nested) {
        if (out.weight == 0) return;
        for (float& v : out.trans) v /= out.weight;
        for (float& v : out.rot) v /= out.weight;
        return;
    }
    if (sum.weight == 0) return;
    const float scale = weight / sum.weight;
    if (first) {
        for (float& v : out.trans) v *= scale;
        for (float& v : out.rot) v *= scale;
        out.weight = weight;
        return;
    }
    for (int k = 0; k < 3; ++k) out.trans[size_t(k)] += sum.trans[size_t(k)] * scale;
    for (int k = 0; k < 2; ++k) out.rot[size_t(k)] += sum.rot[size_t(k)] * scale;
    out.weight += weight;
}

void AnimState::nodeDelta(int n, float& qz, float& qw, std::array<float, 3>& trans) const {
    qz = 0.0f;
    qw = 1.0f;
    trans = {0, 0, 0};
    if (n <= 0 || size_t(n) >= nodes_.size()) return;
    AnimDelta d;
    deltaNode(n, 1.0f, true, false, true, true, d);
    trans = d.trans;
    if (d.rot[0] != 0.0f || d.rot[1] != 0.0f) { // 0x4785d0: a zero rotation becomes (0, 1)
        qz = d.rot[0];
        qw = d.rot[1];
    }
}

// The exe's "no state" (entry index 0) is taken as a node at weight and goal weight 0 (inferred: entries are
// handed out when a node is set).
bool AnimState::playedThrough(int n) const {
    if (n <= 0 || size_t(n) >= nodes_.size()) return true;
    const AnimNodeState& s = nodes_[size_t(n)];
    if (s.weight == 0 && s.goalWeight == 0) return true;
    return !(s.oldTime <= s.time && s.time != 1.0f && s.cycle <= s.oldCycle);
}

AnimDelta AnimState::delta(bool goalWeights) const {
    AnimDelta d;
    deltaNode(0, 1.0f, true, false, goalWeights, false, d);
    return d;
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
