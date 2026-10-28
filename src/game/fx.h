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

// The orientation of an attachment's tag (0x48abb0 output: origin, then axis rows forward / left / up). hasAxes is false when the
// host only knows the position (the axes are then taken as the identity, i.e. the tag only translates).
struct FxTag {
    FVec origin{};
    FxAxes axes{FVec{1, 0, 0}, FVec{0, 1, 0}, FVec{0, 0, 1}};
    bool hasAxes = false;
};
// A bolt (the attachment record of 0x4853f0: entity + tag) as a host callback returning the tag's current orientation;
// false when the attachment is no longer valid (0x485bf0).
using FxBolt = std::function<bool(FxTag&)>;

struct FxParticle {
    const assets::EfxElement* element = nullptr;
    int64_t startMs = 0, endMs = 0;
    FVec origin{};                      // spawn point in world space
    FVec axis[3]{};                     // effect axes (forward, right, up)
    float rnd[8]{};                     // per-channel lerp randoms: rgb, alpha, size, size2, length, rotation, vel, vel2
    float velRnd[3]{}, vel2Rnd[3]{};
    float gravity = 0;
    float rot0 = 0;                     // Particle start angle (+0x44, drawn from `rotation` by 0x48ce70)
    std::shared_ptr<FxBolt> bolt;       // relative (0x2) particle of a bolted effect: follows the tag
    FVec bolt0{};                       // the tag position at spawn
    FVec local0{};                      // relative: the spawn point in tag space (0x48c290 end: (p − tag origin) · tag rows)
    FxTag tag0;                         // relative: the tag orientation at spawn
    float scale[8]{};                   // per-channel scale picked from the scale range
    float velScale[3]{1, 1, 1}, vel2Scale[3]{1, 1, 1}; // velocity / velocity2 X, Y, Z scales (0x4804c0 records +0x1c4.., +0x20c..)
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
    // Line element (type 2, class vtable 0x57c708, spawn 0x48d3c0): fixed end points A (the spawn point, +0x4) and B (+0x24c,
    // from `origin2`, 0x48c850), drawn as a renderer type 8 quad like Tail (update 0x482da0, draw 0x482d90).
    bool line = false;
    FVec lineB{};
    // Cloud element (type 12, class vtable 0x57c78c, spawn 0x48de90): moves like an Emitter (0x480bd0) and spins its
    // 1024-point cube about a random unit axis (+0x24c, 0x482850) by the angle +0x44 (start: `rotation`, advanced by the
    // rotationDelta channel, 0x481b90). lengthRnd is the useRandomLength lerp random (+0x260, 0x426250(0, 1)).
    bool cloud = false;
    FVec cloudAxis{1, 0, 0};
    float cloudAngle = 0, lengthRnd = 0;
    int64_t cloudLastMs = 0;
    std::string model;                  // chosen from `models` at random when useModel (0x10) is set
    FVec pos{};                         // current position (+0x4)
    FVec bounceOff{};                   // +0xc4: velocity offset accumulated by bounces
    FVec ang{}, angRate{};              // +0x278 angles (degrees), +0x284 rates (per ms * 0.01)
    bool rest = false;                  // flag 0x1000000: physics finished
    bool dead = false;                  // the update returned false (impactKills hit, attachment lost): removed
    int64_t lastMs = 0;                 // time the particle was stepped up to
    float bounce = 0;                   // +0x100: `bounce` drawn at spawn (0x48ce70, def +0x280)
    FVec boxMins{}, boxMaxs{};          // +0x14 / +0x20: the element's `min` / `max` (def +0xa0 / +0xac, 0x48cb00)
    std::string impactFx;               // +0x2c: one `impactfx` entry picked at spawn (0x48cb00, def +0x70 / +0x74)
    std::string deathFx;                // +0x30: one `deathfx` entry picked at spawn (0x48cb00, def +0x78 / +0x7c)
    // emitFx (0x100) state of an Emitter (0x483810): the emitter's own ballistic point P (+0x24c, no collision), the attachment
    // drift offset (+0x264), the time of the last emission (+0x270), the distance to the next one (+0x274), the effect
    // (+0x290, from `emitfx`) and `density` / `variance` drawn at spawn (+0x294 / +0x298).
    FVec emitP{}, emitOff{};
    int64_t emitLastMs = 0;
    float emitDist = 0, density = 0, variance = 0;
    std::string emitFx;
};

