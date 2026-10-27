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
using FxAxes = std::array<FVec, 3>; // forward, right, up (rows of the 3x3 axis of the game)

// A bolt (the attachment record of 0x4853f0: entity + tag) as a host callback returning the tag's current world position;
// false when the attachment is no longer valid (0x485bf0).
using FxBolt = std::function<bool(FVec&)>;

struct FxParticle {
    const assets::EfxElement* element = nullptr;
    int64_t startMs = 0, endMs = 0;
    FVec origin{};                      // spawn point in world space
    FVec axis[3]{};                     // effect axes (forward, right, up)
    float rnd[8]{};                     // per-channel lerp randoms: rgb, alpha, size, size2, length, rotation, vel, vel2
    float velRnd[3]{}, vel2Rnd[3]{};
    float gravity = 0;
    std::shared_ptr<FxBolt> bolt;       // relative (0x2) particle of a bolted effect: follows the tag
    FVec bolt0{};                       // the tag position at spawn
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
    // Plays an effect bolted to an entity tag (0x4853f0 with an attachment record): `relative` (flag 0x2) elements follow
    // the tag while they live and are not spawned when the attachment is invalid (0x485bf0).
    void playBolted(const std::shared_ptr<const assets::EfxEffect>& fx, std::shared_ptr<FxBolt> bolt, const FxAxes& axes, int64_t nowMs);
    void playAxes(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FxAxes& axes, int64_t nowMs);
    // The viewer for the per-element spawnRange / cullrange gate (0x485300): origin and tan(fovX / 2) / tan(40 deg)
    // (view struct +0x14 / +0xf4, 0x486e00). While viewValid is false the gate passes. Set by the host each frame.
    FVec viewOrigin{};
    float viewScale = 1.0f;
    bool viewValid = false;
    // The view frustum planes (view struct +0x90.., stride 0x10, count +0xf0; 0x486e90): normal and distance, inward
    // normals. An element with spawn flag frustumCull (0x400) is skipped when n·origin − d < −radius for any plane, radius
    // from `spawnFrustumCullRadius` (+0x2a0, default 0). TODO(spec): the exe's plane set (we pass the four side planes).
    std::vector<std::array<float, 4>> viewPlanes;
    // Drops finished particles and returns the visible ones at nowMs.
    std::vector<FxSprite> sprites(int64_t nowMs);
    size_t count() const { return particles_.size(); }
    // The sight blockers at the time of the last sprites() call (rebuilt there, like 0x48b000 after the update).
    const std::vector<FxSightBlocker>& sightBlockers() const { return sightBlockers_; }

private:
    uint32_t seed_ = 0x13579bdf;
    float frand();
    float range(const std::vector<float>& v, float fallback);
    void spawnDecals(const assets::EfxElement& e, uint32_t flags, const FVec& origin, FxAxes& ax, int64_t startMs);
    void spawnParticle(const assets::EfxElement& e, uint32_t flags, const FVec& origin, FxAxes& ax, int64_t startMs);
    FVec spawnPoint(const assets::EfxElement& e, const FVec& origin, FxAxes& ax);
    bool inRange(const assets::EfxElement& e, const FVec& origin) const;
    float elementDelay(const assets::EfxElement& e, int i, int n);
    std::vector<std::shared_ptr<const assets::EfxEffect>> keep_;
    std::vector<FxParticle> particles_;
    std::pair<int, int> atlasOf(const std::string& material);
    std::vector<FxDecal> decals_;
    // FxRunner children waiting for their spawn time (0x48dac0 → 0x4853f0): they inherit the parent's axes.
    struct PendingRun {
        std::string path;
        FVec origin{};
        FxAxes axes{};
        int64_t startMs = 0;
        std::shared_ptr<FxBolt> bolt;
    };
    std::shared_ptr<FxBolt> curBolt_; // the attachment of the effect being played (children inherit it, 0x48dac0)
    std::vector<PendingRun> pending_;
    void runPending(int64_t nowMs);
    std::vector<FxSightBlocker> sightBlockers_;
};

} // namespace iw2::game
