#include "assets/xmodel.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

constexpr int16_t kVersion = 20;

class Reader {
public:
    Reader(std::span<const uint8_t> d, const char* what) : d_(d), what_(what) {}

    template <typename T>
    T get() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, d_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::array<float, 3> vec3() { return {get<float>(), get<float>(), get<float>()}; }
    std::string str() {
        const size_t start = pos_;
        while (true) {
            need(1);
            if (d_[pos_++] == 0) break;
        }
        return std::string(reinterpret_cast<const char*>(d_.data() + start), pos_ - start - 1);
    }
    size_t pos() const { return pos_; }
    size_t remaining() const { return d_.size() - pos_; }

    [[noreturn]] void fail(const std::string& msg) const { throw std::runtime_error(std::string(what_) + ": " + msg); }

private:
    void need(size_t n) const {
        if (pos_ + n > d_.size()) fail("truncated");
    }
    std::span<const uint8_t> d_;
    const char* what_;
    size_t pos_ = 0;
};

} // namespace

std::array<float, 3> rotate(const Quat& q, const std::array<float, 3>& v) {
    // v' = q * v * q^-1
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    return {v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)};
}

Quat multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XModel parseXModel(std::span<const uint8_t> data) {
    Reader r(data, "xmodel");
    if (const int16_t v = r.get<int16_t>(); v != kVersion) r.fail("version " + std::to_string(v));
    XModel m;
    m.flags = r.get<uint8_t>();
    m.mins = r.vec3();
    m.maxs = r.vec3();
    std::vector<XModelLod> all(4);
    for (auto& lod : all) {
        lod.distance = r.get<float>();
        lod.name = r.str();
    }
    r.get<uint32_t>(); // collision LOD index (inferred)
    // Collision surfaces: count, then per surface its triangles (48 bytes each), bounds, bone, contents, flags.
    const uint32_t collCount = r.get<uint32_t>();
    for (uint32_t c = 0; c < collCount; ++c) {
        const uint32_t triCount = r.get<uint32_t>();
        for (uint32_t t = 0; t < triCount * 12; ++t) r.get<float>();
        for (int k = 0; k < 6; ++k) r.get<float>();
        r.get<int32_t>();
        r.get<uint32_t>();
        r.get<int32_t>();
    }
    for (auto& lod : all) {
        if (lod.name.empty()) continue;
        const int16_t count = r.get<int16_t>();
        for (int i = 0; i < count; ++i) lod.materials.push_back(r.str());
        m.lods.push_back(std::move(lod));
    }
    return m;
}

XModelParts parseXModelParts(std::span<const uint8_t> data) {
    Reader r(data, "xmodelparts");
    if (const int16_t v = r.get<int16_t>(); v != kVersion) r.fail("version " + std::to_string(v));
    XModelParts p;
    const int16_t childCount = r.get<int16_t>();
    p.rootCount = r.get<int16_t>();
    const int total = p.rootCount + childCount;
    if (childCount < 0 || p.rootCount < 0 || total >= 128) r.fail("bad bone counts");
    p.bones.resize(size_t(total));
    for (int i = p.rootCount; i < total; ++i) {
        XBone& b = p.bones[size_t(i)];
        b.parent = r.get<uint8_t>(); // absolute parent index (the loader stores i - parent internally)
        if (b.parent < 0 || b.parent >= i) r.fail("bad parent");
        b.localOffset = r.vec3();
        const int16_t qx = r.get<int16_t>(), qy = r.get<int16_t>(), qz = r.get<int16_t>();
        const int ww = 0x3fff0001 - int(qx) * qx - int(qy) * qy - int(qz) * qz;
        const float qw = ww > 0 ? std::floor(std::sqrt(float(ww)) + 0.5f) : 0.0f;
        constexpr float s = 1.0f / 32767.0f;
        b.localRotation = {qx * s, qy * s, qz * s, qw * s};
    }
    for (auto& b : p.bones) b.name = r.str();
    for (int i = 0; i < total; ++i) r.get<uint8_t>(); // per-bone part classification (inferred)

    for (int i = 0; i < total; ++i) {
        XBone& b = p.bones[size_t(i)];
        if (b.parent < 0) continue;
        const XBone& parent = p.bones[size_t(b.parent)];
        b.rotation = multiply(parent.rotation, b.localRotation);
        const auto o = rotate(parent.rotation, b.localOffset);
        b.position = {parent.position[0] + o[0], parent.position[1] + o[1], parent.position[2] + o[2]};
    }
    return p;
}