// A model instance of an Emitter element with useModel, to be drawn by the host as a static xmodel.
struct FxModelInst {
    std::string model;
    FVec pos{}, angles{};
    float scale = 1.0f;
    // Relative Emitters (0x483719 / 0x483742): world axes = the tag-space rows (forward, left, up from the angles, 0x483e50)
    // turned by the attachment orientation (0x440540); the host uses them instead of `angles` when set.
    bool hasAxes = false;
    FxAxes axes{};
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
    // Particle quad (renderer type 4, 1.3 DLL 0x1003ff20 -> 0x1003fb00): half width `size` (+0x64), half height `size2`
    // (+0x68; < 0 = size) and the angle in degrees (+0x6c, element +0x44).
    float size2 = -1;
    float rotation = 0;
    // OrientedParticle (renderer type 7, 1.3 DLL 0x1003ffe0): the quad lies in the plane of the two vectors 0x10059bd0 builds from
    // the element direction (refEntity +0x14 = element +0x48) instead of facing the view.
    bool oriented = false;
    FVec orientDir{};
    // Tracer beam (0x4b7d90): when set, a quad B +/- perp * widthB, A -/+ perp * widthA with u 0 at A and 1 at B, v across;
    // `position` / `size` are unused.
    bool beam = false;
    // Tail element (renderer type 8, 0x10041470 / quad builder 0x100409c0): the perpendicular is normalize((A - cam) x (B - cam)),
    // both ends are widened by widthA, and the atlas rectangle uv runs s across the width, t from A to B.
    bool beamTail = false;
    FVec beamA{}, beamB{}, beamPerp{};
    float widthA = 0, widthB = 0;
    // Cylinder element (renderer type 9, 1.3 DLL 0x100401f0): a tube of `segments` quads from beamA (radius widthA = size2
    // result, refEntity +0x68) to beamB (radius widthB = size, +0x64); see iw2view spriteModel for the ring build.
    bool cylinder = false;
    int segments = 0;
};

