// Effect runtime: particles spawned from .efx definitions (docs/spec/effects.md).
#pragma once

#include "assets/efx.h"

#include <array>
#include <cstdint>
#include <memory>
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

class FxSystem {
public:
    // Spawns every element of an effect at `origin` facing `forward`.
    void play(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FVec& forward, int64_t nowMs);
    // Drops finished particles and returns the visible ones at nowMs.
    std::vector<FxSprite> sprites(int64_t nowMs);
    size_t count() const { return particles_.size(); }

private:
    uint32_t seed_ = 0x13579bdf;
    float frand();
    float range(const std::vector<float>& v, float fallback);
    std::vector<std::shared_ptr<const assets::EfxEffect>> keep_;
    std::vector<FxParticle> particles_;
};

} // namespace iw2::game
