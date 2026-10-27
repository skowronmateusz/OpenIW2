// Effect runtime: particles spawned from .efx definitions (docs/spec/effects.md).
#pragma once

#include "assets/efx.h"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
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
    // Atlas sequence (0x48cd20): start frame (+0x108), frames per ms (+0x10c), loop mode / times (+0x110 / +0x114).
    int seqStart = 0;
    float seqRate = 0;
    int seqLoopMode = 0, seqLoopTimes = 0;
};

struct FxSprite {
    FVec position{};
    float rgba[4]{};
    float size = 0;
    std::string shader;
    float uv[4]{0, 0, 1, 1}; // atlas frame rectangle u0, v0, u1, v1 (renderer 0x1003fa60)
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
    // Atlas size of a material: {byte 0xe, byte 0xf} of materials/<name> (frame count = their product, refexport
    // 0x617ae8 → 1.3 DLL 0x10017580); {1, 1} when unknown. Set by the host.
    std::function<std::pair<int, int>(const std::string&)> atlas;
    // Loads an effect by its path ("fx/....efx"); set by the host. Used by FxRunner elements.
    std::function<std::shared_ptr<const assets::EfxEffect>(const std::string&)> loader;
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
    std::pair<int, int> atlasOf(const std::string& material);
    std::vector<FxDecal> decals_;
    // FxRunner children waiting for their spawn time (0x48dac0 → 0x4853f0): they inherit the parent's axes.
    struct PendingRun {
        std::string path;
        FVec origin{}, forward{};
        int64_t startMs = 0;
    };
    std::vector<PendingRun> pending_;
    void runPending(int64_t nowMs);
    std::vector<FxSightBlocker> sightBlockers_;
};

} // namespace iw2::game
