// Light grid (lumps 2 and 3 of .d3dbsp), see docs/spec/lightgrid.md.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace iw2::assets {

struct LightGridEntry {
    uint32_t key = 0;   // coarse cell key
    uint8_t sub = 0;    // 4x4x4 sub-cell index << 2, bit 0 = sun visible
    uint8_t unknown = 0;
    uint16_t coeffIndex = 0;
};

// 8 coefficients per color channel, in [0, 1].
using LightGridCoeffs = std::array<std::array<float, 8>, 3>;

class LightGrid {
public:
    LightGrid() = default;
    LightGrid(std::span<const uint8_t> hashLump, std::span<const uint8_t> coeffLump);

    bool empty() const { return entries_.empty(); }

    // Trilinear sample at a world position. Returns false if no grid point was found nearby.
    bool sample(const std::array<float, 3>& pos, LightGridCoeffs& out, float& sunVisibility) const;

    // The 8 direction weights the original renderer bakes into $lightgridweights0/1 for a unit normal.
    static std::array<float, 8> weights(const std::array<float, 3>& normal);

    // Lighting for a normal: sum over channels of weights · coefficients.
    static std::array<float, 3> evaluate(const LightGridCoeffs& c, const std::array<float, 3>& normal);

private:
    const LightGridEntry* find(int x, int y, int z) const;

    std::vector<LightGridEntry> entries_;
    std::vector<std::array<uint8_t, 24>> coeffs_;
};

} // namespace iw2::assets
