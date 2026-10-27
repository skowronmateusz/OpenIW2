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
    // Emitter element (type 5, class vtable 0x57c760, created by 0x48d7d0). The instance is stepped per frame
    // (0x480bd0): pos += velocity(u) * dt, with a bounce offset `bounceOff` (+0xc4) added to the velocity.
    bool emitter = false;
    // Tail element (class vtable 0x57c6b0): stepped like an Emitter (0x480bd0), drawn as a quad from pos to tailB; prev is the
    // position at the start of the update (+0x24c), tailB the end point (+0x9c, 0x483160).
    bool tail = false;
    FVec prev{}, tailB{};
    std::string model;                  // chosen from `models` at random when useModel (0x10) is set
    FVec pos{};                         // current position (+0x4)
    FVec bounceOff{};                   // +0xc4: velocity offset accumulated by bounces
    FVec ang{}, angRate{};              // +0x278 angles (degrees), +0x284 rates (per ms * 0.01)
    bool rest = false;                  // flag 0x1000000: physics finished
    int64_t lastMs = 0;                 // time the particle was stepped up to
};

// A model instance of an Emitter element with useModel, to be drawn by the host as a static xmodel.
struct FxModelInst {
    std::string model;
    FVec pos{}, angles{};
    float scale = 1.0f;
};

// A Light element (class vtable 0x57c734, created by 0x48dbc0) as submitted to the renderer every frame: draw slot 0x4841b0
// calls refexport 0x617b1c (DLL AddLightToScene 0x10024420) with the origin (+0x7c), radius = the size channel (+0x88) and
// r, g, b = the rgb channel (+0x6c..0x74, not clamped; alpha is unused).
struct FxLight {
    FVec origin{};
    float radius = 0;
    float rgb[3]{};
};

struct FxSprite {
    FVec position{};
    float rgba[4]{};
    float size = 0;
    std::string shader;
    float uv[4]{0, 0, 1, 1}; // atlas frame rectangle u0, v0, u1, v1 (renderer 0x1003fa60)
    // Tracer beam (0x4b7d90): when set, a quad B +/- perp * widthB, A -/+ perp * widthA with u 0 at A and 1 at B, v across;
    // `position` / `size` are unused.
    bool beam = false;
    // Tail element (renderer type 8, 0x10041470 / quad builder 0x100409c0): the perpendicular is normalize((A - cam) x (B - cam)),
    // both ends are widened by widthA, and the atlas rectangle uv runs s across the width, t from A to B.
    bool beamTail = false;
    FVec beamA{}, beamB{}, beamPerp{};
    float widthA = 0, widthB = 0;
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
    // Bullet tracers (0x4b8140 -> 0x4b7b60, entity drawn by 0x4c0d98 -> 0x4b7d90): rolls cg_tracerchance (0.4), then a
    // local entity moves from `from` towards `to` at cg_tracerSpeed (5000) for length / speed seconds and is drawn as a
    // streak of up to cg_tracerlength (300) units ahead of its position. frameMs = cg frame time (0xf708d4; the start is
    // moved back by a random part of half of it, 0 = no jitter).
    void addTracer(const FVec& from, const FVec& to, int64_t nowMs, int frameMs = 0);
    // Right / up of the view for the beam's facing (0xf79168 / 0xf79174; only the sign of the quad's perpendicular depends on them).
    FVec viewRight{1, 0, 0}, viewUp{0, 0, 1};
    // Drops finished particles and returns the visible ones at nowMs.
    std::vector<FxSprite> sprites(int64_t nowMs);
    // World trace used by usePhysics (0x20) elements (0x486f70): segment a -> b with an optional box, true on a hit
    // that is not start-solid, giving the fraction and the surface normal. Set by the host.
    std::function<bool(const FVec& a, const FVec& b, float& frac, FVec& normal)> trace;
    // Emitter models at the time of the last sprites() call.
    const std::vector<FxModelInst>& models() const { return models_; }
    // Light elements alive at the last sprites() call (DLL keeps at most r_dlightLimit = 4 of them, TODO(spec): the order
    // 0x10015180 chooses when there are more).
    const std::vector<FxLight>& lights() const { return lights_; }
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
    struct Tracer {
        FVec base{}, dir{};
        float length = 0; // distance base -> impact (+0x44)
        int64_t startMs = 0, endMs = 0;
    };
    std::vector<Tracer> tracers_;
    void runPending(int64_t nowMs);
    std::vector<FxSightBlocker> sightBlockers_;
    std::vector<FxModelInst> models_;
    std::vector<FxLight> lights_;
    FVec velocityAt(const FxParticle& p, float u) const;
    void stepEmitter(FxParticle& p, int64_t nowMs);
};

} // namespace iw2::game