XPose bindPose(const XModelParts& parts) {
    XPose p;
    for (const auto& b : parts.bones) {
        p.position.push_back(b.position);
        p.rotation.push_back(b.rotation);
    }
    return p;
}

void skinVertex(const XVertex& v, const XPose& pose, std::array<float, 3>& position, std::array<float, 3>& normal) {
    position = {0, 0, 0};
    for (const auto& inf : v.influences) {
        const size_t b = size_t(inf.bone);
        const auto o = rotate(pose.rotation[b], inf.offset);
        for (int a = 0; a < 3; ++a) position[size_t(a)] += (pose.position[b][size_t(a)] + o[size_t(a)]) * inf.weight;
    }
    normal = v.influences.empty() ? v.normal : rotate(pose.rotation[size_t(v.influences.front().bone)], v.boneNormal);
}

std::vector<XSurface> parseXModelSurfs(std::span<const uint8_t> data, const XModelParts& parts) {
    Reader r(data, "xmodelsurfs");
    if (const int16_t v = r.get<int16_t>(); v != kVersion) r.fail("version " + std::to_string(v));
    const int16_t surfCount = r.get<int16_t>();
    std::vector<XSurface> out;

    auto boneAt = [&](int16_t index) -> const XBone& {
        if (index < 0 || size_t(index) >= parts.bones.size()) r.fail("bone index out of range");
        return parts.bones[size_t(index)];
    };
    auto toModel = [&](const XBone& b, const std::array<float, 3>& p) {
        const auto o = rotate(b.rotation, p);
        return std::array<float, 3>{b.position[0] + o[0], b.position[1] + o[1], b.position[2] + o[2]};
    };

    for (int s = 0; s < surfCount; ++s) {
        XSurface surf;
        r.get<uint8_t>(); // tile mode (inferred)
        const int16_t vertCount = r.get<int16_t>();
        const int16_t triCount = r.get<int16_t>();
        const int16_t rigidBone = r.get<int16_t>();
        surf.skinned = rigidBone == -1;
        if (surf.skinned) r.get<int16_t>(); // count used to size the weight table
        surf.vertices.resize(size_t(vertCount));
        for (auto& v : surf.vertices) {
            std::array<float, 3> n = r.vec3();
            for (auto& c : v.color) c = r.get<uint8_t>();
            v.uv = {r.get<float>(), r.get<float>()};
            r.vec3(); // tangent (inferred)
            r.vec3(); // binormal (inferred)
            if (surf.skinned) {
                const uint8_t extra = r.get<uint8_t>();
                const int16_t bone0 = r.get<int16_t>();
                const XBone& b0 = boneAt(bone0);
                const auto p0 = r.vec3();
                v.influences.push_back({bone0, 1.0f, p0});
                v.boneNormal = n;
                std::array<float, 3> pos = toModel(b0, p0);
                std::array<float, 3> nrm = rotate(b0.rotation, n);
                if (extra) {
                    r.get<uint8_t>(); // unknown byte (observed in the loader, meaning unknown)
                    float rest = 1.0f;
                    std::array<float, 3> acc{};
                    for (int k = 0; k < extra; ++k) {
                        const int16_t bonek = r.get<int16_t>();
                        const XBone& bk = boneAt(bonek);
                        const auto offk = r.vec3();
                        const auto pk = toModel(bk, offk);
                        const float w = r.get<uint16_t>() / 65535.0f;
                        v.influences.push_back({bonek, w, offk});
                        v.influences.front().weight -= w;
                        for (int a = 0; a < 3; ++a) acc[size_t(a)] += pk[size_t(a)] * w;
                        rest -= w;
                    }
                    for (int a = 0; a < 3; ++a) pos[size_t(a)] = pos[size_t(a)] * rest + acc[size_t(a)];
                }
                v.position = pos;
                v.normal = nrm;
            } else {
                const XBone& b = boneAt(rigidBone);
                const auto off = r.vec3();
                v.position = toModel(b, off);
                v.normal = rotate(b.rotation, n);
                v.influences.push_back({rigidBone, 1.0f, off});
                v.boneNormal = n;
            }
        }
        surf.indices.resize(size_t(triCount) * 3);
        for (auto& i : surf.indices) {
            i = r.get<uint16_t>();
            if (i >= surf.vertices.size()) r.fail("index out of range");
        }
        out.push_back(std::move(surf));
    }
    if (r.remaining() != 0) r.fail(std::to_string(r.remaining()) + " trailing bytes");
    return out;
}

} // namespace iw2::assets
