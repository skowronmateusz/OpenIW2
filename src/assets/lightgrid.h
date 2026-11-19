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

    // Static models (docs/spec/material.md "Model lighting techniques", static-model lighting texture): the eight coefficients sit in a
    // 2 x 2 x 2 texel block that the shader reads with trilinear filtering at the normal turned into the grid's basis, so the weights
    // are linear in the cube coordinates (no smoothing, unlike weights()).
    static std::array<float, 8> smodelWeights(const std::array<float, 3>& normal);
    // The block's texels: coefficients times indirectLightScale and the sun visibility times directLightScale, as bytes (x 255,
    // rounded, clamped to 0..255) and back to 0..1.
    static void smodelLighting(LightGridCoeffs& c, float& sunVisibility, float directLightScale, float indirectLightScale);

    // Re-baked light (docs/render-rebake.md stage 4): float coefficients per grid point (entry), [channel * 8 + k], used instead of the
    // shipped bytes where set. `entryCoefficients` is the shipped values of every entry (bytes / 255), in the order of points().
    size_t entryCount() const { return entries_.size(); }
    std::vector<std::array<float, 24>> entryCoefficients() const;
    bool setEntryCoefficients(std::vector<std::array<float, 24>> c); // false (and nothing set) if the count differs from entryCount()
    bool rebaked() const { return !override_.empty(); }

    // World positions of all grid points (for the enhanced renderer's reflection probes: points known to be in playable space).
    std::vector<std::array<float, 3>> points() const;

    // Lighting for a normal: sum over channels of weights · coefficients.
    static std::array<float, 3> evaluate(const LightGridCoeffs& c, const std::array<float, 3>& normal);

private:
    const LightGridEntry* find(int x, int y, int z) const;

    std::vector<LightGridEntry> entries_;
    std::vector<std::array<uint8_t, 24>> coeffs_;
    std::vector<std::array<float, 24>> override_; // per entry, empty: the shipped coefficients
};

} // namespace iw2::assets
