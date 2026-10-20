#include "game/animtree.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

void AnimState::setWeight(int n, float goal, float blendTime) {
    AnimNodeState& s = nodes_[size_t(n)];
    s.goalWeight = goal;
    if (blendTime <= 0) {
        s.weight = goal;
        s.weightRate = 0;
    } else {
        s.weightRate = std::fabs(goal - s.weight) / blendTime;
    }
}

void AnimState::set(int n, const SetArgs& a) {
    if (n < 0 || size_t(n) >= nodes_.size()) return;
    const auto& nodes = tree_.nodes();
    auto knobSiblings = [&](int node) {
        const int p = nodes[size_t(node)].parent;
        if (p < 0) return;
        for (int sib : nodes[size_t(p)].children)
            if (sib != node) setWeight(sib, 0.0f, a.blendTime);
    };
    setWeight(n, a.weight, a.blendTime);
    AnimNodeState& s = nodes_[size_t(n)];
    s.rate = a.rate;
    if (!a.notify.empty()) s.notify = a.notify;
    if (a.restart) {
        s.time = 0;
        s.ended = false;
    }
    if (a.knob || a.knobAllRoot >= 0) knobSiblings(n);
    if (a.knobAllRoot >= 0) {
        // Walk up to (but not past) the given root: each ancestor goes to weight 1 and its siblings to 0.
        for (int p = nodes[size_t(n)].parent; p >= 0 && p != a.knobAllRoot; p = nodes[size_t(p)].parent) {
            setWeight(p, 1.0f, a.blendTime);
            knobSiblings(p);
        }
    }
}

void AnimState::clear(int n, float blendTime) {
    if (n < 0 || size_t(n) >= nodes_.size()) return;
    if (n == 0) {
        for (size_t i = 1; i < nodes_.size(); ++i) setWeight(int(i), 0.0f, blendTime);
        return;
    }
    setWeight(n, 0.0f, blendTime);
}

void AnimState::update(float dt, const std::function<void(const std::string&, const std::string&)>& onNote) {
    const auto& nodes = tree_.nodes();
    for (size_t i = 0; i < nodes_.size(); ++i) {
        AnimNodeState& s = nodes_[i];
        if (s.weight != s.goalWeight) {
            const float step = s.weightRate * dt;
            if (s.weightRate <= 0 || std::fabs(s.goalWeight - s.weight) <= step) s.weight = s.goalWeight;
            else s.weight += s.goalWeight > s.weight ? step : -step;
        }
        if (s.weight <= 0 && s.goalWeight <= 0) continue;
        const assets::XAnim* a = nodes[i].children.empty() ? lib_.anim(nodes[i].name) : nullptr;
        if (!a) continue;
        const float len = std::max(a->length(), 1.0f / 30.0f);
        const float before = s.time;
        float after = before + dt * s.rate / len;
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

assets::XPose AnimState::pose(const assets::XModelParts& parts) const {
    const auto& nodes = tree_.nodes();
    const size_t nb = parts.bones.size();
    std::vector<std::array<float, 4>> qsum(nb, {0, 0, 0, 0});
    std::vector<std::array<float, 3>> tsum(nb, {0, 0, 0});
    std::vector<float> wq(nb, 0), wt(nb, 0);

    for (size_t i = 1; i < nodes.size(); ++i) {
        if (!nodes[i].children.empty()) continue;
        const float w = effectiveWeight(int(i));
        if (w <= 0.001f) continue;
        const assets::XAnim* a = lib_.anim(nodes[i].name);
        if (!a) continue;
        const float frame = nodes_[i].time * float(std::max(1, a->numFrames - 1));
        for (const auto& ab : a->bones) {
            for (size_t b = 0; b < nb; ++b) {
                const std::string& bn = parts.bones[b].name;
                if (bn.size() != ab.name.size() ||
                    !std::equal(bn.begin(), bn.end(), ab.name.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); }))
                    continue;
                if (!ab.rot.keys.empty()) {
                    assets::Quat q = assets::sampleRot(ab.rot, frame, a->keyCount());
                    // Keep quaternions in one hemisphere before summing.
                    const auto& acc = qsum[b];
                    if (acc[0] * q.x + acc[1] * q.y + acc[2] * q.z + acc[3] * q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
                    qsum[b] = {acc[0] + q.x * w, acc[1] + q.y * w, acc[2] + q.z * w, acc[3] + q.w * w};
                    wq[b] += w;
                }
                if (!ab.trans.keys.empty()) {
                    const auto t = assets::sampleTrans(ab.trans, frame, a->keyCount());
                    for (int k = 0; k < 3; ++k) tsum[b][size_t(k)] += t[size_t(k)] * w;
                    wt[b] += w;
                }
                break;
            }
        }
    }

    assets::XPose pose;
    pose.position.resize(nb);
    pose.rotation.resize(nb);
    for (size_t b = 0; b < nb; ++b) {
        const assets::XBone& bone = parts.bones[b];
        assets::Quat local = bone.localRotation;
        std::array<float, 3> offset = bone.localOffset;
        if (wq[b] > 0) {
            // Blend towards the base pose for the remaining weight.
            const float rest = std::max(0.0f, 1.0f - wq[b]);
            assets::Quat base = local;
            const auto& acc = qsum[b];
            if (acc[0] * base.x + acc[1] * base.y + acc[2] * base.z + acc[3] * base.w < 0) base = {-base.x, -base.y, -base.z, -base.w};
            assets::Quat q{acc[0] + base.x * rest, acc[1] + base.y * rest, acc[2] + base.z * rest, acc[3] + base.w * rest};
            const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (n > 0) local = {q.x / n, q.y / n, q.z / n, q.w / n};
        }
        if (wt[b] > 0) {
            const float rest = std::max(0.0f, 1.0f - wt[b]);
            for (int k = 0; k < 3; ++k) offset[size_t(k)] = tsum[b][size_t(k)] + offset[size_t(k)] * rest;
        }
        if (bone.parent < 0) {
            pose.rotation[b] = bone.rotation;
            pose.position[b] = bone.position;
        } else {
            const size_t p = size_t(bone.parent);
            pose.rotation[b] = assets::multiply(pose.rotation[p], local);
            const auto o = assets::rotate(pose.rotation[p], offset);
            pose.position[b] = {pose.position[p][0] + o[0], pose.position[p][1] + o[1], pose.position[p][2] + o[2]};
        }
    }
    return pose;
}

} // namespace iw2::game
