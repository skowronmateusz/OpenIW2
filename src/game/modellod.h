// Model LOD selection and the static model distance cull, implemented from docs/spec/render-hints.md "Model LOD" and "World and static
// model culling" (exe xmodel functions 0x47dd90 / 0x47dd60, renderer 0x10025b10, 0x1001dbd0, 0x100095b0).
#pragma once

#include <cmath>

namespace iw2::game {

// LOD for a distance (exe 0x47dd90): the first LOD whose distance is 0 or greater than `dist`; -1 when none qualifies (not drawn).
// `dists` holds the distances of the `count` LODs up to the first empty LOD name.
inline int lodForDistance(const float* dists, int count, float dist) {
    for (int i = 0; i < count; ++i)
        if (dists[i] == 0.0f || dist < dists[i]) return i;
    return -1;
}

// Distance of the last LOD (exe 0x47dd60); 0 for a model without LODs.
inline float lodOutDistance(const float* dists, int count) { return count > 0 ? dists[count - 1] : 0.0f; }

// The scene's LOD parameters (renderer 0x10025b10): the view origin, r_lodScale and r_lodBias, both multiplied by
// tan(fov_x / 2) / tan(40 deg) when the field of view is not exactly 80.
struct LodView {
    float origin[3] = {0, 0, 0};
    float scale = 1.0f, bias = 0.0f;
};

inline LodView lodView(const float origin[3], float fovX, float rLodScale = 1.0f, float rLodBias = 0.0f) {
    LodView v;
    for (int a = 0; a < 3; ++a) v.origin[a] = origin[a];
    v.scale = rLodScale;
    v.bias = rLodBias;
    if (fovX != 80.0f) {
        const float k = float(std::tan(double(fovX) * 0.5 * 0.01745329238474369) / std::tan(0.6981317400932312));
        v.scale *= k;
        v.bias *= k;
    }
    return v;
}

// |lod origin - p| * lodScale + lodBias (0x10059120 distance, then the scale and bias of 0x100095b0 / 0x1001dbd0).
inline float lodScaledDistance(const LodView& v, const float p[3]) {
    const float dx = v.origin[0] - p[0], dy = v.origin[1] - p[1], dz = v.origin[2] - p[2];
    return std::sqrt(dz * dz + dx * dx + dy * dy) * v.scale + v.bias;
}

// LOD distance of a model instance (0x1001dbd0): the scaled distance divided by the instance scale (not divided for a scale of 0).
inline float lodDistance(const LodView& v, const float origin[3], float instanceScale) {
    const float d = lodScaledDistance(v, origin);
    return instanceScale != 0.0f ? d / instanceScale : d;
}

// Static model cull distance (0x1002ad90): the last LOD distance times the model scale; 0 = never distance culled.
inline float staticModelCullDistance(const float* dists, int count, float scale) { return lodOutDistance(dists, count) * scale; }

// Static model distance cull (0x100095b0): with a cull distance other than 0, a scaled distance greater than it drops the model.
inline bool staticModelDistanceCulled(const LodView& v, const float origin[3], float cullDist) {
    return cullDist != 0.0f && lodScaledDistance(v, origin) > cullDist;
}

// The LOD a static model draws this frame, -1 for none: the distance cull, then the LOD of its scaled distance.
inline int staticModelLod(const LodView& v, const float origin[3], float scale, const float* dists, int count) {
    if (staticModelDistanceCulled(v, origin, staticModelCullDistance(dists, count, scale))) return -1;
    return lodForDistance(dists, count, lodDistance(v, origin, scale));
}

} // namespace iw2::game
