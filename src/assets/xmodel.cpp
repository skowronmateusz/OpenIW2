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
        XCollSurf cs;
        const uint32_t triCount = r.get<uint32_t>();
        cs.tris.resize(triCount);
        for (XCollTri& t : cs.tris)
            for (auto* plane : {&t.plane, &t.edge0, &t.edge1})
                for (float& f : *plane) f = r.get<float>();
        cs.mins = r.vec3();
        cs.maxs = r.vec3();
        cs.bone = r.get<int32_t>();
        // The loader (0x47d0cb) stores contents & 0xdffffffb and ORs it into the xmodel word +0x5c.
        cs.contents = r.get<uint32_t>() & 0xdffffffbu;
        cs.surfaceFlags = r.get<int32_t>();
        m.collSurfs.push_back(std::move(cs));
    }
    for (auto& lod : all) {
        if (lod.name.empty()) continue;
        const int16_t count = r.get<int16_t>();
        for (int i = 0; i < count; ++i) lod.materials.push_back(r.str());
        m.lods.push_back(std::move(lod));
    }
    // Bone boxes: 24 bytes each, one per skeleton bone (the count comes from xmodelparts in FUN_0047d180).
    while (r.remaining() >= 24) {
        XBoneBox b;
        b.mins = r.vec3();
        b.maxs = r.vec3();
        for (size_t i = 0; i < 3; ++i) b.offset[i] = (b.maxs[i] + b.mins[i]) * 0.5f;
        const float dz = b.maxs[2] - b.offset[2];
        b.radiusSquared = (b.maxs[0] - b.offset[0]) * (b.maxs[0] - b.offset[0]) +
                          (b.maxs[1] - b.offset[1]) * (b.maxs[1] - b.offset[1]) + dz * dz;
        m.boneBoxes.push_back(b);
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
    for (int i = 0; i < total; ++i) p.classification.push_back(r.get<uint8_t>()); // per-bone hit location

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

XPose controlledPose(const XModelParts& parts, const std::vector<std::pair<int, Quat>>& controls) {
    XPose p = bindPose(parts);
    if (controls.empty()) return p;
    for (size_t i = 0; i < parts.bones.size(); ++i) {
        const XBone& b = parts.bones[i];
        Quat local = b.localRotation;
        bool controlled = false;
        for (const auto& c : controls)
            if (c.first == int(i)) { local = multiply(local, c.second); controlled = true; }
        if (b.parent < 0) {
            if (controlled) p.rotation[i] = local;
            continue;
        }
        const size_t pi = size_t(b.parent);
        p.rotation[i] = multiply(p.rotation[pi], local);
        const auto o = rotate(p.rotation[pi], b.localOffset);
        for (int a = 0; a < 3; ++a) p.position[i][size_t(a)] = p.position[pi][size_t(a)] + o[size_t(a)];
    }
    return p;
}

XPose applyControls(const XModelParts& parts, const XPose& base, const std::vector<std::pair<int, Quat>>& controls) {
    std::vector<BoneControl> c;
    c.reserve(controls.size());
    for (const auto& q : controls) c.push_back({q.first, q.second, {0, 0, 0}});
    return applyControls(parts, base, c);
}

// Bone control entries carry a rotation and a translation (0x4746e0: {quat, translation}); the translation is an
// additive delta to the bone's local offset in the parent's frame.
XPose applyControls(const XModelParts& parts, const XPose& base, const std::vector<BoneControl>& controls) {
    XPose p = base;
    if (controls.empty() || base.rotation.size() != parts.bones.size()) return p;
    std::vector<Quat> delta(parts.bones.size(), Quat{0, 0, 0, 1}); // world-frame rotation added to each bone
    for (size_t i = 0; i < parts.bones.size(); ++i) {
        const int par = parts.bones[i].parent;
        Quat d{0, 0, 0, 1};
        if (par >= 0) {
            const size_t pi = size_t(par);
            d = delta[pi];
            const auto o = rotate(d, {base.position[i][0] - base.position[pi][0], base.position[i][1] - base.position[pi][1],
                                      base.position[i][2] - base.position[pi][2]});
            for (size_t a = 0; a < 3; ++a) p.position[i][a] = p.position[pi][a] + o[a];
            p.rotation[i] = multiply(d, base.rotation[i]);
        }
        for (const auto& c : controls)
            if (c.bone == int(i)) {
                p.rotation[i] = multiply(p.rotation[i], c.rotation);
                const Quat& r = base.rotation[i];
                d = multiply(p.rotation[i], Quat{-r.x, -r.y, -r.z, r.w});
                if (par >= 0) {
                    const auto t = rotate(p.rotation[size_t(par)], {c.translation[0], c.translation[1], c.translation[2]});
                    for (size_t a = 0; a < 3; ++a) p.position[i][a] += t[a];
                }
            }
        delta[i] = d;
    }
    return p;
}

Quat anglesQuat(float pitch, float yaw, float roll) {
    const float k = 0.00872664626f; // degrees to half-angle radians (0x591fa4)
    const float sy = std::sin(yaw * k), cy = std::cos(yaw * k), sp = std::sin(pitch * k), cp = std::cos(pitch * k),
                sr = std::sin(roll * k), cr = std::cos(roll * k);
    const float a = -(sy * sp), b = cy * sp, c = cp * sy, d = cp * cy;
    return {cr * a + sr * d, cr * b + sr * c, cr * c - sr * b, cr * d - sr * a};
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