// A Cloud element as handed to the renderer (draw slot 0x482960 -> 0x480010 with renderer type 6 -> 1.3 DLL 0x10041370):
// the world matrix of the shared 1024-point cube (0x1005a7a0: rows axis[k] * size, translation origin), the flake quad
// width / height (refEntity +0x64 / +0x68) and the previous position B (+0x48) that stretches the flakes into streaks
// (0x10041230), colour = the rgb / alpha bytes / 255 (particleCloudColor).
struct FxCloud {
    std::string shader;
    FVec origin{}, prev{};
    FVec axis[3]{};
    float size = 0, width = 0, height = 0;
    uint8_t rgba[4]{255, 255, 255, 255};
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
    // from `spawnFrustumCullRadius` (+0x2a0, default 0). The near plane is not modelled; the far plane is `farDist` below.
    std::vector<std::array<float, 4>> viewPlanes;
    // The far plane (renderer farPlaneDistance, 0 = none): culls when forward·(p - viewOrigin) > farDist + radius. It is the sixth plane of
    // the exe's list (the near plane is not modelled); the draw culls drop it for elements with disableFarPlaneCulling (0x2000000),
    // the spawn gate keeps it (it uses the previous frame's set). Set by the host each frame.
    FVec viewForward{};
    float farDist = 0.0f;
    bool beyondFar(const FVec& p, float radius) const {
        if (!viewValid || farDist <= 0.0f) return false;
        return viewForward[0] * (p[0] - viewOrigin[0]) + viewForward[1] * (p[1] - viewOrigin[1]) + viewForward[2] * (p[2] - viewOrigin[2]) >
               farDist + radius;
    }
    // Bullet tracers (0x4b8140 -> 0x4b7b60, entity drawn by 0x4c0d98 -> 0x4b7d90): rolls cg_tracerchance (0.4), then a
    // local entity moves from `from` towards `to` at cg_tracerSpeed (5000) for length / speed seconds and is drawn as a
    // streak of up to cg_tracerlength (300) units ahead of its position. frameMs = cg frame time (0xf708d4; the start is
    // moved back by a random part of half of it, 0 = no jitter).
    void addTracer(const FVec& from, const FVec& to, int64_t nowMs, int frameMs = 0);
    // Right / up of the view for the beam's facing (0xf79168 / 0xf79174; only the sign of the quad's perpendicular depends on them).
    FVec viewRight{1, 0, 0}, viewUp{0, 0, 1};
    // Drops finished particles and returns the visible ones at nowMs.
    std::vector<FxSprite> sprites(int64_t nowMs);
    // World trace used by usePhysics (0x20) elements (0x486f70 -> CM_BoxTrace 0x417640, model 0, contents 1): segment a -> b
    // with the box mins / maxs (zero without useBBox), true on a hit (fraction < 1) that is neither start- nor all-solid,
    // giving the fraction and the surface normal. Set by the host.
    std::function<bool(const FVec& a, const FVec& b, const FVec& mins, const FVec& maxs, float& frac, FVec& normal)> trace;
    // Emitter models at the time of the last sprites() call.
    const std::vector<FxModelInst>& models() const { return models_; }
    // Light elements alive at the last sprites() call (DLL keeps at most r_dlightLimit = 4 of them, TODO(spec): the order
    // 0x10015180 chooses when there are more).
    const std::vector<FxLight>& lights() const { return lights_; }
    // Cloud elements alive at the last sprites() call.
    const std::vector<FxCloud>& clouds() const { return clouds_; }
    size_t count() const { return particles_.size(); }
    // The sight blockers at the time of the last sprites() call (rebuilt there, like 0x48b000 after the update).
    const std::vector<FxSightBlocker>& sightBlockers() const { return sightBlockers_; }

private:
    uint32_t& seed_ = assets::fxGeneratorSeed(); // the exe's single effect generator (0x59b2a8)
    float frand();
    int crtRand();                      // MSVC CRT rand() (0x553d5d), used by emitFx distances
    uint32_t crtSeed_ = 1;              // CRT default seed (the exe reseeds it with timeGetTime(), 0x40e149)
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
    struct ImpactRun {
        std::string path;
        FVec origin{}, dir{};
        int64_t startMs = 0;
    };
    std::vector<ImpactRun> impactQueue_;
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
    std::vector<FxCloud> clouds_;
    FVec velocityAt(const FxParticle& p, float u, const FxTag* orient) const;
    bool stepEmitter(FxParticle& p, int64_t nowMs);
    void deathEffect(const FxParticle& p, bool expired);
    bool physicsStep(FxParticle& p, int64_t frameMs, const FxTag* orient);
    void emitFx(FxParticle& p, int64_t frameMs, const FxTag* orient, const FVec& drift);
    // Effects started by element updates (impactFx 0x485220, emitFx 0x483810), played after the update loop.
    struct DeferredPlay {
        std::string path;
        FVec origin{};
        FxAxes axes{};
        std::shared_ptr<FxBolt> bolt; // set: bolted play (origin / axes from the tag; origin is only the range gate position)
    };
    std::vector<DeferredPlay> deferred_;
    const FVec* gate_ = nullptr; // spawnRange / cullrange position of a bolted play when it differs from the tag (0x4854ea)
};

} // namespace iw2::game
