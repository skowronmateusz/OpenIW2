// The renderer's outdoor map ("$outdoor", 1.3 DLL 0x10020c80..0x10020d57; docs/spec/effects.md "Outdoor map"): a 512 x 512
// height image of the highest solid surface above every point of the world bounds, used by the *_outdoor shaders to hide
// effects under cover.
#pragma once

#include "assets/d3dbsp.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace iw2::fs {
class Vfs;
}

namespace iw2::game {

class OutdoorMap {
public:
    static constexpr int kSize[3] = {512, 512, 256}; // 0x10190398

    // 0x100208a0: bounds of the world surfaces whose material lacks bit 8 at byte +0xc (start 131072 / -131072; an axis
    // with no surface becomes 0..0; an axis narrower than 1 grows by 0.5 on each side). Then 0x100209f0 (texel scale /
    // bias) and 0x10020a50 (worldOutdoorLookupMatrix: (p - mins) / (maxs - mins)).
    void setBounds(const assets::D3dBsp& bsp, const fs::Vfs& vfs);
    void setBounds(const std::array<float, 3>& mins, const std::array<float, 3>& maxs);
    // 0x10020bb0: for every texel (row = y, column = x) the height returned by `hitZ(x, y)` (0x100207f0: a point trace from
    // maxs.z + 1 down to mins.z - 1 with contents 0x2001, start + (end - start) * fraction) stored as
    // clamp(round_even(z * scale.z + bias.z - 0.4999999990686774), 0, 255) (0x1019bde8).
    void build(const std::function<float(float x, float y, float zTop, float zBottom)>& hitZ);
    bool valid() const { return !image_.empty(); }
    // particle_cloud_outdoor.hlsl: outdoorMapPos = worldOutdoorLookupMatrix * p; alpha 0 when outdoorMapPos.z < the image
    // value at outdoorMapPos.xy. TODO(spec): the outdoor sampler's filter and address mode (point sampling, clamped here).
    bool covered(const std::array<float, 3>& p) const;
    const std::array<float, 3>& mins() const { return mins_; }
    const std::array<float, 3>& maxs() const { return maxs_; }
    const std::vector<uint8_t>& image() const { return image_; }

private:
    std::array<float, 3> mins_{}, maxs_{}, scale_{}, bias_{};
    std::vector<uint8_t> image_;
};

} // namespace iw2::game
