// xmodel / xmodelparts / xmodelsurfs (docs/spec/xmodel.md).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

struct XModelLod {
    float distance = 0;
    std::string name;                  // base name of xmodelparts/<name> and xmodelsurfs/<name>
    std::vector<std::string> materials; // one per surface
};

// Hit box of one bone in bone space (FUN_0047d180 adds offset and radiusSquared, used by FUN_00473d20).
struct XBoneBox {
    std::array<float, 3> mins{}, maxs{};
    std::array<float, 3> offset{}; // (mins + maxs) / 2
    float radiusSquared = 0;       // |maxs − offset|²; 0 = no box
};

// Collision triangle as stored (12 floats): the plane (normal, dist) and two edge planes whose values at a point on
// the triangle are its barycentric coordinates (traced by FUN_0047c210).
struct XCollTri {
    std::array<float, 4> plane{}, edge0{}, edge1{};
};

// Collision surface: triangles in the space of `bone`, their bounds, contents and surface flags (loaded struct
// +0 tris, +4 count, +8 mins, +0x14 maxs, +0x20 bone, +0x24 contents, +0x28 surfaceFlags).
struct XCollSurf {
    std::vector<XCollTri> tris;
    std::array<float, 3> mins{}, maxs{};
    int32_t bone = 0;
    uint32_t contents = 0;
    int32_t surfaceFlags = 0;
};

struct XModel {
    uint8_t flags = 0;
    std::array<float, 3> mins{}, maxs{};
    std::vector<XModelLod> lods; // only non-empty LODs, in file order
    // One per bone of the skeleton (xmodelparts), after the LOD material lists.
    std::vector<XBoneBox> boneBoxes;
    std::vector<XCollSurf> collSurfs;
};

// Parses the xmodel header and per-LOD material lists. Throws std::runtime_error on malformed input.
XModel parseXModel(std::span<const uint8_t> data);

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct XBone {
    std::string name;
    int parent = -1;                 // -1 for root bones
    std::array<float, 3> localOffset{};
    Quat localRotation;
    // Base pose in model space.
    std::array<float, 3> position{};
    Quat rotation;
};

struct XModelParts {
    // Identity of a parsed skeleton for caches keyed by it (unique per parseXModelParts call, kept by copies); 0 for a
    // skeleton built by hand, which is never cached. Parsed skeletons are not modified afterwards.
    uint64_t id = 0;
    int rootCount = 0;
    std::vector<XBone> bones;
    std::vector<uint8_t> classification; // per bone: hit location index (parts+0x10, read by FUN_00473d20)
};

XModelParts parseXModelParts(std::span<const uint8_t> data);

struct XInfluence {
    int16_t bone = 0;
    float weight = 1.0f;
    std::array<float, 3> offset{}; // position relative to the bone
};

struct XVertex {
    std::array<float, 3> position{}; // model space (base pose applied)
    std::array<float, 3> normal{};
    std::array<float, 3> tangent{};  // model space, like normal
    std::array<float, 3> binormal{};
    std::array<uint8_t, 4> color{};
    std::array<float, 2> uv{};
    // Skinning data: influences in bone space and the normal in the space of the first bone.
    std::vector<XInfluence> influences;
    std::array<float, 3> boneNormal{};
    std::array<float, 3> boneTangent{};
    std::array<float, 3> boneBinormal{};
};

struct XSurface {
    std::vector<XVertex> vertices;
    std::vector<uint16_t> indices;
    bool skinned = false;
};

// Parses all surfaces and transforms them into model space with the base pose of `parts`.
std::vector<XSurface> parseXModelSurfs(std::span<const uint8_t> data, const XModelParts& parts);

// A posed skeleton: model-space position and rotation per bone.
struct XPose {
    std::vector<std::array<float, 3>> position;
    std::vector<Quat> rotation;
};

// Base pose of a skeleton.
XPose bindPose(const XModelParts& parts);

// Base pose with bone controls (DObj control tags, 0x4746e0): each controlled bone's local rotation is followed by
// the control rotation, then the skeleton is rebuilt from the roots (the order local · control is inferred; the
// turret tags it is used for have identity local rotations).
XPose controlledPose(const XModelParts& parts, const std::vector<std::pair<int, Quat>>& controls);

// Applies bone controls on top of an already animated pose: a controlled bone's rotation is followed by the control
// rotation (as controlledPose) and its descendants rotate with it about the bone. Parents precede children.
XPose applyControls(const XModelParts& parts, const XPose& base, const std::vector<std::pair<int, Quat>>& controls);
struct BoneControl {
    int bone;
    Quat rotation;
    std::array<float, 3> translation; // added to the bone's local offset, in the parent bone's frame
};
XPose applyControls(const XModelParts& parts, const XPose& base, const std::vector<BoneControl>& controls);

// Control quaternion of angles in degrees (pitch, yaw, roll) as 0x4746e0 builds it: yaw about z, pitch about y,
// roll about x.
Quat anglesQuat(float pitch, float yaw, float roll);

// Re-skins a vertex with a pose (position and normal in model space).
void skinVertex(const XVertex& v, const XPose& pose, std::array<float, 3>& position, std::array<float, 3>& normal);
// The tangent and binormal under a pose. They follow the first bone, as the normal does.
void skinTangents(const XVertex& v, const XPose& pose, std::array<float, 3>& tangent, std::array<float, 3>& binormal);

// v' = q · v · q⁻¹ (inline: the skinning loops call it for every vertex).
inline std::array<float, 3> rotate(const Quat& q, const std::array<float, 3>& v) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    return {v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)};
}
Quat multiply(const Quat& a, const Quat& b);

// The skinning data of a list of vertices in one flat array (for re-skinning many vertices every frame); skin() gives
// exactly what skinVertex and skinTangents give for the source vertex.
struct SkinStream {
    struct Vertex {
        uint32_t firstInfluence = 0;
        uint32_t influenceCount = 0; // 0: the vertex keeps its model-space frame and is placed at the origin (skinVertex)
        std::array<float, 3> normal{}, tangent{}, binormal{}; // in the space of the first bone, or model space without one
    };
    std::vector<Vertex> vertices;
    std::vector<XInfluence> influences;

    void add(const XVertex& v);
    void skin(size_t k, const XPose& pose, std::array<float, 3>& position, std::array<float, 3>& normal, std::array<float, 3>& tangent,
              std::array<float, 3>& binormal) const {
        const Vertex& sv = vertices[k];
        position = {0, 0, 0};
        if (sv.influenceCount == 0) {
            normal = sv.normal, tangent = sv.tangent, binormal = sv.binormal;
            return;
        }
        const XInfluence* inf = &influences[sv.firstInfluence];
        for (uint32_t i = 0; i < sv.influenceCount; ++i) {
            const size_t b = size_t(inf[i].bone);
            const auto o = rotate(pose.rotation[b], inf[i].offset);
            for (int a = 0; a < 3; ++a) position[size_t(a)] += (pose.position[b][size_t(a)] + o[size_t(a)]) * inf[i].weight;
        }
        const Quat& r = pose.rotation[size_t(inf[0].bone)];
        normal = rotate(r, sv.normal), tangent = rotate(r, sv.tangent), binormal = rotate(r, sv.binormal);
    }
};

} // namespace iw2::assets
