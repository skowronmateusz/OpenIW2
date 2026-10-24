// Effect runtime: particles spawned from .efx definitions (docs/spec/effects.md).
#pragma once

#include "assets/efx.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iw2::game {

using FVec = std::array<float, 3>;

struct FxParticle {
    const assets::EfxElement* element = nullptr;
    int64_t startMs = 0, endMs = 0;
    FVec origin{};                      // spawn point in world space
    FVec axis[3]{};                     // effect axes (forward, right, up)
    float rnd[8]{};                     // per-channel lerp randoms: rgb, alpha, size, size2, length, rotation, vel, vel2
    float velRnd[3]{}, vel2Rnd[3]{};
    float gravity = 0;
    float scale[8]{};                   // per-channel scale picked from the scale range
    size_t shader = 0;                  // index into the element's shader list
};

struct FxSprite {
    FVec position{};
    float rgba[4]{};
    float size = 0;
    std::string shader;
};

// A live particle of a blocksSight (0x1000) element as entered in the sight-blocker list (0x480970 → 0xc3ed80,
// count 0xc47a20): its position (+0x7c), radius² from its size (+0x88), and 1 − alpha byte (+0x93) / 255.
struct FxSightBlocker {
    FVec origin{};
    float radiusSq = 0;
    float visibility = 1;
};

// A Decal element instance (0x48da90 → 0x485b60) waiting for its spawn time; the host turns it into marks
// (CG_ImpactMark 0x4ad430, game/marks.h).
struct FxDecal {
    int64_t startMs = 0;
    std::string material;
    FVec origin{}, dir{};
    float rotation = 0;
    std::array<float, 4> rgba{1, 1, 1, 1};
    float size = 0;
};

class FxSystem {
public:
    // Decals whose spawn time has come.
    std::vector<FxDecal> takeDecals(int64_t nowMs);
    // Spawns every element of an effect at `origin` facing `forward`.
    void play(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FVec& forward, int64_t nowMs);
    // Drops finished particles and returns the visible ones at nowMs.
    std::vector<FxSprite> sprites(int64_t nowMs);
    size_t count() const { return particles_.size(); }
    // The sight blockers at the time of the last sprites() call (rebuilt there, like 0x48b000 after the update).
    const std::vector<FxSightBlocker>& sightBlockers() const { return sightBlockers_; }

private:
    uint32_t seed_ = 0x13579bdf;
    float frand();
    float range(const std::vector<float>& v, float fallback);
    void spawnDecals(const assets::EfxElement& e, uint32_t flags, const FVec& origin, const FVec& f, const FVec& r, const FVec& up,
                     int64_t nowMs);
    std::vector<std::shared_ptr<const assets::EfxEffect>> keep_;
    std::vector<FxParticle> particles_;
    std::vector<FxDecal> decals_;
    std::vector<FxSightBlocker> sightBlockers_;
};

} // namespace iw2::game
