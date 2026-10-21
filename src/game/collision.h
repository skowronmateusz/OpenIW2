// World collision from .d3dbsp collision lumps (docs/spec/collision.md).
#pragma once

#include "assets/d3dbsp.h"

#include <array>
#include <cstdint>
#include <memory>

namespace iw2::game {

using Vec = std::array<float, 3>;

namespace Contents {
constexpr uint32_t Solid = 0x1;
constexpr uint32_t PlayerClip = 0x10000;
constexpr uint32_t AiClip = 0x20000;
constexpr uint32_t PlayerSolid = Solid | PlayerClip;
} // namespace Contents

struct TraceResult {
    float fraction = 1.0f;
    Vec endPos{};
    Vec normal{};
    bool startSolid = false;
    bool allSolid = false;
    uint32_t contents = 0;
    uint32_t surfaceFlags = 0; // surface flags of the material that was hit (lump 0)
    int material = -1;         // index of that material in lump 0, -1 if nothing was hit
};

class CollisionWorld {
public:
    CollisionWorld() = default;
    explicit CollisionWorld(const assets::D3dBsp& bsp);
    ~CollisionWorld();
    CollisionWorld(CollisionWorld&&) noexcept;
    CollisionWorld& operator=(CollisionWorld&&) noexcept;

    // Sweeps the box `mins`/`maxs` (relative to the origin) from start to end through the world (model 0).
    // As in the original, the box is traced as a vertical capsule (docs/spec/collision.md).
    TraceResult trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const;

    size_t brushCount() const;
    size_t triangleCount() const;

    struct Data;

private:
    std::unique_ptr<Data> d_;
};

} // namespace iw2::game
