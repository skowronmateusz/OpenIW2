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

// Poses a skeleton with an animation at a fractional frame. Bones the animation does not name keep their base
// local transform; animated bones take the track's local rotation and, if present, local translation.
XPose animatePose(const XModelParts& parts, const XAnim& anim, float frame);

// Samples a rotation or translation track at a fractional frame (linear / normalized-lerp interpolation).
Quat sampleRot(const XAnimRotTrack& t, float frame, int keyCount);
std::array<float, 3> sampleTrans(const XAnimTransTrack& t, float frame, int keyCount);

} // namespace iw2::assets
