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
constexpr uint32_t ActorBody = 0x4000;      // actor r.contents (FUN_004fc640)
constexpr uint32_t PlayerBody = 0x2000000;  // player r.contents (0x4e90e2)
constexpr uint32_t BulletMask = 0x280e831;  // Bullet_Fire, bullettrace with characters
constexpr uint32_t BulletMaskNoCharacters = 0x802831;
constexpr uint32_t SightMask = 0x280d803;   // sighttracepassed with characters
constexpr uint32_t SightMaskNoCharacters = 0x801803;
constexpr uint32_t ActorClipMask = 0x2820011;
constexpr uint32_t PlayerClipMask = 0x281c011;
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

// A solid entity the traces clip against (SV_ClipMoveToEntities 0x41a590 / 0x44b840): a brush submodel, or for
// other entities their r.mins/r.maxs as the temporary box model 0x3ff (FUN_00415d00), traced without rotation.
struct EntityClip {
    int id = -1;        // caller's entity id, reported in TraceResult::entity
    int owner = -1;     // r.ownerNum (ent+0xf4), -1 for none
    int model = 0;      // brush submodel index (lump 35), ≥ 1; 0 for a box
    Vec origin{}, angles{};
    Vec mins{}, maxs{}; // box bounds relative to the origin (model 0)
    uint32_t contents = 0;
    bool hitBoxes = false; // ent+0xa2 & 2: locational traces with a priority map test its bones instead
    bool collSurfs = false; // ent+0xa2 & 4 (script_vehicle 0x50d844): locational traces test its xmodel collision surfaces instead
};

class CollisionWorld {
public:
    CollisionWorld();
    explicit CollisionWorld(const assets::D3dBsp& bsp);
    ~CollisionWorld();
    CollisionWorld(CollisionWorld&&) noexcept;
    CollisionWorld& operator=(CollisionWorld&&) noexcept;

    // Sweeps the box `mins`/`maxs` (relative to the origin) from start to end through the world (model 0).
    // As in the original, the box is traced as a vertical capsule (docs/spec/collision.md).
    // Clips against the world and the entity clips set with setEntityClips.
    // Skips `passEntity`, entities it owns and its owner (0x44b840), and with `skipHitBoxes` the clips with hit boxes
    // (the caller tests their bones), with `skipCollSurfs` the clips with xmodel collision surfaces (locational traces).
    TraceResult trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask, int passEntity = -1,
                      bool skipHitBoxes = false, bool skipCollSurfs = false) const;
    // The temporary box model 0x3ff at `origin` (FUN_00417c20 → FUN_00417640: FUN_00416120 / FUN_00417060).
    TraceResult traceBox(const Vec& origin, const Vec& boxMins, const Vec& boxMaxs, uint32_t boxContents, const Vec& start,
                         const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const;
    // Sight trace (FUN_00418d10 through G's entity walk 0x44c3c0): true when the world or an entity clip blocks.
    // Two pass entities; each also skips the entities it owns (FUN_0044bba0).
    bool sightBlocked(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask, int pass1 = -1,
                      int pass2 = -1) const;
    // Entity clips the trace would test for this pass entity (contents, ownership), for callers that add hit boxes.
    const std::vector<EntityClip>& entityClips() const;
    static bool skipClip(const EntityClip& c, uint32_t mask, int passEntity, int passOwner);
    int ownerOf(int entity) const;
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
