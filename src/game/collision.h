// World collision from .d3dbsp collision lumps (docs/spec/collision.md).
#pragma once

#include "assets/d3dbsp.h"

#include <array>
#include <cstdint>
#include <vector>

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
};

class CollisionWorld {
public:
    CollisionWorld() = default;
    explicit CollisionWorld(const assets::D3dBsp& bsp);

    // Sweeps an axis-aligned box from start to end. `mins`/`maxs` are relative to the origin.
    TraceResult trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const;

    size_t brushCount() const { return brushes_.size(); }
    size_t triangleCount() const { return tris_.size(); }

private:
    struct Plane { Vec normal; float dist; };
    struct Brush {
        Vec mins, maxs;
        std::vector<Plane> planes; // all bounding planes (6 axial + extra)
        uint32_t contents = 0;
    };

    void addTriangleBrush(const Vec& a, const Vec& b, const Vec& c, uint32_t contents);
    void buildGrid();
    void traceBrush(const Brush& b, const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, TraceResult& tr) const;

    std::vector<Brush> brushes_;
    std::vector<std::array<Vec, 3>> tris_;

    // Uniform 2D grid over x/y for broad-phase culling.
    static constexpr float kCell = 256.0f;
    float gridX0_ = 0, gridY0_ = 0;
    int gridW_ = 0, gridH_ = 0;
    std::vector<std::vector<uint32_t>> grid_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t stampValue_ = 0;
};

} // namespace iw2::game
