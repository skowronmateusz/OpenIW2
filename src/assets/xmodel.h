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
    std::array<uint8_t, 4> color{};
    std::array<float, 2> uv{};
    // Skinning data: influences in bone space and the normal in the space of the first bone.
    std::vector<XInfluence> influences;
    std::array<float, 3> boneNormal{};
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

std::array<float, 3> rotate(const Quat& q, const std::array<float, 3>& v);
Quat multiply(const Quat& a, const Quat& b);

} // namespace iw2::assets
