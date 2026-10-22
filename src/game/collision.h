// World collision from .d3dbsp collision lumps (docs/spec/collision.md).
#pragma once

#include "assets/d3dbsp.h"

#include <array>
#include <cstdint>
#include <memory>
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
    uint32_t surfaceFlags = 0; // surface flags of the material that was hit (lump 0)
    int material = -1;         // index of that material in lump 0, -1 if nothing was hit
    int entity = -1;           // id of the entity clip that was hit (setEntityClips), -1 for the world
};

// A solid brush-model entity the traces clip against (SV_ClipMoveToEntities 0x41a590 / 0x44b840).
struct EntityClip {
    int id = -1;        // caller's entity id, reported in TraceResult::entity
    int model = 0;      // brush submodel index (lump 35), ≥ 1
    Vec origin{}, angles{};
    uint32_t contents = 0;
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
    // Clips against the world and the entity clips set with setEntityClips.
    TraceResult trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const;
    // World model 0 only (CM_BoxTrace, FUN_00417640 with model 0).
    TraceResult traceWorld(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const;
    // Brush submodel `model` placed at origin/angles (FUN_00417c20 → FUN_00417640 with model ≠ 0).
    TraceResult traceModel(int model, const Vec& origin, const Vec& angles, const Vec& start, const Vec& end, const Vec& mins,
                           const Vec& maxs, uint32_t mask) const;
    // Contents of a submodel's brushes and triangles (0 for an unknown model).
    uint32_t modelContents(int model) const;
    void setEntityClips(std::vector<EntityClip> clips);

    size_t brushCount() const;
    size_t triangleCount() const;

    struct Data;

private:
    std::unique_ptr<Data> d_;
};

} // namespace iw2::game
