// FUN_00473d20 and the duplicate-bone table of FUN_00472660 (docs/spec/collision.md).
#include "game/hitboxes.h"

#include <cctype>
#include <cmath>
#include <string>

namespace iw2::game {

namespace {

using V3 = std::array<float, 3>;

const char* const kNames[kHitLocationCount] = {
    "none",           "helmet",         "head",           "neck",           "torso_upper",     "torso_lower",
    "right_arm_upper", "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand",      "left_hand",
    "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower", "right_foot",      "left_foot",
    "gun"};

// FUN_00420270: rows = the bone's rotated axes.
void quatAxes(const assets::Quat& q, V3 axis[3]) {
    axis[0] = assets::rotate(q, {1, 0, 0});
    axis[1] = assets::rotate(q, {0, 1, 0});
    axis[2] = assets::rotate(q, {0, 0, 1});
}

// Bone names are compared like the lookups in World::entityModels (case-insensitive).
bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

float dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

const uint8_t kHitPriorityDefault[kHitLocationCount] = {1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0};
const uint8_t kHitPriorityRifle[kHitLocationCount] = {1, 9, 9, 9, 8, 7, 6, 6, 6, 6, 5, 5, 4, 4, 4, 4, 3, 3, 0};

const char* hitLocationName(int location) {
    return location >= 0 && location < kHitLocationCount ? kNames[location] : "none";
}

HitBoxResult traceHitBoxes(const std::vector<HitBoxModel>& models, const V3& start, const V3& end, const uint8_t* priority,
                           float fraction) {
    HitBoxResult res;
    res.fraction = fraction;
    const V3 delta = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float lenSq = dot3(delta, delta);
    if (lenSq == 0 || models.empty()) return res;
    const float invLenSq = 1.0f / lenSq;
    auto prio = [&](int cls) { return cls >= 0 && cls < kHitLocationCount ? priority[cls] : 0; };

    // Global bone index → (model, bone), for duplicates and resolved classes.
    std::vector<int> firstOfModel;
    int total = 0;
    for (const auto& m : models) {
        firstOfModel.push_back(total);
        total += m.parts ? int(m.parts->bones.size()) : 0;
    }
    // FUN_00472660 / FUN_004725f0: a bone of a model attached without a tag duplicates the bone of the same name in
    // the first model that has it (the last such bone within that model).
    auto findName = [&](const std::string& name) {
        for (size_t mi = 0; mi < models.size(); ++mi) {
            if (!models[mi].parts) continue;
            const auto& bones = models[mi].parts->bones;
            for (int b = int(bones.size()) - 1; b >= 0; --b)
                if (sameName(bones[size_t(b)].name, name)) return firstOfModel[mi] + b;
        }
        return -1;
    };
    std::vector<int> resolved(size_t(total), 0);
    float leaveStart = fraction;
    uint8_t best = 2;
    int hitAxis = -1;
    float hitSign = 0;
    const assets::Quat* hitRot = nullptr;

    for (size_t mi = 0; mi < models.size(); ++mi) {
        const HitBoxModel& m = models[mi];
        if (!m.parts) continue;
        const int numBones = int(m.parts->bones.size());
        for (int b = 0; b < numBones; ++b) {
            const int g = firstOfModel[mi] + b;
            const auto& bone = m.parts->bones[size_t(b)];
            int cls = size_t(b) < m.parts->classification.size() ? m.parts->classification[size_t(b)] : 0;
            uint8_t p = prio(cls);
            int dup = -1;
            if (mi > 0 && m.attachBone < 0) {
                const int t = findName(bone.name);
                if (t >= 0 && t != g) dup = t;
            }
            if (dup >= 0) {
                if (p == 1) {
                    cls = resolved[size_t(dup)];
                    p = prio(cls);
                }
            } else if (p == 1) {
                if (b < m.parts->rootCount) cls = m.attachBone >= 0 ? resolved[size_t(m.attachBone)] : 0;
                else cls = resolved[size_t(firstOfModel[mi] + bone.parent)];
                p = prio(cls);
            }
            resolved[size_t(g)] = cls;
            if (m.hidden || !m.boxes || size_t(b) >= m.boxes->size() || size_t(b) >= m.position.size()) continue;
            const assets::XBoneBox& box = (*m.boxes)[size_t(b)];
            if (box.radiusSquared == 0 || p < best) continue;
            const V3& trans = m.position[size_t(b)];
            V3 axis[3];
            quatAxes(m.rotation[size_t(b)], axis);
            // Sphere around the box centre against the segment (FUN_00420330).
            V3 c;
            for (size_t i = 0; i < 3; ++i)
                c[i] = axis[1][i] * box.offset[1] + axis[2][i] * box.offset[2] + axis[0][i] * box.offset[0] + trans[i];
            const V3 sc = {start[0] - c[0], start[1] - c[1], start[2] - c[2]};
            const float t = -(dot3(sc, delta) * invLenSq);
            float distSq;
            if (t >= 1.0f) {
                const V3 ec = {end[0] - c[0], end[1] - c[1], end[2] - c[2]};
                distSq = dot3(ec, ec);
            } else if (t > 0) {
                const V3 q = {delta[0] * t + sc[0], delta[1] * t + sc[1], delta[2] * t + sc[2]};
                distSq = dot3(q, q);
            } else {
                distSq = dot3(sc, sc);
            }
            const float inside = box.radiusSquared - distSq;
            if (!(inside > 0)) continue;
            if (p == best && !(t - std::sqrt(inside * invLenSq) < res.fraction)) continue;
            // Slab test in bone space (FUN_00473c90).
            V3 ls, le;
            const V3 s0 = {start[0] - trans[0], start[1] - trans[1], start[2] - trans[2]};
            const V3 e0 = {end[0] - trans[0], end[1] - trans[1], end[2] - trans[2]};
            for (size_t i = 0; i < 3; ++i) {
                ls[i] = dot3(axis[i], s0);
                le[i] = dot3(axis[i], e0);
            }
            float enterFrac = 0, leaveFrac = p == best ? res.fraction : leaveStart;
            bool startInside = true, endInside = true, miss = false;
            int axisHit = -1;
            float signHit = 0;
            for (int side = 0; side < 2 && !miss; ++side) {
                const float s = side == 0 ? -1.0f : 1.0f;
                const V3& bound = side == 0 ? box.mins : box.maxs;
                for (size_t i = 0; i < 3; ++i) {
                    const float d1 = (ls[i] - bound[i]) * s;
                    const float d2 = (le[i] - bound[i]) * s;
                    if (d1 > 0) {
                        if (d2 > 0) {
                            miss = true;
                            break;
                        }
                        startInside = false;
                        if (enterFrac * (d1 - d2) < d1) {
                            enterFrac = d1 / (d1 - d2);
                            if (!(enterFrac < leaveFrac)) {
                                miss = true;
                                break;
                            }
                            signHit = s;
                            axisHit = int(i);
                        }
                    } else if (d2 > 0) {
                        endInside = false;
                        if ((d1 - d2) * leaveFrac < d1) {
                            leaveFrac = d1 / (d1 - d2);
                            if (leaveFrac <= enterFrac) {
                                miss = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (miss) continue;
            if (startInside) {
                if (!endInside) continue;
                if (!(delta[0] * start[0] + delta[1] * start[1] <= 0)) continue;
                res.hit = true;
                res.fraction = 0;
                res.model = int(mi);
                res.bone = b;
                res.location = cls;
                if (delta[0] == 0 && delta[1] == 0) {
                    res.normal = {0, 0, delta[2] < 0 ? 1.0f : -1.0f};
                } else {
                    const float l = std::sqrt(start[0] * start[0] + start[1] * start[1]);
                    res.normal = l != 0 ? V3{start[0] / l, start[1] / l, 0} : V3{start[0], start[1], 0};
                }
                return res;
            }
            if (p != best || enterFrac < res.fraction) {
                res.hit = true;
                res.fraction = enterFrac;
                res.model = int(mi);
                res.bone = b;
                res.location = cls;
                best = p;
                hitAxis = axisHit;
                hitSign = signHit;
                hitRot = &m.rotation[size_t(b)];
            }
        }
    }
    if (res.hit && hitRot && hitAxis >= 0) {
        V3 axis[3];
        quatAxes(*hitRot, axis);
        res.normal = {hitSign * axis[hitAxis][0], hitSign * axis[hitAxis][1], hitSign * axis[hitAxis][2]};
    }
    return res;
}

} // namespace iw2::game
