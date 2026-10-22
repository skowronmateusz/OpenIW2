#include "assets/xanim.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

class Reader {
public:
    explicit Reader(std::span<const uint8_t> d) : d_(d) {}
    template <typename T>
    T get() {
        if (pos_ + sizeof(T) > d_.size()) throw std::runtime_error("xanim: truncated");
        T v;
        std::memcpy(&v, d_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::string str() {
        const size_t start = pos_;
        while (true) {
            if (pos_ >= d_.size()) throw std::runtime_error("xanim: truncated string");
            if (d_[pos_++] == 0) break;
        }
        return std::string(reinterpret_cast<const char*>(d_.data() + start), pos_ - start - 1);
    }
    const uint8_t* bytes(size_t n) {
        if (pos_ + n > d_.size()) throw std::runtime_error("xanim: truncated");
        const uint8_t* p = d_.data() + pos_;
        pos_ += n;
        return p;
    }
    size_t remaining() const { return d_.size() - pos_; }

private:
    std::span<const uint8_t> d_;
    size_t pos_ = 0;
};

constexpr float kQuatScale = 1.0f / 32767.0f;

// w is rebuilt from the stored components (same rule as xmodelparts).
int16_t rebuildW(int sumSquares) {
    const int r = 0x3fff0001 - sumSquares;
    return r > 0 ? int16_t(std::floor(std::sqrt(float(r)) + 0.5f)) : int16_t(0);
}

Quat readFullQuat(Reader& r, bool flip) {
    const int16_t x = r.get<int16_t>(), y = r.get<int16_t>(), z = r.get<int16_t>();
    const int16_t w = rebuildW(int(x) * x + int(y) * y + int(z) * z);
    const float s = flip ? -kQuatScale : kQuatScale;
    return {x * s, y * s, z * s, w * s};
}

Quat readYawQuat(Reader& r, bool flip) {
    const int16_t z = r.get<int16_t>();
    const int16_t w = rebuildW(int(z) * z);
    const float s = flip ? -kQuatScale : kQuatScale;
    return {0, 0, z * s, w * s};
}

std::vector<uint16_t> readFrameIndices(Reader& r, int count, int keyCount) {
    std::vector<uint16_t> f;
    if (count >= keyCount) return f; // every frame has a key
    f.resize(size_t(count));
    for (auto& v : f) v = keyCount > 256 ? r.get<uint16_t>() : r.get<uint8_t>();
    return f;
}

void readRotTrack(Reader& r, XAnimRotTrack& t, int keyCount, bool yawOnly, bool flip) {
    const uint16_t count = r.get<uint16_t>();
    if (count == 0) return;
    if (count == 1) {
        t.keys.push_back(yawOnly ? readYawQuat(r, flip) : readFullQuat(r, flip));
        return;
    }
    t.frames = readFrameIndices(r, count, keyCount);
    for (int i = 0; i < count; ++i) t.keys.push_back(yawOnly ? readYawQuat(r, flip) : readFullQuat(r, flip));
    // The loader keeps each key in the same hemisphere as the previous one.
    for (size_t i = 1; i < t.keys.size(); ++i) {
        const Quat& a = t.keys[i - 1];
        Quat& b = t.keys[i];
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    }
}

void readTransTrack(Reader& r, XAnimTransTrack& t, int keyCount) {
    const uint16_t count = r.get<uint16_t>();
    if (count == 0) return;
    if (count == 1) {
        t.keys.push_back({r.get<float>(), r.get<float>(), r.get<float>()});
        return;
    }
    t.frames = readFrameIndices(r, count, keyCount);
    for (int i = 0; i < count; ++i) t.keys.push_back({r.get<float>(), r.get<float>(), r.get<float>()});
}

// Finds the key interval for a frame; returns (i0, i1, t).
template <typename Track>
void locate(const Track& t, float frame, int keyCount, size_t& i0, size_t& i1, float& lerp) {
    const size_t n = t.keys.size();
    if (n <= 1) { i0 = i1 = 0; lerp = 0; return; }
    if (t.frames.empty()) {
        const float f = std::clamp(frame, 0.0f, float(std::min<int>(int(n), keyCount) - 1));
        i0 = size_t(f);
        i1 = std::min(i0 + 1, n - 1);
        lerp = f - float(i0);
        return;
    }
    if (frame <= t.frames.front()) { i0 = i1 = 0; lerp = 0; return; }
    if (frame >= t.frames.back()) { i0 = i1 = n - 1; lerp = 0; return; }
    const auto it = std::upper_bound(t.frames.begin(), t.frames.end(), uint16_t(frame));
    i1 = size_t(it - t.frames.begin());
    i0 = i1 - 1;
    const float a = t.frames[i0], b = t.frames[i1];
    lerp = b > a ? (frame - a) / (b - a) : 0.0f;
}

} // namespace

XAnim parseXAnim(std::span<const uint8_t> data) {
    Reader r(data);
    if (const uint16_t v = r.get<uint16_t>(); v != 14) throw std::runtime_error("xanim: version " + std::to_string(v));
    XAnim a;
    a.numFrames = r.get<uint16_t>();
    const uint16_t numBones = r.get<uint16_t>();
    const uint8_t flags = r.get<uint8_t>();
    a.looping = flags & 1;
    a.delta = flags & 2;
    a.frameRate = r.get<int16_t>();
    const int keyCount = a.keyCount();

    if (a.delta) {
        readRotTrack(r, a.deltaRot, keyCount, true, false);
        readTransTrack(r, a.deltaTrans, keyCount);
    }
    std::vector<uint8_t> flipBits, yawBits;
    if (numBones > 0) {
        const size_t n = size_t(((numBones - 1) >> 3) + 1);
        const uint8_t* f = r.bytes(n);
        flipBits.assign(f, f + n);
        const uint8_t* y = r.bytes(n);
        yawBits.assign(y, y + n);
    }
    a.bones.resize(numBones);
    for (auto& b : a.bones) b.name = r.str();
    for (size_t i = 0; i < a.bones.size(); ++i) {
        const bool flip = flipBits[i >> 3] & (1 << (i & 7));
        const bool yawOnly = yawBits[i >> 3] & (1 << (i & 7));
        readRotTrack(r, a.bones[i].rot, keyCount, yawOnly, flip);
        readTransTrack(r, a.bones[i].trans, keyCount);
    }
    const uint8_t notes = r.get<uint8_t>();
    for (int i = 0; i < notes; ++i) {
        XAnimNote n;
        n.name = r.str();
        const uint16_t frame = r.get<uint16_t>();
        n.time = a.numFrames > 0 ? float(frame) / float(a.numFrames) : 0.0f;
        a.notes.push_back(std::move(n));
    }
    a.notes.push_back({"end", 1.0f});
    if (r.remaining() != 0) throw std::runtime_error("xanim: " + std::to_string(r.remaining()) + " trailing bytes");
    return a;
}

namespace {

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

} // namespace

void accumulateAnim(const XModelParts& parts, const XAnim& anim, float frame, float weight, AnimAccum& acc) {
    const int keys = anim.keyCount();
    for (const auto& ab : anim.bones) {
        for (size_t i = 0; i < parts.bones.size(); ++i) {
            if (!sameName(ab.name, parts.bones[i].name)) continue;
            BoneAccum& b = acc.bones[i];
            if (ab.rot.keys.empty()) {
                b.q[3] += weight;
            } else {
                const Quat q = sampleRotRaw(ab.rot, frame, keys);
                b.q[0] += q.x * weight; b.q[1] += q.y * weight; b.q[2] += q.z * weight; b.q[3] += q.w * weight;
            }
            if (!ab.trans.keys.empty()) {
                const auto v = sampleTrans(ab.trans, frame, keys);
                for (int k = 0; k < 3; ++k) b.t[k] += v[size_t(k)] * weight;
            }
            b.w += weight;
            acc.animated[i] = 1;
            break;
        }
    }
}

void mergeBlend(const AnimAccum& node, float weight, AnimAccum& parent) {
    for (size_t i = 0; i < node.bones.size(); ++i) {
        const BoneAccum& c = node.bones[i];
        BoneAccum& p = parent.bones[i];
        const float len2 = c.q[0] * c.q[0] + c.q[1] * c.q[1] + c.q[2] * c.q[2] + c.q[3] * c.q[3];
        if (len2 != 0) {
            const float s = weight / std::sqrt(len2);
            for (int k = 0; k < 4; ++k) p.q[k] += c.q[k] * s;
        }
        if (c.w != 0) {
            const float s = weight / c.w;
            for (int k = 0; k < 3; ++k) p.t[k] += c.t[k] * s;
            p.w += weight;
        }
        parent.animated[i] |= node.animated[i];
    }
}

XPose finishPose(const XModelParts& parts, const AnimAccum& acc) {
    XPose pose;
    pose.position.resize(parts.bones.size());
    pose.rotation.resize(parts.bones.size());
    for (size_t i = 0; i < parts.bones.size(); ++i) {
        const XBone& b = parts.bones[i];
        const bool root = b.parent < 0;
        Quat local = root ? Quat{} : b.localRotation;
        std::array<float, 3> offset = root ? std::array<float, 3>{0, 0, 0} : b.localOffset;
        if (acc.animated[i]) {
            const BoneAccum& a = acc.bones[i];
            const float n = std::sqrt(a.q[0] * a.q[0] + a.q[1] * a.q[1] + a.q[2] * a.q[2] + a.q[3] * a.q[3]);
            local = n != 0 ? Quat{a.q[0] / n, a.q[1] / n, a.q[2] / n, a.q[3] / n} : Quat{};
            if (a.w != 0)
                for (int k = 0; k < 3; ++k) offset[size_t(k)] += a.t[k] / a.w;
        }
        if (root) {
            pose.rotation[i] = local;
            pose.position[i] = offset;
        } else {
            const size_t p = size_t(b.parent);
            pose.rotation[i] = multiply(pose.rotation[p], local);
            const auto o = rotate(pose.rotation[p], offset);
            pose.position[i] = {pose.position[p][0] + o[0], pose.position[p][1] + o[1], pose.position[p][2] + o[2]};
        }
    }
    return pose;
}

XPose animatePose(const XModelParts& parts, const XAnim& anim, float frame) {
    AnimAccum acc(parts.bones.size());
    accumulateAnim(parts, anim, frame, 1.0f, acc);
    return finishPose(parts, acc);
}

XPose blendPose(const XModelParts& parts, const std::vector<PoseLayer>& layers) {
    AnimAccum acc(parts.bones.size());
    size_t active = 0;
    for (const auto& l : layers) active += l.anim && l.weight != 0;
    for (const auto& l : layers)
        if (l.anim && l.weight != 0) accumulateAnim(parts, *l.anim, l.frame, active == 1 ? 1.0f : l.weight, acc);
    return finishPose(parts, acc);
}

Quat sampleRotRaw(const XAnimRotTrack& t, float frame, int keyCount) {
    if (t.keys.empty()) return {0, 0, 0, 1};
    size_t i0, i1;
    float l;
    locate(t, frame, keyCount, i0, i1, l);
    const Quat& a = t.keys[i0];
    const Quat& b = t.keys[i1];
    return {a.x + (b.x - a.x) * l, a.y + (b.y - a.y) * l, a.z + (b.z - a.z) * l, a.w + (b.w - a.w) * l};
}

Quat sampleRot(const XAnimRotTrack& t, float frame, int keyCount) {
    if (t.keys.empty()) return {0, 0, 0, 1};
    size_t i0, i1;
    float l;
    locate(t, frame, keyCount, i0, i1, l);
    Quat a = t.keys[i0], b = t.keys[i1];
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    Quat q{a.x + (b.x - a.x) * l, a.y + (b.y - a.y) * l, a.z + (b.z - a.z) * l, a.w + (b.w - a.w) * l};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    return q;
}

std::array<float, 3> sampleTrans(const XAnimTransTrack& t, float frame, int keyCount) {
    if (t.keys.empty()) return {0, 0, 0};
    size_t i0, i1;
    float l;
    locate(t, frame, keyCount, i0, i1, l);
    const auto& a = t.keys[i0];
    const auto& b = t.keys[i1];
    return {a[0] + (b[0] - a[0]) * l, a[1] + (b[1] - a[1]) * l, a[2] + (b[2] - a[2]) * l};
}

} // namespace iw2::assets
