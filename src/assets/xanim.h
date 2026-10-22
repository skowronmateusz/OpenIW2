// xanim/<name> animations, version 14 (docs/spec/xanim.md).
#pragma once

#include "assets/xmodel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

struct XAnimRotTrack {
    std::vector<uint16_t> frames; // key frame numbers; empty = one key per frame (or a single constant key)
    std::vector<Quat> keys;
};

struct XAnimTransTrack {
    std::vector<uint16_t> frames;
    std::vector<std::array<float, 3>> keys;
};

struct XAnimBone {
    std::string name;
    XAnimRotTrack rot;
    XAnimTransTrack trans;
};

struct XAnimNote {
    std::string name;
    float time = 0; // 0..1
};

struct XAnim {
    int numFrames = 0;   // frames in the file
    int frameRate = 0;
    bool looping = false;
    bool delta = false;
    XAnimRotTrack deltaRot;     // root motion (yaw only)
    XAnimTransTrack deltaTrans;
    std::vector<XAnimBone> bones;
    std::vector<XAnimNote> notes;

    float length() const { return frameRate > 0 ? float(numFrames > 1 ? numFrames - 1 : 0) / float(frameRate) : 0.0f; }
    int keyCount() const { return numFrames + (looping ? 1 : 0); }
};

// Throws std::runtime_error on malformed input; the whole file must be consumed.
XAnim parseXAnim(std::span<const uint8_t> data);

// The engine's animation calc (docs/spec/xanim.md, "Blending"). Per model bone: a quaternion sum, a translation
// sum and a weight sum (the original's 0x20-byte accumulator), plus whether any calculated animation names the bone.
struct BoneAccum {
    float q[4] = {0, 0, 0, 0};
    float t[3] = {0, 0, 0};
    float w = 0;
};
struct AnimAccum {
    std::vector<BoneAccum> bones;
    std::vector<uint8_t> animated;
    explicit AnimAccum(size_t n) : bones(n), animated(n, 0) {}
};

// Adds one animation at a fractional frame with a weight (0x479980): for every bone it names, the interpolated
// quaternion (not normalized; identity if the bone has no rotation keys) and translation (zero if it has no
// translation keys) times the weight, and the weight.
void accumulateAnim(const XModelParts& parts, const XAnim& anim, float frame, float weight, AnimAccum& acc);

// Adds a blend node's result into its parent's accumulator (0x4771e0): per bone the normalized quaternion times the
// node's weight and, if the node has translation weight, its averaged translation times the node's weight.
void mergeBlend(const AnimAccum& node, float weight, AnimAccum& parent);

// Final local pose (0x4782d0, 0x472af0): animated bones take the accumulated quaternion and translation divided by
// the weight sum, the translation being added to the bone's base offset; other bones keep the model's base
// rotation and offset.
XPose finishPose(const XModelParts& parts, const AnimAccum& acc);

// One animation at a fractional frame.
XPose animatePose(const XModelParts& parts, const XAnim& anim, float frame);

// Several animations that are the active children of one blend node at the top of a tree (or the only active
// leaf): each layer is added with its weight and the sums are divided by the weight sum.
struct PoseLayer { const XAnim* anim; float frame; float weight; };
XPose blendPose(const XModelParts& parts, const std::vector<PoseLayer>& layers);

// Samples a rotation or translation track at a fractional frame (linear / normalized-lerp interpolation).
Quat sampleRot(const XAnimRotTrack& t, float frame, int keyCount);
// Linear interpolation of the two keys without normalizing, as the engine samples (0x474b40).
Quat sampleRotRaw(const XAnimRotTrack& t, float frame, int keyCount);
std::array<float, 3> sampleTrans(const XAnimTransTrack& t, float frame, int keyCount);

} // namespace iw2::assets
