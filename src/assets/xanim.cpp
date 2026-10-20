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

XPose animatePose(const XModelParts& parts, const XAnim& anim, float frame) {
    const int keys = anim.keyCount();
    XPose pose;
    pose.position.resize(parts.bones.size());
    pose.rotation.resize(parts.bones.size());
    for (size_t i = 0; i < parts.bones.size(); ++i) {
        const XBone& b = parts.bones[i];
        Quat local = b.localRotation;
        std::array<float, 3> offset = b.localOffset;
        for (const auto& ab : anim.bones) {
            if (ab.name.size() != b.name.size() || !std::equal(ab.name.begin(), ab.name.end(), b.name.begin(),
                                                                [](char x, char y) { return std::tolower(x) == std::tolower(y); }))
                continue;
            if (!ab.rot.keys.empty()) local = sampleRot(ab.rot, frame, keys);
            if (!ab.trans.keys.empty()) offset = sampleTrans(ab.trans, frame, keys);
            break;
        }
        if (b.parent < 0) {
            pose.rotation[i] = local;
            pose.position[i] = offset;
            if (i < size_t(parts.rootCount)) {
                pose.rotation[i] = b.rotation;
                pose.position[i] = b.position;
            }
        } else {
            const size_t p = size_t(b.parent);
            pose.rotation[i] = multiply(pose.rotation[p], local);
            const auto o = rotate(pose.rotation[p], offset);
            pose.position[i] = {pose.position[p][0] + o[0], pose.position[p][1] + o[1], pose.position[p][2] + o[2]};
        }
    }
    return pose;
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
