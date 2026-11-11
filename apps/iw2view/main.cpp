// iw2view: fly through a single-player map with its textures and lightmaps (SDL3 GPU / Metal).
//
//   iw2view <main-dir> <map-name> [--shot out.png] [--pos x,y,z] [--yaw deg] [--pitch deg]
//           [--size WxH] [--lm-scale s] [--lm-weights a,b,c,d] [--sim-seconds s] [--use]
//
// Controls: the game's own bindings from default.cfg (W/A/S/D move, mouse look, MOUSE1 fire, MOUSE2 toggleads,
// R reload, F use, SHIFT melee / hold breath, G or MOUSE3 frag, 4 smoke, 1/2 next weapon, SPACE stand up / jump,
// C crouch, CTRL prone, Q/E lean). Viewer keys: V toggles walk/fly (fly: Space up, C down, Shift faster),
// L fullbright, P prints the camera position, Esc quits.

#include "assets/d3dbsp.h"
#include "assets/font.h"
#include "assets/iwi.h"
#include "assets/lightgrid.h"
#include "assets/material.h"
#include "assets/materialstate.h"
#include "assets/xanim.h"
#include "assets/xmodel.h"
#include "fs/vfs.h"
#include "fs/gamedir.h"
#include "audio/sound.h"
#include "game/itempickup.h"
#include "game/mpbots.h"
#include "game/clienttime.h"
#include "game/linkview.h"
#include "game/screenfade.h"
#include "video/roq.h"
#include "game/compassspring.h"
#include "game/hudlayers.h"
#include "game/grenadeindicator.h"
#include "game/framescale.h"
#include "game/hudfade.h"
#include "game/thirdpersonview.h"
#include "game/textmessage.h"
#include "game/mpchat.h"
#include "game/mphud.h"
#include "game/mpscoreboard.h"
#include "audio/clientsound.h"
#include "game/cursorhint.h"
#include "game/mpspectate.h"
#include "game/clientinput.h"
#include "game/collision.h"
#include "game/playerweapon.h"
#include "game/screenmessages.h"
#include "game/shellshock.h"
#include "game/gammaramp.h"
#include "game/glow_filter.h"
#include "game/screen_blur.h"
#include "game/viewweapon.h"
#include "game/viewflash.h"
#include "game/watersim.h"
#include "game/fx.h"
#include "game/fog.h"
#include "game/viewcull.h"
#include "game/viewspace.h"
#include "game/marks.h"
#include "game/outdoor.h"
#include "ui/focuspulse.h"
#include "game/viewprojection.h"
#include "game/weaponcycle.h"
#include "game/pmove.h"
#include "game/pmoverecord.h"
#include "game/turrets.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "netgame/clienterror.h"
#include "netgame/ambientconfig.h"
#include "netgame/consoleview.h"
#include "netgame/dedicated.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"
#include "net/server.h"
#include "net/lanbrowser.h"
#include "net/resolve.h"
#include "net/oob.h"
#include "netgame/netmove.h"
#include "ui/menu.h"
#include "ui/iw2options.h"
#include "ui/menurun.h"
#include "game/maprotation.h"
#include "ui/mpserver.h"
#include "ui/frontend.h"
#include "ui/menuhide.h"
#include "ui/esckey.h"
#include "ui/profile.h"
#include "ui/profilelist.h"
#include "game/savestate.h"
#include "ui/savegame.h"
#include <filesystem>
#include <fstream>
#include "util/codepage.h"
#include "util/png.h"
#include "util/screenshot.h"
#include "jpeg.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <iomanip>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace iw2;

namespace {

const char kShaderSource[] =
#include "shaders.metal.inc"
    ;

[[noreturn]] void sdlFail(const std::string& what) { throw std::runtime_error(what + ": " + SDL_GetError()); }

// ---------------------------------------------------------------------------------------------
// Math (column-major 4x4)

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// AngleDelta (0x423f50): a − b wrapped to [−180, 180].
float angleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}

using Mat4 = std::array<float, 16>;
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + rr)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + rr)] = s;
        }
    return r;
}

struct Camera {
    Vec3 pos;
    float yaw = 0, pitch = 0; // degrees; yaw 0 looks along +X, pitch positive looks down (game convention)
    float roll = 0;           // degrees; positive tilts the right vector down (AngleVectors)
    float fovX43 = 80.0f;     // cg_fov: horizontal field of view of a 4:3 screen (default 80, 0x4a10f0)

    Vec3 forward() const {
        const float cy = std::cos(yaw * float(M_PI) / 180), sy = std::sin(yaw * float(M_PI) / 180);
        const float cp = std::cos(pitch * float(M_PI) / 180), sp = std::sin(pitch * float(M_PI) / 180);
        return {cy * cp, sy * cp, -sp};
    }
    Vec3 right() const { return normalize(cross(forward(), {0, 0, 1})); }

    Mat4 viewProj(float aspect, float zNear = 4.0f, bool depthHack = false) const {
        const Vec3 f = forward(), r0 = right(), u0 = cross(r0, f);
        const float cr = std::cos(roll * float(M_PI) / 180), sr = std::sin(roll * float(M_PI) / 180);
        const Vec3 r = r0 * cr + u0 * -sr, u = cross(r, f);
        const Mat4 view = {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0, r.z, u.z, -f.z, 0,
                           -dot(r, pos), -dot(u, pos), dot(f, pos), 1};
        const float fovY = 2.0f * std::atan(std::tan(fovX43 * float(M_PI) / 360.0f) * 0.75f);
        const float t = 1.0f / std::tan(fovY / 2);
        const Mat4 proj = depthHack ? game::depthHackProjection(t / aspect, t, zNear) : game::sceneProjection(t / aspect, t, zNear);
        return mul(proj, view);
    }
};

// The renderer far plane (farPlaneDistance: linear fog far or setculldist, whichever is larger; r_zfar is taken as 0, its default is
// not decoded) as a depth-along-view test for the effect culls. The projection itself has no far plane.
static void setFxFar(game::FxSystem& fx, const Camera& cam, const game::FogParams& fog, float cullDist) {
    const Vec3 f = cam.forward();
    fx.viewForward = {f.x, f.y, f.z};
    fx.farDist = game::farPlaneDistance(fog.active && !fog.exponential, fog.farDist, 0.0f, cullDist);
}

// The four side planes of the view frustum for the effect gate frustumCull (0x486e90): inward normals through the eye;
// the vertical field of view is fixed by fovX43 at 4:3 (viewProj above), the horizontal one follows the aspect.
static void setFxFrustum(game::FxSystem& fx, const Camera& cam, float aspect) {
    const Vec3 f = cam.forward(), r = cam.right(), u = cross(r, f);
    const float ty = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) * 0.75f, tx = ty * aspect;
    auto plane = [&](Vec3 n) {
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        n = {n.x / l, n.y / l, n.z / l};
        return std::array<float, 4>{n.x, n.y, n.z, n.x * cam.pos.x + n.y * cam.pos.y + n.z * cam.pos.z};
    };
    auto comb = [&](float a, const Vec3& v, float b, const Vec3& w) { return Vec3{a * v.x + b * w.x, a * v.y + b * w.y, a * v.z + b * w.z}; };
    fx.viewPlanes = {plane(comb(tx, f, 1, r)), plane(comb(tx, f, -1, r)), plane(comb(ty, f, -1, u)), plane(comb(ty, f, 1, u))};
}


// ---------------------------------------------------------------------------------------------
// GPU upload helper: stages data in one transfer buffer and flushes when it is full.

class Uploader {
public:
    // wait = false: per-frame uploads. The copy pass is submitted without waiting for its fence (the queue runs
    // command buffers in submission order, and the staging buffer is cycled by the map call).
    explicit Uploader(SDL_GPUDevice* dev, bool wait = true) : dev_(dev), wait_(wait) {}
    ~Uploader() { flush(); }

    // Releases the pooled staging buffers (before the device is destroyed).
    static void releasePool(SDL_GPUDevice* dev) {
        auto& pool = freeBuffers();
        for (auto it = pool.begin(); it != pool.end();) {
            if (it->first == dev) {
                SDL_ReleaseGPUTransferBuffer(dev, it->second);
                it = pool.erase(it);
            } else
                ++it;
        }
    }

    void buffer(SDL_GPUBuffer* dst, const void* data, uint32_t size) {
        uint32_t done = 0;
        while (done < size) {
            const uint32_t chunk = std::min(size - done, kSize);
            const uint32_t off = reserve(chunk);
            std::memcpy(mapped_ + off, static_cast<const uint8_t*>(data) + done, chunk);
            SDL_GPUTransferBufferLocation src{tb_, off};
            SDL_GPUBufferRegion region{dst, done, chunk};
            SDL_UploadToGPUBuffer(pass(), &src, &region, false);
            done += chunk;
        }
    }

    void texture(SDL_GPUTexture* dst, uint32_t level, uint32_t layer, uint32_t w, uint32_t h, const void* data, uint32_t size) {
        if (size > kSize) throw std::runtime_error("texture level too large for staging buffer");
        const uint32_t off = reserve(size);
        std::memcpy(mapped_ + off, data, size);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb_;
        src.offset = off;
        SDL_GPUTextureRegion region{};
        region.texture = dst;
        region.mip_level = level;
        region.layer = layer;
        region.w = w;
        region.h = h;
        region.d = 1;
        SDL_UploadToGPUTexture(pass(), &src, &region, false);
    }

    void flush() {
        if (!cmd_) return;
        SDL_UnmapGPUTransferBuffer(dev_, tb_);
        SDL_EndGPUCopyPass(copy_);
        if (wait_) {
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
            SDL_WaitForGPUFences(dev_, true, &fence, 1);
            SDL_ReleaseGPUFence(dev_, fence);
        } else {
            SDL_SubmitGPUCommandBuffer(cmd_);
        }
        freeBuffers().emplace_back(dev_, tb_);
        tb_ = nullptr;
        cmd_ = nullptr;
        copy_ = nullptr;
        mapped_ = nullptr;
        used_ = 0;
    }

private:
    static constexpr uint32_t kSize = 64u << 20;

    // Staging buffers are pooled per device: creating a 64 MB buffer for every Uploader (several per frame) was costly.
    static std::vector<std::pair<SDL_GPUDevice*, SDL_GPUTransferBuffer*>>& freeBuffers() {
        static std::vector<std::pair<SDL_GPUDevice*, SDL_GPUTransferBuffer*>> pool;
        return pool;
    }
    void acquireBuffer() {
        auto& pool = freeBuffers();
        for (auto it = pool.begin(); it != pool.end(); ++it)
            if (it->first == dev_) {
                tb_ = it->second;
                pool.erase(it);
                return;
            }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = kSize;
        tb_ = SDL_CreateGPUTransferBuffer(dev_, &info);
        if (!tb_) sdlFail("SDL_CreateGPUTransferBuffer");
    }

    uint32_t reserve(uint32_t size) {
        uint32_t aligned = (used_ + 255u) & ~255u;
        if (cmd_ && aligned + size > kSize) {
            flush();
            aligned = 0;
        }
        if (!cmd_) {
            acquireBuffer();
            mapped_ = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, tb_, true));
            cmd_ = SDL_AcquireGPUCommandBuffer(dev_);
            copy_ = SDL_BeginGPUCopyPass(cmd_);
        }
        used_ = aligned + size;
        return aligned;
    }
    SDL_GPUCopyPass* pass() { return copy_; }

    SDL_GPUDevice* dev_;
    bool wait_ = true;
    SDL_GPUTransferBuffer* tb_ = nullptr;
    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPUCopyPass* copy_ = nullptr;
    uint8_t* mapped_ = nullptr;
    uint32_t used_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Scene

// r_polygonOffsetScale / r_polygonOffsetBias defaults (1.3 renderer dvars registered at 0x1000adaa / 0x1000adce).
constexpr float kPolygonOffsetScale = -1.0f, kPolygonOffsetBias = -1.0f;
constexpr SDL_GPUFrontFace kFrontFace = SDL_GPU_FRONTFACE_CLOCKWISE;

struct GpuVertex {
    float pos[3];
    float normal[3];
    uint8_t color[4];
    float uv[2];
    float lmUv[2];
    float light[3];
    float tangent[3];  // lib/normalmap.hlsl frame, read by the l_point pass
    float binormal[3];
};
static_assert(sizeof(GpuVertex) == 80);

enum class Pass { Opaque, AlphaTest, Blend, Additive, Multiply, Sky, Skip, Water, Objective };
// Grouping of world surfaces into batches; the last six fields are the draw-list fields of Batch.
using BatchKey = std::tuple<int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int>;

struct Batch {
    Pass pass = Pass::Opaque;
    int texture = 0;   // index into Scene::textures
    int lightmap = -1; // -1 = none, -2 = per-vertex light from the light grid
    bool fullbright = false;
    int polygonOffset = 0; // mtlPolygonOffset level 0..2 (material state bits)
    int cull = 0;          // mtlCullFace: 0 none, 1 back, 2 front
    uint32_t firstIndex = 0, indexCount = 0;
    bool scroll = false; // see MatInfo::scroll
    // *_spec techniques (lmap_s: lib/phong.hlsl): indices into Scene::textures of the normalMap / specularMap, -1 = none.
    int normalTex = -1, specTex = -1;
    uint8_t normalState = 0, specState = 0; // MaterialTexture::samplerState of the two maps
    int pointNormalTex = -1; // normalMap of the l_point techniques, -1 for the flat default
    uint8_t pointNormalState = 0;
    uint8_t colorState = 0; // MaterialTexture::samplerState of the colorMap
    assets::AlphaFunc alphaFunc = assets::AlphaFunc::None; // see MatInfo::alphaFunc
    int detail = -1; // Scene::details index
    bool alphaOne = false; // see MatInfo::alphaOne
    int fxKind = 0;        // see MatInfo::fxKind
    float fxParm[2] = {0, 0};
    int sort = 0, materialOrder = 0; // see MatInfo
    bool emissive = false, multiplyFog = false, pointLit = false, pointBlend = false; // see MatInfo
};

struct Lightmap {
    SDL_GPUTexture* coeffs = nullptr; // 3-layer array (R, G, B coefficients)
    SDL_GPUTexture* sun = nullptr;
};

struct MatInfo {
    Pass pass;
    int texture;
    // Flag technique set: vertex alpha is the wave weight, drawn opaque (assets::drawnVertexColor).
    bool flag = false;
    // mtlPolygonOffset: bits 4–5 of the material's second state word (file offset 0x30; statemap variable table of
    // the 1.3 renderer at 0x1018fe20: mask 0x30 of loadBits[1], values "0" / "1" / "2" = 0 / 0x10 / 0x20).
    int polygonOffset = 0;
    // mtlCullFace: bits 14–15 of state word 0 (file offset 0x2c; mask 0xc000, values None / Back / Front = 0x4000 /
    // 0x8000 / 0xc000, 1.3 renderer table 0x1018fa10), set as D3DRS_CULLMODE through {0, NONE, CCW, CW} (0x10190704,
    // 0x1003cb13). Here 0 none, 1 back (D3DCULL_CCW), 2 front (D3DCULL_CW).
    int cull = 0;
    // For Pass::Water `texture` indexes Scene::waters.
    bool scroll = false; // techset `*_scroll` (the scroll technique subtracts the game-time value from the V coordinate)
    uint8_t normalState = 0, specState = 0; // MaterialTexture::samplerState
    int pointNormalTex = -1; // see Batch
    uint8_t pointNormalState = 0;
    uint8_t colorState = 0; // MaterialTexture::samplerState of the colorMap
    assets::AlphaFunc alphaFunc = assets::AlphaFunc::None; // mtlAlphaTest (state word 0), docs/spec/material.md "Alpha test"
    int detail = -1; // Scene::details index (technique set *_detail)
    // Effect techniques that read the post-sun scene copy / $floatz (docs/spec/effects.md "Sprite vertices, $floatz and the
    // post-sun copy"): 1 = effect_zfeather* (zfeather.tech, fxParm[0] = featherParms.x), 2 = distortion (distortion_floatz.tech,
    // fxParm = distortionScale.xy). Both are material constants (constant table at file offset 0x40).
    int fxKind = 0;
    float fxParm[2] = {0, 0};
    int normalTex = -1, specTex = -1; // techset `phong_*_spec*`: normalMap / specularMap images (Scene::textures indices)
    bool waterBlend = false; // mtlBlendOp != Disable (bits 8-10 of state word 0: 0 Disable, 1 Add; inferred, see nofog.sm)
    // The pixel shader writes alpha 1 instead of texture × vertex alpha: the phong_* / ambient_* replace technique sets run the
    // lmap / lprobe / lprobe_sm_ambient shaders with USE_ALPHA 0 (materials/shaders/lib/lightmap.hlsl, lightprobe.hlsl:
    // "fragment.color.a = 1.0f"), and the nofog / fogged state maps write that alpha unblended for mtlBlendOp Disable
    // (separateAlphaBlendFunc Disable, One, Zero). Some models carry vertex alpha 0 (viewmodel_tokarev, crates), which the
    // glow pass would otherwise read as sky (fs_glow_setup nonSky = saturate(4 · alpha)) and bleed the sky colour over.
    bool alphaOne = false;
    int sort = 0;        // draw sort byte (material file offset 0x0d)
    int materialOrder = 0; // assets::materialOrder of the first use of the technique set and of the material
    bool emissive = false;    // effect* technique sets: no lit technique, so drawn in the emissive walk after the post-sun copy
    bool multiplyFog = false; // effect_multiply: the fogged techniques add the multiply_fog pass (effect_multiply_nofog has none)
    bool pointLit = false;    // the technique set has a "pointlight no fog" technique
    bool pointBlend = false;  // ... which is l_point_blend (light scaled by the surface alpha)
};

// water.techset / water_l_sun.tech (lib/water.hlsl): waterColor constant and the cubicEnvMap reflection cube.
struct WaterMat {
    SDL_GPUTexture* cube = nullptr;
    float color[3] = {0, 0, 0};
    SDL_GPUTexture* normalMap = nullptr;       // FFT map, updated every frame (semantic-5 texture entry)
    std::shared_ptr<game::WaterSim> sim;
    bool blend = false; // mtlBlendOp != Disable: SrcAlpha/InvSrcAlpha, no depth write (nofog.sm); else One/Zero + depth write
};

// A model uploaded for dynamic (entity) drawing, in model space.
struct GpuModel {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    std::vector<Batch> batches;
};

struct Scene {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    SDL_GPUBuffer* tbn = nullptr;           // world vertices' tangent + binormal (6 floats each), parallel to `vertices`
    SDL_GPUTexture* lmWeightTex = nullptr;  // "$lightmapweights" (32 x 32 RGBA8): (alpha, green) of the normal map -> 4 lightmap coefficient weights
    SDL_GPUTexture* specularity = nullptr;  // "$specularity" lookup (32 x 256 L8): U = specular power, V = N.H
    std::vector<SDL_GPUTexture*> textures; // [0] = white
    struct Detail { int tex; uint8_t state; float scale[2]; }; // material detailMap + detailScale.xy (lib/shade.hlsl USE_DETAIL)
    std::vector<Detail> details;
    std::vector<Lightmap> lightmaps;
    Lightmap dummyLightmap;
    std::vector<Batch> batches;
    SDL_GPUTexture* sky = nullptr;
    std::vector<WaterMat> waters;
    std::unique_ptr<game::CollisionWorld> collision;
    game::MarkSystem marks;
    game::OutdoorMap outdoor; // "$outdoor" (game/outdoor.h), built on first use by a Cloud with particle_cloud_outdoor
    std::unique_ptr<assets::LightGrid> grid;
    std::unordered_map<std::string, int> imageIndex;
    std::unordered_map<std::string, MatInfo> matCache;
    std::map<std::string, int> techsetRanks; // technique set name -> first-use order
    int materialsSeen = 0;                   // materials registered so far
    std::unordered_map<std::string, std::optional<GpuModel>> models;
    // Brush submodels "*N" of script_brushmodel entities (lump 35 index → batches into `vertices` / `indices`, in model
    // space), drawn with the world shaders per entity at its origin/angles while the entity is not hidden
    // (docs/spec/d3dbsp.md "Brush models"). Empty without a level (then they are baked into `batches`).
    std::unordered_map<int, std::vector<Batch>> brushBatches;
    std::unordered_map<int, std::array<float, 6>> brushBounds; // per brush model: local mins xyz, maxs xyz of its drawn soups' vertices
    // CPU-side models for skinned (animated) drawing.
    struct CpuModel {
        assets::XModelParts parts;
        std::vector<assets::XSurface> surfaces;
        std::vector<MatInfo> materials;
    };
    std::unordered_map<std::string, std::optional<CpuModel>> cpuModels;
    // Per (entity, model) GPU buffers re-filled every frame with skinned vertices.
    std::map<std::pair<int, std::string>, GpuModel> skinned;
    Vec3 sunDirection{0, 0, 1};
    Vec3 sunColor{1, 1, 1};
    Camera start;
};

// L8 height map with mips (the r_rendererInUse = dx9 path 0x1002cbd0 + 0x1002cc70, format D3DFMT_L8).
void uploadWaterMap(Uploader& up, WaterMat& w, float seconds) {
    std::vector<std::vector<uint8_t>> levels;
    w.sim->updateL8(seconds, levels);
    uint32_t dim = uint32_t(w.sim->size());
    for (size_t l = 0; l < levels.size(); ++l, dim = std::max(1u, dim / 2))
        up.texture(w.normalMap, uint32_t(l), 0, dim, dim, levels[l].data(), uint32_t(levels[l].size()));
}

// Regenerates every water normal map for the given time in seconds (1002cd30: once per frame, r_drawWater on).
void updateWaterMaps(Scene& scene, SDL_GPUDevice* dev, float seconds) {
    if (scene.waters.empty()) return;
    Uploader up(dev, false);
    for (auto& w : scene.waters)
        if (w.sim && w.normalMap) uploadWaterMap(up, w, seconds);
}

Pass classify(const std::string& techset) {
    if (techset == "sky") return Pass::Sky;
    if (techset == "water") return Pass::Water;
    if (techset == "tools" || techset.rfind("tools", 0) == 0) return Pass::Skip;
    // Technique set "distortion" (iw_07.iwd materials/techniques/distortion.tech, shaders/lib/distortion.hlsl): the colorMap is a
    // du/dv map that only offsets a lookup of the resolved scene (sampler.resolvedPostSun); the output is the scene sample times
    // the vertex colour, alpha = vertex alpha · map alpha. Its texels are never shown as colours (drawn as a sprite they gave the
    // green / red halo around fires, gfx_distortion_heat). The scene-copy refraction pass is implemented for effect sprites (below).
    // What the shipped files fix: distortion.techset maps the emissive variants to distortion.tech (stateMap nofog: the
    // material's blend) and the feathered ones to distortion_floatz.tech; the vertex shader offsets the projected position by
    // du·T' + dv·B' with T' = distortionScale.x · (1,-1,1,1) · clipSpaceLookupScale · clip(tangent), B' likewise with
    // -distortionScale.y and the binormal (non-float-z: map in [0,1], start shifted by -(T' + B'), T' and B' doubled).
    // Found in the 1.3 DLL: clipSpaceLookupScale = (0.5 w / W, -0.5 h / H, 0, 1) and
    // clipSpaceLookupOffset = ((x + w / 2 + 0.5) / W, (y + h / 2 + 0.5) / H, 0, 0) for the viewport (x, y, w, h) in a W x H
    // target (0x1003eae0, constants at 0x1070c2e0 / 0x1070c2f0); sampler resolvedPostSun (code sampler 0xe, switch
    // 0x10038dd0) is the render target 0x1070eb54, which render command 0x100310a0 sets by a StretchRect of the back buffer
    // (0x1002eeb0) when r_distortion (default 1) or the glow chain needs it, else "none" (0xe).
    // Frame order (scene builder 0x100254a9..0x10025671; commands dispatch through the table at 0x10190518, 0x1003417c): draw
    // lists with technique type 1 "sky" and 6 "lightmap no fog" (0x10021cf0), then 0x10021d80, then the post-sun resolve
    // (command 0x19 = 0x100310a0, emitted by 0x10022510 at 0x100255b9 when the view flag +4 is set), later the effects with
    // technique type 0x18 "emissive feathered no fog" when r_zFeather (0x101ce694, default 1) is on and $floatz (0x101d489c)
    // exists, else 0x15 "emissive no fog" (types: name table built at 0x1001c0d1..). So distortion samples the opaque scene
    // and, with the default r_zFeather, runs distortion_floatz.tech, which needs the float-z target.
    // Sprite vertices (docs/spec/effects.md "Sprite vertices, $floatz and the post-sun copy"): tangent = -(half width vector),
    // binormal = half height vector (billboard: -width * view row 1, height * view row 2). The post-sun copy only happens when the
    // scene viewport equals the window. $floatz = R32F target of clip z built by technique type 2 "build floatz" (sky: max extent).
    // Implemented in Renderer::draw (post-sun copy + $floatz from the depth buffer) and fs_distort; as a world / model material it
    // stays skipped (only effect sprites carry the tangent / binormal); sprites keep it through MatInfo::fxKind 2.
    if (techset == "distortion") return Pass::Skip;
    // Technique set "objective" (objective.techset: every lit variant -> objective_base.tech, stateMap objective.sm): the glowing
    // twin of an objective model (weapon_tt30_obj, military_tntbomb_obj, ...), added over the scene, never opaque.
    if (techset == "objective") return Pass::Objective;
    if (techset.find("_add") != std::string::npos) return Pass::Additive;
    // effect_multiply -> vertcol_mul.tech -> mul.hlsl: multiplied into the framebuffer (DST_COLOR, ZERO), unlit.
    if (techset == "effect_multiply") return Pass::Multiply;
    if (techset.find("blend") != std::string::npos || techset.rfind("effect", 0) == 0) return Pass::Blend;
    if (techset.find("alphatest") != std::string::npos) return Pass::AlphaTest;
    return Pass::Opaque;
}

SDL_GPUTexture* createTexture(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h, uint32_t levels,
                              uint32_t layers = 1, SDL_GPUTextureType type = SDL_GPU_TEXTURETYPE_2D) {
    SDL_GPUTextureCreateInfo info{};
    info.type = type;
    info.format = fmt;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = layers;
    info.num_levels = levels;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &info);
    if (!t) sdlFail("SDL_CreateGPUTexture");
    return t;
}

SDL_GPUTexture* loadCubeImage(SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::string& name) {
    const auto data = vfs.read("images/" + name + ".iwi");
    if (!data) return nullptr;
    try {
        const auto img = assets::parseIwi(*data);
        if (img.faceCount != 6) return nullptr;
        const auto& l0 = img.levels[0];
        SDL_GPUTexture* tex = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, uint32_t(l0.width), uint32_t(l0.height), 1, 6,
                                            SDL_GPU_TEXTURETYPE_CUBE);
        for (int f = 0; f < 6; ++f) {
            const auto rgba = assets::decodeToRgba(img, 0, f);
            up.texture(tex, 0, uint32_t(f), uint32_t(l0.width), uint32_t(l0.height), rgba.data(), uint32_t(rgba.size()));
        }
        return tex;
    } catch (const std::exception& e) {
        std::cerr << "warning: sky image " << name << ": " << e.what() << "\n";
        return nullptr;
    }
}

SDL_GPUTexture* loadImage(SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::string& name) {
    const auto data = vfs.read("images/" + name + ".iwi");
    if (!data) return nullptr;
    try {
        const auto img = assets::parseIwi(*data);
        if (img.faceCount != 1) return nullptr;
        const auto f = img.header.format;
        const bool bc = f == assets::IwiFormat::DXT1 || f == assets::IwiFormat::DXT3 || f == assets::IwiFormat::DXT5;
        const bool bcOk = bc && img.header.width % 4 == 0 && img.header.height % 4 == 0;
        // Only keep mips that stay block-aligned for compressed formats.
        uint32_t levels = uint32_t(img.levels.size());
        if (bcOk)
            for (uint32_t l = 0; l < img.levels.size(); ++l)
                if (img.levels[l].width % 4 || img.levels[l].height % 4) { levels = std::max(1u, l); break; }

        SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if (bcOk) fmt = f == assets::IwiFormat::DXT1   ? SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM
                        : f == assets::IwiFormat::DXT3 ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM
                                                       : SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
        SDL_GPUTexture* tex = createTexture(dev, fmt, uint32_t(img.header.width), uint32_t(img.header.height), levels);
        for (uint32_t l = 0; l < levels; ++l) {
            const auto& lvl = img.levels[l];
            if (bcOk) {
                up.texture(tex, l, 0, uint32_t(lvl.width), uint32_t(lvl.height), lvl.faces[0].data(), uint32_t(lvl.faces[0].size()));
            } else {
                const auto rgba = assets::decodeToRgba(img, int(l));
                up.texture(tex, l, 0, uint32_t(lvl.width), uint32_t(lvl.height), rgba.data(), uint32_t(rgba.size()));
            }
        }
        return tex;
    } catch (const std::exception& e) {
        std::cerr << "warning: image " << name << ": " << e.what() << "\n";
        return nullptr;
    }
}

Vec3 parseVec3(const std::string& s) {
    Vec3 v;
    std::istringstream in(s);
    in >> v.x >> v.y >> v.z;
    return v;
}

MatInfo sceneMaterial(Scene& scene, SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::string& name) {
    if (auto it = scene.matCache.find(name); it != scene.matCache.end()) return it->second;
    MatInfo mi{Pass::Opaque, 0};
    if (const auto md = vfs.read("materials/" + name)) {
        if (md->size() > 0x30) {
            mi.polygonOffset = ((*md)[0x30] >> 4) & 3;
            const int cf = ((*md)[0x2d] >> 6) & 3; // bits 14–15 of the word at 0x2c
            mi.cull = cf == 2 ? 1 : cf == 3 ? 2 : 0;
            mi.waterBlend = (((*md)[0x2d]) & 7) != 0; // blendOp, bits 8-10
            mi.alphaFunc = assets::decodeAlphaFunc(uint32_t((*md)[0x2c]) | uint32_t((*md)[0x2d]) << 8);
        }
        try {
            const auto m = assets::parseMaterial(*md);
            mi.pass = classify(m.techniqueSet);
            mi.flag = assets::isFlagTechniqueSet(m.techniqueSet);
            mi.sort = md->size() > 0x0d ? (*md)[0x0d] : 0;
            mi.materialOrder = assets::materialOrder(scene.techsetRanks.try_emplace(m.techniqueSet, int(scene.techsetRanks.size())).first->second, scene.materialsSeen++);
            if (const auto ts = vfs.read("materials/techniquesets/" + m.techniqueSet + ".techset")) {
                const std::string text(ts->begin(), ts->end());
                mi.pointLit = text.find("\"pointlight no fog\"") != std::string::npos;
                mi.pointBlend = text.find("l_point_blend") != std::string::npos;
            }
            mi.emissive = m.techniqueSet.rfind("effect", 0) == 0;
            mi.multiplyFog = m.techniqueSet == "effect_multiply";
            // The state maps pass the blend through from the material, so it decides the pass, not the technique set name
            // (effect_multiply_nofog multiplies, nine unlit_blend materials add).
            if (md->size() > 0x30 && (mi.pass == Pass::Blend || mi.pass == Pass::Additive || mi.pass == Pass::Multiply)) {
                const uint32_t word0 = uint32_t((*md)[0x2c]) | uint32_t((*md)[0x2d]) << 8;
                const assets::MaterialBlend blend = assets::decodeMaterialBlend(word0);
                if (blend == assets::MaterialBlend::Additive) mi.pass = Pass::Additive;
                else if (blend == assets::MaterialBlend::Multiply) mi.pass = Pass::Multiply;
                else if (blend == assets::MaterialBlend::Alpha) mi.pass = Pass::Blend;
            }
            if (m.techniqueSet == "distortion" || m.techniqueSet.rfind("effect_zfeather", 0) == 0) {
                const bool dist = m.techniqueSet == "distortion";
                mi.fxKind = dist ? 2 : m.techniqueSet.rfind("effect_zfeather_outdoor", 0) == 0 ? 3 : 1;
                for (const auto& c : m.constants)
                    if (c.name == (dist ? "distortionScale" : "featherParms")) mi.fxParm[0] = c.value[0], mi.fxParm[1] = c.value[1];
            }
            if (mi.pass == Pass::Water) {
                WaterMat wm;
                for (const auto& t : m.textures)
                    if (t.samplerName == "cubicEnvMap") wm.cube = loadCubeImage(dev, up, vfs, t.imageName);
                for (const auto& t : m.textures)
                    if (t.semantic == assets::TextureSemantic::Water && t.water.size >= 2) {
                        game::WaterParams wp;
                        wp.size = t.water.size;
                        wp.lx = t.water.lx, wp.ly = t.water.ly, wp.amplitude = t.water.amplitude, wp.v = t.water.v;
                        wp.windX = t.water.windX, wp.windY = t.water.windY;
                        wm.sim = std::make_shared<game::WaterSim>(wp);
                        uint32_t lv = 1;
                        for (int d = wp.size; d > 1; d /= 2) ++lv;
                        wm.normalMap = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, uint32_t(wp.size), uint32_t(wp.size), lv);
                        uploadWaterMap(up, wm, 0.0f);
                    }
                for (const auto& c : m.constants)
                    if (c.name == "waterColor") wm.color[0] = c.value[0], wm.color[1] = c.value[1], wm.color[2] = c.value[2];
                wm.blend = mi.waterBlend;
                mi.texture = int(scene.waters.size());
                scene.waters.push_back(wm);
                scene.matCache.emplace(name, mi);
                return mi;
            }
            mi.scroll = m.techniqueSet.size() > 7 && m.techniqueSet.compare(m.techniqueSet.size() - 7, 7, "_scroll") == 0;
            mi.alphaOne = mi.pass == Pass::Opaque && (m.techniqueSet.rfind("phong_", 0) == 0 || m.techniqueSet.rfind("ambient_", 0) == 0);
            for (const auto& t : m.textures)
                if (t.samplerName == "colorMap") { mi.colorState = t.samplerState; break; }
            const std::string& img = m.colorImage();
            auto imageSlot = [&](const std::string& image) {
                auto it = scene.imageIndex.find(image);
                if (it == scene.imageIndex.end()) {
                    SDL_GPUTexture* t = nullptr;
                    if (image == "$black") { // built-in image: 1.3 DLL 0x10010920 fills "$black" with one colour (0, 0, 0, 0xff) via 0x10010170 ("$white": 255, 255, 255, 0xff); a 1x1 texture samples the same
                        const uint8_t black[4] = {0, 0, 0, 255};
                        t = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1);
                        up.texture(t, 0, 0, 1, 1, black, 4);
                    } else t = loadImage(dev, up, vfs, image);
                    int idx = 0;
                    if (t) {
                        idx = int(scene.textures.size());
                        scene.textures.push_back(t);
                    }
                    it = scene.imageIndex.emplace(image, idx).first;
                }
                return it->second;
            };
            if (mi.pass == Pass::Sky) {
                if (!scene.sky) scene.sky = loadCubeImage(dev, up, vfs, img);
            } else if (!img.empty() && (img == "$black" || (img[0] != '$' && (mi.pass != Pass::Skip || mi.fxKind == 2)))) { // the "black" material (technique set "tools") is a UI background
                mi.texture = imageSlot(img);
                // The l_point techniques of every phong_* techset sample material.normalMap; "$identitynormalmap" is the flat default.
                if (m.techniqueSet.rfind("phong_", 0) == 0)
                    for (const auto& t : m.textures)
                        if (t.semantic == assets::TextureSemantic::Normal && t.samplerName == "normalMap" && !t.imageName.empty() && t.imageName[0] != '$') {
                            mi.pointNormalTex = imageSlot(t.imageName);
                            mi.pointNormalState = t.samplerState;
                        }
                // phong_*_spec* techsets (lmap_s / lprobe_s): normalMap (semantic 3) + specularMap (semantic 4).
                if (m.techniqueSet.rfind("phong_", 0) == 0 && m.techniqueSet.find("_spec") != std::string::npos)
                    for (const auto& t : m.textures) {
                        if (t.imageName.empty() || t.imageName[0] == '$') continue;
                        if (t.semantic == assets::TextureSemantic::Normal && t.samplerName == "normalMap") { mi.normalTex = imageSlot(t.imageName); mi.normalState = t.samplerState; }
                        if (t.semantic == assets::TextureSemantic::Specular && t.samplerName == "specularMap") { mi.specTex = imageSlot(t.imageName); mi.specState = t.samplerState; }
                    }
                // phong_*_detail techsets (lmap_dtl / lmap_a_dtl / lprobe_dtl / l_point_dtl and the _s variants): detailMap + detailScale.
                if (m.techniqueSet.rfind("phong_", 0) == 0 && m.techniqueSet.find("_detail") != std::string::npos)
                    for (const auto& t : m.textures) {
                        if (t.imageName.empty() || t.imageName[0] == '$' || t.samplerName != "detailMap") continue;
                        Scene::Detail dt{imageSlot(t.imageName), t.samplerState, {1.0f, 1.0f}};
                        for (const auto& c : m.constants)
                            if (c.name == "detailScale") dt.scale[0] = c.value[0], dt.scale[1] = c.value[1];
                        mi.detail = int(scene.details.size());
                        scene.details.push_back(dt);
                    }
            }
        } catch (const std::exception& e) {
            std::cerr << "warning: material " << name << ": " << e.what() << "\n";
        }
    }
    scene.matCache.emplace(name, mi);
    return mi;
}

// Loads an xmodel's LOD 0 into GPU buffers for entity drawing (cached; nullptr if it cannot be loaded).
const GpuModel* sceneModel(Scene& scene, SDL_GPUDevice* dev, const fs::Vfs& vfs, const std::string& name) {
    if (auto it = scene.models.find(name); it != scene.models.end()) return it->second ? &*it->second : nullptr;
    auto& slot = scene.models[name];
    try {
        const auto md = vfs.read(name);
        if (!md) throw std::runtime_error("missing");
        const auto xm = assets::parseXModel(*md);
        const auto pd = vfs.read("xmodelparts/" + xm.lods.at(0).name);
        const auto sd = vfs.read("xmodelsurfs/" + xm.lods.at(0).name);
        if (!pd || !sd) throw std::runtime_error("missing parts or surfs");
        const auto surfs = assets::parseXModelSurfs(*sd, assets::parseXModelParts(*pd));
        Uploader up(dev);
        std::vector<GpuVertex> verts;
        std::vector<uint32_t> indices;
        GpuModel gm;
        for (size_t si = 0; si < surfs.size(); ++si) {
            const MatInfo mi = si < xm.lods[0].materials.size() ? sceneMaterial(scene, dev, up, vfs, xm.lods[0].materials[si]) : MatInfo{Pass::Opaque, 0};
            if (mi.pass == Pass::Skip || mi.pass == Pass::Sky) continue;
            const uint32_t base = uint32_t(verts.size());
            for (const auto& v : surfs[si].vertices) {
                GpuVertex g{};
                std::memcpy(g.pos, v.position.data(), 12);
                std::memcpy(g.normal, v.normal.data(), 12);
                std::memcpy(g.tangent, v.tangent.data(), 12);
                std::memcpy(g.binormal, v.binormal.data(), 12);
                { const auto c = assets::drawnVertexColor(mi.flag, v.color); std::memcpy(g.color, c.data(), 4); }
                g.uv[0] = v.uv[0];
                g.uv[1] = v.uv[1];
                g.light[0] = g.light[1] = g.light[2] = 1.0f;
                verts.push_back(g);
            }
            Batch b;
            b.pass = mi.pass;
            b.sort = mi.sort, b.materialOrder = mi.materialOrder, b.emissive = mi.emissive, b.pointLit = mi.pointLit, b.pointBlend = mi.pointBlend, b.pointNormalTex = mi.pointNormalTex, b.pointNormalState = mi.pointNormalState;
            b.texture = mi.texture;
            b.lightmap = -3;
            b.polygonOffset = mi.polygonOffset;
            b.cull = mi.cull;
        b.scroll = mi.scroll;
            b.detail = mi.detail;
            b.alphaOne = mi.alphaOne;
            b.colorState = mi.colorState;
            b.alphaFunc = mi.alphaFunc;
            b.firstIndex = uint32_t(indices.size());
            for (uint16_t i : surfs[si].indices) indices.push_back(base + i);
            b.indexCount = uint32_t(indices.size()) - b.firstIndex;
            gm.batches.push_back(b);
        }
        if (verts.empty()) throw std::runtime_error("no drawable surfaces");
        SDL_GPUBufferCreateInfo vb{};
        vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb.size = uint32_t(verts.size() * sizeof(GpuVertex));
        gm.vertices = SDL_CreateGPUBuffer(dev, &vb);
        SDL_GPUBufferCreateInfo ib{};
        ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib.size = uint32_t(indices.size() * 4);
        gm.indices = SDL_CreateGPUBuffer(dev, &ib);
        up.buffer(gm.vertices, verts.data(), vb.size);
        up.buffer(gm.indices, indices.data(), ib.size);
        up.flush();
        slot = std::move(gm);
        return &*slot;
    } catch (const std::exception& e) {
        std::cerr << "warning: model " << name << ": " << e.what() << "\n";
        slot.reset();
        return nullptr;
    }
}

const Scene::CpuModel* sceneCpuModel(Scene& scene, SDL_GPUDevice* dev, const fs::Vfs& vfs, const std::string& name) {
    if (auto it = scene.cpuModels.find(name); it != scene.cpuModels.end()) return it->second ? &*it->second : nullptr;
    auto& slot = scene.cpuModels[name];
    try {
        const auto md = vfs.read(name);
        if (!md) throw std::runtime_error("missing");
        const auto xm = assets::parseXModel(*md);
        const auto pd = vfs.read("xmodelparts/" + xm.lods.at(0).name);
        const auto sd = vfs.read("xmodelsurfs/" + xm.lods.at(0).name);
        if (!pd || !sd) throw std::runtime_error("missing parts or surfs");
        Scene::CpuModel cm;
        cm.parts = assets::parseXModelParts(*pd);
        cm.surfaces = assets::parseXModelSurfs(*sd, cm.parts);
        Uploader up(dev);
        for (const auto& mn : xm.lods[0].materials) cm.materials.push_back(sceneMaterial(scene, dev, up, vfs, mn));
        up.flush();
        slot = std::move(cm);
        return &*slot;
    } catch (const std::exception& e) {
        std::cerr << "warning: model " << name << ": " << e.what() << "\n";
        slot.reset();
        return nullptr;
    }
}

// Skins a CPU model with a pose into its per-entity GPU buffers; returns the model to draw.
const GpuModel* skinModel(Scene& scene, SDL_GPUDevice* dev, Uploader& up, int entity, const std::string& name,
                          const Scene::CpuModel& cm, const assets::XPose& pose) {
    auto key = std::make_pair(entity, name);
    auto it = scene.skinned.find(key);
    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    for (size_t si = 0; si < cm.surfaces.size(); ++si) {
        const MatInfo mi = si < cm.materials.size() ? cm.materials[si] : MatInfo{Pass::Opaque, 0};
        if (mi.pass == Pass::Skip || mi.pass == Pass::Sky) continue;
        const uint32_t base = uint32_t(verts.size());
        for (const auto& v : cm.surfaces[si].vertices) {
            GpuVertex g{};
            std::array<float, 3> p, n;
            assets::skinVertex(v, pose, p, n);
            std::memcpy(g.pos, p.data(), 12);
            std::memcpy(g.normal, n.data(), 12);
            std::array<float, 3> tangent, binormal;
            assets::skinTangents(v, pose, tangent, binormal);
            std::memcpy(g.tangent, tangent.data(), 12);
            std::memcpy(g.binormal, binormal.data(), 12);
            { const auto c = assets::drawnVertexColor(mi.flag, v.color); std::memcpy(g.color, c.data(), 4); }
            g.uv[0] = v.uv[0];
            g.uv[1] = v.uv[1];
            verts.push_back(g);
        }
        Batch b;
        b.pass = mi.pass;
        b.sort = mi.sort, b.materialOrder = mi.materialOrder, b.emissive = mi.emissive, b.pointLit = mi.pointLit, b.pointBlend = mi.pointBlend, b.pointNormalTex = mi.pointNormalTex, b.pointNormalState = mi.pointNormalState;
        b.texture = mi.texture;
        b.lightmap = -3;
        b.polygonOffset = mi.polygonOffset;
        b.cull = mi.cull;
        b.scroll = mi.scroll;
        b.detail = mi.detail;
        b.alphaOne = mi.alphaOne;
        b.colorState = mi.colorState;
        b.alphaFunc = mi.alphaFunc;
        b.firstIndex = uint32_t(indices.size());
        for (uint16_t i : cm.surfaces[si].indices) indices.push_back(base + i);
        b.indexCount = uint32_t(indices.size()) - b.firstIndex;
        batches.push_back(b);
    }
    if (verts.empty()) return nullptr;
    if (it == scene.skinned.end()) {
        GpuModel gm;
        SDL_GPUBufferCreateInfo vb{};
        vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb.size = uint32_t(verts.size() * sizeof(GpuVertex));
        gm.vertices = SDL_CreateGPUBuffer(dev, &vb);
        SDL_GPUBufferCreateInfo ib{};
        ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib.size = uint32_t(indices.size() * 4);
        gm.indices = SDL_CreateGPUBuffer(dev, &ib);
        up.buffer(gm.indices, indices.data(), ib.size);
        gm.batches = batches;
        it = scene.skinned.emplace(key, gm).first;
    }
    up.buffer(it->second.vertices, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
    return &it->second;
}

// Builds camera-facing quads for effect sprites into a per-frame model (size = half width; inferred).
const GpuModel* spriteModel(Scene& scene, SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::vector<game::FxSprite>& sprites,
                            const Vec3& right, const Vec3& upv);


const GpuModel* spriteModel(Scene& scene, SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::vector<game::FxSprite>& sprites,
                            const Vec3& right, const Vec3& upv) {
    if (sprites.empty()) return nullptr;
    constexpr uint32_t kMax = 16384;
    auto key = std::make_pair(-2, std::string("fx"));
    auto it = scene.skinned.find(key);
    if (it == scene.skinned.end()) {
        GpuModel gm;
        SDL_GPUBufferCreateInfo vb{};
        vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb.size = kMax * 4 * sizeof(GpuVertex);
        gm.vertices = SDL_CreateGPUBuffer(dev, &vb);
        SDL_GPUBufferCreateInfo ib{};
        ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib.size = kMax * 6 * 4;
        gm.indices = SDL_CreateGPUBuffer(dev, &ib);
        std::vector<uint32_t> idx;
        for (uint32_t q = 0; q < kMax; ++q)
            for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(q * 4 + k);
        up.buffer(gm.indices, idx.data(), uint32_t(idx.size() * 4));
        it = scene.skinned.emplace(key, gm).first;
    }
    // Group by shader so each batch binds one texture.
    std::map<std::string, std::vector<const game::FxSprite*>> byShader;
    for (const auto& s : sprites) byShader[s.shader].push_back(&s);
    std::vector<GpuVertex> verts;
    std::vector<Batch> batches;
    for (const auto& [shader, list] : byShader) {
        const MatInfo mi = sceneMaterial(scene, dev, up, vfs, shader);
        if ((mi.pass == Pass::Skip && mi.fxKind != 2) || mi.pass == Pass::Sky) continue;
        Batch b;
        b.pass = mi.pass == Pass::Opaque || mi.pass == Pass::AlphaTest || mi.fxKind == 2 ? Pass::Blend : mi.pass;
        b.sort = mi.sort, b.materialOrder = mi.materialOrder, b.emissive = mi.emissive, b.pointLit = mi.pointLit, b.pointBlend = mi.pointBlend, b.pointNormalTex = mi.pointNormalTex, b.pointNormalState = mi.pointNormalState;
        b.texture = mi.texture;
        b.colorState = mi.colorState;
        b.lightmap = -3;
        b.fullbright = true;
        b.fxKind = mi.fxKind;
        b.fxParm[0] = mi.fxParm[0], b.fxParm[1] = mi.fxParm[1];
        b.firstIndex = uint32_t(verts.size() / 4 * 6);
        for (const auto* s : list) {
            if (verts.size() / 4 >= kMax) break;
            if (s->cylinder) {
                // 1.3 DLL 0x100401f0: d = normalize(A − B); p1 = (d.z, −d.x, d.y) minus its d part, normalised; p2 = p1 × d
                // (0x10059bd0). Ring k (k = 0..n−1, (cos, sin) turned by 2π/n each step from (1, 0)): A + (p1·sin + p2·cos)·rA with
                // texcoord (t, 1) and B + (p1·sin + p2·cos)·rB with (t, 0), t += 1/n; a closing copy of pair 0 with t = 1;
                // triangles (b, b+1, b+3), (b+3, b+2, b) per step (drawn here as the quad b, b+1, b+3, b+2). Colour = the bytes.
                const Vec3 A{s->beamA[0], s->beamA[1], s->beamA[2]}, B{s->beamB[0], s->beamB[1], s->beamB[2]};
                Vec3 d = A + B * -1.0f;
                const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (dl != 0.0f) d = d * (1.0f / dl);
                Vec3 p1{d.z, -d.x, d.y};
                const float pd = -(d.x * p1.x + d.z * p1.z + p1.y * d.y);
                p1 = p1 + d * pd;
                const float pl = std::sqrt(p1.x * p1.x + p1.y * p1.y + p1.z * p1.z);
                if (pl != 0.0f) p1 = p1 * (1.0f / pl);
                const Vec3 p2{p1.y * d.z - p1.z * d.y, p1.z * d.x - d.z * p1.x, d.y * p1.x - p1.y * d.x};
                const int n = std::max(1, s->segments);
                const float stepT = 1.0f / float(n), ang = stepT * 6.2831855f;
                const float C = std::cos(ang), S = std::sin(ang);
                std::vector<std::pair<Vec3, Vec3>> ring; // (A side, B side)
                std::vector<float> ts;
                float co = 1.0f, si = 0.0f, t = 0.0f;
                for (int k = 0; k < n; ++k) {
                    ring.push_back({A + p1 * (si * s->widthA) + p2 * (co * s->widthA), B + p1 * (si * s->widthB) + p2 * (co * s->widthB)});
                    ts.push_back(t);
                    t += stepT;
                    const float ns = si * C + co * S, nc = co * C - si * S;
                    si = ns, co = nc;
                }
                ring.push_back(ring[0]);
                ts.push_back(1.0f);
                for (int k = 0; k < n && verts.size() / 4 < kMax; ++k) {
                    const Vec3 q[4] = {ring[size_t(k)].first, ring[size_t(k)].second, ring[size_t(k) + 1].second, ring[size_t(k) + 1].first};
                    const float quv[4][2] = {{ts[size_t(k)], 1}, {ts[size_t(k)], 0}, {ts[size_t(k) + 1], 0}, {ts[size_t(k) + 1], 1}};
                    for (int v = 0; v < 4; ++v) {
                        GpuVertex g{};
                        g.pos[0] = q[v].x; g.pos[1] = q[v].y; g.pos[2] = q[v].z;
                        g.normal[2] = 1;
                        for (int ch = 0; ch < 4; ++ch) g.color[ch] = uint8_t(std::clamp(s->rgba[ch], 0.0f, 1.0f) * 255.0f);
                        g.uv[0] = quv[v][0];
                        g.uv[1] = quv[v][1];
                        verts.push_back(g);
                    }
                }
                continue;
            }
            const Vec3 c{s->position[0], s->position[1], s->position[2]};
            // 1.3 DLL 0x1003fb00: with L / U the view's left / up axes (view +0x18 / +0x24) and angle a: R = w(cos a · L − sin a · U),
            // V = h(cos a · U + sin a · L); corners C + R + V (u0, v0), C − R + V (u1, v0), C − R − V (u1, v1), C + R − V (u0, v1).
            // Here right = −L, so r = −R = w(cos a · right + sin a · up), u = V = h(cos a · up − sin a · right).
            const float hw = s->size, hh = s->size2 >= 0 ? s->size2 : s->size;
            const float ra = s->rotation * 0.017453292f, rc = std::cos(ra), rs = std::sin(ra);
            Vec3 r = (right * rc + upv * rs) * hw, u = (upv * rc + right * -rs) * hh;
            if (s->oriented) {
                // OrientedParticle (renderer type 7, 1.3 DLL 0x1003ffe0): from the axis row 0 d (refEntity +0x14), 0x10059bd0 gives
                // p = normalize((d.z, −d.x, d.y) − d (d · that)) and q = p × d; R = w(cos a · p − sin a · q), V = h(cos a · q + sin a · p)
                // (a = 0: R = w p, V = h q), then the particle corner builder 0x1003f560 (C + R + V (u0, v0), …). r = −R, u = V here.
                const Vec3 d{s->orientDir[0], s->orientDir[1], s->orientDir[2]};
                Vec3 pv{d.z, -d.x, d.y};
                pv = pv + d * -(d.x * pv.x + d.z * pv.z + pv.y * d.y);
                const float pl = std::sqrt(pv.x * pv.x + pv.y * pv.y + pv.z * pv.z);
                if (pl != 0) pv = pv * (1.0f / pl);
                const Vec3 qv{pv.y * d.z - pv.z * d.y, pv.z * d.x - pv.x * d.z, pv.x * d.y - pv.y * d.x};
                const Vec3 R = s->rotation == 0 ? pv * hw : (pv * rc + qv * -rs) * hw;
                u = s->rotation == 0 ? qv * hh : (qv * rc + pv * rs) * hh;
                r = R * -1.0f;
            }
            const Vec3 nr = r * -1.0f, nu = u * -1.0f;
            Vec3 corners[4] = {c + nr + nu, c + r + nu, c + r + u, c + nr + u};
            float uv[4][2] = {{s->uv[0], s->uv[3]}, {s->uv[2], s->uv[3]}, {s->uv[2], s->uv[1]}, {s->uv[0], s->uv[1]}};
            if (s->beam) { // tracer quad (0x4b7d90): B + w, B - w, A - w, A + w with uv (1,1) (1,0) (0,0) (0,1)
                const Vec3 a{s->beamA[0], s->beamA[1], s->beamA[2]}, bb{s->beamB[0], s->beamB[1], s->beamB[2]};
                const Vec3 w{s->beamPerp[0], s->beamPerp[1], s->beamPerp[2]};
                corners[0] = bb + w * s->widthB; corners[1] = bb + w * -s->widthB;
                corners[2] = a + w * -s->widthA; corners[3] = a + w * s->widthA;
                const float tuv[4][2] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};
                std::memcpy(uv, tuv, sizeof uv);
                if (s->beamTail) { // 0x100409c0: A + w (s0,t0), A - w (s1,t0), B - w (s1,t1), B + w (s0,t1)
                    corners[0] = a + w * s->widthA; corners[1] = a + w * -s->widthA;
                    corners[2] = bb + w * -s->widthB; corners[3] = bb + w * s->widthB;
                    const float t2[4][2] = {{s->uv[0], s->uv[1]}, {s->uv[2], s->uv[1]}, {s->uv[2], s->uv[3]}, {s->uv[0], s->uv[3]}};
                    std::memcpy(uv, t2, sizeof uv);
                }
            }
            for (int k = 0; k < 4; ++k) {
                GpuVertex g{};
                g.pos[0] = corners[k].x; g.pos[1] = corners[k].y; g.pos[2] = corners[k].z;
                g.normal[2] = 1;
                if (mi.fxKind == 2 && !s->beam) {
                    // Distortion reads code.tangent / code.binormal: the quad writer 0x1003f560 stores tangent = −A (minus the
                    // width vector R) and binormal = B (the height vector V) in all four vertices; here −R = r and V = u. Carried
                    // in the normal / light slots, which the distortion shader reads as tangent / binormal.
                    g.normal[0] = r.x, g.normal[1] = r.y, g.normal[2] = r.z;
                    g.light[0] = u.x, g.light[1] = u.y, g.light[2] = u.z;
                } else if (mi.fxKind == 2 && s->beamTail) {
                    // Tail / Line quad (type 8, builder 0x100409c0): the 64-byte vertex carries
                    // binormal (+0x28) = B − A and tangent (+0x34) = c, the unit vector normalize((A − cam) × (B − cam)).
                    g.normal[0] = s->beamPerp[0], g.normal[1] = s->beamPerp[1], g.normal[2] = s->beamPerp[2];
                    g.light[0] = s->beamB[0] - s->beamA[0], g.light[1] = s->beamB[1] - s->beamA[1], g.light[2] = s->beamB[2] - s->beamA[2];
                } else if (mi.fxKind == 2) {
                    g.normal[2] = 0; // tracer / cylinder geometry: no shipped distortion user (census in docs/spec/effects.md)
                }
                for (int ch = 0; ch < 4; ++ch) g.color[ch] = uint8_t(std::clamp(s->rgba[ch], 0.0f, 1.0f) * 255.0f);
                g.uv[0] = uv[k][0];
                g.uv[1] = uv[k][1];
                verts.push_back(g);
            }
        }
        b.indexCount = uint32_t(verts.size() / 4 * 6) - b.firstIndex;
        if (b.indexCount) batches.push_back(b);
    }
    if (verts.empty()) return nullptr;
    up.buffer(it->second.vertices, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
    it->second.batches = batches;
    return &it->second;
}
// Cloud elements (renderer type 6, 1.3 DLL 0x10041370): every cloud draws the same static 1024-quad buffer ("Initializing
// particle cloud buffer...", 0x10011ca0) with the particle_cloud(_outdoor) shader (iw_07.iwd materials/shaders/lib/
// particle_cloud.hlsl). Buffer: for i = 0..7, j = 0..7, k = 0..15 one quad whose four vertices share the point
// x = (rand · (1/32767) + i) · 0.25 − 1, y = (rand · (1/32767) + j) · 0.25 − 1, z = (rand · (1/32767) + k) · 0.125 − 1
// (0x1019bec8 / 0x1019be48 / 0x1019be2c, three CRT rand() calls x, y, z per quad; the DLL never calls srand, so the CRT
// starts from 1), texcoords (0,0) (0,1) (1,0) (1,1), indices 0 1 2, 2 1 3. Vertex shader: the point goes through the world
// matrix (0x1005a7a0: rows axis · size, translation origin) into view space, then gets offset in the view plane by
// (u − 0.5) · m.xy + (v − 0.5) · m.zw, m = particleCloudMatrix (0x10041230 / 0x10041180, see below), colour =
// texture · particleCloudColor. Done on the CPU here: view x / y are the view right / up. The outdoor map test of
// particle_cloud_outdoor (alpha 0 for a covered point) drops whole flakes below, as all four vertices share the point.
const GpuModel* cloudModel(Scene& scene, SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs, const std::vector<game::FxCloud>& clouds,
                           const Vec3& right, const Vec3& upv) {
    if (clouds.empty()) return nullptr;
    constexpr uint32_t kQuads = 1024, kMaxClouds = 64;
    static std::array<std::array<float, 3>, kQuads> points = [] {
        std::array<std::array<float, 3>, kQuads> pts{};
        uint32_t holdrand = 1; // MSVC rand(): holdrand = holdrand * 214013 + 2531011, (holdrand >> 16) & 0x7fff
        auto crand = [&] { holdrand = holdrand * 214013u + 2531011u; return int((holdrand >> 16) & 0x7fff); };
        uint32_t q = 0;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                for (int k = 0; k < 16; ++k, ++q) {
                    pts[q][0] = (float(crand()) * 3.0518509e-05f + float(i)) * 0.25f - 1.0f;
                    pts[q][1] = (float(crand()) * 3.0518509e-05f + float(j)) * 0.25f - 1.0f;
                    pts[q][2] = (float(crand()) * 3.0518509e-05f + float(k)) * 0.125f - 1.0f;
                }
        return pts;
    }();
    auto key = std::make_pair(-4, std::string("cloud"));
    auto it = scene.skinned.find(key);
    if (it == scene.skinned.end()) {
        GpuModel gm;
        SDL_GPUBufferCreateInfo vb{};
        vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb.size = kMaxClouds * kQuads * 4 * sizeof(GpuVertex);
        gm.vertices = SDL_CreateGPUBuffer(dev, &vb);
        SDL_GPUBufferCreateInfo ib{};
        ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib.size = kMaxClouds * kQuads * 6 * 4;
        gm.indices = SDL_CreateGPUBuffer(dev, &ib);
        std::vector<uint32_t> idx;
        for (uint32_t qd = 0; qd < kMaxClouds * kQuads; ++qd)
            for (uint32_t k : {0u, 1u, 2u, 2u, 1u, 3u}) idx.push_back(qd * 4 + k);
        up.buffer(gm.indices, idx.data(), uint32_t(idx.size() * 4));
        it = scene.skinned.emplace(key, gm).first;
    }
    std::map<std::string, std::vector<const game::FxCloud*>> byShader;
    for (const auto& c : clouds) byShader[c.shader].push_back(&c);
    std::vector<GpuVertex> verts;
    std::vector<Batch> batches;
    size_t drawn = 0, coveredFlakes = 0;
    for (const auto& [shader, list] : byShader) {
        const MatInfo mi = sceneMaterial(scene, dev, up, vfs, shader);
        if (mi.pass == Pass::Skip || mi.pass == Pass::Sky) continue;
        bool outdoorTest = false; // technique set particle_cloud_outdoor -> particle_cloud_outdoor.hlsl (OUTDOOR 1)
        if (const auto md = vfs.read("materials/" + shader)) {
            try { outdoorTest = assets::parseMaterial(*md).techniqueSet == "particle_cloud_outdoor"; } catch (...) {}
        }
        const bool useOutdoor = outdoorTest && scene.outdoor.valid() && !std::getenv("IW2_NO_OUTDOOR");
        Batch b;
        b.pass = mi.pass == Pass::Opaque || mi.pass == Pass::AlphaTest ? Pass::Blend : mi.pass;
        b.sort = mi.sort, b.materialOrder = mi.materialOrder, b.emissive = mi.emissive, b.pointLit = mi.pointLit, b.pointBlend = mi.pointBlend, b.pointNormalTex = mi.pointNormalTex, b.pointNormalState = mi.pointNormalState;
        b.texture = mi.texture;
        b.colorState = mi.colorState;
        b.lightmap = -3;
        b.fullbright = true;
        b.firstIndex = uint32_t(verts.size() / 4 * 6);
        for (const game::FxCloud* c : list) {
            if (drawn >= kMaxClouds) break;
            ++drawn;
            // particleCloudMatrix (0x10041230): (width, 0, 0, height) when width == height or A ~ B (each squared component
            // difference <= 1e-6, 0x1003f260); else d = normalize(B - A) * height in view space (0x100410e0) and 0x10041180:
            // if d.x < 0.001 and d.y < 0.001 (signed compares, 0x1019be64) again (width, 0, 0, height), otherwise
            // m = (d.y, -d.x, d.x, d.y) with m.xy scaled to length width and m.zw scaled by the same factor when |d.xy| < width.
            const Vec3 A{c->origin[0], c->origin[1], c->origin[2]}, B{c->prev[0], c->prev[1], c->prev[2]};
            float m[4] = {c->width, 0, 0, c->height};
            const Vec3 dAB = B + A * -1.0f;
            const bool same = dAB.x * dAB.x <= 1.0000001e-06f && dAB.y * dAB.y <= 1.0000001e-06f && dAB.z * dAB.z <= 1.0000001e-06f;
            if (c->width != c->height && !same) {
                const float l = std::sqrt(dAB.x * dAB.x + dAB.y * dAB.y + dAB.z * dAB.z);
                const Vec3 d = dAB * (c->height / l);
                const float vx = d.x * right.x + d.y * right.y + d.z * right.z, vy = d.x * upv.x + d.y * upv.y + d.z * upv.z;
                if (!(vx < 0.001f) || !(vy < 0.001f)) {
                    m[0] = vy, m[1] = -vx, m[2] = vx, m[3] = vy;
                    const float len = std::sqrt(m[0] * m[0] + m[1] * m[1]);
                    const float f = 1.0f / len * c->width;
                    m[0] *= f, m[1] *= f;
                    if (len < c->width) m[2] *= f, m[3] *= f;
                }
            }
            const Vec3 ax0 = Vec3{c->axis[0][0], c->axis[0][1], c->axis[0][2]} * c->size;
            const Vec3 ax1 = Vec3{c->axis[1][0], c->axis[1][1], c->axis[1][2]} * c->size;
            const Vec3 ax2 = Vec3{c->axis[2][0], c->axis[2][1], c->axis[2][2]} * c->size;
            static const float tc[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
            Vec3 off[4];
            for (int k = 0; k < 4; ++k) {
                const float su = tc[k][0] - 0.5f, sv = tc[k][1] - 0.5f;
                off[k] = right * (su * m[0] + sv * m[2]) + upv * (su * m[1] + sv * m[3]);
            }
            for (const auto& pt : points) {
                const Vec3 ctr = A + ax0 * pt[0] + ax1 * pt[1] + ax2 * pt[2];
                // The outdoor map coordinate comes from the point alone (all four vertices share it), so a covered flake has
                // alpha 0 over its whole quad: it is left out.
                if (useOutdoor && scene.outdoor.covered({ctr.x, ctr.y, ctr.z})) { ++coveredFlakes; continue; }
                for (int k = 0; k < 4; ++k) {
                    GpuVertex g{};
                    const Vec3 w = ctr + off[k];
                    g.pos[0] = w.x; g.pos[1] = w.y; g.pos[2] = w.z;
                    g.normal[2] = 1;
                    for (int ch = 0; ch < 4; ++ch) g.color[ch] = c->rgba[ch];
                    g.uv[0] = tc[k][0];
                    g.uv[1] = tc[k][1];
                    verts.push_back(g);
                }
            }
        }
        b.indexCount = uint32_t(verts.size() / 4 * 6) - b.firstIndex;
        if (b.indexCount) batches.push_back(b);
    }
    if (std::getenv("IW2_FX_DEBUG")) std::cerr << "fx clouds " << drawn << " flakes drawn " << verts.size() / 4 << " covered " << coveredFlakes << "\n";
    if (verts.empty()) return nullptr;
    up.buffer(it->second.vertices, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
    it->second.batches = batches;
    return &it->second;
}
// Impact marks (game/marks.h) into a per-frame model: one batch per (material, lightmap), drawn in the blend pass
// with the lightmap of the surface each fragment came from (mark +0x20, the surface's lightmap, FUN_10002a00).
// The renderer's mark entry (refexport slot 0x68, 0x10024320) only copies the vertices into a type-1 draw record under the mark's
// material, so depth offset and culling are the material's own state, as for any batch.
const GpuModel* marksModel(Scene& scene, SDL_GPUDevice* dev, Uploader& up, const fs::Vfs& vfs) {
    const auto& marks = scene.marks.marks();
    if (marks.empty()) return nullptr;
    constexpr uint32_t kMaxVerts = 1024 * 9, kMaxIdx = 1024 * 7 * 3;
    auto key = std::make_pair(-3, std::string("marks"));
    auto it = scene.skinned.find(key);
    if (it == scene.skinned.end()) {
        GpuModel gm;
        SDL_GPUBufferCreateInfo vb{};
        vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vb.size = kMaxVerts * sizeof(GpuVertex);
        gm.vertices = SDL_CreateGPUBuffer(dev, &vb);
        SDL_GPUBufferCreateInfo ib{};
        ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ib.size = kMaxIdx * 4;
        gm.indices = SDL_CreateGPUBuffer(dev, &ib);
        it = scene.skinned.emplace(key, gm).first;
    }
    std::map<std::pair<std::string, int>, std::vector<const game::Mark*>> groups;
    for (const auto& m : marks) groups[{m.material, m.lightmap}].push_back(&m);
    std::vector<GpuVertex> verts;
    std::vector<uint32_t> idx;
    std::vector<Batch> batches;
    for (const auto& [k, list] : groups) {
        const MatInfo mi = sceneMaterial(scene, dev, up, vfs, k.first);
        if (mi.pass == Pass::Skip || mi.pass == Pass::Sky) continue;
        Batch b;
        b.pass = mi.pass == Pass::Additive ? Pass::Additive : Pass::Blend;
        b.sort = mi.sort, b.materialOrder = mi.materialOrder, b.emissive = mi.emissive, b.pointLit = mi.pointLit, b.pointBlend = mi.pointBlend, b.pointNormalTex = mi.pointNormalTex, b.pointNormalState = mi.pointNormalState;
        b.texture = mi.texture;
        b.colorState = mi.colorState;
        b.lightmap = k.second < int(scene.lightmaps.size()) ? k.second : -3;
        b.polygonOffset = mi.polygonOffset;
        b.firstIndex = uint32_t(idx.size());
        for (const game::Mark* m : list) {
            if (verts.size() + m->vertices.size() > kMaxVerts) break;
            const uint32_t base = uint32_t(verts.size());
            for (const auto& mv : m->vertices) {
                GpuVertex g{};
                std::memcpy(g.pos, mv.position.data(), 12);
                std::memcpy(g.normal, mv.normal.data(), 12);
                std::memcpy(g.color, mv.rgba.data(), 4);
                g.uv[0] = mv.uv[0];
                g.uv[1] = mv.uv[1];
                g.lmUv[0] = mv.lightmapUv[0];
                g.lmUv[1] = mv.lightmapUv[1];
                verts.push_back(g);
            }
            for (uint32_t t = 1; t + 1 < m->vertices.size(); ++t)
                for (uint32_t v : {base, base + t, base + t + 1}) idx.push_back(v);
        }
        b.indexCount = uint32_t(idx.size()) - b.firstIndex;
        if (b.indexCount) batches.push_back(b);
    }
    if (verts.empty() || idx.empty()) return nullptr;
    up.buffer(it->second.vertices, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
    up.buffer(it->second.indices, idx.data(), uint32_t(idx.size() * 4));
    it->second.batches = batches;
    return &it->second;
}

// Bone transforms by lower-case name, accumulated over a body and its attachments.
using BoneMap = std::unordered_map<std::string, std::pair<std::array<float, 3>, assets::Quat>>;

std::string lowerName(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Sun colour from the worldspawn keys (gfx DLL derive step 0x10004e00, 0x10004ef6..0x10004f2b): (sunlight − ambient) · (1 −
// diffuseFraction) · suncolor with suncolor normalised so its largest component is 1 (0x1005e300). Absent keys: ambient 0,
// diffuseFraction 0.5 (r_lightTweakDiffuseFraction), sunlight 1. TODO(spec): [constant upload not traced] that this RGB is exactly the shader's lightColor0.
static Vec3 worldSunColor(const Vec3& suncolor, float sunlight, float ambient, float diffuseFraction) {
    const float mx = std::max({suncolor.x, suncolor.y, suncolor.z});
    const float k = mx > 0 ? 1.0f / mx : 1.0f;
    return suncolor * (k * (sunlight - ambient) * (1.0f - diffuseFraction));
}

void addBones(BoneMap& map, const assets::XModelParts& parts, const assets::XPose& pose) {
    for (size_t i = 0; i < parts.bones.size(); ++i) map.emplace(lowerName(parts.bones[i].name), std::make_pair(pose.position[i], pose.rotation[i]));
}

// Poses an attached model. All models on an entity share one skeleton: bones whose names already exist (on the
// body or an earlier attachment) follow them; a tag places the model's root bone at that bone (inferred rules).
assets::XPose attachmentPose(const BoneMap& bones, const assets::XModelParts& att, const std::string& tag,
                             const assets::XPose* animated = nullptr) {
    assets::XPose pose = assets::bindPose(att);
    const auto tagIt = tag.empty() ? bones.end() : bones.find(lowerName(tag));
    for (size_t i = 0; i < att.bones.size(); ++i) {
        const auto it = tag.empty() ? bones.find(lowerName(att.bones[i].name)) : bones.end();
        if (it != bones.end()) {
            pose.position[i] = it->second.first;
            pose.rotation[i] = it->second.second;
        } else if (att.bones[i].parent < 0 && tagIt != bones.end()) {
            pose.position[i] = tagIt->second.first;
            pose.rotation[i] = tagIt->second.second;
        } else if (att.bones[i].parent >= 0) {
            const size_t p = size_t(att.bones[i].parent);
            assets::Quat lr = att.bones[i].localRotation;
            auto lo = att.bones[i].localOffset;
            if (animated) { // local transform of the animated pose relative to its parent
                const auto& pr = animated->rotation[p];
                const assets::Quat inv{-pr.x, -pr.y, -pr.z, pr.w};
                lr = assets::multiply(inv, animated->rotation[i]);
                lo = assets::rotate(inv, {animated->position[i][0] - animated->position[p][0], animated->position[i][1] - animated->position[p][1],
                                          animated->position[i][2] - animated->position[p][2]});
            }
            pose.rotation[i] = assets::multiply(pose.rotation[p], lr);
            const auto o = assets::rotate(pose.rotation[p], lo);
            pose.position[i] = {pose.position[p][0] + o[0], pose.position[p][1] + o[1], pose.position[p][2] + o[2]};
        }
    }
    return pose;
}

// Cached view-model animations (xanim/<name>); nullptr when missing.
const assets::XAnim* viewAnim(const fs::Vfs& vfs, const std::string& name) {
    static std::map<std::string, std::optional<assets::XAnim>> cache;
    auto [it, inserted] = cache.try_emplace(lowerName(name));
    if (inserted) {
        try {
            if (const auto d = vfs.read("xanim/" + lowerName(name))) it->second = assets::parseXAnim(*d);
        } catch (const std::exception& e) {
            std::cerr << "warning: xanim " << name << ": " << e.what() << "\n";
        }
    }
    return it->second ? &*it->second : nullptr;
}

// Releases a scene's GPU resources (used when the level changes).
void releaseScene(SDL_GPUDevice* dev, Scene& scene) {
    auto buf = [&](SDL_GPUBuffer* b) { if (b) SDL_ReleaseGPUBuffer(dev, b); };
    auto tex = [&](SDL_GPUTexture* t) { if (t) SDL_ReleaseGPUTexture(dev, t); };
    buf(scene.vertices);
    buf(scene.indices);
    buf(scene.tbn);
    tex(scene.specularity);
    tex(scene.lmWeightTex);
    for (size_t i = 1; i < scene.textures.size(); ++i) tex(scene.textures[i]); // [0] is the shared white texture
    for (auto& lm : scene.lightmaps) { tex(lm.coeffs); tex(lm.sun); }
    tex(scene.sky);
    for (auto& [name, m] : scene.models)
        if (m) { buf(m->vertices); buf(m->indices); }
    for (auto& [key, m] : scene.skinned) { buf(m.vertices); buf(m.indices); }
    scene = Scene{};
}

// "$specularity" (gfx DLL 0x1000fd60 -> generator 0x10029710, shift argument 0): 32 columns x 256 rows of L8, stored at
// y * 32 + x. Column x has t = clamp(x / 30, 0, 1) and exponent P = ln 0.1 / ln cos((180 - 175 t) / 4 degrees), i.e. cos^P
// reaches 0.1 at that angle (column 31: P = 30). Row y is N.H = y / 255 and the byte is round(255 * (y / 255)^P).
// U = specularMap.a (the power), V = N.H, sampled with linear filtering and clamped (sampler state 0x32 at 0x10038e0a).
// r_specularColorScale (gfx DLL dvar 0x101ce638, default 1, range 0.01..100): lightSpecular0 = lightColor0 * scale. IW2_SPEC_SCALE sets it.
float specColorScaleDvar() {
    static const float v = [] {
        const char* e = std::getenv("IW2_SPEC_SCALE");
        return e ? std::clamp(float(std::atof(e)), 0.01f, 100.0f) : 1.0f;
    }();
    return v;
}

// r_cosinePowerMapShift (dvar 0x101ce680, default 0, range -1..1): 0x10034000 passes it as arg 1 of 0x1000d3a0 -> 0x10029710, which adds it
// to x / 30 before the 0..1 clamp (FILD x; FMUL 1/30; FADD shift at 0x10029741). IW2_COS_SHIFT sets it.
float cosinePowerShiftDvar() {
    static const float v = [] {
        const char* e = std::getenv("IW2_COS_SHIFT");
        return e ? std::clamp(float(std::atof(e)), -1.0f, 1.0f) : 0.0f;
    }();
    return v;
}

// "$lightmapweights" (gfx 0x1000ff10 -> basis 0x1000fdf0): 32 x 32 A8R8G8B8. Texel (row i, column j) holds the weights of the direction
// v = (2(j+.5)/32 - 1, 2(i+.5)/32 - 1, sqrt(1 - x^2 - y^2)) (outside the unit disc: x, y normalised, z = 0); w0 = clamp(1 - acos(z) / acos(1/sqrt 3)),
// azimuth a = atan2(y, x) * 3 / 2pi - 0.75 wrapped into [0, 3): a < 1: w1 = 1 - a, w2 = a; a < 2: w2 = 2 - a, w3 = a - 1; else w1 = a - 2, w3 = 3 - a;
// w1..w3 are scaled by 1 - w0. Bytes are B,G,R,A = w0,w1,w2,w3, so the sampled rgba is (w2, w1, w0, w3) and a flat normal gives (0, 0, 1, 0).
// Rounded as the DLL does: byte = (int)(w * 255 + 2^-30) (0x1019be00).
std::vector<uint8_t> lightmapWeightsTable() {
    std::vector<uint8_t> t(32 * 32 * 4);
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 32; ++j) {
            float x = (float(j) + 0.5f) / 32.0f * 2.0f - 1.0f, y = (float(i) + 0.5f) / 32.0f * 2.0f - 1.0f;
            float z2 = 1.0f - (x * x + y * y);
            if (!(z2 > 0.0f)) {
                const float len = std::sqrt(x * x + y * y);
                if (len != 0.0f) { x /= len; y /= len; }
                z2 = 0.0f;
            }
            const float z = std::sqrt(z2);
            float a = std::atan2(y, x) * 0.47746482f - 0.75f;
            if (a < 0.0f) a += 3.0f;
            else if (a > 3.0f) a -= 3.0f;
            float w[4];
            if (a < 1.0f) { w[3] = 0; w[1] = 1.0f - a; w[2] = a; }
            else if (a < 2.0f) { a -= 1.0f; w[1] = 0; w[2] = 1.0f - a; w[3] = a; }
            else { a -= 2.0f; w[2] = 0; w[1] = a; w[3] = 1.0f - a; }
            w[0] = std::clamp(1.0f - std::acos(z) * 1.0467734f, 0.0f, 1.0f);
            for (int k = 1; k < 4; ++k) w[k] *= 1.0f - w[0];
            uint8_t* px = &t[(size_t(i) * 32 + size_t(j)) * 4];
            px[0] = uint8_t(int(w[2] * 255.0f + 9.313225746154785e-10f)); // R = w2
            px[1] = uint8_t(int(w[1] * 255.0f + 9.313225746154785e-10f)); // G = w1
            px[2] = uint8_t(int(w[0] * 255.0f + 9.313225746154785e-10f)); // B = w0
            px[3] = uint8_t(int(w[3] * 255.0f + 9.313225746154785e-10f)); // A = w3
        }
    return t;
}

std::vector<uint8_t> specularityTable(float shift) {
    std::vector<uint8_t> t(32 * 256);
    for (int x = 0; x < 32; ++x) {
        double P = 30.0;
        if (x != 31) {
            const double u = std::clamp(double(float(x) * 0.033333335f + shift), 0.0, 1.0);
            P = std::log(0.1) / std::log(std::cos((180.0 - 175.0 * u) * 0.004363323251406352));
        }
        for (int y = 0; y < 256; ++y) {
            const float v = float(std::pow(double(float(y) * 0.003921569f), P) * 255.0);
            t[size_t(y) * 32 + size_t(x)] = uint8_t(std::lrint(double(v) + 9.313225746154785e-10)); // + 2^-30 (0x1019be00)
        }
    }
    return t;
}

Scene loadScene(SDL_GPUDevice* dev, const fs::Vfs& vfs, const std::string& mapName, bool staticScriptModels) {
    Scene scene;
    const std::string path = "maps/" + mapName + ".d3dbsp";
    const auto data = vfs.read(path);
    if (!data) throw std::runtime_error("map not found: " + path);
    const assets::D3dBsp bsp(*data);
    scene.collision = std::make_unique<game::CollisionWorld>(bsp);
    scene.marks.setWorld(bsp, vfs);
    scene.outdoor.setBounds(bsp, vfs);
    Uploader up(dev);

    // White fallback texture.
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        SDL_GPUTexture* t = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1);
        up.texture(t, 0, 0, 1, 1, white, 4);
        scene.textures.push_back(t);
    }

    auto material = [&](const std::string& name) { return sceneMaterial(scene, dev, up, vfs, name); };
    // Registered in soup order first (the 1.3 renderer's soup loader 0x10002a00): that fixes the first-use order of the technique sets.
    for (const auto& soup : bsp.soups()) material(bsp.materials()[soup.material].name);
    std::vector<MatInfo> mats;
    for (const auto& bm : bsp.materials()) mats.push_back(material(bm.name));

    // Lightmaps.
    const auto lmLump = bsp.lump(assets::BspLump::Lightmaps);
    for (size_t i = 0; i < bsp.lightmapCount(); ++i) {
        const auto parts = assets::splitLightmap(lmLump.subspan(i * assets::D3dBsp::kLightmapBytes, assets::D3dBsp::kLightmapBytes));
        Lightmap lm;
        lm.coeffs = createTexture(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 512, 512, 1, 3, SDL_GPU_TEXTURETYPE_2D_ARRAY);
        up.texture(lm.coeffs, 0, 0, 512, 512, parts.coeffR.data(), uint32_t(parts.coeffR.size()));
        up.texture(lm.coeffs, 0, 1, 512, 512, parts.coeffG.data(), uint32_t(parts.coeffG.size()));
        up.texture(lm.coeffs, 0, 2, 512, 512, parts.coeffB.data(), uint32_t(parts.coeffB.size()));
        lm.sun = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1024, 1024, 1);
        up.texture(lm.sun, 0, 0, 1024, 1024, parts.sun.data(), uint32_t(parts.sun.size()));
        scene.lightmaps.push_back(lm);
    }
    {
        const uint8_t zero[4 * 3] = {};
        scene.dummyLightmap.coeffs = createTexture(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 1, 3, SDL_GPU_TEXTURETYPE_2D_ARRAY);
        for (uint32_t l = 0; l < 3; ++l) up.texture(scene.dummyLightmap.coeffs, 0, l, 1, 1, zero, 4);
        scene.dummyLightmap.sun = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, 1);
        up.texture(scene.dummyLightmap.sun, 0, 0, 1, 1, zero, 1);
    }

    // Vertices.
    std::vector<GpuVertex> verts(bsp.vertices().size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const auto& v = bsp.vertices()[i];
        GpuVertex& g = verts[i];
        std::memcpy(g.pos, v.position.data(), 12);
        std::memcpy(g.normal, v.normal.data(), 12);
        std::memcpy(g.tangent, v.tangent.data(), 12);
        std::memcpy(g.binormal, v.binormal.data(), 12);
        std::memcpy(g.color, v.color.data(), 4);
        std::memcpy(g.uv, v.uv.data(), 8);
        std::memcpy(g.lmUv, v.lightmapUv.data(), 8);
    }

    // Brush submodels (lump 35, 48-byte rows; the 1.3 renderer loader 0x10002df0 keeps the soup count from +0x1c and
    // the first soup from +0x18, -1 when the count is 0). Model 0 is the static world; the soups of a submodel are
    // stored relative to its entity origin. With a running level, the submodels of script_brushmodel entities are not
    // baked into the world but drawn per entity (hide() sets bit 0x80 of ent+0x128, 0x4efd10; show() clears it,
    // 0x4efcb0), so hidden exploder / collapse pieces and moving brush models show their scripted state.
    // TODO(spec): [draw path not traced] the client-side draw path (entity → brush model → R_Add*) was not traced; drawing at the entity's
    // lerped origin/angles mirrors the collision model trace (0x417c20).
    std::vector<int> soupOwner(bsp.soups().size(), 0);
    if (!staticScriptModels) {
        for (const auto& e : bsp.entities()) {
            const std::string* cls = e.get("classname");
            const std::string* model = e.get("model");
            if (!cls || *cls != "script_brushmodel" || !model || model->size() < 2 || (*model)[0] != '*') continue;
            const int n = std::atoi(model->c_str() + 1);
            if (n <= 0) continue;
            const auto [first, count] = bsp.modelSoups(n);
            for (size_t k = 0; k < count; ++k) soupOwner[first + k] = n;
        }
    }

    // Batches: group soups by (pass, texture, lightmap) and emit global 32-bit indices.
    std::map<BatchKey, std::vector<uint32_t>> groups;
    std::map<int, std::map<BatchKey, std::vector<uint32_t>>> brushGroups;
    for (size_t si = 0; si < bsp.soups().size(); ++si) {
        const auto& s = bsp.soups()[si];
        const MatInfo& mi = mats[s.material];
        if (mi.pass == Pass::Skip) continue;
        // Brush-model water soups go into the entity's batches (drawn by the water pass at its transform); a scan of every
        // brush submodel of the 27 shipped SP maps found water only in duhoc_assault *26 / duhoc_defend *128 (water_section,
        // material water_duhoc) and no sky soup, so sky soups of brush models are dropped: no shipped map reaches that path.
        if (soupOwner[si] > 0 && mi.pass == Pass::Sky) continue;
        const int lm = s.lightmap < scene.lightmaps.size() ? int(s.lightmap) : -1;
        // Specular only on lightmapped soups with both maps present (lmap_s); IW2_NO_SPEC disables it.
        static const bool noSpec = std::getenv("IW2_NO_SPEC") != nullptr;
        const bool spec = !noSpec && lm >= 0 && mi.normalTex > 0 && mi.specTex > 0 &&
                          (mi.pass == Pass::Opaque || mi.pass == Pass::AlphaTest || mi.pass == Pass::Blend);
        auto& dst = (soupOwner[si] > 0 ? brushGroups[soupOwner[si]] : groups)[{int(mi.pass), mi.texture, lm, mi.polygonOffset, mi.cull, spec ? mi.normalTex : -1, spec ? mi.specTex : -1, spec ? int(mi.normalState) : 0, spec ? int(mi.specState) : 0, mi.detail, int(mi.alphaOne), int(mi.colorState), int(mi.alphaFunc), mi.sort, mi.materialOrder, int(mi.emissive), int(mi.multiplyFog), int(mi.pointLit), int(mi.pointBlend), mi.pointNormalTex, int(mi.pointNormalState)}];
        for (uint32_t k = 0; k < s.indexCount; ++k) dst.push_back(s.firstVertex + bsp.indices()[s.firstIndex + k]);
        if (soupOwner[si] > 0) {
            auto bb = scene.brushBounds.try_emplace(soupOwner[si], std::array<float, 6>{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f}).first;
            for (uint32_t k = 0; k < s.indexCount; ++k) {
                const auto& pos = bsp.vertices()[s.firstVertex + bsp.indices()[s.firstIndex + k]].position;
                for (int a = 0; a < 3; ++a) {
                    bb->second[size_t(a)] = std::min(bb->second[size_t(a)], pos[size_t(a)]);
                    bb->second[size_t(3 + a)] = std::max(bb->second[size_t(3 + a)], pos[size_t(a)]);
                }
            }
        }
    }
    // Sun from worldspawn (needed for baking model light).
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        if (!cls || *cls != "worldspawn") continue;
        float light = 1.0f, ambient = 0.0f, diffuseFraction = 0.5f;
        if (auto sl = e.get("sunlight")) light = std::stof(*sl);
        if (auto am = e.get("ambient")) ambient = std::stof(*am);
        if (auto c = e.get("suncolor")) scene.sunColor = worldSunColor(parseVec3(*c), light, ambient, diffuseFraction);
        if (auto d = e.get("sundirection")) {
            const Vec3 a = parseVec3(*d);
            Camera c;
            c.pitch = a.x;
            c.yaw = a.y;
            scene.sunDirection = c.forward();
        }
    }
    scene.grid = std::make_unique<assets::LightGrid>(bsp.lump(2), bsp.lump(3));
    const assets::LightGrid& grid = *scene.grid;

    // Static models placed by entities (world-space copies, lit per vertex from the light grid).
    struct ModelData { std::vector<assets::XSurface> surfaces; std::vector<MatInfo> materials; };
    std::unordered_map<std::string, std::optional<ModelData>> modelCache;
    size_t modelCount = 0;
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        const std::string* model = e.get("model");
        if (!cls || !model || (*cls != "misc_model" && !(staticScriptModels && *cls == "script_model"))) continue;
        if (model->rfind("xmodel/", 0) != 0) continue;
        auto& cached = modelCache[*model];
        if (!cached) {
            try {
                const auto md = vfs.read(*model);
                if (!md) throw std::runtime_error("missing");
                const auto xm = assets::parseXModel(*md);
                const auto pd = vfs.read("xmodelparts/" + xm.lods.at(0).name);
                const auto sd = vfs.read("xmodelsurfs/" + xm.lods.at(0).name);
                if (!pd || !sd) throw std::runtime_error("missing parts or surfs");
                ModelData d;
                d.surfaces = assets::parseXModelSurfs(*sd, assets::parseXModelParts(*pd));
                for (const auto& mn : xm.lods[0].materials) d.materials.push_back(material(mn));
                cached = std::move(d);
            } catch (const std::exception& ex) {
                std::cerr << "warning: " << *model << ": " << ex.what() << "\n";
                cached = ModelData{};
            }
        }
        if (cached->surfaces.empty()) continue;

        Vec3 origin, angles;
        float scale = 1.0f;
        if (auto o = e.get("origin")) origin = parseVec3(*o);
        if (auto a = e.get("angles")) angles = parseVec3(*a);
        else if (auto a1 = e.get("angle")) angles.y = std::stof(*a1);
        if (auto sc = e.get("modelscale")) scale = std::stof(*sc);
        const float d2r = float(M_PI) / 180;
        const float sp = std::sin(angles.x * d2r), cp = std::cos(angles.x * d2r);
        const float sy = std::sin(angles.y * d2r), cy = std::cos(angles.y * d2r);
        const float sr = std::sin(angles.z * d2r), cr = std::cos(angles.z * d2r);
        const Vec3 ax0{cp * cy, cp * sy, -sp};
        const Vec3 ax1{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
        const Vec3 ax2{cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
        auto xf = [&](const std::array<float, 3>& v, float k, bool translate) {
            Vec3 r = ax0 * (v[0] * k) + ax1 * (v[1] * k) + ax2 * (v[2] * k);
            return translate ? r + origin : r;
        };

        // One light-grid sample at the model origin, as the original does per model (inferred).
        assets::LightGridCoeffs coeffs{};
        float sunVis = 1.0f;
        const bool haveGrid = grid.sample({origin.x, origin.y, origin.z + 4.0f}, coeffs, sunVis);

        for (size_t si = 0; si < cached->surfaces.size(); ++si) {
            const auto& surf = cached->surfaces[si];
            const MatInfo mi = si < cached->materials.size() ? cached->materials[si] : MatInfo{Pass::Opaque, 0};
            if (mi.pass == Pass::Skip) continue;
            const uint32_t base = uint32_t(verts.size());
            for (const auto& v : surf.vertices) {
                GpuVertex g{};
                const Vec3 p = xf(v.position, scale, true), n = normalize(xf(v.normal, 1.0f, false));
                g.pos[0] = p.x; g.pos[1] = p.y; g.pos[2] = p.z;
                g.normal[0] = n.x; g.normal[1] = n.y; g.normal[2] = n.z;
                const Vec3 tangent = normalize(xf(v.tangent, 1.0f, false)), binormal = normalize(xf(v.binormal, 1.0f, false));
                g.tangent[0] = tangent.x; g.tangent[1] = tangent.y; g.tangent[2] = tangent.z;
                g.binormal[0] = binormal.x; g.binormal[1] = binormal.y; g.binormal[2] = binormal.z;
                { const auto c = assets::drawnVertexColor(mi.flag, v.color); std::memcpy(g.color, c.data(), 4); }
                g.uv[0] = v.uv[0]; g.uv[1] = v.uv[1];
                const float ndl = std::max(0.0f, dot(n, scene.sunDirection));
                std::array<float, 3> probe = {0.45f, 0.45f, 0.45f};
                if (haveGrid) probe = assets::LightGrid::evaluate(coeffs, {n.x, n.y, n.z});
                g.light[0] = probe[0] + sunVis * ndl * scene.sunColor.x;
                g.light[1] = probe[1] + sunVis * ndl * scene.sunColor.y;
                g.light[2] = probe[2] + sunVis * ndl * scene.sunColor.z;
                verts.push_back(g);
            }
            auto& dst = groups[{int(mi.pass), mi.texture, -2, mi.polygonOffset, mi.cull, -1, -1, 0, 0, -1, int(mi.alphaOne), int(mi.colorState), int(mi.alphaFunc), mi.sort, mi.materialOrder, int(mi.emissive), int(mi.multiplyFog), int(mi.pointLit), int(mi.pointBlend), mi.pointNormalTex, int(mi.pointNormalState)}];
            for (uint16_t i : surf.indices) dst.push_back(base + i);
        }
        ++modelCount;
    }
    std::cout << "placed " << modelCount << " static models (" << modelCache.size() << " unique)\n";

    std::vector<uint32_t> indices;
    auto emitBatches = [&](const auto& grouped, std::vector<Batch>& out) {
    for (const auto& [key, idx] : grouped) {
        Batch b;
        b.pass = Pass(std::get<0>(key));
        b.texture = std::get<1>(key);
        b.lightmap = std::get<2>(key);
        b.polygonOffset = std::get<3>(key);
        b.cull = std::get<4>(key);
        b.normalTex = std::get<5>(key);
        b.specTex = std::get<6>(key);
        b.normalState = uint8_t(std::get<7>(key));
        b.specState = uint8_t(std::get<8>(key));
        b.detail = std::get<9>(key);
        b.alphaOne = std::get<10>(key) != 0;
        b.colorState = uint8_t(std::get<11>(key));
        b.alphaFunc = assets::AlphaFunc(std::get<12>(key));
        b.sort = std::get<13>(key);
        b.materialOrder = std::get<14>(key);
        b.emissive = std::get<15>(key) != 0;
        b.multiplyFog = std::get<16>(key) != 0;
        b.pointLit = std::get<17>(key) != 0;
        b.pointBlend = std::get<18>(key) != 0;
        b.pointNormalTex = std::get<19>(key);
        b.pointNormalState = uint8_t(std::get<20>(key));
        b.firstIndex = uint32_t(indices.size());
        b.indexCount = uint32_t(idx.size());
        indices.insert(indices.end(), idx.begin(), idx.end());
        out.push_back(b);
    }
    };
    emitBatches(groups, scene.batches);
    for (const auto& [n, bgroups] : brushGroups) emitBatches(bgroups, scene.brushBatches[n]);

    SDL_GPUBufferCreateInfo vb{};
    vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb.size = uint32_t(verts.size() * sizeof(GpuVertex));
    scene.vertices = SDL_CreateGPUBuffer(dev, &vb);
    SDL_GPUBufferCreateInfo ib{};
    ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ib.size = uint32_t(indices.size() * 4);
    scene.indices = SDL_CreateGPUBuffer(dev, &ib);
    if (!scene.vertices || !scene.indices) sdlFail("SDL_CreateGPUBuffer");
    up.buffer(scene.vertices, verts.data(), vb.size);
    up.buffer(scene.indices, indices.data(), ib.size);
    {
        // Tangent frame of the BSP vertices (lib/normalmap.hlsl Normal_VertexSetup); static-model vertices appended after them have none.
        std::vector<float> tbn(verts.size() * 6, 0.0f);
        for (size_t i = 0; i < bsp.vertices().size(); ++i) {
            std::memcpy(&tbn[i * 6], bsp.vertices()[i].tangent.data(), 12);
            std::memcpy(&tbn[i * 6 + 3], bsp.vertices()[i].binormal.data(), 12);
        }
        SDL_GPUBufferCreateInfo tb{};
        tb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        tb.size = uint32_t(tbn.size() * sizeof(float));
        scene.tbn = SDL_CreateGPUBuffer(dev, &tb);
        if (!scene.tbn) sdlFail("SDL_CreateGPUBuffer");
        up.buffer(scene.tbn, tbn.data(), tb.size);
        const auto lut = specularityTable(cosinePowerShiftDvar());
        scene.specularity = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 32, 256, 1);
        up.texture(scene.specularity, 0, 0, 32, 256, lut.data(), uint32_t(lut.size()));
        const auto wl = lightmapWeightsTable();
        scene.lmWeightTex = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 32, 32, 1);
        up.texture(scene.lmWeightTex, 0, 0, 32, 32, wl.data(), uint32_t(wl.size()));
    }
    up.flush();

    // Sun and spawn point from the entity string.
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        if (!cls) continue;
        if (*cls == "worldspawn") {
            float light = 1.0f, ambient = 0.0f, diffuseFraction = 0.5f;
            if (auto s = e.get("sunlight")) light = std::stof(*s);
            if (auto am = e.get("ambient")) ambient = std::stof(*am);
            if (auto c = e.get("suncolor")) scene.sunColor = worldSunColor(parseVec3(*c), light, ambient, diffuseFraction);
            if (auto d = e.get("sundirection")) {
                const Vec3 a = parseVec3(*d); // pitch yaw roll; the resulting forward vector points towards the sun
                Camera c;
                c.pitch = a.x;
                c.yaw = a.y;
                scene.sunDirection = c.forward();
            }
        } else if (*cls == "info_player_start" && scene.start.pos.x == 0 && scene.start.pos.y == 0) {
            if (auto o = e.get("origin")) scene.start.pos = parseVec3(*o) + Vec3{0, 0, 60};
            if (auto a = e.get("angles")) scene.start.yaw = parseVec3(*a).y;
            else if (auto a1 = e.get("angle")) scene.start.yaw = std::stof(*a1);
        }
    }

    size_t tris = indices.size() / 3;
    std::cout << "loaded " << mapName << ": " << scene.batches.size() << " batches, " << tris << " triangles, "
              << scene.textures.size() << " textures, " << scene.lightmaps.size() << " lightmaps\n";
    return scene;
}

// The front end (main menu) runs without a level: a scene with only the shared white texture, a dummy lightmap and
// empty geometry buffers. `collision` stays null, which the main loop reads as "no level loaded".
Scene emptyScene(SDL_GPUDevice* dev) {
    Scene scene;
    Uploader up(dev);
    const uint8_t white[4] = {255, 255, 255, 255};
    SDL_GPUTexture* t = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1);
    up.texture(t, 0, 0, 1, 1, white, 4);
    scene.textures.push_back(t);
    const uint8_t zero[4 * 3] = {};
    scene.dummyLightmap.coeffs = createTexture(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 1, 3, SDL_GPU_TEXTURETYPE_2D_ARRAY);
    for (uint32_t l = 0; l < 3; ++l) up.texture(scene.dummyLightmap.coeffs, 0, l, 1, 1, zero, 4);
    scene.dummyLightmap.sun = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, 1);
    up.texture(scene.dummyLightmap.sun, 0, 0, 1, 1, zero, 1);
    SDL_GPUBufferCreateInfo vb{};
    vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb.size = sizeof(GpuVertex);
    scene.vertices = SDL_CreateGPUBuffer(dev, &vb);
    SDL_GPUBufferCreateInfo ib{};
    ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ib.size = 4;
    scene.indices = SDL_CreateGPUBuffer(dev, &ib);
    if (!scene.vertices || !scene.indices) sdlFail("SDL_CreateGPUBuffer");
    return scene;
}

// ---------------------------------------------------------------------------------------------
// Renderer

// r_dlightLimit (default 4): of the submitted lights the DLL keeps the ones with the smallest distSq / radius^2 to the view
// origin (quick-select with comparator 0x100150a0 in 0x10015180, called from 0x10015210); order inside the kept set is not defined.
static std::vector<std::array<float, 7>> selectDlights(const std::vector<game::FxLight>& in, const Vec3& eye, size_t limit = 4,
                                                           const std::vector<std::array<float, 4>>& planes = {}) {
    static const bool off = std::getenv("IW2_NO_DLIGHT") != nullptr;
    std::vector<std::array<float, 7>> out;
    if (off) return out;
    std::vector<std::pair<float, const game::FxLight*>> v;
    for (const auto& l : in) {
        if (!(l.radius > 0.0f)) continue; // AddLightToScene drops radius <= 0 (0x1019bd04)
        // The per-light flag array 0x10657098 is written by 0x1000a880: 1 when n.origin - d < -radius for any view plane (sphere fully
        // outside the frustum); 0x10015210 only considers lights whose flag is 0. TODO(spec): [DLL plane builder not read] the plane set (the effect gate's four sides here).
        bool outside = false;
        for (const auto& pl : planes)
            if (pl[0] * l.origin[0] + pl[1] * l.origin[1] + pl[2] * l.origin[2] - pl[3] < -l.radius) outside = true;
        if (outside) continue;
        const float dx = eye.x - l.origin[0], dy = eye.y - l.origin[1], dz = eye.z - l.origin[2];
        v.push_back({(dx * dx + dy * dy + dz * dz) / (l.radius * l.radius), &l});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < v.size() && i < limit; ++i) {
        const auto& l = *v[i].second;
        out.push_back({l.origin[0], l.origin[1], l.origin[2], l.radius, l.rgb[0], l.rgb[1], l.rgb[2]});
    }
    return out;
}

// r_gamma (float 0.5..3, default 1, archived; gfx 0x1000ab8a). r_ignorehwgamma (bool, default 0) switches the hardware ramp off (gfx 0x10033f65);
// TODO(spec): [off by default, needs a texture reload path] its software path (gamma baked into texture bytes at load, 0x100120a0) is not implemented, the ramp is simply skipped.
static float gammaFrom(const std::map<std::string, std::string>& cvars) {
    const auto ig = cvars.find("r_ignorehwgamma");
    if (ig != cvars.end() && std::atoi(ig->second.c_str()) != 0) return game::kGammaDefault;
    const auto it = cvars.find("r_gamma");
    return it == cvars.end() ? game::kGammaDefault : game::clampGamma(float(std::atof(it->second.c_str())));
}

// r_zFeather (bool, default 1; the graphics menu's "soft smoke edges" sets it on close from ui_zfeather): the menu cvar store.
static bool zFeatherFrom(const std::map<std::string, std::string>& cvars) {
    for (const char* k : {"r_zFeather", "r_zfeather"})
        if (const auto it = cvars.find(k); it != cvars.end()) return std::atoi(it->second.c_str()) != 0;
    return true;
}

// r_dlightLimit dvar (int 0..32, registered at DLL 0x1000b318 with default 4): the menu cvar store, else the registered default.
static size_t dlightLimitFrom(const std::map<std::string, std::string>& cvars) {
    for (const char* k : {"r_dlightLimit", "r_dlightlimit"}) {
        const auto it = cvars.find(k);
        if (it != cvars.end()) return size_t(std::clamp(std::atoi(it->second.c_str()), 0, 32));
    }
    return size_t(std::clamp(std::atoi(ui::registeredDefault("r_dlightLimit").c_str()), 0, 32));
}

struct FragmentUniforms {
    float sunDirection[4];
    float sunColor[4];
    float lmWeights[4];
    float params[4];
    float ambient[4];
    float scroll[4]{}; // x: V offset subtracted from the texture coordinate (tread scroll)
    float fogParams[4]{}; // x: 1 = on, y: scale, z: shift, w: exponent (!= 0: exponential fog)
    float fogColor[4]{};
    float probe[6][4]{}; // light-grid coefficients R0,R1,G0,G1,B0,B1 (lightprobe.hlsl lightGridColors); used when sunDirection.w == 1
};

// An entity model to draw this frame.
struct DrawItem {
    const GpuModel* model = nullptr;
    // Brush model (Scene::brushBatches): batches into the world buffers, drawn by the world passes with `transform`;
    // `model` is null then.
    const std::vector<Batch>* worldBatches = nullptr;
    Mat4 transform{};
    // Depth-hack items (the view model) only: the same transform with the eye subtracted from its translation (game/viewspace.h).
    // They are drawn in camera-relative space (applyViewModelSpace) so the clip position is not rounded at map-coordinate size.
    Mat4 relTransform{};
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float sunVisibility = 1.0f;
    bool hasProbe = false;
    float probe[6][4] = {}; // light-grid coefficients for per-pixel ProbeLighting(N)
    float scrollV = 0.0f; // vehicle tread scroll
    // Viewport depth range maximum (the minimum is 0): the renderer draws groups with the 0x8 class in 0 .. 0.2 and the 0x10
    // class in 0 .. 0.5 of the depth buffer (D3D SetViewport MinZ/MaxZ via 0x1003ec50, called at 0x1002fafc..0x1002fc85;
    // class = group flags & 0x18, table at 0x1002fcd8), so the view weapon never goes through walls.
    float depthMax = 1.0f;
    // Near plane of the depth-hack projection: the dvar r_znear_depthhack (default 0.1, registered at 0x1000ad62 / 0x10192e24)
    // replaces r_znear (default 4) for these groups (game/viewprojection.h).
    float zNear = 4.0f;
    // Effect sprites / clouds (the fx sprite model). The DLL sorts type-2 (sprite) draw records after every other record of the
    // emissive list (docs/spec/effects.md "Draw order of effect sprites"), so the blended / additive / multiplied parts of these
    // items are drawn after the same passes of the world and the other models.
    bool effects = false;
};

// Packs the light-grid coefficients as the lightprobe.hlsl lightGridColors{R,G,B}{0,1} constants (corner order 0..3, 4..7).
static void storeProbe(DrawItem& it, const assets::LightGridCoeffs& c) {
    it.hasProbe = !std::getenv("IW2_NO_PROBE");
    for (int ch = 0; ch < 3; ++ch)
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < 4; ++k) it.probe[ch * 2 + h][k] = c[size_t(ch)][size_t(h * 4 + k)];
}

const Mat4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

Mat4 entityTransform(const Vec3& origin, const Vec3& angles, float scale = 1.0f) {
    const float d2r = float(M_PI) / 180;
    const float sp = std::sin(angles.x * d2r), cp = std::cos(angles.x * d2r);
    const float sy = std::sin(angles.y * d2r), cy = std::cos(angles.y * d2r);
    const float sr = std::sin(angles.z * d2r), cr = std::cos(angles.z * d2r);
    const Vec3 f{cp * cy, cp * sy, -sp};
    const Vec3 l{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
    const Vec3 u{cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    return {f.x * scale, f.y * scale, f.z * scale, 0, l.x * scale, l.y * scale, l.z * scale, 0,
            u.x * scale, u.y * scale, u.z * scale, 0, origin.x, origin.y, origin.z, 1};
}

// Depth-hack items are drawn in camera-relative space: model = relTransform, view matrix with the eye at the origin, eye uniform 0.
// Every pass of the item (light, fog, decal, l_point) must use it, since they rely on bit-identical depth (world_clip).
template <class V>
static void applyViewModelSpace(V& mu, const Camera& cam, const DrawItem& it, float aspect) {
    Camera rel = cam;
    rel.pos = {0.0f, 0.0f, 0.0f};
    mu.vp = rel.viewProj(aspect, it.zNear, true);
    mu.model = it.relTransform;
    mu.eye[0] = mu.eye[1] = mu.eye[2] = 0.0f;
}

// A screen-space textured quad in pixels of the render target (HUD).
struct Quad2D {
    float x0, y0, x1, y1;
    float s0 = 0, t0 = 0, s1 = 1, t1 = 1;
    uint8_t rgba[4] = {255, 255, 255, 255};
    int texture = 0; // index into Scene::textures
    int rot = 0;     // quarter turns of the image, clockwise on screen (0x4b0310 with 90 · rot degrees)
    float rotDeg = 0;  // free rotation about the quad centre, degrees clockwise on screen (0x4b0310); 0 = none
    bool free = false; // corners qx/qy (screen pixels) in vertex order instead of the x0..y1 box
    float qx[4] = {}, qy[4] = {};
};

struct Renderer {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUBuffer* overlayVerts = nullptr;
    SDL_GPUBuffer* overlayIndices = nullptr;
    static constexpr uint32_t kMaxQuads = 8192;
    SDL_GPUGraphicsPipeline* pipelines[5] = {}; // Opaque, AlphaTest, Blend, Additive, Multiply
    SDL_GPUGraphicsPipeline* statePipelines[5][3][3] = {}; // [pass][polygon offset 0..2][cull none / back / front]
    SDL_GPUGraphicsPipeline* specPipelines[3][3][3] = {}; // lmap_s: [Opaque, AlphaTest, Blend][polygon offset][cull]
    SDL_GPUGraphicsPipeline* skyPipeline = nullptr;
    SDL_GPUGraphicsPipeline* objectivePipelines[3] = {}; // objective.sm: [polygon offset level]; cull back
    float gameTimeSeconds = 0.0f; // provisional gameTime.z of the objective technique (see the objective pass in draw)
    SDL_GPUGraphicsPipeline* dlightPipelines[3][3] = {}; // l_point: [polygon offset][cull]
    struct DLight { float pos[3]; float radius; float rgb[3]; };
    std::vector<DLight> dlights; // lights of this frame (already limited to r_dlightLimit)
    SDL_GPUGraphicsPipeline* waterPipelines[2][3] = {}; // [blend][cull none / back / front]
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUSampler* pointClampSampler = nullptr; // $floatz reads: code sampler 0x15 has state 0x31 (point min/mag, no mip, clamp U/V; lookup 0x10039030)
    // Post-sun scene copy (resolvedPostSun, render command 0x100310a0) and $floatz (R32F clip z, build pass 0x10024ee0).
    SDL_GPUTexture* postSunTex = nullptr;
    SDL_GPUTexture* floatzTex = nullptr;
    SDL_GPUGraphicsPipeline* floatzPipes[3][3] = {}; // "build floatz" technique (build_floatz(_atest).tech), [polygon offset][cull none / back / front]
    SDL_GPUTexture* outdoorTex = nullptr;          // "$outdoor" (L8 512 x 512) for zfeather_outdoor
    const game::OutdoorMap* outdoorFor = nullptr;
    SDL_GPUGraphicsPipeline* distortionPipe = nullptr;
    SDL_GPUGraphicsPipeline* featherPipes[2] = {}; // Blend (zfeather.tech), Additive (zfeather_add.tech)
    SDL_GPUSampler* waterMapSampler = nullptr;
    SDL_GPUDevice* samplerDev = nullptr;
    SDL_GPUSampler* matSamplers[128] = {};
    assets::TextureMode textureMode = assets::TextureMode::Bilinear; // r_textureMode
    int anisotropy = assets::kDefaultAnisotropy;                     // r_anisotropy
    // r_textureMode / r_anisotropy decide the sampler of every texture without filter bits of its own: rebuilt when they change.
    void setTextureFilter(assets::TextureMode mode, int aniso) {
        aniso = assets::clampAnisotropy(aniso);
        if (mode == textureMode && aniso == anisotropy) return;
        textureMode = mode, anisotropy = aniso;
        for (SDL_GPUSampler*& s : matSamplers)
            if (s) SDL_ReleaseGPUSampler(samplerDev, s), s = nullptr;
    }
    // Per-material sampler state (MaterialTexture::samplerState), docs/spec/material.md "Texture sampler state".
    SDL_GPUSampler* matSampler(uint8_t state) {
        state &= 0x7f;
        if (!matSamplers[state]) {
            const assets::SamplerDesc d = assets::decodeSamplerState(state, textureMode);
            SDL_GPUSamplerCreateInfo si{};
            si.min_filter = si.mag_filter = d.linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            si.mipmap_mode = d.mip == assets::MipFilter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            si.address_mode_u = d.clampU ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
            si.address_mode_v = d.clampV ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
            si.address_mode_w = d.clampW ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
            si.enable_anisotropy = d.anisotropic;
            si.max_anisotropy = float(anisotropy);
            si.max_lod = d.mip == assets::MipFilter::None ? 0.0f : 1000.0f;
            matSamplers[state] = SDL_CreateGPUSampler(samplerDev, &si);
        }
        return matSamplers[state];
    }
    SDL_GPUTexture* depth = nullptr;
    uint32_t depthW = 0, depthH = 0;
    // Blur post effect: full-size scene copy, w/2 step and the w/4 x h/4 "$blurred_screen".
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* sceneTex = nullptr;
    int sceneSamples = 1; // fixed at init, like the latched r_aaSamples
    SDL_GPUTexture* msaaColor = nullptr; // scene pass targets when sceneSamples > 1; resolved into sceneTex
    SDL_GPUTexture* msaaDepth = nullptr;
    SDL_GPUGraphicsPipeline* overlayPipe = nullptr; // the 2D / composite pass is single-sampled; the Blend state with one sample
    // Shell shock screen blend (cmd 0xa capture + cmd 0x1d blend, gfx 0x10031120/0x10031160): ping-pong of composed frames.
    SDL_GPUTexture* ghostTex[2] = {nullptr, nullptr};
    int ghostCur = 0;
    bool ghostValid = false;   // a previous composed frame has been captured
    bool ghostActive = false;  // set by the caller each frame: shell shock blend running
    float ghostBlendMs = 0.0f, ghostElapsedMs = 0.0f; // blend window and time since the capture
    SDL_GPUTexture* quarterTex = nullptr;
    SDL_GPUBuffer* blurVerts = nullptr;
    // Glow / bloom post chain (materials/shaders/glow_*.hlsl, filter_symmetric_N): quarter-resolution $glow_0 / $glow_1.
    SDL_GPUTexture* pingpongTex[2] = {nullptr, nullptr}; // $pingpong_0 / _1
    SDL_GPUTexture* glowTex = nullptr; // $glow_0; $glow_1 is created by the DLL but never written
    SDL_GPUGraphicsPipeline* copyPipe = nullptr;
    SDL_GPUGraphicsPipeline* gammaPipe = nullptr; // final r_gamma pass (fs_gamma)
    SDL_GPUTexture* gammaTex = nullptr;           // the whole frame (scene, menus, HUD) when r_gamma != 1
    uint32_t gammaW = 0, gammaH = 0;
    float gamma = 1.0f;                           // r_gamma, set by the caller each frame (game/gammaramp.h)
    bool zFeather = true;                         // r_zFeather (gfx 0x101ce694, default 1), set by the caller each frame
    SDL_GPUGraphicsPipeline* glowSetupPipe = nullptr;
    SDL_GPUGraphicsPipeline* glowBlurPipe = nullptr;
    SDL_GPUGraphicsPipeline* glowApplyPipe = nullptr;
    SDL_GPUGraphicsPipeline* glowBloomPipe = nullptr;
    SDL_GPUGraphicsPipeline* feedbackBlendPipe = nullptr;
    uint32_t blurW = 0, blurH = 0;
    float lmScale = 1.0f;
    // Weights for a flat normal map from the $lightmapweights rule (docs/spec/material.md): all weight on
    // stored byte 0, which a B8G8R8A8 texture returns in .b.
    float lmWeights[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    bool fullbright = false;
    game::FogConsts fog; // set by the caller each frame (client fog state)
    void applyFog(FragmentUniforms& fu) const {
        fu.fogParams[0] = fog.active ? 1.0f : 0.0f, fu.fogParams[1] = fog.scale, fu.fogParams[2] = fog.shift, fu.fogParams[3] = fog.exponential ? fog.exponent : 0.0f;
        std::memcpy(fu.fogColor, fog.color, sizeof fog.color);
    }
    // Pass-aware fog: a destination blend of One (Pass::Additive) takes the black fixed-function fog colour (game::fogColorIsBlack, gfx 0x10035050).
    void applyFog(FragmentUniforms& fu, Pass p) const {
        applyFog(fu);
        if (p == Pass::Additive) fu.fogColor[0] = fu.fogColor[1] = fu.fogColor[2] = 0.0f;
    }

    // params.x of the world / model fragment shaders: the mtlAlphaTest function, -1 for a USE_ALPHA 0 technique. The multiply
    // techniques write a constant alpha of 1, which the material's GT0 / GE128 always passes.
    float alphaParam(const Batch& b, Pass p) const {
        if (p == Pass::Multiply) return 0.0f;
        return assets::alphaShaderParam(b.alphaFunc, b.alphaOne, sceneSamples > 1 && p == Pass::AlphaTest);
    }

    // The scene passes render with r_aaSamples samples (docs/spec/material.md "Multisampling"); alpha-tested passes turn alpha into coverage.
    void multisampleScene(SDL_GPUGraphicsPipelineCreateInfo& pi, bool coverage) const {
        pi.multisample_state.sample_count = sampleCountEnum();
        pi.multisample_state.enable_alpha_to_coverage = coverage && sceneSamples > 1;
    }
    SDL_GPUSampleCount sampleCountEnum() const {
        return sceneSamples == 8 ? SDL_GPU_SAMPLECOUNT_8 : sceneSamples == 4 ? SDL_GPU_SAMPLECOUNT_4 : sceneSamples == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    }
    static SDL_GPUStoreOp sdlStoreOp(assets::StoreOp op) {
        switch (op) {
            case assets::StoreOp::Store: return SDL_GPU_STOREOP_STORE;
            case assets::StoreOp::Resolve: return SDL_GPU_STOREOP_RESOLVE;
            case assets::StoreOp::ResolveAndStore: return SDL_GPU_STOREOP_RESOLVE_AND_STORE;
            case assets::StoreOp::DontCare: break;
        }
        return SDL_GPU_STOREOP_DONT_CARE;
    }

    void init(SDL_GPUDevice* device, SDL_GPUTextureFormat colorFormat_, int samples = 1) {
        dev = device;
        colorFormat = colorFormat_;
        sceneSamples = samples;
        while (sceneSamples > 1 && !(SDL_GPUTextureSupportsSampleCount(dev, colorFormat, sampleCountEnum()) &&
                                     SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, sampleCountEnum())))
            sceneSamples /= 2;
        SDL_GPUShaderCreateInfo vs{};
        vs.code = reinterpret_cast<const Uint8*>(kShaderSource);
        vs.code_size = sizeof(kShaderSource) - 1;
        vs.entrypoint = "vs_main";
        vs.format = SDL_GPU_SHADERFORMAT_MSL;
        vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        vs.num_uniform_buffers = 1;
        SDL_GPUShader* vsh = SDL_CreateGPUShader(dev, &vs);
        SDL_GPUShaderCreateInfo fs = vs;
        fs.entrypoint = "fs_main";
        fs.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fs.num_samplers = 4;
        SDL_GPUShader* fsh = SDL_CreateGPUShader(dev, &fs);
        if (!vsh || !fsh) sdlFail("SDL_CreateGPUShader");

        SDL_GPUVertexBufferDescription vbd{};
        vbd.slot = 0;
        vbd.pitch = sizeof(GpuVertex);
        vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attrs[6] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, pos)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, normal)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(GpuVertex, color)},
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv)},
            {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, lmUv)},
            {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, light)},
        };

        SDL_GPUVertexBufferDescription vbd2[2] = {vbd, {1, 24, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
        SDL_GPUVertexAttribute attrs8[8] = {attrs[0], attrs[1], attrs[2], attrs[3], attrs[4], attrs[5],
                                            {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                                            {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12}};
        SDL_GPUShaderCreateInfo svsi = vs;
        svsi.entrypoint = "vs_spec";
        SDL_GPUShaderCreateInfo sfsi = fs;
        sfsi.entrypoint = "fs_spec";
        sfsi.num_samplers = 8;
        SDL_GPUShaderCreateInfo dfsi = fs;
        dfsi.entrypoint = "fs_dlight";
        dfsi.num_samplers = 3;
        SDL_GPUShader* dlightFsh = SDL_CreateGPUShader(dev, &dfsi);
        SDL_GPUShaderCreateInfo dvsi = vs;
        dvsi.entrypoint = "vs_dlight";
        SDL_GPUShader* dlightVsh = SDL_CreateGPUShader(dev, &dvsi);
        if (!dlightFsh || !dlightVsh) sdlFail("dlight shader");
        SDL_GPUVertexAttribute dlightAttrs[8] = {attrs[0], attrs[1], attrs[2], attrs[3], attrs[4], attrs[5],
                                                 {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, tangent)},
                                                 {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVertex, binormal)}};
        SDL_GPUShader* specVsh = SDL_CreateGPUShader(dev, &svsi);
        SDL_GPUShader* specFsh = SDL_CreateGPUShader(dev, &sfsi);
        if (!specVsh || !specFsh) sdlFail("spec shaders");
        for (int p = 0; p < 5; ++p) {
            SDL_GPUColorTargetDescription ct{};
            ct.format = colorFormat;
            if (p >= 2) {
                ct.blend_state.enable_blend = true;
                ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.src_color_blendfactor = p == 4 ? SDL_GPU_BLENDFACTOR_DST_COLOR : p == 2 ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.dst_color_blendfactor = p == 4 ? SDL_GPU_BLENDFACTOR_ZERO : p == 2 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            }
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = vsh;
            pi.fragment_shader = fsh;
            pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
            pi.vertex_input_state.num_vertex_buffers = 1;
            pi.vertex_input_state.vertex_attributes = attrs;
            pi.vertex_input_state.num_vertex_attributes = 6;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pi.depth_stencil_state.enable_depth_test = true;
            pi.depth_stencil_state.enable_depth_write = p < 2;
            pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            pi.target_info.has_depth_stencil_target = true;
            if (p == int(Pass::Blend)) {
                overlayPipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                if (!overlayPipe) sdlFail("SDL_CreateGPUGraphicsPipeline");
            }
            multisampleScene(pi, p == int(Pass::AlphaTest));
            pipelines[p] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (!pipelines[p]) sdlFail("SDL_CreateGPUGraphicsPipeline");
            // Polygon offset (1.3 renderer 0x1003d230): level = state bits 4–5; D3DRS_SLOPESCALEDEPTHBIAS =
            // level · r_polygonOffsetScale and D3DRS_DEPTHBIAS = level · r_polygonOffsetBias / 65536 (0x1019be30);
            // both dvars default −1 (0x1000adaa / 0x1000adce). Without slope-scale support the depth bias is
            // doubled instead (not used here). The slope part is the pipeline's slope factor; the constant part is
            // added to the clip depth in the vertex shader (eye.w), which is the D3D9 meaning of DEPTHBIAS.
            // Cull: D3D9 treats clockwise screen-space triangles as front faces; kFrontFace is the winding that
            // matches it under this viewer's projection (checked on the rendered world, not a game rule).
            for (int lvl = 0; lvl <= 2; ++lvl)
                for (int c = 0; c < 3; ++c) {
                    pi.rasterizer_state.enable_depth_bias = lvl > 0;
                    pi.rasterizer_state.depth_bias_slope_factor = float(lvl) * kPolygonOffsetScale;
                    pi.rasterizer_state.cull_mode = c == 0 ? SDL_GPU_CULLMODE_NONE : c == 1 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT;
                    pi.rasterizer_state.front_face = kFrontFace;
                    multisampleScene(pi, p == int(Pass::AlphaTest));
                    statePipelines[p][lvl][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                    if (!statePipelines[p][lvl][c]) sdlFail("SDL_CreateGPUGraphicsPipeline");
                }
            if (p == 3) { // l_point (stateMap additive_nofog): ONE / ONE, depth test only
                pi.vertex_shader = dlightVsh;
                pi.fragment_shader = dlightFsh;
                pi.vertex_input_state.vertex_attributes = dlightAttrs;
                pi.vertex_input_state.num_vertex_attributes = 8;
                for (int lvl = 0; lvl <= 2; ++lvl)
                    for (int c = 0; c < 3; ++c) {
                        pi.rasterizer_state.enable_depth_bias = lvl > 0;
                        pi.rasterizer_state.depth_bias_slope_factor = float(lvl) * kPolygonOffsetScale;
                        pi.rasterizer_state.cull_mode = c == 0 ? SDL_GPU_CULLMODE_NONE : c == 1 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT;
                        pi.rasterizer_state.front_face = kFrontFace;
                        multisampleScene(pi, false);
                        dlightPipelines[lvl][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                        if (!dlightPipelines[lvl][c]) sdlFail("dlight pipeline");
                    }
                pi.vertex_shader = vsh;
                pi.fragment_shader = fsh;
                pi.vertex_input_state.vertex_attributes = attrs;
                pi.vertex_input_state.num_vertex_attributes = 6;
                pi.rasterizer_state.enable_depth_bias = false;
                pi.rasterizer_state.depth_bias_slope_factor = 0;
                pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            }
            if (p < 3) { // lmap_s (specular) variants: tangent frame in vertex buffer 1, six samplers
                pi.vertex_shader = specVsh;
                pi.fragment_shader = specFsh;
                pi.vertex_input_state.vertex_buffer_descriptions = vbd2;
                pi.vertex_input_state.num_vertex_buffers = 2;
                pi.vertex_input_state.vertex_attributes = attrs8;
                pi.vertex_input_state.num_vertex_attributes = 8;
                for (int lvl = 0; lvl <= 2; ++lvl)
                    for (int c = 0; c < 3; ++c) {
                        pi.rasterizer_state.enable_depth_bias = lvl > 0;
                        pi.rasterizer_state.depth_bias_slope_factor = float(lvl) * kPolygonOffsetScale;
                        pi.rasterizer_state.cull_mode = c == 0 ? SDL_GPU_CULLMODE_NONE : c == 1 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT;
                        pi.rasterizer_state.front_face = kFrontFace;
                        multisampleScene(pi, p == int(Pass::AlphaTest));
                        specPipelines[p][lvl][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                        if (!specPipelines[p][lvl][c]) sdlFail("spec pipeline");
                    }
                pi.vertex_shader = vsh;
                pi.fragment_shader = fsh;
                pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
                pi.vertex_input_state.num_vertex_buffers = 1;
                pi.vertex_input_state.vertex_attributes = attrs;
                pi.vertex_input_state.num_vertex_attributes = 6;
            }
            pi.rasterizer_state.enable_depth_bias = false;
            pi.rasterizer_state.depth_bias_slope_factor = 0;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        }
        SDL_ReleaseGPUShader(dev, specVsh);
        SDL_ReleaseGPUShader(dev, specFsh);
        {
            SDL_GPUShaderCreateInfo svs = vs;
            svs.entrypoint = "vs_sky";
            SDL_GPUShaderCreateInfo sfs = fs;
            sfs.entrypoint = "fs_sky";
            sfs.num_samplers = 1;
            SDL_GPUShader* sv = SDL_CreateGPUShader(dev, &svs);
            SDL_GPUShader* sf = SDL_CreateGPUShader(dev, &sfs);
            if (!sv || !sf) sdlFail("sky shaders");
            SDL_GPUColorTargetDescription ct{};
            ct.format = colorFormat;
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = sv;
            pi.fragment_shader = sf;
            pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
            pi.vertex_input_state.num_vertex_buffers = 1;
            pi.vertex_input_state.vertex_attributes = attrs;
            pi.vertex_input_state.num_vertex_attributes = 6;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pi.depth_stencil_state.enable_depth_test = true;
            pi.depth_stencil_state.enable_depth_write = false;
            pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            pi.target_info.has_depth_stencil_target = true;
            ct.blend_state.enable_color_write_mask = true;
            ct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B; // alpha stays 0 for sky (scene alpha = non-sky mask for glow_setup)
            multisampleScene(pi, false);
            skyPipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (!skyPipeline) sdlFail("sky pipeline");
            // objective.sm: blendFunc Add, SrcAlpha, One; separateAlphaBlendFunc Disable; cullFace Back; depthTest LessEqual;
            // depthWrite Disable; colorWrite Enable, Disable (rgb only); fog Disable; polygonOffset passthrough (the material's).
            {
                SDL_GPUShaderCreateInfo ofi = fs;
                ofi.entrypoint = "fs_objective";
                ofi.num_samplers = 1;
                SDL_GPUShader* ofs = SDL_CreateGPUShader(dev, &ofi);
                if (!ofs) sdlFail("objective shader");
                SDL_GPUColorTargetDescription oct{};
                oct.format = colorFormat;
                oct.blend_state.enable_blend = true;
                oct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                oct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                oct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                oct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                oct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
                oct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                oct.blend_state.enable_color_write_mask = true;
                oct.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
                SDL_GPUGraphicsPipelineCreateInfo opi{};
                opi.vertex_shader = vsh;
                opi.fragment_shader = ofs;
                opi.vertex_input_state.vertex_buffer_descriptions = &vbd;
                opi.vertex_input_state.num_vertex_buffers = 1;
                opi.vertex_input_state.vertex_attributes = attrs;
                opi.vertex_input_state.num_vertex_attributes = 6;
                opi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                opi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
                opi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
                opi.rasterizer_state.front_face = kFrontFace;
                opi.depth_stencil_state.enable_depth_test = true;
                opi.depth_stencil_state.enable_depth_write = false;
                opi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                opi.target_info.color_target_descriptions = &oct;
                opi.target_info.num_color_targets = 1;
                opi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
                opi.target_info.has_depth_stencil_target = true;
                for (int lvl = 0; lvl <= 2; ++lvl) {
                    opi.rasterizer_state.enable_depth_bias = lvl > 0;
                    opi.rasterizer_state.depth_bias_slope_factor = float(lvl) * kPolygonOffsetScale;
                    multisampleScene(opi, false);
                    objectivePipelines[lvl] = SDL_CreateGPUGraphicsPipeline(dev, &opi);
                    if (!objectivePipelines[lvl]) sdlFail("objective pipeline");
                }
                SDL_ReleaseGPUShader(dev, ofs);
            }
            SDL_ReleaseGPUShader(dev, sv);
            SDL_ReleaseGPUShader(dev, sf);
        }
        {
            SDL_GPUShaderCreateInfo gv = vs;
            gv.entrypoint = "vs_fs";
            gv.num_uniform_buffers = 0;
            SDL_GPUShader* gvs = SDL_CreateGPUShader(dev, &gv);
            auto mk = [&](const char* entry, uint32_t samplers, uint32_t ubs, bool blend, bool depthTarget, bool additive = false) {
                SDL_GPUShaderCreateInfo gf = fs;
                gf.entrypoint = entry;
                gf.num_samplers = samplers;
                gf.num_uniform_buffers = ubs;
                SDL_GPUShader* gfs = SDL_CreateGPUShader(dev, &gf);
                if (!gvs || !gfs) sdlFail("glow shaders");
                SDL_GPUColorTargetDescription ct{};
                ct.format = colorFormat;
                if (blend) { // premultiplied over: rgb + dst * (1 - a)
                    ct.blend_state.enable_blend = true;
                    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                    ct.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
                    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                }
                SDL_GPUGraphicsPipelineCreateInfo pi{};
                pi.vertex_shader = gvs;
                pi.fragment_shader = gfs;
                pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
                pi.depth_stencil_state.enable_depth_test = false;
                pi.depth_stencil_state.enable_depth_write = false;
                pi.target_info.color_target_descriptions = &ct;
                pi.target_info.num_color_targets = 1;
                if (depthTarget) {
                    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
                    pi.target_info.has_depth_stencil_target = true;
                }
                SDL_GPUGraphicsPipeline* gp = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                if (!gp) sdlFail("glow pipeline");
                SDL_ReleaseGPUShader(dev, gfs);
                return gp;
            };
            copyPipe = mk("fs_copy", 1, 0, false, true);
            glowSetupPipe = mk("fs_glow_setup", 2, 1, false, false);
            glowBlurPipe = mk("fs_glow_blur", 1, 1, false, false);
            glowApplyPipe = mk("fs_glow_apply", 2, 1, true, true);
            glowBloomPipe = mk("fs_glow_bloom", 1, 1, true, true, true);
            feedbackBlendPipe = mk("fs_feedback_blend", 1, 1, true, true);
            gammaPipe = mk("fs_gamma", 1, 1, false, false);
            SDL_ReleaseGPUShader(dev, gvs);
        }
        {
            // Effects that read the post-sun copy / $floatz: distortion_floatz.tech (stateMap nofog with the material's blend
            // 0x65 = SrcAlpha / InvSrcAlpha, depth test, no depth write) and zfeather(_add).tech.
            auto mkFx = [&](const char* vsEntry, const char* fsEntry, uint32_t samplers, int blend) {
                SDL_GPUShaderCreateInfo ev = vs;
                ev.entrypoint = vsEntry;
                SDL_GPUShaderCreateInfo ef = fs;
                ef.entrypoint = fsEntry;
                ef.num_samplers = samplers;
                ef.num_uniform_buffers = 1;
                SDL_GPUShader* evs = SDL_CreateGPUShader(dev, &ev);
                SDL_GPUShader* efs = SDL_CreateGPUShader(dev, &ef);
                if (!evs || !efs) sdlFail("effect shaders");
                SDL_GPUColorTargetDescription ct{};
                ct.format = colorFormat;
                ct.blend_state.enable_blend = true;
                ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                ct.blend_state.dst_color_blendfactor = blend == 1 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                SDL_GPUGraphicsPipelineCreateInfo pi{};
                pi.vertex_shader = evs;
                pi.fragment_shader = efs;
                pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
                pi.vertex_input_state.num_vertex_buffers = 1;
                pi.vertex_input_state.vertex_attributes = attrs;
                pi.vertex_input_state.num_vertex_attributes = 6;
                pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
                pi.depth_stencil_state.enable_depth_test = true;
                pi.depth_stencil_state.enable_depth_write = false;
                pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                pi.target_info.color_target_descriptions = &ct;
                pi.target_info.num_color_targets = 1;
                pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
                pi.target_info.has_depth_stencil_target = true;
                multisampleScene(pi, false);
                SDL_GPUGraphicsPipeline* gp = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                if (!gp) sdlFail("effect pipeline");
                SDL_ReleaseGPUShader(dev, evs);
                SDL_ReleaseGPUShader(dev, efs);
                return gp;
            };
            distortionPipe = mkFx("vs_distort", "fs_distort", 3, 0);
            {
                // build_floatz.sm: no blend, depth write on, colour write on, alpha test Always (the atest shader clips), cull and
                // polygon offset passthrough.
                SDL_GPUShaderCreateInfo ev = vs;
                ev.entrypoint = "vs_floatz";
                SDL_GPUShaderCreateInfo ef = fs;
                ef.entrypoint = "fs_floatz";
                ef.num_samplers = 1;
                ef.num_uniform_buffers = 1;
                SDL_GPUShader* evs = SDL_CreateGPUShader(dev, &ev);
                SDL_GPUShader* efs = SDL_CreateGPUShader(dev, &ef);
                if (!evs || !efs) sdlFail("floatz shaders");
                SDL_GPUColorTargetDescription ct{};
                ct.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
                SDL_GPUGraphicsPipelineCreateInfo pi{};
                pi.vertex_shader = evs;
                pi.fragment_shader = efs;
                pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
                pi.vertex_input_state.num_vertex_buffers = 1;
                pi.vertex_input_state.vertex_attributes = attrs;
                pi.vertex_input_state.num_vertex_attributes = 6;
                pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pi.rasterizer_state.front_face = kFrontFace;
                pi.depth_stencil_state.enable_depth_test = true;
                pi.depth_stencil_state.enable_depth_write = true;
                pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                pi.target_info.color_target_descriptions = &ct;
                pi.target_info.num_color_targets = 1;
                pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
                pi.target_info.has_depth_stencil_target = true;
                for (int lvl = 0; lvl < 3; ++lvl)
                    for (int c = 0; c < 3; ++c) {
                        pi.rasterizer_state.cull_mode = c == 0 ? SDL_GPU_CULLMODE_NONE : c == 1 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT;
                        pi.rasterizer_state.enable_depth_bias = lvl > 0;
                        pi.rasterizer_state.depth_bias_slope_factor = float(lvl) * kPolygonOffsetScale;
                        floatzPipes[lvl][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                        if (!floatzPipes[lvl][c]) sdlFail("floatz pipeline");
                    }
                SDL_ReleaseGPUShader(dev, evs);
                SDL_ReleaseGPUShader(dev, efs);
            }
            featherPipes[0] = mkFx("vs_feather", "fs_feather", 3, 0);
            featherPipes[1] = mkFx("vs_feather", "fs_feather", 3, 1); // zfeather_add_nofog.sm: Add, SrcAlpha, One
        }
        {
            SDL_GPUShaderCreateInfo wvs = vs;
            wvs.entrypoint = "vs_water";
            SDL_GPUShaderCreateInfo wfs = fs;
            wfs.entrypoint = "fs_water";
            wfs.num_samplers = 2;
            wfs.num_uniform_buffers = 1;
            SDL_GPUShader* wv = SDL_CreateGPUShader(dev, &wvs);
            SDL_GPUShader* wf = SDL_CreateGPUShader(dev, &wfs);
            if (!wv || !wf) sdlFail("water shaders");
            for (int blend = 0; blend < 2; ++blend) {
                SDL_GPUColorTargetDescription ct{};
                ct.format = colorFormat;
                if (blend) { // nofog.sm: blendFunc passthrough (SrcAlpha, InvSrcAlpha), separateAlphaBlendFunc Add InvDestAlpha, One
                    ct.blend_state.enable_blend = true;
                    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
                    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                }
                SDL_GPUGraphicsPipelineCreateInfo pi{};
                pi.vertex_shader = wv;
                pi.fragment_shader = wf;
                pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
                pi.vertex_input_state.num_vertex_buffers = 1;
                pi.vertex_input_state.vertex_attributes = attrs;
                pi.vertex_input_state.num_vertex_attributes = 6;
                pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pi.rasterizer_state.front_face = kFrontFace;
                pi.depth_stencil_state.enable_depth_test = true;
                pi.depth_stencil_state.enable_depth_write = !blend; // nofog.sm depthWrite: Enable only when mtlBlendOp == Disable
                pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                pi.target_info.color_target_descriptions = &ct;
                pi.target_info.num_color_targets = 1;
                pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
                pi.target_info.has_depth_stencil_target = true;
                for (int c = 0; c < 3; ++c) {
                    pi.rasterizer_state.cull_mode = c == 0 ? SDL_GPU_CULLMODE_NONE : c == 1 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT;
                    multisampleScene(pi, false);
                    waterPipelines[blend][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                    if (!waterPipelines[blend][c]) sdlFail("water pipeline");
                }
            }
            SDL_ReleaseGPUShader(dev, wv);
            SDL_ReleaseGPUShader(dev, wf);
        }
        SDL_ReleaseGPUShader(dev, vsh);
        SDL_ReleaseGPUShader(dev, fsh);

        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.enable_anisotropy = true;
        si.max_anisotropy = 8;
        si.max_lod = 1000;
        samplerDev = dev;
        repeatSampler = SDL_CreateGPUSampler(dev, &si);
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.enable_anisotropy = false;
        clampSampler = SDL_CreateGPUSampler(dev, &si);
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        waterMapSampler = SDL_CreateGPUSampler(dev, &si);
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        pointClampSampler = SDL_CreateGPUSampler(dev, &si);
    }

    void ensureBlurTargets(uint32_t w, uint32_t h) {
        if (sceneTex && blurW == w && blurH == h) return;
        for (SDL_GPUTexture** t : {&sceneTex, &quarterTex, &pingpongTex[0], &pingpongTex[1], &glowTex, &ghostTex[0], &ghostTex[1],
                                   &postSunTex, &floatzTex})
            if (*t) SDL_ReleaseGPUTexture(dev, *t), *t = nullptr;
        auto make = [&](uint32_t tw, uint32_t th) {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = colorFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = std::max(tw, 1u);
            info.height = std::max(th, 1u);
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &info);
            if (!t) sdlFail("blur texture");
            return t;
        };
        sceneTex = make(w, h);
        pingpongTex[0] = make(w / 4, h / 4); // all screen filter targets: (width >> 2, height >> 2), gfx 0x100235aa onwards
        pingpongTex[1] = make(w / 4, h / 4);
        glowTex = make(w / 4, h / 4);
        ghostTex[0] = make(w, h);
        ghostTex[1] = make(w, h);
        ghostValid = false;
        postSunTex = make(w, h);
        {
            // $floatz: D3DFMT_R32F, window size (0x10023240..0x10023276).
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = std::max(w, 1u);
            info.height = std::max(h, 1u);
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            floatzTex = SDL_CreateGPUTexture(dev, &info);
            if (!floatzTex) sdlFail("floatz texture");
        }
        quarterTex = make(w / 4, h / 4); // $blurred_screen: (screenW>>2, screenH>>2) clamped >= 1 (gfx 0x100235b5)
        if (!blurVerts) {
            SDL_GPUBufferCreateInfo vb{};
            vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            vb.size = uint32_t(12 * sizeof(GpuVertex));
            blurVerts = SDL_CreateGPUBuffer(dev, &vb);
        }
        blurW = w;
        blurH = h;
    }

    void ensureDepth(uint32_t w, uint32_t h) {
        if (depth && depthW == w && depthH == h) return;
        if (depth) SDL_ReleaseGPUTexture(dev, depth);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER; // read by the $floatz build
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth = SDL_CreateGPUTexture(dev, &info);
        if (!depth) sdlFail("depth texture");
        depthW = w;
        depthH = h;
        if (sceneSamples > 1) {
            for (SDL_GPUTexture** t : {&msaaColor, &msaaDepth})
                if (*t) SDL_ReleaseGPUTexture(dev, *t), *t = nullptr;
            SDL_GPUTextureCreateInfo ms = info;
            ms.sample_count = sampleCountEnum();
            ms.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            msaaDepth = SDL_CreateGPUTexture(dev, &ms);
            ms.format = colorFormat;
            ms.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            msaaColor = SDL_CreateGPUTexture(dev, &ms);
            if (!msaaDepth || !msaaColor) sdlFail("multisample targets");
        }
    }

    // Draws the frame; with r_gamma != 1 the whole frame (menus and HUD included, like the hardware ramp of the original) goes to an
    // offscreen texture first and a final full-screen pass applies the ramp (game/gammaramp.h, docs/spec/ui.md "Brightness").
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam,
              const std::vector<DrawItem>& items = {}, const std::vector<Quad2D>& overlay = {}, float blur = 0.0f) {
        const float g = game::clampGamma(gamma);
        if (g == 1.0f) return drawFrame(cmd, target, w, h, scene, cam, items, overlay, blur);
        if (!gammaTex || gammaW != w || gammaH != h) {
            if (gammaTex) SDL_ReleaseGPUTexture(dev, gammaTex);
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = colorFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = w;
            info.height = h;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            gammaTex = SDL_CreateGPUTexture(dev, &info);
            if (!gammaTex) sdlFail("gamma texture");
            gammaW = w;
            gammaH = h;
        }
        drawFrame(cmd, gammaTex, w, h, scene, cam, items, overlay, blur);
        SDL_GPUColorTargetInfo c{};
        c.texture = target;
        c.load_op = SDL_GPU_LOADOP_DONT_CARE;
        c.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &c, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(p, gammaPipe);
        SDL_GPUTextureSamplerBinding tb{gammaTex, clampSampler};
        SDL_BindGPUFragmentSamplers(p, 0, &tb, 1);
        const float u[4] = {g, 1.0f / g, 0, 0};
        SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof(u));
        SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
        SDL_EndGPURenderPass(p);
    }

    void drawFrame(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam,
                   const std::vector<DrawItem>& items, const std::vector<Quad2D>& overlay, float blur) {
        ensureDepth(w, h);
        // Blur post effect: only when the composed radius is > 0 (gate in gfx 0x10030ed0); the scene goes to an offscreen
        // texture, then composite pass: scene, blurred quarter copy with alpha min(R / F, 1), then the HUD (not blurred).
        const float blurA = game::blurAlpha(blur, float(h));
        // r_glow (default 1, gfx 0x1000b7b8); IW2_NO_GLOW=1 disables the chain.
        static const bool noGlow = std::getenv("IW2_NO_GLOW") != nullptr;
        const bool glowOn = !noGlow;
        const bool post = blurA > 0.0f || glowOn || ghostActive || sceneSamples > 1; // the resolve target is sceneTex
        if (post) {
            ensureBlurTargets(w, h);
            GpuVertex bv[12]{};
            // quad 0: scene, 1: blur, 2: shock ghost (previous composed frame); alpha = min(0.01^(elapsed/blend), 0.99) (gfx 0x10031160)
            const float ghostA = ghostBlendMs > 0.0f && ghostElapsedMs >= 0.0f && ghostElapsedMs < ghostBlendMs
                                     ? std::min(std::pow(0.01f, ghostElapsedMs / ghostBlendMs), 0.99f) : 0.0f;
            for (int q = 0; q < 3; ++q)
                for (int k = 0; k < 4; ++k) {
                    GpuVertex& g = bv[q * 4 + k];
                    const float xs[4] = {-1, 1, 1, -1}, ys[4] = {1, 1, -1, -1}, us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
                    g.pos[0] = xs[k];
                    g.pos[1] = ys[k];
                    g.normal[2] = 1;
                    std::memset(g.color, 255, 3);
                    g.color[3] = q == 0 ? 255 : q == 2 ? uint8_t(std::lround(ghostA * 255.0f)) : uint8_t(std::lround(game::blurAlpha(blur, float(h)) * 255.0f));
                    g.uv[0] = us[k];
                    g.uv[1] = vs[k];
                }
            Uploader up(dev, false);
            up.buffer(blurVerts, bv, sizeof(bv));
        }
        // HUD quads go to their own buffers before the render pass (alpha-blended, no lighting, at the near plane).
        const size_t nq = std::min<size_t>(overlay.size(), kMaxQuads);
        if (nq) {
            if (!overlayVerts) {
                SDL_GPUBufferCreateInfo vb{};
                vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                vb.size = uint32_t(kMaxQuads * 4 * sizeof(GpuVertex));
                overlayVerts = SDL_CreateGPUBuffer(dev, &vb);
                SDL_GPUBufferCreateInfo ib{};
                ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                ib.size = kMaxQuads * 6 * 4;
                overlayIndices = SDL_CreateGPUBuffer(dev, &ib);
                std::vector<uint32_t> idx;
                for (uint32_t q = 0; q < kMaxQuads; ++q)
                    for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(q * 4 + k);
                Uploader up(dev);
                up.buffer(overlayIndices, idx.data(), uint32_t(idx.size() * 4));
            }
            std::vector<GpuVertex> verts;
            verts.reserve(nq * 4);
            for (size_t i = 0; i < nq; ++i) {
                const Quad2D& q = overlay[i];
                float xs[4] = {q.x0, q.x1, q.x1, q.x0}, ys[4] = {q.y0, q.y0, q.y1, q.y1};
                if (q.free)
                    for (int k = 0; k < 4; ++k) { xs[k] = q.qx[k]; ys[k] = q.qy[k]; }
                if (q.rotDeg != 0) { // rotate the corners about the box (or free-corner) centre, clockwise on screen (y down)
                    const float cx = (xs[0] + xs[2]) * 0.5f, cy = (ys[0] + ys[2]) * 0.5f;
                    const float a = q.rotDeg * 0.0174532925f, ca = std::cos(a), sa = std::sin(a);
                    for (int k = 0; k < 4; ++k) {
                        const float dx = xs[k] - cx, dy = ys[k] - cy;
                        xs[k] = cx + dx * ca - dy * sa;
                        ys[k] = cy + dx * sa + dy * ca;
                    }
                }
                const float s4[4] = {q.s0, q.s1, q.s1, q.s0}, t4[4] = {q.t0, q.t0, q.t1, q.t1};
                // 0x4b0310: the corner the image's corner i lands on is i + rot (TL, TR, BR, BL order).
                float ss[4], ts[4];
                for (int k = 0; k < 4; ++k) {
                    ss[(k + q.rot) & 3] = s4[k];
                    ts[(k + q.rot) & 3] = t4[k];
                }
                for (int k = 0; k < 4; ++k) {
                    GpuVertex g{};
                    g.pos[0] = xs[k] / float(w) * 2.0f - 1.0f;
                    g.pos[1] = 1.0f - ys[k] / float(h) * 2.0f;
                    g.pos[2] = 0.0f;
                    g.normal[2] = 1;
                    std::memcpy(g.color, q.rgba, 4);
                    g.uv[0] = ss[k];
                    g.uv[1] = ts[k];
                    verts.push_back(g);
                }
            }
            Uploader up(dev, false);
            up.buffer(overlayVerts, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = post ? sceneTex : target;
        color.clear_color = {0.55f, 0.65f, 0.78f, post ? 0.0f : 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        if (sceneSamples > 1) {
            color.resolve_texture = sceneTex;
            color.texture = msaaColor;
        }
        // Effects that read the post-sun scene copy or $floatz (distortion, zfeather). The copy needs the scene in an offscreen
        // target (post) and is made only when the scene viewport is the whole window (0x10025ce5 flag; always here).
        // IW2_NO_POSTSUN=1 disables the copy and the float-z pass (distortion then skipped, zfeather drawn unfeathered).
        static const bool noPostSun = std::getenv("IW2_NO_POSTSUN") != nullptr;
        bool postSunWanted = false;
        if (post && !noPostSun)
            for (const DrawItem& it : items)
                if (it.model)
                    for (const Batch& b : it.model->batches) postSunWanted = postSunWanted || b.fxKind != 0;
        bool postSunReady = false;
        if (postSunWanted && scene.outdoor.valid() && outdoorFor != &scene.outdoor) {
            if (outdoorTex) SDL_ReleaseGPUTexture(dev, outdoorTex);
            outdoorTex = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 512, 512, 1);
            Uploader oup(dev, false);
            oup.texture(outdoorTex, 0, 0, 512, 512, scene.outdoor.image().data(), uint32_t(scene.outdoor.image().size()));
            outdoorFor = &scene.outdoor;
        }
        // The scene pass is interrupted before the blended passes for the post-sun copy (also read by glow_setup) and resumed with
        // LOAD, so its first part must keep the colour samples and the depth (assets::scenePassStores, docs/spec/material.md
        // "Multisampling"); a resolve-only / DONT_CARE store left the resumed pass reading undefined sample memory.
        const bool resumedPass = postSunWanted || glowOn;
        const assets::ScenePassStores stores = assets::scenePassStores(sceneSamples, resumedPass);
        color.store_op = sdlStoreOp(stores.color);
        SDL_GPUDepthStencilTargetInfo ds{};
        ds.texture = sceneSamples > 1 ? msaaDepth : depth;
        ds.clear_depth = 1.0f;
        ds.load_op = SDL_GPU_LOADOP_CLEAR;
        ds.store_op = sdlStoreOp(stores.depth);
        ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        struct VU { Mat4 vp; float eye[4]; Mat4 model; } vu{cam.viewProj(float(w) / float(h)), {cam.pos.x, cam.pos.y, cam.pos.z, 0}, kIdentity};
        if (postSunWanted) {
            // $floatz build pass (0x10024ee0, first thing in the scene builder): render target 6 ($floatz, R32F), depth and stencil
            // cleared (0x10024e90: z 1.0), then the draw list of technique type 2 "build floatz". Only the *_replace /
            // *_alphatest technique sets (phong, ambient, unlit, default) have that technique (build_floatz / build_floatz_atest(_scroll)):
            // Pass::Opaque / Pass::AlphaTest batches here; sky.techset uses build_floatz_clear (262144 · 2 · 1.732 everywhere),
            // which the clear colour reproduces. Blended, effect and water surfaces are not in it.
            const float kMaxExtent = 262144.0f * 2.0f * 1.732f;
            SDL_GPUColorTargetInfo fz{};
            fz.texture = floatzTex;
            fz.clear_color = {kMaxExtent, kMaxExtent, kMaxExtent, kMaxExtent};
            fz.load_op = SDL_GPU_LOADOP_CLEAR;
            fz.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo fds = ds;
            fds.store_op = SDL_GPU_STOREOP_DONT_CARE;
            fds.texture = depth; // $floatz is single-sampled
            SDL_GPURenderPass* fp = SDL_BeginGPURenderPass(cmd, &fz, 1, &fds);
            auto drawFz = [&](const Batch& b, VU u) {
                if (b.pass != Pass::Opaque && b.pass != Pass::AlphaTest) return;
                SDL_BindGPUGraphicsPipeline(fp, floatzPipes[std::clamp(int(b.polygonOffset), 0, 2)][std::clamp(b.cull, 0, 2)]);
                u.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f;
                SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
                SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                SDL_BindGPUFragmentSamplers(fp, 0, &tb, 1);
                const struct { float p[4]; } fzu{{b.pass == Pass::AlphaTest ? 1.0f : 0.0f, 0.0f, 0, 0}};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fzu, sizeof(fzu));
                SDL_DrawGPUIndexedPrimitives(fp, b.indexCount, 1, b.firstIndex, 0, 0);
            };
            SDL_GPUBufferBinding fvb{scene.vertices, 0};
            SDL_BindGPUVertexBuffers(fp, 0, &fvb, 1);
            SDL_GPUBufferBinding fib{scene.indices, 0};
            SDL_BindGPUIndexBuffer(fp, &fib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            for (const Batch& b : scene.batches) drawFz(b, vu);
            for (const DrawItem& it : items) {
                if (!it.worldBatches) continue;
                VU u = vu;
                u.model = it.transform;
                for (const Batch& b : *it.worldBatches) drawFz(b, u);
            }
            for (const DrawItem& it : items) {
                if (!it.model) continue;
                // TODO(spec): [inference, command 0x17 dispatch to walker 0x1002f830 not traced] the view model goes in with its depth class.
                const SDL_GPUViewport fzRange{0, 0, float(w), float(h), 0.0f, it.depthMax};
                SDL_SetGPUViewport(fp, &fzRange);
                SDL_GPUBufferBinding mvb{it.model->vertices, 0};
                SDL_BindGPUVertexBuffers(fp, 0, &mvb, 1);
                SDL_GPUBufferBinding mib{it.model->indices, 0};
                SDL_BindGPUIndexBuffer(fp, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                VU u = vu;
                u.model = it.transform;
                if (it.depthMax < 1.0f) u.vp = cam.viewProj(float(w) / float(h), it.zNear, true);
                for (const Batch& b : it.model->batches) drawFz(b, u);
            }
            SDL_EndGPURenderPass(fp);
        }
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &ds);

        // World surface groups: the static world, then each visible brush model at its entity transform (same shaders,
        // lightmaps and buffers).
        struct WorldGroup { const std::vector<Batch>* batches; Mat4 model; };
        std::vector<WorldGroup> worldGroups{{&scene.batches, kIdentity}};
        for (const DrawItem& it : items)
            if (it.worldBatches) worldGroups.push_back({it.worldBatches, it.transform});
        // Binds the pass pipeline for a batch's polygon offset level; w of eye carries the constant depth bias.
        // lib/shade.hlsl USE_DETAIL: texture slot 3 + detailScale in FragmentUniforms.scroll.yzw (white texture when absent).
        auto detailBinding = [&](int d) {
            return d >= 0 ? SDL_GPUTextureSamplerBinding{scene.textures[size_t(scene.details[size_t(d)].tex)], matSampler(scene.details[size_t(d)].state)}
                          : SDL_GPUTextureSamplerBinding{scene.textures[0], repeatSampler};
        };
        auto detailUniforms = [&](FragmentUniforms& fu, int d) {
            static const bool noDetail = std::getenv("IW2_NO_DETAIL") != nullptr;
            if (d < 0 || noDetail) return;
            fu.scroll[1] = 1.0f;
            fu.scroll[2] = scene.details[size_t(d)].scale[0];
            fu.scroll[3] = scene.details[size_t(d)].scale[1];
        };
        auto bindOffset = [&](Pass p, int level, int cull, VU u, float tieBias = 0.0f) {
            SDL_BindGPUGraphicsPipeline(pass, statePipelines[int(p)][level][cull]);
            u.eye[3] = float(level) * kPolygonOffsetBias / 65536.0f + tieBias;
            SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        };
        SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
        SDL_GPUBufferBinding vb{scene.vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_GPUBufferBinding ib{scene.indices, 0};
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        if (scene.sky) {
            SDL_BindGPUGraphicsPipeline(pass, skyPipeline);
            SDL_GPUTextureSamplerBinding sb{scene.sky, clampSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sb, 1);
            for (const Batch& b : scene.batches)
                if (b.pass == Pass::Sky) SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
        }
        // One l_point draw of a world batch (the DLL collects the touched surfaces per light, 0x10014fe0).
        auto drawDlightBatch = [&](const DLight& dl, const Batch& b, const Mat4& model) {
            const bool pointWritesDepth = b.pass == Pass::Opaque || b.pass == Pass::AlphaTest;
            static const float ramp[16] = {255, 255, 244, 223, 202, 181, 159, 138, 117, 96, 74, 53, 32, 11, 0, 0}; // images/falloff_linear.iwi
            static const bool depthProbe = std::getenv("IW2_DEPTH_PROBE") != nullptr;
            SDL_BindGPUGraphicsPipeline(pass, dlightPipelines[b.polygonOffset][b.cull]);
            VU du = vu;
            du.model = model;
            du.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f + assets::layerTieBias(pointWritesDepth);
            SDL_PushGPUVertexUniformData(cmd, 0, &du, sizeof(du));
            const bool hasNormalMap = b.pointNormalTex >= 0;
            const SDL_GPUTextureSamplerBinding tb[3] = {{scene.textures[size_t(b.texture)], matSampler(b.colorState)},
                                                        {scene.textures[size_t(hasNormalMap ? b.pointNormalTex : b.texture)], matSampler(b.pointNormalState)},
                                                        detailBinding(b.detail)};
            SDL_BindGPUFragmentSamplers(pass, 0, tb, 3);
            struct DU { float light[4], color[4], params[4], eye[4], ramp[16], detail[4]; } fu{};
            if (b.detail >= 0) fu.detail[0] = scene.details[size_t(b.detail)].scale[0], fu.detail[1] = scene.details[size_t(b.detail)].scale[1], fu.detail[2] = 1.0f;
            std::memcpy(fu.light, dl.pos, 12);
            fu.light[3] = dl.radius;
            std::memcpy(fu.color, dl.rgb, 12);
            fu.params[0] = float(int(b.alphaFunc));
            fu.params[1] = depthProbe ? 1.0f : 0.0f;
            fu.params[2] = b.pointBlend ? 1.0f : 0.0f;
            fu.params[3] = hasNormalMap ? 1.0f : 0.0f;
            fu.eye[0] = cam.pos.x, fu.eye[1] = cam.pos.y, fu.eye[2] = cam.pos.z;
            for (int k = 0; k < 16; ++k) fu.ramp[k] = ramp[k] / 255.0f;
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
        };
        // The scene draw list (1.3 renderer key writer 0x100247d0, sort 0x10024900): world, brush-model, static and dynamic surfaces in one order.
        // Effect sprites and clouds stay after it (type-2 records sort after every other record).
        struct ListRef { const Batch* batch; const Mat4* model; const DrawItem* item; assets::SurfaceKey key; };
        std::vector<ListRef> drawList;
        for (size_t g = 0; g < worldGroups.size(); ++g)
            for (const Batch& b : *worldGroups[g].batches) {
                if (b.pass == Pass::Sky || b.pass == Pass::Skip || b.pass == Pass::Objective) continue;
                const int entity = g ? int(g) : b.lightmap == -2 ? assets::kStaticModelEntity : assets::kWorldEntity;
                drawList.push_back({&b, &worldGroups[g].model, nullptr, {b.sort, b.materialOrder, b.lightmap >= 0 ? b.lightmap : 31, entity}});
            }
        for (size_t n = 0; n < items.size(); ++n) {
            if (!items[n].model || items[n].effects) continue;
            for (const Batch& b : items[n].model->batches)
                if (b.fxKind == 0 && b.pass != Pass::Objective && b.pass != Pass::Skip && b.pass != Pass::Sky)
                    drawList.push_back({&b, &items[n].transform, &items[n], {b.sort, b.materialOrder, 31, int(n)}});
        }
        std::stable_sort(drawList.begin(), drawList.end(), [](const ListRef& x, const ListRef& y) { return assets::surfaceDrawsBefore(x.key, y.key); });
        int stream = -1; // what the world buffers are bound as: 0 single vertex stream, 1 with the tangent frame, -1 not bound
        auto bindStream = [&](int want) {
            if (stream == want) return;
            if (want) {
                SDL_GPUBufferBinding tvb[2] = {{scene.vertices, 0}, {scene.tbn, 0}};
                SDL_BindGPUVertexBuffers(pass, 0, tvb, 2);
            } else SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            stream = want;
        };
        auto bindItem = [&](const DrawItem& it) {
            SDL_GPUBufferBinding mvb{it.model->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &mvb, 1);
            SDL_GPUBufferBinding mib{it.model->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            stream = -1;
            if (it.depthMax < 1.0f) {
                const SDL_GPUViewport vpDepth{0, 0, float(w), float(h), 0.0f, it.depthMax};
                SDL_SetGPUViewport(pass, &vpDepth);
            }
        };
        auto unbindItem = [&](const DrawItem& it) {
            if (it.depthMax < 1.0f) {
                const SDL_GPUViewport vpFull{0, 0, float(w), float(h), 0.0f, 1.0f};
                SDL_SetGPUViewport(pass, &vpFull);
            }
        };
        auto itemVu = [&](const DrawItem& it) {
            VU mu = vu;
            mu.model = it.transform;
            if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
            return mu;
        };
        auto drawItemBatch = [&](const DrawItem& it, const Batch& b) {
            const Pass p = b.pass;
            bindItem(it);
            bindOffset(p, b.polygonOffset, b.cull, itemVu(it));
            const Lightmap& elm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
            SDL_GPUTextureSamplerBinding tex[4] = {
                {scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {elm.coeffs, clampSampler}, {elm.sun, clampSampler}, detailBinding(b.detail)};
            SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);
            FragmentUniforms fu = {
                {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, it.hasProbe ? 1.0f : 0.0f},
                {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                {alphaParam(b, p), b.lightmap >= 0 ? 1.0f : 3.0f, lmScale, fullbright ? 1.0f : 0.0f},
                {it.ambient[0], it.ambient[1], it.ambient[2], it.sunVisibility},
                {b.scroll ? it.scrollV : 0.0f, 0, 0, 0}};
            std::memcpy(fu.probe, it.probe, sizeof(fu.probe));
            detailUniforms(fu, b.detail);
            if (p != Pass::Multiply) applyFog(fu, p); // additive: black fog colour (render-hints.md); the multiply fix-up pass is world-only
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
            unbindItem(it);
        };
        auto drawItemDlight = [&](const DLight& dl, const DrawItem& it, const Batch& b) {
            static const float ramp[16] = {255, 255, 244, 223, 202, 181, 159, 138, 117, 96, 74, 53, 32, 11, 0, 0}; // images/falloff_linear.iwi
            static const bool depthProbe = std::getenv("IW2_DEPTH_PROBE") != nullptr;
            bindItem(it);
            SDL_BindGPUGraphicsPipeline(pass, dlightPipelines[b.polygonOffset][b.cull]);
            VU mu = itemVu(it);
            mu.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f;
            SDL_PushGPUVertexUniformData(cmd, 0, &mu, sizeof(mu));
            const bool hasNormalMap = b.pointNormalTex >= 0;
            const SDL_GPUTextureSamplerBinding tb[3] = {{scene.textures[size_t(b.texture)], matSampler(b.colorState)},
                                                        {scene.textures[size_t(hasNormalMap ? b.pointNormalTex : b.texture)], matSampler(b.pointNormalState)},
                                                        detailBinding(b.detail)};
            SDL_BindGPUFragmentSamplers(pass, 0, tb, 3);
            struct DU { float light[4], color[4], params[4], eye[4], ramp[16], detail[4]; } fu{};
            if (b.detail >= 0) fu.detail[0] = scene.details[size_t(b.detail)].scale[0], fu.detail[1] = scene.details[size_t(b.detail)].scale[1], fu.detail[2] = 1.0f;
            std::memcpy(fu.light, dl.pos, 12);
            fu.light[3] = dl.radius;
            std::memcpy(fu.color, dl.rgb, 12);
            fu.params[0] = float(int(b.alphaFunc));
            fu.params[1] = depthProbe ? 1.0f : 0.0f;
            fu.params[2] = b.pointBlend ? 1.0f : 0.0f;
            fu.params[3] = hasNormalMap ? 1.0f : 0.0f;
            fu.eye[0] = cam.pos.x, fu.eye[1] = cam.pos.y, fu.eye[2] = cam.pos.z;
            if (it.depthMax < 1.0f) { // camera-relative item (applyViewModelSpace): the light moves with the eye
                fu.light[0] -= cam.pos.x, fu.light[1] -= cam.pos.y, fu.light[2] -= cam.pos.z;
                fu.eye[0] = fu.eye[1] = fu.eye[2] = 0.0f;
            }
            for (int k = 0; k < 16; ++k) fu.ramp[k] = ramp[k] / 255.0f;
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
            unbindItem(it);
        };
        auto drawWater = [&](const Batch& b, const Mat4& model) {
            const WaterMat& wm = scene.waters[size_t(b.texture)];
            if (!wm.cube || !wm.normalMap) return;
            bindStream(0);
            SDL_BindGPUGraphicsPipeline(pass, waterPipelines[wm.blend ? 1 : 0][b.cull]);
            VU wu = vu;
            wu.model = model;
            SDL_PushGPUVertexUniformData(cmd, 0, &wu, sizeof(wu));
            // cubicEnvMap: sampler state 0x30 (clamp U/V); normalMap: 0x6 = linear min/mag, point mip filter, repeat (0x1003dd..e1b1).
            SDL_GPUTextureSamplerBinding wsb[2] = {{wm.cube, clampSampler}, {wm.normalMap, waterMapSampler}};
            SDL_BindGPUFragmentSamplers(pass, 0, wsb, 2);
            FragmentUniforms fu = {
                {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0},
                {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                {0, 0, 0, 0},
                {0, 0, 0, 0},
                {wm.color[0], wm.color[1], wm.color[2], 0}};
            applyFog(fu);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
        };
        // One walk of the list: the lit surfaces (technique type 6) or the emissive ones (0x18 / 0x15).
        auto drawListWalk = [&](bool emissive) {
            static const bool noMultiply = std::getenv("IW2_DEPTH_PROBE") && std::atoi(std::getenv("IW2_DEPTH_PROBE")) == 2;
            stream = -1;
            for (const ListRef& r : drawList) {
                const Batch& b = *r.batch;
                const Pass p = b.pass;
                if (b.emissive != emissive || (noMultiply && p == Pass::Multiply)) continue;
                if (r.item) { drawItemBatch(*r.item, b); continue; }
                if (p == Pass::Water) { drawWater(b, *r.model); continue; }
                const bool writesDepth = p == Pass::Opaque || p == Pass::AlphaTest;
                const float tieBias = assets::layerTieBias(writesDepth);
                if (b.specTex >= 0) {
                    if (!scene.tbn || !scene.specularity || !scene.lmWeightTex) continue;
                    bindStream(1);
                    SDL_BindGPUGraphicsPipeline(pass, specPipelines[int(p)][b.polygonOffset][b.cull]);
                    const float specColorScale = specColorScaleDvar();
                    struct SVU { VU base; float sun[4]; } svu{vu, {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0}};
                    svu.base.model = *r.model;
                    svu.base.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f + tieBias;
                    SDL_PushGPUVertexUniformData(cmd, 0, &svu, sizeof(svu));
                    const Lightmap& lm = scene.lightmaps[size_t(b.lightmap)];
                    SDL_GPUTextureSamplerBinding tex[8] = {{scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {lm.coeffs, clampSampler},
                                                           {lm.sun, clampSampler}, {scene.textures[size_t(b.normalTex)], matSampler(b.normalState)},
                                                           {scene.textures[size_t(b.specTex)], matSampler(b.specState)}, {scene.specularity, clampSampler}, {scene.lmWeightTex, clampSampler},
                                                           {scene.textures[b.detail >= 0 ? size_t(scene.details[size_t(b.detail)].tex) : 0],
                                                            matSampler(b.detail >= 0 ? scene.details[size_t(b.detail)].state : uint8_t(0))}};
                    SDL_BindGPUFragmentSamplers(pass, 0, tex, 8);
                    FragmentUniforms fu = {
                        {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, specColorScale},
                        {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                        {b.detail >= 0 ? scene.details[size_t(b.detail)].scale[0] : 0.0f, b.detail >= 0 ? scene.details[size_t(b.detail)].scale[1] : 0.0f, b.detail >= 0 ? 1.0f : 0.0f, 0.0f},
                        {alphaParam(b, p), 1.0f, lmScale, fullbright ? 1.0f : 0.0f},
                        {0, 0, 0, 0}};
                    applyFog(fu);
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                } else {
                    bindStream(0);
                    VU gu = vu;
                    gu.model = *r.model;
                    bindOffset(p, b.polygonOffset, b.cull, gu, tieBias);
                    const Lightmap& lm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
                    SDL_GPUTextureSamplerBinding tex[4] = {
                        {scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {lm.coeffs, clampSampler}, {lm.sun, clampSampler}, detailBinding(b.detail)};
                    SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);
                    FragmentUniforms fu = {
                        {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0},
                        {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                        {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                        {alphaParam(b, p), p == Pass::Multiply ? 4.0f : b.lightmap >= 0 ? 1.0f : (b.lightmap == -2 ? 2.0f : 0.0f), lmScale, fullbright ? 1.0f : 0.0f},
                        {0, 0, 0, 0}};
                    detailUniforms(fu, b.detail);
                    // Additive world surfaces fog through the fixed-function stage, whose colour is black for a destination blend of One
                    // (gfx 0x10035050; docs/spec/render-hints.md "Additive fog").
                    if (p != Pass::Multiply) applyFog(fu, p);
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                    if (p == Pass::Multiply && b.multiplyFog && fog.active) {
                        // effect_multiply.techset "unlit linear / exp fog" = vertcol_mul_lfog / _efog: pass 1 is the plain multiply
                        // (stateMap nofog), pass 2 (stateMap multiply_fog: blend Add One One, depth write off, depth test and polygon
                        // offset of the material) adds the fog the multiply took away (docs/spec/render-hints.md "Multiply decals in fog").
                        bindOffset(Pass::Additive, b.polygonOffset, b.cull, gu, tieBias);
                        fu.params[1] = 5.0f;
                        applyFog(fu);
                        SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                        SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                    }
                }
            }
            bindStream(0);
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
        };
        drawListWalk(false);
        if (resumedPass) { // glow_setup reads the copy too
            // Scene builder 0x10025490: the sky and "lightmap no fog" lists, then render command 0x19 (0x100310a0: StretchRect of
            // the back buffer into resolvedPostSun), then the effect lists. ($floatz was built at the start of the frame.)
            SDL_EndGPURenderPass(pass);
            {
                SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureLocation src{}, dst{};
                src.texture = sceneTex;
                dst.texture = postSunTex;
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
                SDL_EndGPUCopyPass(cp);
            }
            SDL_GPUColorTargetInfo again = color;
            again.load_op = SDL_GPU_LOADOP_LOAD;
            if (sceneSamples > 1) again.store_op = SDL_GPU_STOREOP_RESOLVE;
            SDL_GPUDepthStencilTargetInfo dsAgain = ds;
            dsAgain.load_op = SDL_GPU_LOADOP_LOAD;
            dsAgain.store_op = SDL_GPU_STOREOP_DONT_CARE;
            pass = SDL_BeginGPURenderPass(cmd, &again, 1, &dsAgain);
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
            postSunReady = true;
        }
        stream = -1;
        // Dynamic lights (0x10015210, emitted at 0x10025643): after the lit walk and the post-sun copy, one reversed walk per light, over every
        // surface whose technique set has a pointlight technique.
        {
            static const bool depthProbe = std::getenv("IW2_DEPTH_PROBE") != nullptr;
            if (depthProbe) {
                dlights.clear();
                dlights.push_back({{cam.pos.x, cam.pos.y, cam.pos.z}, 1e9f, {1, 0, 0}});
            }
            for (const DLight& dl : dlights)
                for (auto r = drawList.rbegin(); r != drawList.rend(); ++r) {
                    if (!assets::receivesDynamicLight(r->batch->pointLit, r->batch->fullbright)) continue;
                    if (r->item) drawItemDlight(dl, *r->item, *r->batch);
                    else {
                        bindStream(0);
                        drawDlightBatch(dl, *r->batch, *r->model);
                    }
                }
            bindStream(0);
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
        }
        drawListWalk(true);
        // Effect items: after every list surface.
        for (const Pass p : {Pass::Blend, Pass::Additive, Pass::Multiply}) {
            SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
            // Entity models of this pass. Effect items draw their emissive parts in the last steps (fxStep).
            for (const DrawItem& it : items) {
                if (!it.model) continue;
                bool bound = false;
                if (it.depthMax < 1.0f) {
                    const SDL_GPUViewport vpDepth{0, 0, float(w), float(h), 0.0f, it.depthMax};
                    SDL_SetGPUViewport(pass, &vpDepth);
                }
                for (const Batch& b : it.model->batches) {
                    if (b.pass != p || (!it.effects && b.fxKind == 0)) continue;
                    if (b.fxKind == 2 && !postSunReady) continue; // distortion needs the post-sun copy (Skip before, docs/spec/effects.md)
                    // r_zFeather 0: the effects use technique type 0x15 "emissive no fog" instead of 0x18 "emissive feathered" (the
                    // distortion keeps its float-z variant), i.e. the ordinary unfeathered draw below.
                    if (b.fxKind != 0 && postSunReady && (b.fxKind == 2 || zFeather)) {
                        // distortion_floatz.tech / zfeather(_add).tech: vertex uniforms + the material constant, samplers colorMap,
                        // resolvedPostSun / floatZ.
                        SDL_GPUBufferBinding mvb{it.model->vertices, 0};
                        SDL_BindGPUVertexBuffers(pass, 0, &mvb, 1);
                        SDL_GPUBufferBinding mib{it.model->indices, 0};
                        SDL_BindGPUIndexBuffer(pass, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                        bound = true;
                        SDL_BindGPUGraphicsPipeline(pass, b.fxKind == 2 ? distortionPipe : featherPipes[p == Pass::Additive ? 1 : 0]);
                        struct EVU { VU base; float parms[4]; float outMin[4]; float outInv[4]; } evu{vu, {b.fxParm[0], b.fxParm[1], 0, 0}, {}, {}};
                        evu.base.model = it.transform;
                        SDL_PushGPUVertexUniformData(cmd, 0, &evu, sizeof(evu));
                        if (b.fxKind == 2) {
                            SDL_GPUTextureSamplerBinding tex[3] = {{scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {postSunTex, clampSampler},
                                                                   {floatzTex, pointClampSampler}};
                            SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                        } else {
                            const bool outdoorOk = b.fxKind == 3 && outdoorTex;
                            SDL_GPUTextureSamplerBinding tex[3] = {{scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {floatzTex, pointClampSampler},
                                                                   {outdoorOk ? outdoorTex : floatzTex, pointClampSampler}};
                            SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                            evu.parms[1] = outdoorOk ? 1.0f : 0.0f;
                            if (outdoorOk) {
                                // worldOutdoorLookupMatrix (0x10020a50): p -> (p - mins) / (maxs - mins); outdoorFeatherParms (code constant 0xb2,
                                // 0x1070c320) = r_outdoorFeather (dvar 0x101ce6bc, registered at 0x1000b927 with default 8) in all four lanes
                                // (0x10033fd0 / 0x10034082).
                                const auto& mn = scene.outdoor.mins();
                                const auto& mx = scene.outdoor.maxs();
                                for (int k = 0; k < 3; ++k) evu.outMin[k] = mn[size_t(k)], evu.outInv[k] = mx[size_t(k)] - mn[size_t(k)] != 0 ? 1.0f / (mx[size_t(k)] - mn[size_t(k)]) : 0.0f;
                                evu.outMin[3] = 8.0f;
                            }
                            SDL_PushGPUVertexUniformData(cmd, 0, &evu, sizeof(evu));
                        }
                        // The effect list is drawn with technique type 0x18 "emissive feathered no fog" (scene builder, docs/spec/effects.md),
                        // so zfeather / distortion always run their no-fog variants (zfeather_lfog / _efog techniques are not even shipped).
                        FragmentUniforms fu{};
                        SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                        SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                        SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
                        VU mu = vu;
                        mu.model = it.transform;
                        if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
                        SDL_PushGPUVertexUniformData(cmd, 0, &mu, sizeof(mu));
                        continue;
                    }
                    if (!bound) {
                        SDL_GPUBufferBinding mvb{it.model->vertices, 0};
                        SDL_BindGPUVertexBuffers(pass, 0, &mvb, 1);
                        SDL_GPUBufferBinding mib{it.model->indices, 0};
                        SDL_BindGPUIndexBuffer(pass, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                        VU mu = vu;
                        mu.model = it.transform;
                        if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
                        SDL_PushGPUVertexUniformData(cmd, 0, &mu, sizeof(mu));
                        bound = true;
                    }
                    const Lightmap& elm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
                    SDL_GPUTextureSamplerBinding tex[4] = {
                        {scene.textures[size_t(b.texture)], matSampler(b.colorState)}, {elm.coeffs, clampSampler}, {elm.sun, clampSampler}, detailBinding(b.detail)};
                    SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);
                    FragmentUniforms fu = {
                        {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, it.hasProbe ? 1.0f : 0.0f},
                        {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                        {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                        {alphaParam(b, p), b.lightmap >= 0 ? 1.0f : 3.0f, lmScale, fullbright ? 1.0f : 0.0f},
                        {it.ambient[0], it.ambient[1], it.ambient[2], it.sunVisibility},
                        {b.scroll ? it.scrollV : 0.0f, 0, 0, 0}};
                    std::memcpy(fu.probe, it.probe, sizeof(fu.probe));
                    detailUniforms(fu, b.detail);
                    if (p != Pass::Multiply) applyFog(fu, p); // additive: black fog colour (render-hints.md); the multiply fix-up pass is world-only
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    if (b.polygonOffset || b.cull) {
                        VU mu = vu;
                        mu.model = it.transform;
                        if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
                        bindOffset(p, b.polygonOffset, b.cull, mu);
                    }
                    SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                    if (b.polygonOffset || b.cull) {
                        VU mu = vu;
                        mu.model = it.transform;
                        if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
                        bindOffset(p, 0, 0, mu);
                    }
                }
                if (it.depthMax < 1.0f) {
                    const SDL_GPUViewport vpFull{0, 0, float(w), float(h), 0.0f, 1.0f};
                    SDL_SetGPUViewport(pass, &vpFull);
                }
                if (bound) {
                    // Restore world buffers for the next pass's world batches.
                    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
                }
            }
        }
        // Objective technique (Pass::Objective, shaders fs_objective) after the other passes. gameTime is code constant 0x82,
        // stored at 0x1070c020 (code constants 0x80.. live at 0x1070c000 + (id - 0x80) * 16, 0x100394d0). 0x1002f6a0 sets it per
        // draw from t = scene time (0x1070c3bc, seconds) - the refEntity's material time (+0x5c, 0x1002fa49): gameTime =
        // (sin 2 pi f, cos 2 pi f, f, t) with f = t - floor(t). The cgame writes a material time only for vehicles (0x4ac839;
        // zero for other entities after the memset at 0x4ac7b0), so here gameTime.z = frac(game seconds) (the shader's sin has
        // period 1, so the fraction is the same pulse). TODO(spec): [walker position of this pass not traced] this pass's place in the sort order (drawn last here).
        {
            const Vec3 fwd = cam.forward();
            const struct { float forward[4]; } ou{{fwd.x, fwd.y, fwd.z, gameTimeSeconds}};
            auto drawObjective = [&](const Batch& b, const VU& u) {
                if (b.pass != Pass::Objective) return;
                SDL_BindGPUGraphicsPipeline(pass, objectivePipelines[std::clamp(b.polygonOffset, 0, 2)]);
                VU ov = u;
                ov.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f;
                SDL_PushGPUVertexUniformData(cmd, 0, &ov, sizeof(ov));
                SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, &ou, sizeof(ou));
                SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
            };
            for (const WorldGroup& wg : worldGroups) {
                VU gu = vu;
                gu.model = wg.model;
                for (const Batch& b : *wg.batches) drawObjective(b, gu);
            }
            for (const DrawItem& it : items) {
                if (!it.model) continue;
                bool any = false;
                for (const Batch& b : it.model->batches) any = any || b.pass == Pass::Objective;
                if (!any) continue;
                if (it.depthMax < 1.0f) {
                    const SDL_GPUViewport vpDepth{0, 0, float(w), float(h), 0.0f, it.depthMax};
                    SDL_SetGPUViewport(pass, &vpDepth);
                }
                SDL_GPUBufferBinding mvb{it.model->vertices, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &mvb, 1);
                SDL_GPUBufferBinding mib{it.model->indices, 0};
                SDL_BindGPUIndexBuffer(pass, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                VU mu = vu;
                mu.model = it.transform;
                if (it.depthMax < 1.0f) applyViewModelSpace(mu, cam, it, float(w) / float(h));
                for (const Batch& b : it.model->batches) drawObjective(b, mu);
                if (it.depthMax < 1.0f) {
                    const SDL_GPUViewport vpFull{0, 0, float(w), float(h), 0.0f, 1.0f};
                    SDL_SetGPUViewport(pass, &vpFull);
                }
                SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            }
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
        }
        if (nq && !post) drawOverlay(pass, cmd, scene, overlay, nq);
        SDL_EndGPURenderPass(pass);
        if (post) {
            // Screen filter chain, docs/spec/effects.md "Glow and the blurred screen". Passes alternate between the pingpong images, the
            // last one writes dst. A one-pass chain reading dst stays in the pingpong image (the result is returned).
            const uint32_t gw = std::max(w / 4, 1u), gh = std::max(h / 4, 1u);
            auto fullscreen = [&](SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* dst, const SDL_GPUTextureSamplerBinding* src, uint32_t nsrc, const void* gu, uint32_t size) {
                SDL_GPUColorTargetInfo gc{};
                gc.texture = dst;
                gc.load_op = SDL_GPU_LOADOP_DONT_CARE;
                gc.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* gp = SDL_BeginGPURenderPass(cmd, &gc, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(gp, pipe);
                SDL_BindGPUFragmentSamplers(gp, 0, src, nsrc);
                SDL_PushGPUFragmentUniformData(cmd, 0, gu, size);
                SDL_DrawGPUPrimitives(gp, 3, 1, 0, 0);
                SDL_EndGPURenderPass(gp);
            };
            auto filterChain = [&](SDL_GPUTexture* src, SDL_GPUTexture* dst, const std::vector<game::FilterPass>& plan, size_t done) {
                for (size_t i = 0; i < plan.size(); ++i) {
                    const size_t k = i + done;
                    SDL_GPUTexture* out = i + 1 == plan.size() && src != dst ? dst : pingpongTex[k & 1];
                    if (out == src) out = pingpongTex[(k + 1) & 1];
                    struct { float tap[8][4]; float count[4]; } fu{};
                    for (int t = 0; t < plan[i].count; ++t) {
                        const game::FilterTap& ft = plan[i].taps[size_t(t)];
                        fu.tap[t][0] = ft.x;
                        fu.tap[t][1] = ft.y;
                        fu.tap[t][3] = ft.w;
                    }
                    fu.count[0] = float(plan[i].count);
                    SDL_GPUTextureSamplerBinding fb{src, clampSampler};
                    fullscreen(glowBlurPipe, out, &fb, 1, &fu, sizeof(fu));
                    src = out;
                }
                return src;
            };
            SDL_GPUTexture* blurredTex = quarterTex;
            if (blurA > 0.0f) { // sigma in scene pixels; pixels are square
                const float sigma = float(h) * game::blurImageRadius(blur, float(h)) / 480.0f;
                blurredTex = filterChain(sceneTex, quarterTex, game::filterPlan(sigma, sigma, int(w), int(h), int(gw), int(gh)), 0);
            }
            const game::GlowDvars glowDvars;
            const bool worldSky = scene.sky != nullptr;
            const game::GlowChain glow = glowOn ? game::glowChain(glowDvars, worldSky, float(gw) / float(w)) : game::GlowChain{};
            SDL_GPUTexture* glowResult = nullptr;
            if (glow.images > 0) {
                const float setupU[16] = {1.0f / float(gw), 1.0f / float(gh), 0, 0, glowDvars.cutoff, 1.0f / (1.0f - glowDvars.cutoff), glowDvars.desaturation, 0};
                const float px = float(h) / 480.0f;
                auto plan = game::filterPlan(glow.stage[0] * px, glow.stage[0] * px, int(gw), int(gh), int(gw), int(gh));
                SDL_GPUTextureSamplerBinding sb[2] = {{sceneTex, clampSampler}, {postSunReady ? postSunTex : sceneTex, clampSampler}};
                SDL_GPUTexture* setupDst = plan.empty() ? glowTex : pingpongTex[0];
                fullscreen(glowSetupPipe, setupDst, sb, 2, setupU, sizeof(setupU));
                glowResult = plan.empty() ? glowTex : filterChain(setupDst, glowTex, plan, 1);
                if (glow.images > 1)
                    glowResult = filterChain(glowResult, glowTex, game::filterPlan(glow.stage[1] * px, glow.stage[1] * px, int(gw), int(gh), int(gw), int(gh)), 0);
            }
            SDL_GPUColorTargetInfo c2{};
            // Shock blend: compose into the persistent ghost texture, which then becomes next frame's blend source.
            SDL_GPUTexture* composeDst = ghostActive ? ghostTex[ghostCur] : target;
            c2.texture = composeDst;
            c2.load_op = SDL_GPU_LOADOP_DONT_CARE;
            c2.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo d2 = ds;
            d2.texture = depth; // the composite pass is single-sampled
            SDL_GPURenderPass* p2 = SDL_BeginGPURenderPass(cmd, &c2, 1, &d2);
            SDL_BindGPUGraphicsPipeline(p2, copyPipe);
            {
                SDL_GPUTextureSamplerBinding cb{sceneTex, clampSampler};
                SDL_BindGPUFragmentSamplers(p2, 0, &cb, 1);
                SDL_DrawGPUPrimitives(p2, 3, 1, 0, 0);
            }
            // Each image is applied once, alternating layers. The code sampler "sky" (0x10, 1.3 DLL switch 0x10038dd0 case 0x10038f4f) is
            // the world's sky image (world +0x20): without one the sky bleed is 0 and the bloom-only material draws (gfx 0x10030910).
            if (glowResult) {
                // glow_apply_sky_bleed.hlsl: skyCoords = nearPlaneOrg + (u - 0.5) * nearPlaneDx + (v - 0.5) * nearPlaneDy.
                const Vec3 f = cam.forward(), r = cam.right(), up = cross(r, f);
                const float ty = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) * 0.75f, tx = ty * float(w) / float(h);
                const Vec3 dx = r * (2.0f * tx), dy = up * (-2.0f * ty);
                for (int n = 0, l = glow.first; n < glow.images; ++n, l = 1 - l) {
                    const float sky = game::glowSkyBleed(glowDvars, l, worldSky);
                    const float gu[16] = {glowDvars.bloom[l], sky, 0, 0, f.x, f.y, f.z, 0, dx.x, dx.y, dx.z, 0, dy.x, dy.y, dy.z, 0};
                    SDL_BindGPUGraphicsPipeline(p2, sky != 0.0f ? glowApplyPipe : glowBloomPipe);
                    SDL_GPUTextureSamplerBinding ab[2] = {{glowResult, clampSampler}, {scene.sky, clampSampler}};
                    SDL_BindGPUFragmentSamplers(p2, 0, ab, sky != 0.0f ? 2 : 1);
                    SDL_PushGPUFragmentUniformData(cmd, 0, gu, sizeof(gu));
                    SDL_DrawGPUPrimitives(p2, 3, 1, 0, 0);
                }
            }
            // Material feedbackblend (passthru_vertalpha, SrcAlpha / InvSrcAlpha): colour from the image, alpha from the vertex only, so
            // the sky's zero scene alpha does not let the sharp scene through.
            if (blurA > 0.0f) {
                const float fa[4] = {std::round(blurA * 255.0f) / 255.0f, 0, 0, 0};
                SDL_BindGPUGraphicsPipeline(p2, feedbackBlendPipe);
                SDL_GPUTextureSamplerBinding fb{blurredTex, clampSampler};
                SDL_BindGPUFragmentSamplers(p2, 0, &fb, 1);
                SDL_PushGPUFragmentUniformData(cmd, 0, fa, sizeof(fa));
                SDL_DrawGPUPrimitives(p2, 3, 1, 0, 0);
            }
            auto bindBlend = [&]() {
                SDL_BindGPUGraphicsPipeline(p2, overlayPipe);
                struct VU2 { Mat4 vp; float eye[4]; Mat4 model; } ou{kIdentity, {0, 0, 0, 0}, kIdentity};
                SDL_PushGPUVertexUniformData(cmd, 0, &ou, sizeof(ou));
                SDL_GPUBufferBinding bvb{blurVerts, 0};
                SDL_BindGPUVertexBuffers(p2, 0, &bvb, 1);
                SDL_GPUBufferBinding bib{overlayIndices ? overlayIndices : nullptr, 0};
                if (!bib.buffer) bib.buffer = ensureOverlayIndices();
                SDL_BindGPUIndexBuffer(p2, &bib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                FragmentUniforms fu = {{0, 0, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 0, 0}};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            };
            bindBlend();
            // Order from the exe: the scene command (gfx 0x10030fc0) runs glow + blur (0x10030ed0) itself, the exe queues the shock
            // blend (cmd 0x1d) and the capture (cmd 0xa) after it (0x4bfccd), so the blurred scene is what gets blended and captured.
            if (ghostActive) {
                if (ghostValid && ghostElapsedMs >= 0.0f && ghostElapsedMs < ghostBlendMs) {
                    SDL_GPUTextureSamplerBinding tex[4] = {{ghostTex[1 - ghostCur], clampSampler},
                                                           {scene.dummyLightmap.coeffs, clampSampler},
                                                           {scene.dummyLightmap.sun, clampSampler},
                                                           {scene.textures[0], repeatSampler}};
                    SDL_BindGPUFragmentSamplers(p2, 0, tex, 4);
                    SDL_DrawGPUIndexedPrimitives(p2, 6, 1, 0, 2 * 4, 0);
                }
                // Capture (cmd 0xa) happens after the blend and before the 2D pass: end the pass, copy the composed frame to the target.
                SDL_EndGPURenderPass(p2);
                SDL_GPUColorTargetInfo c3{};
                c3.texture = target;
                c3.load_op = SDL_GPU_LOADOP_DONT_CARE;
                c3.store_op = SDL_GPU_STOREOP_STORE;
                p2 = SDL_BeginGPURenderPass(cmd, &c3, 1, &d2);
                SDL_BindGPUGraphicsPipeline(p2, copyPipe);
                SDL_GPUTextureSamplerBinding cb{ghostTex[ghostCur], clampSampler};
                SDL_BindGPUFragmentSamplers(p2, 0, &cb, 1);
                SDL_DrawGPUPrimitives(p2, 3, 1, 0, 0);
                bindBlend();
                ghostCur = 1 - ghostCur;
                ghostValid = true;
            } else {
                ghostValid = false; // flag 0xf7e8a4 cleared: the next shock starts with a capture only
            }
            if (nq) drawOverlay(p2, cmd, scene, overlay, nq);
            SDL_EndGPURenderPass(p2);
        }
    }

    SDL_GPUBuffer* ensureOverlayIndices() {
        if (!overlayIndices) {
            SDL_GPUBufferCreateInfo ib{};
            ib.usage = SDL_GPU_BUFFERUSAGE_INDEX;
            ib.size = kMaxQuads * 6 * 4;
            overlayIndices = SDL_CreateGPUBuffer(dev, &ib);
            std::vector<uint32_t> idx;
            for (uint32_t q = 0; q < kMaxQuads; ++q)
                for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(q * 4 + k);
            Uploader up(dev);
            up.buffer(overlayIndices, idx.data(), uint32_t(idx.size() * 4));
        }
        return overlayIndices;
    }

    void drawOverlay(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* cmd, const Scene& scene, const std::vector<Quad2D>& overlay, size_t nq) {
        {
            SDL_BindGPUGraphicsPipeline(pass, overlayPipe);
            struct VU2 { Mat4 vp; float eye[4]; Mat4 model; } ou{kIdentity, {0, 0, 0, 0}, kIdentity};
            SDL_PushGPUVertexUniformData(cmd, 0, &ou, sizeof(ou));
            SDL_GPUBufferBinding ovb{overlayVerts, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &ovb, 1);
            SDL_GPUBufferBinding oib{overlayIndices, 0};
            SDL_BindGPUIndexBuffer(pass, &oib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            FragmentUniforms fu = {{0, 0, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 0, 0}};
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            // Consecutive quads with the same texture share a draw call.
            size_t i = 0;
            while (i < nq) {
                size_t j = i + 1;
                while (j < nq && overlay[j].texture == overlay[i].texture) ++j;
                SDL_GPUTextureSamplerBinding tex[4] = {{scene.textures[size_t(overlay[i].texture)], clampSampler},
                                                       {scene.dummyLightmap.coeffs, clampSampler},
                                                       {scene.dummyLightmap.sun, clampSampler},
                                                       {scene.textures[0], repeatSampler}};
                SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);
                SDL_DrawGPUIndexedPrimitives(pass, uint32_t((j - i) * 6), 1, uint32_t(i * 6), 0, 0);
                i = j;
            }
        }
    }
};

// r_textureMode (default "bilinear", 0x1000abb6) and r_anisotropy (int 2..16, default 8, 0x1000abd1): the menu cvar store, else the defaults.
static void applyTextureFilter(Renderer& r, const std::map<std::string, std::string>& cvars) {
    assets::TextureMode mode = assets::TextureMode::Bilinear;
    for (const char* k : {"r_textureMode", "r_texturemode"})
        if (const auto it = cvars.find(k); it != cvars.end()) mode = assets::parseTextureMode(it->second);
    int aniso = assets::kDefaultAnisotropy;
    for (const char* k : {"r_anisotropy"})
        if (const auto it = cvars.find(k); it != cvars.end()) aniso = std::atoi(it->second.c_str());
    r.setTextureFilter(mode, aniso);
}

// Renders one frame into an offscreen target (the same draw as to the swapchain) and returns its pixels, top row first.
std::vector<uint8_t> captureRgba(SDL_GPUDevice* dev, Renderer& r, const Scene& scene, const Camera& cam, uint32_t w, uint32_t h,
                                 const std::vector<DrawItem>& items, const std::vector<Quad2D>& overlay, float blur) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(dev, &info);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!target || !tb) sdlFail("screenshot resources");

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    r.draw(cmd, target, w, h, scene, cam, items, overlay, blur);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = target;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);

    const auto* px = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(dev, tb, false));
    std::vector<uint8_t> out(px, px + size_t(w) * size_t(h) * 4);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTexture(dev, target);
    return out;
}

void screenshot(SDL_GPUDevice* dev, Renderer& r, const Scene& scene, const Camera& cam, uint32_t w, uint32_t h, const std::string& out,
                const std::vector<DrawItem>& items, const std::vector<Quad2D>& overlay = {}, float blur = 0.0f) {
    const std::vector<uint8_t> px = captureRgba(dev, r, scene, cam, w, h, items, overlay, blur);
    util::writePngRgba(out, px.data(), int(w), int(h));
    std::cout << "wrote " << out << "\n";
}

std::vector<float> parseFloats(const std::string& s) {
    std::vector<float> out;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) out.push_back(std::stof(item));
    return out;
}

} // namespace

// The engine's key names (as default.cfg binds them) for SDL scancodes; both shift, ctrl and alt keys count as one.
static std::string keyName(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, char('A' + (sc - SDL_SCANCODE_A)));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, char('1' + (sc - SDL_SCANCODE_1)));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return "F" + std::to_string(1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "SHIFT";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "CTRL";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "ALT";
    case SDL_SCANCODE_SPACE: return "SPACE";
    case SDL_SCANCODE_TAB: return "TAB";
    case SDL_SCANCODE_RETURN: return "ENTER";
    case SDL_SCANCODE_ESCAPE: return "ESCAPE";
    case SDL_SCANCODE_BACKSPACE: return "BACKSPACE";
    case SDL_SCANCODE_UP: return "UPARROW";
    case SDL_SCANCODE_DOWN: return "DOWNARROW";
    case SDL_SCANCODE_LEFT: return "LEFTARROW";
    case SDL_SCANCODE_RIGHT: return "RIGHTARROW";
    case SDL_SCANCODE_INSERT: return "INS";
    case SDL_SCANCODE_DELETE: return "DEL";
    case SDL_SCANCODE_PAGEUP: return "PGUP";
    case SDL_SCANCODE_PAGEDOWN: return "PGDN";
    case SDL_SCANCODE_HOME: return "HOME";
    case SDL_SCANCODE_END: return "END";
    case SDL_SCANCODE_PAUSE: return "PAUSE";
    case SDL_SCANCODE_GRAVE: return "`";
    case SDL_SCANCODE_MINUS: return "-";
    case SDL_SCANCODE_EQUALS: return "=";
    case SDL_SCANCODE_LEFTBRACKET: return "[";
    case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_SEMICOLON: return ";";
    case SDL_SCANCODE_APOSTROPHE: return "'";
    case SDL_SCANCODE_COMMA: return ",";
    case SDL_SCANCODE_PERIOD: return ".";
    case SDL_SCANCODE_SLASH: return "/";
    case SDL_SCANCODE_BACKSLASH: return "\\";
    default: return std::string();
    }
}

// Start without arguments (from Finder / the .app): stdout and stderr go to ~/Library/Logs/OpenIW2/openiw2.log (the previous
// log is kept as .1), the `main` folder is the one remembered in <pref>/settings.cfg or chosen in a folder dialog, and it is
// checked before it is mounted (src/fs/gamedir.h). Returns false when the user quits.
struct AppStart { std::string mainDir, language; std::filesystem::path pref; uint32_t width = 0, height = 0; };
// Where profiles, saves and screenshots live for a start that gave the game folder on the command line: the app's preference
// folder when the binary runs from inside an application bundle (so a Terminal start of the app sees the same profile as a
// Finder start), else next to the game folder (a development build).
static std::filesystem::path commandLineBase(const std::string& mainDir) {
    if (const char* base = SDL_GetBasePath(); base && ui::runsInAppBundle(base)) {
        if (char* pref = SDL_GetPrefPath("OpenIW2", "OpenIW2")) {
            std::filesystem::path p = pref;
            SDL_free(pref);
            return p;
        }
    }
    return std::filesystem::path(mainDir).parent_path();
}
static bool appStartup(AppStart& out) {
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path log = std::filesystem::path(home) / "Library/Logs/OpenIW2/openiw2.log";
        if (fs::rotateLog(log)) {
            std::freopen(log.string().c_str(), "w", stdout);
            std::freopen(log.string().c_str(), "a", stderr);
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << "SDL_Init: " << SDL_GetError() << "\n"; return false; }
    char* prefC = SDL_GetPrefPath("OpenIW2", "OpenIW2");
    if (!prefC) { std::cerr << "SDL_GetPrefPath: " << SDL_GetError() << "\n"; return false; }
    out.pref = prefC;
    SDL_free(prefC);
    const std::filesystem::path settings = out.pref / "settings.cfg";
    std::cout << "app start, preferences in " << out.pref.string() << "\n";

    std::optional<std::filesystem::path> dir = fs::readMainPath(settings);
    if (!dir && fs::checkGameDir(out.pref / "main").ok()) dir = out.pref / "main"; // a `main` folder put next to the settings
    bool explained = false;
    for (;;) {
        if (dir) {
            const fs::GameDirCheck c = fs::checkGameDir(*dir);
            if (c.ok()) {
                out.mainDir = dir->string();
                out.language = c.language;
                fs::writeMainPath(settings, *dir);
                break;
            }
            std::string text = "The folder\n" + dir->string() + "\nis not a complete Call of Duty 2 `main` folder. Missing:\n";
            for (size_t i = 0; i < c.missing.size() && i < 20; ++i) text += "  " + c.missing[i] + "\n";
            if (c.missing.size() > 20) text += "  ... and " + std::to_string(c.missing.size() - 20) + " more\n";
            text += "\nYou need your own Call of Duty 2 (PC) game files, patched to 1.3.";
            std::cerr << text << "\n";
            const SDL_MessageBoxButtonData buttons[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose Another Folder"},
                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
            };
            SDL_MessageBoxData mb{};
            mb.flags = SDL_MESSAGEBOX_WARNING;
            mb.title = "OpenIW2";
            mb.message = text.c_str();
            mb.numbuttons = 2;
            mb.buttons = buttons;
            int hit = 0;
            if (!SDL_ShowMessageBox(&mb, &hit) || hit != 1) return false;
            explained = true;
        }
        if (!explained) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "OpenIW2",
                                     "OpenIW2 plays Call of Duty 2 with your own game files.\n\nIn the next window, choose the `main` folder of "
                                     "your Call of Duty 2 (PC) installation, patched to 1.3. It contains iw_00.iwd ... iw_15.iwd and the "
                                     "localized_<language>_iw00.iwd ... files. OpenIW2 remembers the folder.",
                                     nullptr);
            explained = true;
        }
        struct Pick { bool done = false; std::optional<std::filesystem::path> path; } pick;
        SDL_ShowOpenFolderDialog(
            [](void* user, const char* const* list, int) {
                auto* p = static_cast<Pick*>(user);
                if (list && list[0]) p->path = std::filesystem::path(list[0]);
                p->done = true;
            },
            &pick, nullptr, dir ? dir->string().c_str() : nullptr, false);
        while (!pick.done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {}
            SDL_Delay(10);
        }
        if (!pick.path) return false; // cancelled
        dir = pick.path;
    }
    std::cout << "game folder: " << out.mainDir << ", language " << out.language << "\n";
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) out.width = uint32_t(m->w), out.height = uint32_t(m->h);
    return true;
}

// The dedicated server's console window (docs/spec/mp-ui.md "Dedicated start"): the original shows its Win32 console
// (log above, command line below) once the client is down; openiw2 draws a window of its own with SDL's renderer: the
// log (standard output, which still goes to the terminal too), the command line with history (up / down), page up /
// down and the wheel to scroll. Closing the window queues "quit" (as the console window's close does while
// "dedicated" is set, MP 0x468532). Without a display the server runs on without it.
static int runDedicatedConsole(iw2::fs::Vfs& vfs, iw2::netgame::DedicatedSettings settings) {
    settings.readStdin = isatty(0) != 0; // the stdin console only from a terminal (a Finder start has /dev/null)
    iw2::netgame::StdoutTee tee;
    iw2::netgame::ConsoleView view;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (SDL_Init(SDL_INIT_VIDEO)) {
        win = SDL_CreateWindow("OpenIW2 Dedicated Server", 900, 560, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (win) ren = SDL_CreateRenderer(win, nullptr);
        if (!ren) {
            if (win) SDL_DestroyWindow(win);
            win = nullptr;
            SDL_Quit();
        }
    }
    if (!win) std::cerr << "dedicated server: no console window (" << SDL_GetError() << "), log on stdout only\n";
    else {
        SDL_StartTextInput(win);
        tee.start();
    }
    uint64_t lastDraw = 0;
    settings.frontEnd = [&]() {
        std::vector<std::string> out;
        if (!win) return out;
        view.append(tee.take());
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) out.push_back("quit");
            else if (e.type == SDL_EVENT_TEXT_INPUT) view.insert(e.text.text);
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) view.scroll(int(e.wheel.y * 3));
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_RETURN: case SDLK_KP_ENTER: if (auto c = view.enter()) out.push_back(*c); break;
                case SDLK_BACKSPACE: view.backspace(); break;
                case SDLK_DELETE: view.del(); break;
                case SDLK_LEFT: view.left(); break;
                case SDLK_RIGHT: view.right(); break;
                case SDLK_HOME: view.home(); break;
                case SDLK_END: view.end(); break;
                case SDLK_UP: view.historyUp(); break;
                case SDLK_DOWN: view.historyDown(); break;
                case SDLK_PAGEUP: view.scroll(10); break;
                case SDLK_PAGEDOWN: view.scroll(-10); break;
                default: break;
                }
            }
        }
        const uint64_t now = SDL_GetTicks();
        if (now - lastDraw < 33) return out;
        lastDraw = now;
        int pw = 0, ph = 0;
        SDL_GetCurrentRenderOutputSize(ren, &pw, &ph);
        const float scale = std::max(2.0f, std::round(SDL_GetWindowPixelDensity(win) * 1.5f));
        SDL_SetRenderScale(ren, scale, scale);
        const float w = float(pw) / scale, h = float(ph) / scale, lineH = 10.0f, pad = 4.0f;
        SDL_SetRenderDrawColor(ren, 16, 16, 16, 255);
        SDL_RenderClear(ren);
        const float inputY = h - lineH - pad;
        const size_t rows = size_t(std::max(1.0f, (inputY - 2 * pad) / lineH));
        auto plain = [](const std::string& t) { // without ^N colour codes
            std::string r;
            for (size_t i = 0; i < t.size(); ++i) {
                if (t[i] == '^' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9') { ++i; continue; }
                r += t[i];
            }
            return r;
        };
        SDL_SetRenderDrawColor(ren, 210, 210, 210, 255);
        float y = pad;
        for (const auto& l : view.visible(rows)) {
            SDL_RenderDebugText(ren, pad, y, plain(l).c_str());
            y += lineH;
        }
        SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
        SDL_RenderLine(ren, 0, inputY - pad / 2, w, inputY - pad / 2);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDebugText(ren, pad, inputY, ("]" + view.input()).c_str());
        if ((now / 500) % 2 == 0) SDL_RenderDebugText(ren, pad + 8.0f * float(view.cursor() + 1), inputY + 1, "_");
        // debug: IW2_CONSOLE_SHOT=<file.bmp> writes the console window once, 3 s after it opened (docs/debug-flags.md)
        static const char* shotFile = std::getenv("IW2_CONSOLE_SHOT");
        static const uint64_t opened = now;
        if (shotFile && now - opened > 3000) {
            if (SDL_Surface* sf = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_SaveBMP(sf, shotFile);
                SDL_DestroySurface(sf);
            }
            shotFile = nullptr;
        }
        SDL_RenderPresent(ren);
        return out;
    };
    const int rc = iw2::netgame::runDedicated(vfs, settings);
    tee.stop();
    if (win) {
        SDL_StopTextInput(win);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
    }
    return rc;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered, so a log written to a file is current while playing
    std::vector<std::string> args;
    std::string shot;
    std::optional<Vec3> pos;
    std::optional<float> yaw, pitch;
    uint32_t width = 1600, height = 900;
    float walkSeconds = 0;
    float simSeconds = 0;
    float fireSeconds = 0;
    bool atExitMarks = false;
    float forcedBlur = 0; // --blur N: force the composed blur radius (debug)
    int shootMarks = 0; // --shoot N: with --shot, the player fires N bullets along the view (fanned 1° apart) before the shot
    bool audioOn = true;
    bool skipIntro = false; // --no-intro: no start-up movies in the front end
    bool menuMode = false; // --menu: start in the front end (main menu) without a level
    float shotViewHeight = 60;
    float shotLean = 0;
    bool shotAds = false;
    bool pendingF10 = false; // F10: god -> demigod -> off through the cheat commands
    bool pendingGod = false, pendingDemigod = false, pendingNotarget = false; // --god / --demigod / --notarget, F10 = god: the cheat commands (0x4e9f90 / 0x4ea030 / 0x4ea0d0; need sv_cheats)
    bool pendingNoclip = false, pendingUfo = false; // --noclip / --ufo: the cheat commands run once when the level is up (need sv_cheats)
    bool shotUse = false; // --shot: press F (+activate) after the simulated seconds
    std::string language; // empty: detected from the localized_<language>_*.iwd packs in the game directory (see below)
    bool scripts = true;
    std::string connectAddr; // +connect host[:port]: join a network game (docs/spec/net.md section 10)
    bool spawnAll = false;
    std::vector<std::pair<std::string, std::string>> extraCvars; // --cvar name=value (e.g. start=ride)
    const auto cheatsOn = [&extraCvars]() {
        bool on = false;
        for (const auto& [k, v] : extraCvars) if (k == "sv_cheats") on = std::atoi(v.c_str()) != 0;
        return on;
    };
    std::optional<std::array<float, 2>> rideDrive; // --ride F,R: with --shot, drive the player's vehicle (forward, right in -1..1) after --ride-after
    float rideAfter = 0;
    uint32_t rideButtons = 0; // --ride-fire sets the attack button
    int followVehicle = -2; // -2 off, -1 list only, N the Nth entity with vehicle state
    float followDist = 400.0f, followYaw = 180.0f, followPitch = 12.0f;
    std::optional<std::array<float, 2>> forceTurret, forceJolt;
    Renderer renderer;
    // No arguments (or only the -psn_ argument older macOS versions pass to apps): the app start (appStartup above).
    std::optional<AppStart> app;
    // Options only (`open OpenIW2.app --args --cvar sv_cheats=1`) also take the app start; the options are parsed below.
    const bool optionsOnly = argc >= 2 && std::string(argv[1]).rfind("--", 0) == 0;
    if (argc == 1 || (argc == 2 && std::string(argv[1]).rfind("-psn_", 0) == 0) || optionsOnly) {
        app.emplace();
        if (!appStartup(*app)) { SDL_Quit(); return 0; }
        args = {app->mainDir};
        language = app->language;
        menuMode = true;
        if (app->width && app->height) width = app->width, height = app->height;
        if (!optionsOnly) argc = 1;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--shot") shot = next();
            else if (a == "--follow-vehicle") followVehicle = std::stoi(next()); // debug: the shot is taken from behind vehicle N
            else if (a == "--cvar") { const std::string kv = next(); const auto eq = kv.find('='); extraCvars.emplace_back(kv.substr(0, eq), eq == std::string::npos ? "1" : kv.substr(eq + 1)); }
            else if (a == "--ride") { auto v = parseFloats(next()); rideDrive = {v.at(0), v.at(1)}; }
            else if (a == "--ride-after") rideAfter = std::stof(next());
            else if (a == "--ride-fire") rideButtons = 1;
            else if (a == "--follow-pitch") followPitch = std::stof(next());
            else if (a == "--force-turret") { auto v = parseFloats(next()); forceTurret = {v.at(0), v.at(1)}; } // debug: pitch,yaw of every vehicle
            else if (a == "--force-jolt") { auto v = parseFloats(next()); forceJolt = {v.at(0), v.at(1)}; } // debug: tag_body pitch,roll
            else if (a == "--follow-dist") followDist = std::stof(next());
            else if (a == "--follow-yaw") followYaw = std::stof(next()); // degrees added to the vehicle yaw (orbit)
            else if (a == "--pos") { auto v = parseFloats(next()); pos = Vec3{v.at(0), v.at(1), v.at(2)}; }
            else if (a == "--yaw") yaw = std::stof(next());
            else if (a == "--pitch") pitch = std::stof(next());
            else if (a == "--size") { const std::string s = next(); width = uint32_t(std::stoul(s)); height = uint32_t(std::stoul(s.substr(s.find('x') + 1))); }
            else if (a == "--lm-scale") renderer.lmScale = std::stof(next());
            else if (a == "--walk-seconds") walkSeconds = std::stof(next());
            else if (a == "--sim-seconds") simSeconds = std::stof(next());
            else if (a == "--fire-seconds") fireSeconds = std::stof(next());
            else if (a == "--blur") forcedBlur = std::stof(next());
            else if (a == "--shoot") shootMarks = std::stoi(next());
            else if (a == "--no-audio") audioOn = false;
            else if (a == "--god") pendingGod = true;
            else if (a == "--demigod") pendingDemigod = true;
            else if (a == "--notarget") pendingNotarget = true;
            else if (a == "--noclip") pendingNoclip = true;
            else if (a == "--ufo") pendingUfo = true;
            else if (a == "--stance") { const std::string st = next(); shotViewHeight = st == "prone" ? 11.0f : st == "crouch" ? 40.0f : 60.0f; }
            else if (a == "--ads") shotAds = true;
            else if (a == "--use") shotUse = true;
            else if (a == "--lean") shotLean = std::stof(next());
            else if (a == "--language") language = next();
            else if (a == "--no-scripts") scripts = false;
            else if (a == "--menu") menuMode = true;
            else if (a == "+connect" || a == "--connect") connectAddr = next();
            else if (a == "--no-intro") skipIntro = true;
            else if (a == "--spawn-all") spawnAll = true;
            else if (a == "--lm-weights") { auto v = parseFloats(next()); for (int k = 0; k < 4; ++k) renderer.lmWeights[k] = v.at(size_t(k)); }
            else args.push_back(a);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
    }
    if (args.size() < (menuMode || !connectAddr.empty() ? 1u : 2u)) {
        std::cerr << "usage: iw2view <main-dir> <map-name> [--shot out.png] [--pos x,y,z] [--yaw d] [--pitch d] [--size WxH]\n";
        return 2;
    }
    if (const std::string e = fs::mainDirError(args[0]); !e.empty()) { // a command-line path is not checked by the app start
        std::cerr << "iw2view: error: " << e << "\n";
        SDL_Quit();
        return 1;
    }

    // Network client (docs/spec/net.md section 10): the handshake runs before the level loads; the gamestate names the map.
    std::unique_ptr<net::GameClient> netClient;
    netgame::RemoteView remoteView;
    // Network killcam: the followed client's view between the drawn snapshots (RemoteView::killcamView), for the view
    // model and the HUD as in the local killcam.
    game::World::MpKillcamView netKillcam;
    net::LanBrowser lanBrowser;
    auto netNowMs = [] { return int64_t(SDL_GetTicksNS() / 1000000ull); };
    // A refused or lost connection (docs/spec/net.md section 14): back to the MP main menu with error_popmenu.
    std::optional<netgame::ErrorDialog> netError;
    int netConnection = 0; // counts the connections: per-connection state is keyed by it, not by the client's address
    if (!connectAddr.empty()) {
        net::NetAddress to;
        const bool resolved = net::resolveBlocking(connectAddr, to, net::kDefaultPort, net::Resolver::kTimeoutMs); // docs/spec/mp-ui.md "Addresses and name lookup"
        if (!resolved) {
            std::cerr << "error: bad address " << connectAddr << "\n";
            netError = netgame::errorDialog(false, "\x14" "EXE_BADSERVERADDRESS");
            menuMode = true;
        }
        else {
        netClient = std::make_unique<net::GameClient>();
        ++netConnection;
        netClient->name = ui::profilePlayerName((app ? app->pref : commandLineBase(args[0])) / "players", "");
        if (const char* n = std::getenv("IW2_NET_NAME")) netClient->name = n;
        for (const auto& [k, v] : extraCvars) if (k == "password") netClient->password = v; // --cvar password=...
        if (!netClient->connect(to, netNowMs())) {
            std::cerr << "error: " << netClient->error << "\n";
            return 1;
        }
        std::cout << "connecting to " << connectAddr << "\n";
        netClient->waitForHandshake(netNowMs, 20);
        if (netClient->state() == net::ClientState::Disconnected) {
            std::cerr << "error: " << netClient->error << "\n";
            netError = netgame::errorDialog(netClient->dropFromServer, netClient->dropReason);
            netClient.reset();
            menuMode = true;
        }
        }
    }
    if (netClient) {
        const std::string map = net::infoValueForKey(netClient->configstrings[0], "mapname");
        std::cout << "connected as client " << netClient->clientNum << ", map " << map << "\n";
        args.resize(1);
        args.push_back("mp/" + map);
        menuMode = false;
    }

    try {
        fs::Vfs vfs;
        vfs.mountDirectory(args[0]);
        if (language.empty()) { // the shipped game keeps one localized_<language>_iw*.iwd set per install
            language = "english";
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(args[0], ec)) {
                const std::string n = e.path().filename().string();
                if (n.rfind("localized_", 0) != 0) continue;
                const size_t end = n.find("_iw", 10);
                if (end != std::string::npos) { language = n.substr(10, end - 10); break; }
            }
        }
        if (const char* overlay = std::getenv("IW2_OVERLAY")) vfs.mountDirectory(overlay); // debug: loose test scripts override the game files

        if (!SDL_Init(SDL_INIT_VIDEO | (shot.empty() && audioOn ? SDL_INIT_AUDIO : 0))) sdlFail("SDL_Init");

        // Sound: aliases for this level, a software mixer and an SDL audio stream that pulls from it.
        audio::SoundAliases aliases;
        if (menuMode) aliases.load(vfs, "menu"); // front-end sounds (loadspec "menu")
        else if (args[1].rfind("mp/", 0) == 0) aliases.loadLevel(vfs, args[1].substr(3), "all_mp"); // MP: the bare map name, mode tag all_mp (docs/spec/mp.md, sound.md)
        else aliases.loadLevel(vfs, args[1]); // plus the interface rows: the pause menu's music and clicks work on a direct level start
        constexpr int kAudioRate = 48000;
        audio::Mixer mixer(vfs, aliases, kAudioRate);
        mixer.logPlays = std::getenv("IW2_SOUND_LOG") != nullptr;
        mixer.logPositions = std::getenv("IW2_SOUND_LOG") && std::atoi(std::getenv("IW2_SOUND_LOG")) >= 2; // [snd-pos] lines // debug: one [snd] line per sound request (docs/debug-flags.md)
        SDL_AudioStream* audioStream = nullptr;
        if (shot.empty() && audioOn) {
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kAudioRate};
            audioStream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                [](void* user, SDL_AudioStream* stream, int additional, int) {
                    auto* m = static_cast<audio::Mixer*>(user);
                    std::vector<float> buf(size_t(additional / int(sizeof(float))) & ~size_t(1));
                    if (buf.empty()) return;
                    m->mix(buf.data(), int(buf.size() / 2));
                    SDL_PutAudioStreamData(stream, buf.data(), int(buf.size() * sizeof(float)));
                },
                &mixer);
            if (audioStream) SDL_ResumeAudioStreamDevice(audioStream);
            else std::cerr << "warning: no audio: " << SDL_GetError() << "\n";
        }
        std::cout << "sound aliases: " << aliases.size() << "\n";
        SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
        if (!dev) sdlFail("SDL_CreateGPUDevice");

        // Key bindings: the game's default.cfg (exec'd at startup; default_localize.cfg only changes the console key).
        game::ClientInput input;
        if (const auto cfg = vfs.read("default.cfg")) input.loadConfig(std::string(cfg->begin(), cfg->end()));
        else std::cerr << "warning: default.cfg not found, no key bindings\n";
        std::optional<std::string> spBindings; // the SP key table while an MP world has the default_mp.cfg one
        // The shipped default.cfg binds F12 to screenshotJpeg, but OpenIW2's F12 is the frame-rate counter (see the event
        // loop), so as a deliberate, documented deviation (README Play; docs/spec/controls.md, Screenshots) the screenshot
        // key moves to F11 whenever the loaded bindings keep it on F12. The user can rebind it in the controls menu
        // (the screenshot row of the interact page); a saved F11 binding is left alone.
        auto moveScreenshotKeyOffF12 = [&] {
            std::string cmd = input.commandFromKey("F12");
            for (char& ch : cmd) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (cmd != "screenshotjpeg") return;
            input.loadConfig("unbind F12\nbind F11 \"screenshotJpeg\"\n");
        };
        moveScreenshotKeyOffF12();
        // A level's game world. game[] survives only a persistent level change (changelevel / missionsuccess with persistent set:
        // G_ShutdownGame 0x4d9d3c -> 0x46e1c0 -> 0x4689e0 frees the game object 0x9bef30 otherwise, and G_InitGame 0x4da481 creates
        // a new one); then its fields are copied into the new world before its script starts (maps/_loadout.gsc "gaveweapons").
        struct Subtitle { std::string key; uint64_t startNs; float soundSec; }; // shown for max(soundSec, cg_subtitleMinTime)
        std::vector<Subtitle> subtitles; // cg subtitle lines (cgame 0x4a3540 → console channel → 0x4bf2e0)
        // HUD fade "touch" times (world ms; 0 = never touched = hidden when the fade dvar is non-zero): ammo display 0xf79a40 and
        // compass 0xf79a38 (touched by 0x4a6130 / 0x4be5f0 / weapon changes 0x4b76f2, 0x4b8391), and the weapon change time 0xf796e4.
        game::CompassSpring compassSpring; // angle spring of the compass face (0x4b09a0)
        game::CompassSpring pointerSpring; // springy pointer yaw (0x4b0ce0, cg_hudCompassSpringyPointers)
        game::CursorHintFade cursorHint; // the held cursor hint and its fade; one state for the icon and the text
        struct HudFade { int64_t compass = 0, ammo = 0, weaponChange = 0, flashStart = 0, stance = 0, offhand = 0, stanceChange = 0, health = 0, objectives = 0, healthTrailTimer = 0, healthPulse = 0, lastHudMs = 0; float healthTrail = 0; uint32_t stanceState = 0; std::string weapon; float pitch = 0, yaw = 0; uint32_t buttons = 0; bool objectivesHeld = false; } hudFade;
        // MP scoreboard state (docs/spec/mp-scoreboard.md): scroll line, the team block drawn first on equal scores (0x5ddad0,
        // initially allies), the release time for the 100 ms fade, the last "b" command of a network server and the time of
        // the last "score" request (sent at most every 2000 ms while shown, 0x4dabf0).
        struct MpScoreboardState { int scroll = 0, firstTeam = 2; bool shown = false; int64_t hiddenAt = -1000000; std::string netScores; game::MpScoreRequest request; } mpSb;
        bool netControlsFrozen = false; // network client: the server set pm_flags 0x8000 (freezecontrols): no look, no move or weapon buttons
        bool mpIntermissionView = false; // the local client's session state is intermission (pm_type 5)
        std::map<std::string, std::string> uiCvars; // client-side (menu) cvars, kept across levels and handed to each new world
        // Player profile: <game dir>/players/<name>/config.cfg (exe strings players/%s/, config.cfg; TODO(spec): [app folder is a deliberate choice, exe default not traced] the real base path
        // and the profile name when none is chosen, here "default").
        // The app start keeps profiles and saves in its preference folder, so the game folder may stay read-only.
        const std::filesystem::path prefBase = app ? app->pref : commandLineBase(args[0]);
        const std::filesystem::path playersDir = prefBase / "players";
        // Screenshots (screenshotJpeg / screenshot) go under the same writable base as profiles and saves, never into the game folder.
        const std::filesystem::path shotBase = prefBase;
        std::string profileName = "default";
        if (std::ifstream af(playersDir / "active.txt"); af) { std::string n; std::getline(af, n); if (!n.empty()) profileName = n; }
        std::filesystem::path profileCfg = playersDir / profileName / "config.cfg";
        bool profileDirty = false;
        const auto aaSamples = [&] { const auto it = uiCvars.find("r_aasamples"); return assets::parseAaSamples(it == uiCvars.end() ? "" : it->second); }; // read once, at renderer start
        ui::ProfileList profiles(playersDir); // FEEDER_PLAYER_PROFILES (src/ui/profilelist.h)
        if (std::ifstream cf(profileCfg); cf) {
            const std::string cfgText(std::istreambuf_iterator<char>(cf), {});
            uiCvars = ui::parseConfig(cfgText);
            if (cfgText.find("unbindall") != std::string::npos) input.loadConfig(cfgText); // saved key bindings replace the defaults
        }
        moveScreenshotKeyOffF12(); // a profile saved by the original game still carries F12
        for (const auto& [k, v] : extraCvars) if (k == "r_gamma" || k == "r_ignorehwgamma" || k == "r_texturemode" || k == "r_anisotropy" || k == "r_aasamples") uiCvars[k] = v; // renderer dvars live in the menu store
        for (const char* k : {"snd_slavefadetime", "snd_slaveFadeTime"}) if (uiCvars.count(k)) mixer.slaveFadeMs = std::clamp(std::atoi(uiCvars[k].c_str()), 0, 5000);
        mixer.masterVolume = uiCvars.count("snd_volume") ? std::clamp(float(std::atof(uiCvars["snd_volume"].c_str())), 0.0f, 1.0f) * 0.75f : mixer.masterVolume;
        // Debug IW2_START_TRACE=1: level start timing (real ms since this point, level time, sound starts, load end, first frames).
        const bool startTrace = std::getenv("IW2_START_TRACE") != nullptr;
        const uint64_t traceT0 = SDL_GetTicksNS();
        bool firstFramePending = false; // the first frame after a load (pipeline and upload warm-up) is not frame time either
        int traceFramesAfterLoad = -1; // >= 0: counting presented frames since the load ended
        auto trace = [&](const std::string& what, int64_t levelMs) {
            if (startTrace) std::cout << "[start-trace] real " << (SDL_GetTicksNS() - traceT0) / 1000000ull << " ms, level " << levelMs << " ms: " << what << "\n";
        };
        // Bots of a listen server started from the multiplayer create-server screen (openiw2 extension, docs/spec/mp-ui.md):
        // {count, skill}; IW2_MP_BOTS / IW2_MP_BOT_SKILL take precedence.
        std::optional<std::pair<int, int>> mpMenuBots;
        // Dedicated start (docs/spec/mp-ui.md "Dedicated start"): StartServer with the dedicated choice ends the client; the
        // process goes on as the server with this console window after the window, renderer and audio are gone.
        std::optional<netgame::DedicatedSettings> dedicatedStart;
        int worldGeneration = 0; // counts the levels built; a new level may reuse the old World's address
        auto makeWorld = [&](const std::string& map, const script::ObjectPtr& carriedGame, const game::LoadedSave* save = nullptr,
                             std::optional<game::World::CarriedPlayer> carriedPlayer = std::nullopt) {
            ++worldGeneration;
            mixer.setLoading(true); // the level's sounds wait for the end of the load (see Mixer::setLoading)
            // mp/<map>: offline multiplayer (docs/spec/mp.md), gametype from IW2_MP_GAMETYPE (default dm).
            const bool mp = map.rfind("mp/", 0) == 0;
            auto w = std::make_unique<game::World>(vfs, mp ? map.substr(3) : map, mp);
            trace("world created", 0);
            for (const auto& [k, v] : uiCvars) w->cvars[k] = v;
            for (const auto& [k, v] : extraCvars) w->cvars[k] = v;
            w->print = [](const std::string& m) { std::cout << "[script] " << m << "\n"; };
            w->playSound = [&mixer, &subtitles, &trace, wp = w.get()](const std::string& alias, const game::Vec3* origin, float* duration, int owner) {
                float dur = 0;
                // The level script's first sounds start before the client has a view (the map load): the first cgame frame (0x402ac0)
                // plays them with the listener at the first snapshot's view, so the player's spawn stands in for it; at the origin
                // a dialogue line would be culled by distance (0x4424e6) and the script would see no sound at all.
                if (mixer.loading() && wp->player()) {
                    const script::Value o = wp->player()->obj->get("origin"), an = wp->player()->obj->get("angles");
                    if (o.isVector()) {
                        const float yaw = (an.isVector() ? an.asVector()[1] : 0.0f) * float(M_PI) / 180.0f;
                        const script::Vec3 p = o.asVector();
                        mixer.setListener({p[0], p[1], p[2]}, {std::cos(yaw), std::sin(yaw), 0}, {std::sin(yaw), -std::cos(yaw), 0});
                    }
                }
                trace("sound " + alias, wp->timeMs()); // [server time]
                const int handle = mixer.play(alias, origin ? std::optional<audio::Vec3>(*origin) : std::nullopt, &dur, {.owner = owner});
                if (duration) *duration = dur;
                trace("  -> handle " + std::to_string(handle) + ", length " + std::to_string(dur) + " s", wp->timeMs()); // [server time]
                // A started alias with a subtitle column shows its caption for max(sound length, cg_subtitleMinTime 3 s) (0x4a3540).
                if (handle && !mixer.lastSubtitle().empty()) subtitles.push_back({mixer.lastSubtitle(), SDL_GetTicksNS(), dur});
                return handle;
            };
            w->stopSound = [&mixer](int h) { mixer.stop(h); };
            w->soundExists = [&aliases](const std::string& n) { return aliases.exists(n); };
            w->playBlendSound = [&mixer](const std::string& lo, const std::string& hi, float f, const game::Vec3& o, int owner) {
                return mixer.playBlend(lo, hi, f, audio::Vec3{o[0], o[1], o[2]}, owner);
            };
            w->setBlendSound = [&mixer](int h, float f, const game::Vec3& o) { return mixer.setBlend(h, f, audio::Vec3{o[0], o[1], o[2]}); };
            if (carriedGame)
                for (const auto& [k, v] : carriedGame->fields) w->vm().game()->set(k, v);
            if (carriedPlayer) w->setCarriedPlayer(std::move(*carriedPlayer));
            w->clientInput = &input; // buttonpressed / getcommandfromkey read its key table
            // The bindings from default.cfg, reported to scripts through getkeybinding.
            for (const auto& [key, command] : input.bindings()) w->keyBindings[command].push_back(key);
            if (save) { // savegame load: map entities, then the saved state instead of the level script (docs/spec/savegame.md 5)
                w->prepareStart();
                game::restoreSave(*w, *save);
            } else if (mp) {
                if (netClient) { // network client: the server runs the gametype (docs/spec/net.md section 10)
                    w->mpStartRemote();
                    std::vector<std::string> menus;
                    for (int i = netgame::kMenuConfigstrings; netClient->configstrings.count(i); ++i) menus.push_back(netClient->configstrings[i]);
                    w->mpSetMenuNames(std::move(menus));
                } else {
                // Gametype: IW2_MP_GAMETYPE, else g_gametype (set by the create-server StartServer), else "dm" (its registered default).
                const auto gtc = uiCvars.find("g_gametype");
                w->mpStart(std::getenv("IW2_MP_GAMETYPE") ? std::getenv("IW2_MP_GAMETYPE")
                           : gtc != uiCvars.end() && !gtc->second.empty() ? gtc->second.c_str() : "dm");
                }
                // MP key bindings (default_mp.cfg: x = "openscriptmenu ingame changeweapon", TAB = +scores, ESC = togglemenu).
                // TODO(spec): the MP profile's own saved config.
                if (const auto cfg = vfs.read("default_mp.cfg")) {
                    if (!spBindings) spBindings = input.formatBindings();
                    input.loadConfig(std::string(cfg->begin(), cfg->end()));
                    w->keyBindings.clear();
                    for (const auto& [key, command] : input.bindings()) w->keyBindings[command].push_back(key);
                }
                if (!netClient) {
                const int c = w->mpConnect("player"); // the local client
                w->mpBegin(c);
                w->frame();
                // The gametype opens its menus (serverinfo, team, weapon) for the local client; IW2_MP_AUTOJOIN=1 skips them.
                if (std::getenv("IW2_MP_AUTOJOIN") && std::atoi(std::getenv("IW2_MP_AUTOJOIN")) != 0)
                    w->mpAutoJoin(c, std::getenv("IW2_MP_WEAPON") ? std::getenv("IW2_MP_WEAPON") : "", // debug: IW2_MP_WEAPON=kar98k_mp,
                                  std::getenv("IW2_MP_TEAM") ? std::getenv("IW2_MP_TEAM") : "");              // IW2_MP_TEAM=allies
                // IW2_MP_BOTS=N adds N bots (docs/spec/mp-bots.md), IW2_MP_BOT_SKILL 0..3 their skill; one connect per frame.
                const char* nb = std::getenv("IW2_MP_BOTS");
                if (nb || mpMenuBots) {
                    if (const char* sk = std::getenv("IW2_MP_BOT_SKILL")) w->mpSetBotSkill(std::atoi(sk));
                    else if (!nb && mpMenuBots) w->mpSetBotSkill(mpMenuBots->second);
                    for (int i = 0, n = nb ? std::atoi(nb) : mpMenuBots->first; i < n; ++i) {
                        const int bot = w->mpAddBot();
                        if (bot < 0) break;
                        // Debug: IW2_MP_BOT_COMBAT=0 bots never shoot (controlled kill / killcam shots with IW2_SHOT_MP_KILL).
                        if (const char* bc = std::getenv("IW2_MP_BOT_COMBAT"); bc && std::atoi(bc) == 0) w->mpBotSetCombat(bot, false);
                        w->frame();
                    }
                }
                // Debug: IW2_MP_SAY="text|text..." says each line from the local client (as typed: 0x15 + text); IW2_MP_QUICK=
                // "<menu> <response>" sends a quick-message menu response (docs/spec/mp-chat.md, docs/debug-flags.md).
                if (const char* say = std::getenv("IW2_MP_SAY")) {
                    const std::string all = say;
                    for (size_t p = 0; p <= all.size();) {
                        const size_t q = std::min(all.find('|', p), all.size());
                        if (q > p) w->mpSay(c, 0, "\x15" + all.substr(p, q - p));
                        p = q + 1;
                    }
                }
                if (const char* qm = std::getenv("IW2_MP_QUICK")) {
                    std::istringstream in(qm);
                    std::string menu, response;
                    in >> menu >> response;
                    w->mpMenuResponse(c, menu, response);
                    w->frame();
                }
                }
            } else {
                if (spBindings) { // back from MP: the SP key table again
                    input.loadConfig(*spBindings);
                    spBindings.reset();
                    w->keyBindings.clear();
                    for (const auto& [key, command] : input.bindings()) w->keyBindings[command].push_back(key);
                }
                w->start();
            }
            trace("world started", w->timeMs()); // [server time]
            return w;
        };
        std::string currentMap = menuMode ? std::string() : args[1];
        std::unique_ptr<game::World> world;
        // Listen server (docs/spec/net.md section 11): a local MP game started from the menu (createserver) takes network clients,
        // a map started otherwise only with IW2_LISTEN=1; port net_port / IW2_LISTEN_PORT, default 28960.
        bool listenWanted = std::getenv("IW2_LISTEN") && std::atoi(std::getenv("IW2_LISTEN")) != 0;
        std::unique_ptr<netgame::WorldGame> listenGame;
        std::unique_ptr<net::Server> listenServer;
        int listenWorldGeneration = -1; // the level the listen server serves
        if (scripts && !menuMode) {
            world = makeWorld(currentMap, nullptr);
            if (spawnAll) std::cout << "spawned " << world->spawnAllActors() << " actors\n";
        }
        Scene scene = menuMode ? emptyScene(dev) : loadScene(dev, vfs, args[1], !world);
        // Objective and other on-screen messages from the level script (client commands obj_update/obj_complete).
        using game::ScreenMessage;
        std::vector<ScreenMessage> screenMessages;
        // MP centre print (0x4c8020 / 0x4c8360, docs/spec/mp-hud.md): text (with 0x14 / 0x15 segments) and its start (world ms).
        std::string mpCenterText;
        int64_t mpCenterStartMs = -1;
        // MP crosshair name (0x4c97c0): the client the crosshair trace last hit and when (world ms).
        int mpCrossClient = -1;
        int64_t mpCrossMs = -1;
        // MP chat (docs/spec/mp-chat.md): the chat lines (client 0x4d0e10) and the message field (chatmodepublic / chatmodeteam).
        game::MpChatBuffer chatBuffer;
        struct ChatField { bool open = false, team = false; std::string text; } chatField;
        // Death screen client state (docs/spec/campaign.md "Death screen"): opendeadscreen start (world time,
        // 0xf7966c), time_slow (real ms, 0xf7965c..0xf79668 → frame scale 0x747edc).
        int64_t deadScreenMs = 0;
        uint64_t slowStartNs = 0, slowEndNs = 0;
        audio::ClientSoundHandles clientSnd; // mu_play voice (cgame music slot 0); ambient voice (slots 1/2, 0x443080); shell shock and heartbeat loops
        float slowFrom = 1, slowTo = 1, slowmo = 1;
        game::ScreenFade screenFade; // scr_fade (0x4bec70, 0x4bda70)
        // The "fog" server command (client 0x4a5770) goes to World::clientFog: the renderer fog state that a savegame carries.
        game::ScreenBlur screenBlur; // scr_blur / clear_blur (0x4beb80, 0x4bebf0), updated per frame (0x4beea0)

        // Effects started by scripts (playfx & co.), simulated by FxSystem and drawn as sprites.
        game::FxSystem fxSystem;
        {
            auto cache = std::make_shared<std::unordered_map<std::string, std::pair<int, int>>>();
            fxSystem.atlas = [cache, &vfs](const std::string& name) {
                auto it = cache->find(name);
                if (it == cache->end()) {
                    std::pair<int, int> rc{1, 1};
                    if (const auto md = vfs.read("materials/" + name); md && md->size() > 0x10) rc = {(*md)[0xe], (*md)[0xf]};
                    it = cache->emplace(name, rc).first;
                }
                return it->second;
            };
        }
        std::map<std::string, std::shared_ptr<const assets::EfxEffect>> efxCache;
        auto efx = [&](const std::string& path) -> std::shared_ptr<const assets::EfxEffect> {
            auto [it, inserted] = efxCache.try_emplace(path);
            if (inserted) {
                try {
                    if (path.rfind("file:", 0) == 0) { // IW2_FX_TEST=file:<local path>: a test effect from disk (debug only)
                        std::ifstream in(path.substr(5), std::ios::binary);
                        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        if (!text.empty()) it->second = std::make_shared<assets::EfxEffect>(assets::parseEfx(text));
                    } else if (const auto d = vfs.read(path))
                        it->second = std::make_shared<assets::EfxEffect>(
                            assets::parseEfx(std::string_view(reinterpret_cast<const char*>(d->data()), d->size())));
                } catch (const std::exception& e) {
                    std::cerr << "warning: effect " << path << ": " << e.what() << "\n";
                }
            }
            return it->second;
        };
        fxSystem.loader = efx;
        // usePhysics elements trace the world (0x486f70 -> CM_BoxTrace 0x417640: model 0, contents 1, the useBBox box).
        fxSystem.trace = [&world](const game::FVec& a, const game::FVec& b, const game::FVec& mins, const game::FVec& maxs, float& frac, game::FVec& n) {
            if (!world) return false;
            const game::TraceResult tr = world->collision().traceWorld(a, b, mins, maxs, game::Contents::Solid);
            if (tr.startSolid || tr.allSolid || tr.fraction >= 1.0f) return false;
            frac = tr.fraction;
            n = tr.normal;
            return true;
        };
        Vec3 fxRight{0, -1, 0}, fxUp{0, 0, 1};
        // The view weapon's muzzle flash (0x4b6c37): a shot sets the fire flag (+0x142, 0x4b7800); the next view model update
        // plays viewFlashEffect (0xf48e44) bolted to the view model's tag_flash (0x4b6660, edx = 1).
        struct ViewFlash { bool pending = false, valid = false; game::FVec pos{}; game::FxAxes axes{}; bool brassPending = false, brassValid = false; game::FVec brassPos{}; game::FxAxes brassAxes{}; };
        auto viewFlash = std::make_shared<ViewFlash>();

        // Entity models currently visible in the game world.
        // Client interpolation state: entity origin/angles before the latest server frame and the fraction of the
        // next frame already elapsed (cg.frameInterpolation).
        std::map<int, std::pair<script::Vec3, script::Vec3>> lerpPrev;
        // Anim state before the last server frame: the drawn pose is that state advanced by the interpolation fraction, so poses move
        // smoothly with the lerped origin instead of stepping at 20 Hz (the original advances anim time per client frame, 0x4afb70).
        std::map<int, game::AnimState> prevAnim;
        std::map<int, std::pair<float, float>> prevTurret; // turret pitch/yaw before the last server frame (drawn interpolated)
        std::map<int, int> prevTexScroll; // vehicle tread accumulator (entityState +0x5c) before the last server frame
        float lerpFrac = 1.0f;
        // The client's per-frame clock (cg.time 0xf708dc, docs/spec/net.md section 10 "Frame clock"): everything drawn that moves
        // continuously reads it. It always pairs the world time with the fraction stored by the same frame's tick loop, so it gives
        // the same value before the loop (the camera reads last frame's pair) as at the end of the previous frame and never goes
        // back across a tick. It runs up to one server frame behind the world time: stamp on this clock whatever is read on it; a
        // stamp from the game (level time) is read through game::elapsedMs. Everything the client draws reads it: the view impulses
        // (0x4ae3ce, 0x4ae62f, 0x4addb1), the HUD fades and hudelems (0x4a9bd0, 0x4c1f30), fog (0x4a5770), screen fade (0x4bda70),
        // shellshock (0x4b24a0) and the refdef time the renderer uses (0x4afc99). It advances every drawn frame, so a visual evaluated
        // at the 20 Hz server time moves in 50 ms steps. Read the server time in this file only for simulation and debug output,
        // marked "[server time]" on the line (checked by test_renderclock).
        auto cgNowMs = [&]() -> int64_t { return world ? game::clientRenderTimeMs(world->timeMs(), lerpFrac) : 0; };
        std::optional<int64_t> netClockOffset; // network client: server time to this clock (game::heldClockOffset)
        // The effect system's time of this rendered frame: cg.time (0xf708dc) fed to the system each frame by 0x486b30 (system +0x0,
        // advanced copy +0x4 that plays read, e.g. 0x4855e4) before the view weapon is added (0x4afd17, then 0x4b026f). Both the view
        // flash play (0x4b6c37 -> 0x4853f0, no time argument) and the particle draw use it. The shot-script turn steps override it.
        std::optional<int64_t> fxTimeOverride;
        // r_swapInterval (bool, default 0, latched; gfx 0x100115a3: the D3D presentation interval is 1 when set, else immediate):
        // applied when the window is created and on vid_restart.
        bool presentModeDirty = true;
        auto fxTime = [&]() -> int64_t {
            if (fxTimeOverride) return *fxTimeOverride;
            return cgNowMs();
        };
        // The player's turret pose of this rendered frame (pitch, yaw), from the pre-kick view; the gun is drawn with it and
        // the eye (tag_player) is taken at the same pair.
        std::optional<std::pair<float, float>> playerTurretPose;
        // Tag orientations of the drawn (interpolated) entity poses of the current rendered frame. The effect attachment
        // 0x48abb0 reads the cgame entity (0xf7eb80 + n · 0x184: origin +0x148 and angles +0x154 of the frame's interpolated
        // pose, 0x4a25c0) and the client skeleton (0x4acea0), not the server state; gatherItems fills this for the tags that
        // bolted world effects use, and their bolts prefer it over World::entityTagOrientation (server pose).
        struct DrawnTag { int64_t stamp = -1; game::FxTag tag; };
        std::map<std::pair<int, std::string>, DrawnTag> drawnTags;
        std::map<int, std::set<std::string>> boltTags; // tags of bolted world effects per entity (filled when played)
        int64_t renderStamp = 0;
        // World generation: bumped whenever the world is destroyed. A bolt callback of an older generation never touches
        // the (freed) world; endWorldFx also drops every effect and cached tag of the old level.
        uint64_t worldGen = 0;
        auto endWorldFx = [&]() {
            ++worldGen;
            fxSystem.clear();
            drawnTags.clear();
            boltTags.clear();
        };
        auto recordDrawnTags = [&](int number, const BoneMap& bones, const Vec3& ov, const Vec3& av) {
            // tag_flash / tag_brass always (a flash event of this frame is drained after the entity loop, before its bolt is
            // known), plus any other tag a bolted world effect of this entity uses.
            std::set<std::string> tags{"tag_flash", "tag_brass"};
            if (const auto bt = boltTags.find(number); bt != boltTags.end()) tags.insert(bt->second.begin(), bt->second.end());
            const Mat4 m = entityTransform(ov, av);
            for (const std::string& tag : tags) {
                const auto it = bones.find(lowerName(tag));
                if (it == bones.end()) continue;
                auto toWorld = [&](const std::array<float, 3>& v, float w) {
                    return game::FVec{m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * w, m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * w,
                                      m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * w};
                };
                DrawnTag d;
                d.stamp = renderStamp;
                d.tag.origin = toWorld(it->second.first, 1.0f);
                d.tag.hasAxes = true;
                for (int k = 0; k < 3; ++k) {
                    std::array<float, 3> u{0, 0, 0};
                    u[size_t(k)] = 1;
                    d.tag.axes[size_t(k)] = toWorld(assets::rotate(it->second.second, u), 0.0f);
                }
                drawnTags[{number, tag}] = d;
            }
        };
        // World effect / tracer events (server events) are played when they are taken: per rendered frame in play, and per
        // simulated server frame in --shot mode, so a shot shows each effect at its own age rather than every queued event of
        // the whole simulation at age 0 (cgame consumes events per frame, effect time = cg.time, 0x486b30).
        auto playWorldFx = [&]() {
            if (!world) return;
            for (const auto& ev : world->takeFxEvents()) {
                if (std::getenv("IW2_MARK_DEBUG")) std::cerr << "fx event " << ev.path << "\n";
                if (ev.entity >= 0 && !ev.tag.empty()) {
                    // Bolted (0x4853f0 attachment): the tag position is re-read while relative particles live (cached per world time).
                    game::World* wp = world.get();
                    // 0x48abb0: the tag orientation (origin + axes), refreshed once per frame stamp (cached per world time here).
                    boltTags[ev.entity].insert(ev.tag);
                    auto bolt = std::make_shared<game::FxBolt>([wp, &worldGen, gen = worldGen, &drawnTags, &renderStamp, n = ev.entity, tag = ev.tag, lastT = int64_t(-1), last = game::FxTag{}, ok = false](game::FxTag& out) mutable {
                        if (gen != worldGen) return false; // the world this bolt was made for is gone
                        if (const auto dt = drawnTags.find({n, tag}); dt != drawnTags.end() && dt->second.stamp == renderStamp) {
                            out = dt->second.tag; // the drawn pose of this rendered frame
                            return true;
                        }
                        if (wp->timeMs() != lastT) { // [server time]
                            lastT = wp->timeMs(); // [server time]
                            game::Vec3 p{};
                            std::array<game::Vec3, 3> ax{};
                            bool hasAxes = false;
                            ok = wp->entityTagOrientation(n, tag, p, ax, hasAxes);
                            if (ok) {
                                last = game::FxTag{};
                                last.origin = {p[0], p[1], p[2]};
                                last.hasAxes = hasAxes;
                                if (hasAxes)
                                    for (int k = 0; k < 3; ++k) last.axes[size_t(k)] = {ax[size_t(k)][0], ax[size_t(k)][1], ax[size_t(k)][2]};
                            }
                        }
                        out = last;
                        return ok;
                    });
                    game::FVec f{ev.forward[0], ev.forward[1], ev.forward[2]};
                    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                    f = fl > 0 ? game::FVec{f[0] / fl, f[1] / fl, f[2] / fl} : game::FVec{0, 0, 1};
                    game::FVec r = std::fabs(f[2]) < 0.99f ? game::FVec{f[1], -f[0], 0} : game::FVec{1, 0, 0};
                    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                    r = {r[0] / rl, r[1] / rl, r[2] / rl};
                    const game::FVec up{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
                    fxSystem.playBolted(efx(ev.path), bolt, game::FxAxes{f, r, up}, fxTime() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 0)); // IW2_FX_MS: debug age
                    continue;
                }
                fxSystem.noImmediateMarks = ev.noMarks;
                fxSystem.onlyMarks = ev.marksOnly;
                fxSystem.play(efx(ev.path), ev.origin, ev.forward, fxTime());
                fxSystem.noImmediateMarks = false;
                fxSystem.onlyMarks = false;
            }
            for (const auto& te : world->takeTracerEvents())
                fxSystem.addTracer({te.from[0], te.from[1], te.from[2]}, {te.to[0], te.to[1], te.to[2]}, fxTime(), 0);
        };
        auto gatherItems = [&](const Camera& viewCam) {
            std::vector<DrawItem> items;
            if (!world) return items;
            // View frustum (Gribb-Hartmann planes of the view-projection; 32:9 aspect so any window is covered) used to skip
            // the CPU skinning of animated entities that cannot be seen. The player's view model is never culled.
            const Mat4 cullVp = viewCam.viewProj(3.6f);
            std::array<std::array<float, 4>, 5> cullPlanes{};
            {
                auto row = [&](int i) { return std::array<float, 4>{cullVp[size_t(i)], cullVp[size_t(4 + i)], cullVp[size_t(8 + i)], cullVp[size_t(12 + i)]}; };
                const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
                for (int k = 0; k < 4; ++k) {
                    cullPlanes[0][size_t(k)] = r3[size_t(k)] + r0[size_t(k)];
                    cullPlanes[1][size_t(k)] = r3[size_t(k)] - r0[size_t(k)];
                    cullPlanes[2][size_t(k)] = r3[size_t(k)] + r1[size_t(k)];
                    cullPlanes[3][size_t(k)] = r3[size_t(k)] - r1[size_t(k)];
                    cullPlanes[4][size_t(k)] = r2[size_t(k)]; // near (depth range 0..w)
                }
                for (auto& pl : cullPlanes) {
                    const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                    for (float& v : pl) v /= len;
                }
            }
            static const bool noCull = std::getenv("IW2_NO_CULL") != nullptr; // debug: disable the culling above
            auto sphereVisible = [&](float x, float y, float z, float radius) {
                for (const auto& pl : cullPlanes)
                    if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < -radius) return false;
                return true;
            };
            const game::FogParams fogNow = world->clientFog.current(cgNowMs());
            Uploader up(dev, false);
            ++renderStamp;
            for (const auto& e : world->entities()) {
                // Multiplayer clients other than the local one are drawn in third person (docs/spec/mp-players.md).
                if (e->obj->removed || e->isSpawner || (e->isPlayer && !world->mpPlayerDrawn(*e)) || e->obj->get("$hidden").truthy()) continue;
                const script::Value m = e->obj->get("model");
                if (m.isString() && e->classname == "script_brushmodel" && m.asString().size() > 1 && m.asString()[0] == '*') {
                    // Brush model at the entity's interpolated origin/angles (vertices are model-local; loadScene).
                    const auto bm = scene.brushBatches.find(std::atoi(m.asString().c_str() + 1));
                    const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                    if (bm == scene.brushBatches.end() || !o.isVector()) continue;
                    auto ov = o.asVector();
                    auto av = a.isVector() ? a.asVector() : script::Vec3{0, 0, 0};
                    if (auto pv = lerpPrev.find(e->number); pv != lerpPrev.end()) {
                        for (int k = 0; k < 3; ++k) {
                            ov[size_t(k)] = pv->second.first[size_t(k)] + (ov[size_t(k)] - pv->second.first[size_t(k)]) * lerpFrac;
                            const float d = std::remainder(av[size_t(k)] - pv->second.second[size_t(k)], 360.0f);
                            av[size_t(k)] = pv->second.second[size_t(k)] + d * lerpFrac;
                        }
                    }
                    DrawItem it; // drawn by the world passes (Renderer::draw), with its baked lightmaps
                    it.worldBatches = &bm->second;
                    it.transform = entityTransform({ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                    // Renderer near / far plane box cull (docs/spec/render-hints.md; the dll culls brush-model entities by a world box,
                    // 0x10009210). TODO(spec): [0x10008900 not read] the side planes and the dll's own box builder 0x10008900; IW2_NO_CULL disables this.
                    if (!noCull) {
                        const auto bb = scene.brushBounds.find(bm->first);
                        if (bb != scene.brushBounds.end()) {
                            float wmn[3], wmx[3];
                            game::transformedBoxBounds(&bb->second[0], &bb->second[3], it.transform.data(), wmn, wmx);
                            const Vec3 fw = viewCam.forward();
                            const float eye[3] = {viewCam.pos.x, viewCam.pos.y, viewCam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
                            const float farD = game::farPlaneDistance(fogNow.active && !fogNow.exponential, fogNow.farDist, 0.0f, world->cullDist());
                            if (game::boxCulledByDepthPlanes(wmn, wmx, eye, fwd, farD)) continue;
                        }
                    }
                    items.push_back(it);
                    continue;
                }
                if (!m.isString() || m.asString().rfind("xmodel/", 0) != 0) continue;
                const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                if (!o.isVector()) continue;
                auto ov = o.asVector();
                auto av = a.isVector() ? a.asVector() : script::Vec3{0, 0, 0};
                // CG_CalcEntityLerpPositions (0x4acca0): entities the server moves every frame (TR_INTERPOLATE) are
                // drawn between the previous and the latest server frame at cg.frameInterpolation (0xf708d0):
                // origin lerped (0x4acb90), angles by LerpAngle (0x4acc10 → 0x423ee0). TODO(spec): [anim pose rate unproven] entities with
                // other trajectory types (evaluated at cg.time) and the anim poses, which still step at 20 Hz.
                if (auto pv = lerpPrev.find(e->number); pv != lerpPrev.end()) {
                    for (int k = 0; k < 3; ++k) {
                        ov[size_t(k)] = pv->second.first[size_t(k)] + (ov[size_t(k)] - pv->second.first[size_t(k)]) * lerpFrac;
                        const float d = std::remainder(av[size_t(k)] - pv->second.second[size_t(k)], 360.0f);
                        av[size_t(k)] = pv->second.second[size_t(k)] + d * lerpFrac;
                    }
                }
                // IW2_NET_TRACE: the drawn (post-lerp) origin of every other player per rendered frame (docs/debug-flags.md).
                if (static const bool netTrace = std::getenv("IW2_NET_TRACE") != nullptr;
                    netTrace && (e->isPlayer || m.asString().rfind("xmodel/projectile_", 0) == 0))
                    std::printf("nettrace rt=%lld t=%lld client=%d org=%.2f,%.2f,%.2f ang=%.1f,%.1f,%.1f ent=%d\n", static_cast<long long>(SDL_GetTicksNS() / 1000000ull),
                                static_cast<long long>(netClient ? netClient->renderTime(netNowMs()) : cgNowMs()),
                                e->obj->get("$client").isInt() ? e->obj->get("$client").asInt() : e->isPlayer ? -1 : -2 - e->number, double(ov[0]), double(ov[1]), double(ov[2]),
                                double(av[0]), double(av[1]), double(av[2]), e->number);
                DrawItem base;
                base.transform = entityTransform({ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                // gameTime.z of the *_scroll techniques (texscroll.hlsl subtracts it from V): frac(t), t = scene time - the refEntity
                // material time (gfx 0x1002f6a0 / 0x1002fa49). The cgame (0x4ac839) sets the material time only for a vehicle whose
                // accumulator (entityState +0x5c) is >= 0: (cg.time - trunc((next - cur) * cg.frameInterpolation) - cur) * 0.001, cur /
                // next = centity +0x5c / +0xfc (currentState at +0, nextState at +0xa0, copies at 0x4a4395 / 0x4a4480); so
                // t = (cur + trunc((next - cur) * frac)) * 0.001. Otherwise the material time stays 0 (memset 0x4ac7b0) and t is
                // the scene time.
                {
                    const double sceneMs = double(cgNowMs());
                    double t = sceneMs * 0.001;
                    if (e->vehicle) {
                        const int next = e->vehicle->texScroll;
                        const auto pv = prevTexScroll.find(e->number);
                        const int cur = pv != prevTexScroll.end() ? pv->second : next;
                        if (cur >= 0) t = double(cur + int(double(next - cur) * double(lerpFrac))) * 0.001;
                    }
                    base.scrollV = float(t - std::floor(t));
                }
                assets::LightGridCoeffs c;
                float sun = 1.0f;
                if (scene.grid && scene.grid->sample({ov[0], ov[1], ov[2] + 4.0f}, c, sun)) {
                    const auto l = assets::LightGrid::evaluate(c, {0, 0, 1});
                    base.ambient[0] = l[0]; base.ambient[1] = l[1]; base.ambient[2] = l[2];
                    base.sunVisibility = sun;
                    storeProbe(base, c);
                }
                const auto corpse = world ? game::World::corpseVehicle(*e) : std::nullopt;
                if ((e->vehicle && e->vehicle->info) || corpse) {
                    // A freed vehicle's wreck (eType 0xd) goes through the same client path (0x4acd30 -> 0x4ac2b0) with the fields
                    // freevehicle left (turret, body tilt, steer, suspension travel); its wheels are still placed on the ground.
                    // Vehicle turret (cgame 0x4ac2b0 bone controls on tag_turret / tag_barrel; es+0x70 pitch, es+0x74 yaw, lerped).
                    // TODO(spec): [bone control order unproven] which control gets the pitch and which the yaw is a hypothesis (yaw on tag_turret, pitch on tag_barrel).
                    const Scene::CpuModel* vm = sceneCpuModel(scene, dev, vfs, m.asString());
                    if (!vm) continue;
                    float tp = e->vehicle ? e->vehicle->turretPitch : corpse->turretPitch, ty = e->vehicle ? e->vehicle->turretYaw : corpse->turretYaw;
                    const float bodyPitch = e->vehicle ? e->vehicle->joltPitch : corpse->bodyPitch, bodyRoll = e->vehicle ? e->vehicle->joltRoll : corpse->bodyRoll;
                    const float travel = e->vehicle ? e->vehicle->info->suspensionTravel : corpse->travel, steer = e->vehicle ? e->vehicle->steerAngle : corpse->steer;
                    if (auto pt = prevTurret.find(e->number); pt != prevTurret.end()) {
                        tp = pt->second.first + angleDelta(tp, pt->second.first) * lerpFrac;
                        ty = pt->second.second + angleDelta(ty, pt->second.second) * lerpFrac;
                    }
                    std::vector<assets::BoneControl> ctl;
                    for (size_t bi = 0; bi < vm->parts.bones.size(); ++bi) {
                        std::string n = vm->parts.bones[bi].name;
                        for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                        if (n == "tag_body") ctl.push_back({int(bi), assets::anglesQuat(bodyPitch, 0, bodyRoll), {0, 0, 0}});
                        else if (n == "tag_turret") ctl.push_back({int(bi), assets::anglesQuat(0, ty, 0), {0, 0, 0}});
                        else if (n == "tag_barrel") ctl.push_back({int(bi), assets::anglesQuat(tp, 0, 0), {0, 0, 0}});
                    }
                    // Script-animated vehicles (the flak88 firing recoil) are drawn at the client-frame anim time like actors (0x4afb70
                    // advances the DObj of every entity per client frame), not at the last 20 Hz server frame.
                    assets::XPose vpose;
                    if (auto pa = prevAnim.find(e->number); e->anim && pa != prevAnim.end() && lerpFrac < 1.0f)
                        vpose = game::clientAnimState(pa->second, lerpFrac).pose(vm->parts);
                    else
                        vpose = e->anim ? e->anim->pose(vm->parts) : assets::bindPose(vm->parts);
                    // Wheel suspension (cgame 0x4ac2b0): each wheel bone is moved to the ground under it.
                    if (world)
                        for (const auto& wc : game::vehicleWheelControls(world->collision(), vm->parts, vpose, {ov[0], ov[1], ov[2]},
                                                                         {av[0], av[1], av[2]}, travel, steer, e->number))
                            ctl.push_back(wc);
                    const assets::XPose drawnPose = assets::applyControls(vm->parts, vpose, ctl);
                    DrawItem it = base;
                    it.model = skinModel(scene, dev, up, e->number, m.asString(), *vm, drawnPose);
                    if (it.model) items.push_back(it);
                    {
                        // Vehicle flash / fx tags from the drawn pose (lerped turret controls, jolt, wheels) like actors and turrets:
                        // 0x48abb0 reads the client entity and skeleton for every entity type (World::vehicleTagPosition is the
                        // server-side position only).
                        BoneMap vbones;
                        addBones(vbones, vm->parts, drawnPose);
                        recordDrawnTags(e->number, vbones, {ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                    }
                    continue;
                }
                // Actors and animated script_models (drones: spawn("script_model") + useanimtree + attach head/gun, maps/_drones.gsc) are
                // skinned with the blended pose and carry their attachments.
                if (e->isActor || ((e->anim || !e->attachments.empty()) && !e->turret)) {
                    // Animated: skin the body with the blended pose and attach head/helmet/etc.
                    static const bool kcTrace = std::getenv("IW2_KILLCAM_TRACE") != nullptr;
                    const bool vis = noCull || sphereVisible(ov[0], ov[1], ov[2] + 36.0f, 256.0f);
                    const Scene::CpuModel* body = vis ? sceneCpuModel(scene, dev, vfs, m.asString()) : nullptr;
                    if (kcTrace && ((e->isPlayer && world->mpKillcamSwapped()) || (netKillcam.active && e->obj->get("$deathanim").isDefined())))
                        std::printf("kctrace draw t=%lld ent=%d org=%.1f,%.1f,%.1f vis=%d body=%d anim=%d model=%s\n", static_cast<long long>(world->timeMs()), e->number, // [server time]
                                    double(ov[0]), double(ov[1]), double(ov[2]), int(vis), int(body != nullptr), int(e->anim != nullptr), m.asString().c_str());
                    if (!vis) continue; // anim state already stepped
                    if (!body) continue;
                    assets::XPose pose;
                    const game::AnimState* animNow = e->anim.get();
                    std::optional<game::AnimState> tmpAnim;
                    if (auto pa = prevAnim.find(e->number); e->anim && pa != prevAnim.end() && lerpFrac < 1.0f) {
                        tmpAnim.emplace(game::clientAnimState(pa->second, lerpFrac)); // no notifies: they already fired in the server frame
                        animNow = &*tmpAnim;
                        pose = tmpAnim->pose(body->parts);
                    } else {
                        pose = e->anim ? e->anim->pose(body->parts) : assets::bindPose(body->parts);
                    }
                    // Prone tilt (cgame 0x4c1cc0 / 0x4c1b90): a DObj control on tag_origin, rotation pitch (0x423970) times
                    // roll (0x423a10, corpse blend only) = anglesQuat(pitch, 0, roll), lift on z. The fraction uses cg.time,
                    // which lies between the two snapshots it interpolates (0x4a4c10): the effect time below.
                    const int64_t cgTime = cgNowMs();
                    static const bool noTilt = std::getenv("IW2_NO_PRONE_TILT") != nullptr;
                    if (float tiltPitch = 0, tiltRoll = 0, tiltLift = 0; !noTilt && world->actorProneTilt(*e, cgTime, tiltPitch, tiltRoll, tiltLift)) {
                        for (size_t bi = 0; bi < body->parts.bones.size(); ++bi) {
                            std::string n = body->parts.bones[bi].name;
                            for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                            if (n != "tag_origin") continue;
                            pose = assets::applyControls(body->parts, pose,
                                                         std::vector<assets::BoneControl>{{int(bi), assets::anglesQuat(tiltPitch, 0, tiltRoll), {0, 0, tiltLift}}});
                            break;
                        }
                    }
                    DrawItem it = base;
                    it.model = skinModel(scene, dev, up, e->number, m.asString(), *body, pose);
                    if (it.model) items.push_back(it);
                    BoneMap bones;
                    addBones(bones, body->parts, pose);
                    for (const auto& at : e->attachments) {
                        const Scene::CpuModel* am = sceneCpuModel(scene, dev, vfs, at.model);
                        if (!am) continue;
                        // Facial bones (j_jaw, j_mouth_*, ...) exist only in the head parts and carry tracks in the full-body
                        // anims (animscripts/face.gsc): pose the attachment from the anim too.
                        std::optional<assets::XPose> animated;
                        if (animNow) animated = animNow->pose(am->parts);
                        const assets::XPose ap = attachmentPose(bones, am->parts, at.tag, animated ? &*animated : nullptr);
                        addBones(bones, am->parts, ap);
                        DrawItem ai = base;
                        ai.model = skinModel(scene, dev, up, e->number, at.model + "@" + at.tag, *am, ap);
                        if (ai.model) items.push_back(ai);
                    }
                    // The weapon world model in a multiplayer client's hands (tag_weapon_right, docs/spec/mp-players.md).
                    if (const std::string wm = world->mpPlayerWeaponModel(*e); !wm.empty())
                        if (const Scene::CpuModel* wmodel = sceneCpuModel(scene, dev, vfs, wm)) {
                            const assets::XPose wp = attachmentPose(bones, wmodel->parts, "tag_weapon_right");
                            addBones(bones, wmodel->parts, wp);
                            DrawItem wi = base;
                            wi.model = skinModel(scene, dev, up, e->number, wm + "@tag_weapon_right", *wmodel, wp);
                            if (wi.model) items.push_back(wi);
                        }
                    recordDrawnTags(e->number, bones, {ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                    continue;
                }
                if (e->turret) {
                    // Turrets are posed by their bone controls (cgame 0x4ab0d0): the turret's pitch and yaw relative
                    // to its angles; the player's turret follows the view (view angles − base, already clamped).
                    const Scene::CpuModel* tm = sceneCpuModel(scene, dev, vfs, m.asString());
                    if (!tm) continue;
                    float tp = e->turret->pitch, ty = e->turret->yaw;
                    if (auto pt = prevTurret.find(e->number); pt != prevTurret.end()) { // CG_CalcEntityLerpPositions-style smoothing
                        tp = pt->second.first + angleDelta(tp, pt->second.first) * lerpFrac;
                        ty = pt->second.second + angleDelta(ty, pt->second.second) * lerpFrac;
                    }
                    if (world->playerOnTurret() && e->turret->owner == world->player()->obj && playerTurretPose) {
                        tp = playerTurretPose->first;
                        ty = playerTurretPose->second;
                    }
                    const assets::XPose tpose = assets::controlledPose(tm->parts, game::World::turretBoneControls(tm->parts, tp, ty, e->turret->drop));
                    DrawItem it = base;
                    it.model = skinModel(scene, dev, up, e->number, m.asString(), *tm, tpose);
                    if (it.model) items.push_back(it);
                    {
                        BoneMap tbones;
                        addBones(tbones, tm->parts, tpose);
                        recordDrawnTags(e->number, tbones, {ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                    }
                    continue;
                }
                const GpuModel* gm = sceneModel(scene, dev, vfs, m.asString());
                if (!gm) continue;
                DrawItem it = base;
                it.model = gm;
                items.push_back(it);
            }
            // Effects after the entities: bolted world effects follow this frame's drawn tags (recordDrawnTags).
            playWorldFx();
            fxSystem.viewRight = {fxRight.x, fxRight.y, fxRight.z};
            fxSystem.viewUp = {fxUp.x, fxUp.y, fxUp.z};
            for (const auto& d : fxSystem.takeDecals(cgNowMs()))
                scene.marks.impactMark(d.material, d.origin, d.dir, d.rotation, d.rgba, d.size, cgNowMs());
            if (const GpuModel* mm = marksModel(scene, dev, up, vfs)) {
                DrawItem it;
                it.model = mm;
                it.transform = kIdentity;
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
                scene.marks.markDrawn(cgNowMs()); // TODO(spec): [view cull not read] the per-mark view cull (0x486e30)
            }
            // Effects are evaluated at the drawn time (cg.time in the original, per client frame): the server time minus the one
            // frame the drawn state lags plus the interpolation fraction, so particles move smoothly instead of at 20 Hz.
            const int64_t fxNow = fxTime();
            const auto fxSprites = fxSystem.sprites(fxNow);
            if (std::getenv("IW2_FX_DEBUG"))
                for (size_t i = 0; i < fxSprites.size(); i += std::max<size_t>(1, fxSprites.size() / 12))
                    std::cerr << "fx sprite " << i << "/" << fxSprites.size() << " " << fxSprites[i].shader << " pos " << fxSprites[i].position[0] << ","
                              << fxSprites[i].position[1] << "," << fxSprites[i].position[2] << " size " << fxSprites[i].size << " rgba "
                              << fxSprites[i].rgba[0] << "," << fxSprites[i].rgba[1] << "," << fxSprites[i].rgba[2] << "," << fxSprites[i].rgba[3] << "\n";
            world->setFxSightBlockers(fxSystem.sightBlockers());
            // Emitter models (shell casings, 0x48d7d0): static xmodels at the particle position and angles.
            for (const auto& fm : fxSystem.models()) {
                const GpuModel* gm = sceneModel(scene, dev, vfs, fm.model);
                if (!gm) continue;
                DrawItem it;
                it.model = gm;
                it.transform = entityTransform({fm.pos[0], fm.pos[1], fm.pos[2]}, {fm.angles[0], fm.angles[1], fm.angles[2]}, fm.scale);
                if (fm.hasAxes) { // relative Emitter: world axis rows from the tag
                    const float k = fm.scale;
                    it.transform = {fm.axes[0][0] * k, fm.axes[0][1] * k, fm.axes[0][2] * k, 0, fm.axes[1][0] * k, fm.axes[1][1] * k, fm.axes[1][2] * k, 0,
                                    fm.axes[2][0] * k, fm.axes[2][1] * k, fm.axes[2][2] * k, 0, fm.pos[0], fm.pos[1], fm.pos[2], 1};
                }
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
            }
            bool outdoorFeather = false; // zfeather_outdoor sprites read "$outdoor" too
            {
                std::set<std::string> seen;
                for (const auto& sp : fxSprites)
                    if (!outdoorFeather && seen.insert(sp.shader).second) outdoorFeather = sceneMaterial(scene, dev, up, vfs, sp.shader).fxKind == 3;
            }
            if ((!fxSystem.clouds().empty() || outdoorFeather) && world && !scene.outdoor.valid()) {
                // The renderer builds "$outdoor" when the world loads (0x10020c80); here on the first cloud (it needs the
                // collision world). 0x100207f0: CM_BoxTrace with model 0, a point box, contents 0x2001.
                scene.outdoor.build([&](float x, float y, float top, float bottom) {
                    const game::TraceResult tr = world->collision().traceWorld({x, y, top}, {x, y, bottom}, {0, 0, 0}, {0, 0, 0}, 0x2001);
                    return top + (bottom - top) * tr.fraction;
                });
                if (const char* dump = std::getenv("IW2_OUTDOOR_DUMP")) { // 512 x 512 PGM of the outdoor image (row 0 = mins.y)
                    if (FILE* f = std::fopen(dump, "wb")) {
                        std::fprintf(f, "P5 512 512 255\n");
                        std::fwrite(scene.outdoor.image().data(), 1, scene.outdoor.image().size(), f);
                        std::fclose(f);
                    }
                }
                if (std::getenv("IW2_FX_DEBUG"))
                    std::cerr << "outdoor map bounds " << scene.outdoor.mins()[0] << "," << scene.outdoor.mins()[1] << "," << scene.outdoor.mins()[2] << " .. "
                              << scene.outdoor.maxs()[0] << "," << scene.outdoor.maxs()[1] << "," << scene.outdoor.maxs()[2] << "\n";
            }
            if (const GpuModel* cm = cloudModel(scene, dev, up, vfs, fxSystem.clouds(), fxRight, fxUp)) {
                DrawItem it;
                it.model = cm;
                it.effects = true;
                it.transform = kIdentity;
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
            }
            if (std::getenv("IW2_FX_DEBUG"))
                for (const auto& c : fxSystem.clouds())
                    std::cerr << "fx cloud " << c.shader << " pos " << c.origin[0] << "," << c.origin[1] << "," << c.origin[2] << " size " << c.size
                              << " w " << c.width << " h " << c.height << " rgba " << int(c.rgba[0]) << "," << int(c.rgba[3]) << "\n";
            if (const GpuModel* fxm = spriteModel(scene, dev, up, vfs, fxSprites, fxRight, fxUp)) {
                DrawItem it;
                it.model = fxm;
                it.effects = true;
                it.transform = kIdentity;
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
            }
            up.flush();
            return items;
        };
        // First-person weapon: hands and gun posed by the weapon's current view-model animation and drawn in
        // camera space. The gun's root follows the hands' tag_weapon bone (inferred from the skeletons).
        game::PlayerWeapon pweapon;
        // EV_FIRE_WEAPON (0x4b7800): the flash, and the brass unless the weapon is boltAction (0x4b79d1: a bolt-action casing
        // comes out with event 0xa2 when the bolt is worked, PlayerWeapon::Event::EjectBrass).
        auto viewShot = [&]() {
            viewFlash->pending = true;
            if (pweapon.number("boltAction") == 0) viewFlash->brassPending = true;
        };
        game::ViewWeaponMotion vmMotion;
        game::RecoilKick recoil;
        int hostWeaponStatePrev = 0; // the host's weapon state of the previous frame (its offhand prepare event)
        unsigned turretJitterSeed = 12345; // turret view jitter RNG
        game::ViewShake viewShake; // cgame 0x4bf5e0 / 0x4bf700: MG42 fire and earthquake shake
        game::AimBlend aimBlend; // the view model aim layers' cross-fade (0x4b25e0)
        std::string aimBlendWeapon;
        game::ViewImpulses viewImp; // damage kick, landing dip
        int lastLandSeq = 0;
        int lastEventSeq = 0; // player-state events handled so far (ps.eventSeq)
        int netDeltaApplied[3] = {0, 0, 0}; // the server's delta_angles the view has turned by (ps+84)
        netgame::Predictor netPredictor; // network game: own-player prediction (docs/spec/net.md section 10)
        netgame::PredictedWeapon netPredictedWeapon; // network game: own weapon, counters and shots (docs/spec/net.md section 10)
        int netWeaponSnapshot = -1; // the snapshot message the predicted weapon was last compared with
        std::pair<const net::GameClient*, int> netPredictorFor{nullptr, -1}; // the connection and gamestate it belongs to
        int64_t netDrawNowMs = 0; // the client time of the last prediction (correction decay)
        bool breathWeapon = false; // the equipped weapon has a scope reticle and is not an item (hold-breath weapons, 0x4dd350)
        game::V3 gunBobAngles{0, 0, 0}; // cgame 0x4b4a90
        game::V3 rawGunBob{0, 0, 0}, rawGunBobPrev{0, 0, 0}; // 0xf79940..48 as stored by 0x4b4a90 (no reticle factor); 0x4ae210 sees last frame's
        // The command's weapon (cg.weaponSelect): follows the scripts' weapon changes (switchtoweapon, giveweapon set
        // $weapon) and weapnext; PM_Weapon in PlayerWeapon drops the old one and raises it, then $weapon follows.
        std::string weaponSelect, weaponWritten, prevSelect;
        uint64_t netSelectUntilMs = 0; // network client: own weapon change not yet in the snapshots (syncWeapon)
        auto syncWeapon = [&]() {
            if (!world || !world->player()) return;
            pweapon.lookup = [&](const std::string& n) { return world->weapon(n); };
            pweapon.mpAnimRates = world->multiplayer(); // the MP client scales action anims to their weapon times (weapons.md)
            pweapon.animLength = [&](const std::string& n) {
                const assets::XAnim* an = n.empty() ? nullptr : viewAnim(vfs, n);
                return an && !an->looping ? an->length() : 0.0f;
            };
            pweapon.findOffhand = [&](const std::string& cls) { return world->playerOffhandFor(cls); };
            pweapon.binocularDef = [&]() { return world->binocularWeapon(); };
            // The weapon's clip and stock are the player state's (clip by clipName, stock by ammoName; the inventory): only for owned
            // weapons, so a weapon that was just dropped or swapped never writes its old counters back.
            auto wfield = [&](const std::string& w, const char* key) -> std::string {
                const auto* d = world->weapon(w);
                if (!d) return {};
                const auto it = d->find(key);
                return it == d->end() ? std::string() : it->second;
            };
            pweapon.readAmmo = [&, wfield](const std::string& n) -> std::optional<std::pair<int, int>> {
                if (!world || !world->player()) return std::nullopt;
                game::Inventory& inv = world->inventory(*world->player());
                std::string k = n;
                for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (!inv.owned.count(k)) return std::nullopt;
                return std::make_pair(inv.clip[wfield(k, "clipname")], inv.ammo[wfield(k, "ammoname")]);
            };
            pweapon.writeAmmo = [&, wfield](const std::string& n, int clip, int stock) {
                if (!world || !world->player()) return;
                game::Inventory& inv = world->inventory(*world->player());
                std::string k = n;
                for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (!inv.owned.count(k)) return;
                inv.clip[wfield(k, "clipname")] = clip;
                inv.ammo[wfield(k, "ammoname")] = stock;
            };
            if (world->player()->obj->get("$weapontaken").truthy()) {
                world->player()->obj->set("$weapontaken", script::Value());
                pweapon.weaponTaken();
            }
            const script::Value w = world->player()->obj->get("$weapon");
            // No weapon ("none", ps weapon 0) means nothing in hand, as at the start of moscow (only the potato).
            std::string name = w.isString() && !w.asString().empty() && w.asString() != "none" ? w.asString() : std::string();
            if (const char* forced = std::getenv("IW2_WEAPON")) name = forced; // debug override
            // A network client: the snapshots carry the server's $weapon, which lags behind this client's own weapon change; until
            // the server shows the selected weapon (or 2 s pass) an older one is not a scripted change.
            const bool netLag = netClient && !weaponSelect.empty() && name != weaponSelect && SDL_GetTicks() < netSelectUntilMs;
            if (name == weaponSelect) netSelectUntilMs = 0;
            if (!netLag && game::takeScriptedWeapon(name, weaponWritten)) weaponSelect = name; // a respawn's same weapon too
            if (!pweapon.equipped() && !pweapon.holstered()) // the first weapon is in hand at once
                if (const auto* def = world->weapon(weaponSelect)) pweapon.equip(weaponSelect, *def);
        };
        auto writeWeapon = [&]() {
            if (!world || !world->player() || !game::writesHeldWeapon(pweapon, weaponSelect)) return;
            world->player()->obj->set("$weapon", script::Value(pweapon.name()));
            weaponWritten = pweapon.name();
        };
        // The gun's rotation relative to the view: sway and rotation offsets (0x4b51a0, 0x4b5730), gun bob (0x4b4d10), gun kick (0x4b5d10).
        auto gunRelativeAngles = [&]() {
            auto rt = vmMotion.rot();
            const auto gk = recoil.gunKick();
            for (size_t k = 0; k < 3; ++k) rt[k] += gunBobAngles[k] + gk[k];
            return rt;
        };
        // One source for the first-person weapon poser (docs/spec/mp-hud.md "The recorded view state"): the client's own
        // weapon (pweapon with its motion, aim-blend, bob and recoil state) or the killcam's followed weapon (the recorded view
        // state with the killcam rig's own motion, aim-blend, bob and recoil state). Both are posed by poseViewWeapon.
        struct ViewWeaponSource {
            std::function<std::string(const char*)> field; // the view weapon's file fields (the offhand's during a throw)
            std::string handsName, gunName;               // xmodel paths
            std::string animName;                          // the action animation ("" bind pose)
            float animFrame = 0;                           // its frame (looping already applied)
            float adsFrac = 0;
            float aimUp = 0, aimDown = 0;                  // the aim layers' weights (game::AimBlend)
            game::V3 move{}, rot{};                        // the placement offsets: motion, plus gun bob and gun kick in rot
            std::shared_ptr<ViewFlash> flash;              // the muzzle flash and brass tags and their pending plays
            int skinKey = -1;                              // the skinned-model cache key (-1 the own weapon, -2 the killcam's)
        };
        // The first-person weapon pass (0x4b69b0): hands and gun posed from the action animation and the two aim animations,
        // placed with the movement, sway, bob and kick offsets; the muzzle flash and the brass bolted to the gun's tags.
        auto poseViewWeapon = [&](std::vector<DrawItem>& items, const Camera& c, const ViewWeaponSource& s, bool drawn) { // drawn = 0x4b69b0's third argument (0 under the scope overlay)
            const Scene::CpuModel* hands = s.handsName.empty() ? nullptr : sceneCpuModel(scene, dev, vfs, s.handsName);
            const Scene::CpuModel* gun = s.gunName.empty() ? nullptr : sceneCpuModel(scene, dev, vfs, s.gunName);
            const assets::XAnim* an = s.animName.empty() ? nullptr : viewAnim(vfs, s.animName);
            // The view model's anim tree (0x4b2c90, 0x4b25e0): the current action animation (idle, fire, reload...) with
            // weight 1 and both aim animations: adsUpAnim at time = aim fraction, adsDownAnim at time = 1 - aim fraction,
            // their weights cross-fading over 0.5 s (game::AimBlend); all are children of the root, so the engine's calc
            // adds them and divides by the weight sum (docs/spec/xanim.md, "Blending").
            std::vector<assets::PoseLayer> layers;
            if (an) layers.push_back({an, s.animFrame, 1.0f});
            if (!std::getenv("IW2_VM_NOADS")) {
                for (const bool up : {true, false}) {
                    const float wgt = up ? s.aimUp : s.aimDown;
                    if (wgt <= 0) continue;
                    const float t = up ? s.adsFrac : 1.0f - s.adsFrac;
                    if (const assets::XAnim* ads = viewAnim(vfs, s.field(up ? "adsUpAnim" : "adsDownAnim")))
                        layers.push_back({ads, t * float(std::max(0, ads->numFrames - 1)), wgt});
                }
            }
            DrawItem base;
            if (!std::getenv("IW2_NO_DEPTH_HACK")) { base.depthMax = 0.2f; base.zNear = 0.1f; } // TODO(spec): [flag writer not found] the cgame side that sets the 0x8 group flag for the view model; the projection _43 swap (0x1003e6a0)
            // Stance, movement and aiming offsets (forward, right, up; pitch, yaw, roll) from ViewWeaponMotion, the gun bob (0x4b4d10)
            // and the gun kick (0x4b5d10) already in the source's rotation; the position in camera-relative space (relTransform).
            const game::V3& mv = s.move;
            const game::V3& rt = s.rot;
            const Vec3 vmFwd = c.forward(), vmRight = c.right(), vmUp = cross(vmRight, vmFwd);
            const auto vmOff = game::viewModelOffset({vmFwd.x, vmFwd.y, vmFwd.z}, {vmRight.x, vmRight.y, vmRight.z}, {vmUp.x, vmUp.y, vmUp.z},
                                                     {mv[0], mv[1], mv[2]});
            base.transform = entityTransform(c.pos + Vec3{vmOff[0], vmOff[1], vmOff[2]}, {c.pitch + rt[0], c.yaw + rt[1], rt[2]});
            base.relTransform = entityTransform({vmOff[0], vmOff[1], vmOff[2]}, {c.pitch + rt[0], c.yaw + rt[1], rt[2]});
            if (const char* dbg = std::getenv("IW2_VIEWMODEL_DEBUG")) {
                // Debug: show the view model from outside, `dist` units ahead, turned by `yaw` degrees.
                float dist = 40, turn = 180;
                std::sscanf(dbg, "%f,%f", &dist, &turn);
                base.transform = entityTransform(c.pos + c.forward() * dist, {0, c.yaw + turn, 0});
                base.relTransform = entityTransform(c.forward() * dist, {0, c.yaw + turn, 0});
            }
            assets::LightGridCoeffs lc;
            float sun = 1.0f;
            if (scene.grid && scene.grid->sample({c.pos.x, c.pos.y, c.pos.z}, lc, sun)) {
                const auto l = assets::LightGrid::evaluate(lc, {0, 0, 1});
                base.ambient[0] = l[0]; base.ambient[1] = l[1]; base.ambient[2] = l[2];
                base.sunVisibility = sun;
                storeProbe(base, lc);
            }
            Uploader up(dev, false);
            BoneMap bones;
            if (hands) {
                const assets::XPose hp = an ? assets::blendPose(hands->parts, layers) : assets::bindPose(hands->parts);
                addBones(bones, hands->parts, hp);
                DrawItem it = base;
                it.model = skinModel(scene, dev, up, s.skinKey, s.handsName, *hands, hp);
                if (it.model) items.push_back(it);
            }
            if (gun) {
                assets::XPose gp = an ? assets::blendPose(gun->parts, layers) : assets::bindPose(gun->parts);
                if (const auto tw = bones.find("tag_weapon"); tw != bones.end()) {
                    for (size_t i = 0; i < gp.position.size(); ++i) {
                        const auto o = assets::rotate(tw->second.second, gp.position[i]);
                        gp.position[i] = {tw->second.first[0] + o[0], tw->second.first[1] + o[1], tw->second.first[2] + o[2]};
                        gp.rotation[i] = assets::multiply(tw->second.second, gp.rotation[i]);
                    }
                }
                DrawItem it = base;
                it.model = skinModel(scene, dev, up, s.skinKey, s.gunName, *gun, gp);
                if (it.model) items.push_back(it);
                if (s.flash) {
                    const std::shared_ptr<ViewFlash>& vf = s.flash;
                    BoneMap gunBones;
                    addBones(gunBones, gun->parts, gp);
                    const auto tf = gunBones.find("tag_flash");
                    vf->valid = tf != gunBones.end();
                    if (vf->valid) {
                        const auto& m = base.transform; // columns: forward, left, up, origin (entityTransform)
                        const auto& q = tf->second.first;
                        for (int k = 0; k < 3; ++k) vf->pos[size_t(k)] = q[0] * m[size_t(k)] + q[1] * m[size_t(4 + k)] + q[2] * m[size_t(8 + k)] + m[size_t(12 + k)];
                        // The tag's own axes (0x48abb0 gives the attachment's origin AND axis rows, used as the effect axes by 0x485479):
                        // the bone rotation applied to x / y / z, then the view model's rotation.
                        auto tagAxes = [&](const assets::Quat& rq) {
                            game::FxAxes ax{};
                            for (int a = 0; a < 3; ++a) {
                                std::array<float, 3> e{0, 0, 0};
                                e[size_t(a)] = 1;
                                const auto r = assets::rotate(rq, e);
                                for (int k = 0; k < 3; ++k) ax[size_t(a)][size_t(k)] = r[0] * m[size_t(k)] + r[1] * m[size_t(4 + k)] + r[2] * m[size_t(8 + k)];
                            }
                            return ax;
                        };
                        vf->axes = tagAxes(tf->second.second);
                        if (std::getenv("IW2_FX_DEBUG"))
                            std::cerr << "view flash tag axes f " << vf->axes[0][0] << "," << vf->axes[0][1] << "," << vf->axes[0][2]
                                      << " model f " << m[0] << "," << m[1] << "," << m[2] << "\n";
                        if (std::getenv("IW2_FX_DEBUG")) std::cerr << "view flash tag " << vf->pos[0] << "," << vf->pos[1] << "," << vf->pos[2] << " pending " << vf->pending << " fx \"" << s.field("viewFlashEffect") << "\"\n";
                        // CG_EjectWeaponBrass (0x4b7730) for the local client: shellEjectEffect bolted on the view weapon's tag_brass.
                        const auto tb = gunBones.find("tag_brass");
                        vf->brassValid = tb != gunBones.end();
                        if (vf->brassValid) {
                            const auto& qb = tb->second.first;
                            for (int k = 0; k < 3; ++k) vf->brassPos[size_t(k)] = qb[0] * m[size_t(k)] + qb[1] * m[size_t(4 + k)] + qb[2] * m[size_t(8 + k)] + m[size_t(12 + k)];
                            vf->brassAxes = tagAxes(tb->second.second);
                        }
                        if (vf->brassPending && world) {
                            vf->brassPending = false;
                            const std::string path = s.field("shellEjectEffect");
                            if (vf->brassValid && !path.empty() && path != "none") {
                                auto bolt = std::make_shared<game::FxBolt>([vf](game::FxTag& o) { // tag_brass origin + axes (0x48abb0)
                                    o.origin = vf->brassPos;
                                    o.axes = vf->brassAxes;
                                    o.hasAxes = true;
                                    return vf->brassValid;
                                });
                                const game::FVec f{m[0], m[1], m[2]}, l{m[4], m[5], m[6]}, u{m[8], m[9], m[10]};
                                fxSystem.playBolted(efx(path), bolt, game::FxAxes{f, l, u}, fxTime() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 0));
                            }
                        }
                        if (vf->pending && world) {
                            const bool play = game::consumeViewFlash(vf->pending, drawn); // cleared in every posed frame (0x4b6c13), played only when drawn (0x4b6c37)
                            const std::string path = s.field("viewFlashEffect");
                            if (play && !path.empty()) {
                                auto bolt = std::make_shared<game::FxBolt>([vf](game::FxTag& o) { // tag_flash origin + axes (0x48abb0)
                                    o.origin = vf->pos;
                                    o.axes = vf->axes;
                                    o.hasAxes = true;
                                    return vf->valid;
                                });
                                const game::FVec f{m[0], m[1], m[2]}, l{m[4], m[5], m[6]}, u{m[8], m[9], m[10]};
                                fxSystem.playBolted(efx(path), bolt, game::FxAxes{f, l, u}, fxTime() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 0)); // IW2_FX_MS: debug age
                            }
                        }
                    }
                }
            }
            up.flush();
        };
        // The client's own view weapon through the shared poser.
        auto addViewModel = [&](std::vector<DrawItem>& items, const Camera& c, bool drawn = true) {
            if (!pweapon.equipped() && !pweapon.binocularsUp()) return; // binoculars also with the hand weapon holstered (vehicle)
            ViewWeaponSource s;
            s.field = [&](const char* k) { return pweapon.field(k); };
            // MP: the hands the player's character script set (setviewmodel, docs/spec/weapons.md "Multiplayer hands model").
            s.handsName = pweapon.handModel();
            if (world && world->multiplayer() && !pweapon.binocularsUp() && world->player())
                if (const script::Value vmv = world->player()->obj->get("$viewmodel"); vmv.isString())
                    s.handsName = game::viewHandsModel(true, vmv.asString(), s.handsName);
            s.gunName = pweapon.gunModel();
            s.animName = pweapon.anim();
            if (const assets::XAnim* an = viewAnim(vfs, s.animName)) {
                const float last = float(std::max(0, an->numFrames - 1));
                const float f = pweapon.animTime() * pweapon.animRate() * float(an->frameRate); // MP: the slot rate (weapons.md)
                s.animFrame = pweapon.animLoops() && last > 0 ? std::fmod(f, last) : std::min(f, last);
                if (std::getenv("IW2_VM_TRACE")) std::cout << "vm anim " << s.animName << " frame " << s.animFrame << "/" << an->numFrames << " t " << pweapon.animTime() << " hands '" << s.handsName << "' gun '" << s.gunName << "'\n";
            }
            if (const char* forced = std::getenv("IW2_VM_ANIM")) { // debug: "anim,frame"
                std::string spec = forced;
                const auto comma = spec.find(',');
                if (viewAnim(vfs, spec.substr(0, comma))) {
                    s.animName = spec.substr(0, comma);
                    s.animFrame = comma == std::string::npos ? 0.0f : std::stof(spec.substr(comma + 1));
                }
            }
            s.adsFrac = pweapon.adsFrac();
            s.aimUp = aimBlend.up();
            s.aimDown = aimBlend.down();
            s.move = vmMotion.move();
            s.rot = vmMotion.rot();
            for (int k = 0; k < 3; ++k) s.rot[size_t(k)] += gunBobAngles[size_t(k)]; // 0x4b4d10
            {
                const auto gk = recoil.gunKick(); // 0x4b5d10
                for (int k = 0; k < 3; ++k) s.rot[size_t(k)] += gk[size_t(k)];
            }
            s.flash = viewFlash;
            s.skinKey = -1;
            poseViewWeapon(items, c, s, drawn);
        };
        // Killcam (docs/spec/mp-hud.md "Killcam", "The recorded view state"): the local killcam reads the followed client's
        // recorded view state from the archived frame, the network killcam from the followed player state the server sent;
        // both feed the killcam rig, the killcam's own copy of the first-person state the own view keeps (movement and sway
        // offsets, aim cross-fade, gun bob, recoil springs, muzzle flash), stepped once per rendered frame, and the shared
        // poser draws the killer's weapon from it. The camera takes the recorded eye and angles plus the rig's view kick,
        // view bob and the aim zoom, as the own view does.
        struct KillcamRig {
            bool active = false;
            game::World::MpKillcamView view;      // this frame's killcam view (the HUD, scope overlay and crosshair read it)
            game::ViewWeaponMotion motion;
            game::RecoilKick recoil;
            game::AimBlend aim;
            game::V3 gunBob{}, rawGunBob{}, rawGunBobPrev{};
            std::array<float, 2> bobAngles{0, 0};
            std::shared_ptr<ViewFlash> flash = std::make_shared<ViewFlash>();
            std::string weapon, viewWeapon, animKey; // the record last seen: a change restarts the animation clock
            int animMsSeen = -1;                      // the record's animation time last seen
            float animMs = 0;                         // the animation clock (advanced by the frame time between records)
            int fireCount = -1;                       // the followed client's shots already kicked and flashed
            int64_t lastTicks = -1;                   // SDL ticks of the last step
        } killcamRig;
        auto killcamField = [&](const std::string& weapon, const char* key) -> std::string {
            const auto* def = world ? world->weapon(weapon) : nullptr;
            if (!def) return std::string();
            std::string k = key; // weapon file keys are stored lower case
            for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            const auto it = def->find(k);
            return it == def->end() ? std::string() : it->second;
        };
        auto killcamNumber = [&](const std::string& weapon, const char* key) { return float(std::atof(killcamField(weapon, key).c_str())); };
        // Steps the rig to this frame's view (the record of the followed client) at the camera it is drawn from.
        auto killcamRigStep = [&](const game::World::MpKillcamView& v) {
            KillcamRig& r = killcamRig;
            const int64_t ticks = int64_t(SDL_GetTicks());
            const int dtMs = r.active && r.lastTicks >= 0 ? int(std::clamp<int64_t>(ticks - r.lastTicks, 0, 100)) : 16;
            r.lastTicks = ticks;
            const game::MpViewState& vs = v.view;
            const std::string& vw = vs.viewWeapon.empty() ? v.weapon : vs.viewWeapon;
            auto num = [&](const char* k) { return killcamNumber(vw, k); };
            const std::string ret = killcamField(vw, "adsOverlayReticle");
            const bool reticle = !ret.empty() && ret != "none";
            if (!r.active || r.weapon != v.weapon || r.viewWeapon != vw) { // a new killcam or another weapon in hand
                r.aim.reset();
                r.recoil.reset();
                r.motion = game::ViewWeaponMotion{};
                r.weapon = v.weapon;
                r.viewWeapon = vw;
                r.fireCount = r.active ? r.fireCount : v.fireCount; // the shots before this killcam are not kicked
                r.animKey.clear();
            }
            // The animation clock: the record's time when it changes (a new frame or snapshot), the frame time in between,
            // and a restart when the record names another animation or started it again.
            if (vs.animKey != r.animKey || vs.animMs < r.animMsSeen - 100) {
                r.animKey = vs.animKey;
                r.animMs = float(vs.animMs);
            } else if (vs.animMs != r.animMsSeen) {
                r.animMs = float(vs.animMs);
            } else {
                r.animMs += float(dtMs);
            }
            r.animMsSeen = vs.animMs;
            // Aiming for the aim layers (0x4b2918-0x4b2969): the aim flag, not in the offhand states or a reload.
            const bool offhand = vs.weaponState >= 12 && vs.weaponState <= 16;
            const bool aiming = vs.adsHeld && !offhand && vs.weaponState != 5 && vs.weaponState != 7;
            r.aim.update(aiming, float(dtMs) * 0.001f);
            const float xySpeed = std::sqrt(vs.velocity[0] * vs.velocity[0] + vs.velocity[1] * vs.velocity[1]);
            const int stance = vs.viewHeightTarget <= 11 ? 2 : vs.viewHeightTarget <= 40 ? 1 : 0;
            r.motion.update(num, dtMs, xySpeed, 190.0f, vs.viewHeight, vs.adsFrac, vs.lean);
            r.motion.sway(num, dtMs, v.angles[0], v.angles[1], vs.adsFrac, stance, reticle, 0.0f);
            r.gunBob = game::gunBob(vs.bobCycle, xySpeed, vs.viewHeightTarget, vs.adsFrac, num("adsBobFactor"), reticle);
            r.rawGunBobPrev = r.rawGunBob;
            r.rawGunBob = game::gunBob(vs.bobCycle, xySpeed, vs.viewHeightTarget, vs.adsFrac, num("adsBobFactor"), false);
            const auto vb = game::viewBob(vs.bobCycle, xySpeed, vs.viewHeightTarget, vs.adsFrac, num("adsViewBobMult"));
            r.bobAngles = {vb[2], vb[3]};
            // The recoil (0x4dfea0 per fire event, the springs 0x4ade00 / 0x4b5d10) and the muzzle flash of each new shot.
            if (r.fireCount >= 0 && v.fireCount > r.fireCount) {
                for (int i = r.fireCount; i < v.fireCount; ++i) r.recoil.fire(num, vs.adsFrac, false);
                r.flash->pending = true;
                if (num("boltAction") == 0) r.flash->brassPending = true;
            }
            r.fireCount = v.fireCount;
            r.recoil.update(num, dtMs, !v.weapon.empty(), vs.adsFrac);
            r.view = v;
            r.active = true;
            return std::array<float, 4>{vb[0], vb[1], vb[2], vb[3]};
        };
        auto killcamCamera = [&](Camera& c, float frac) -> std::optional<game::World::MpKillcamView> {
            if (!world || !world->multiplayer()) { killcamRig.active = false; return std::nullopt; }
            const auto v = world->mpKillcamView(frac).active ? world->mpKillcamView(frac) : netKillcam;
            if (!v.active) {
                killcamRig.active = false;
                killcamRig.flash->valid = false; // the bolted flash particles of an ended killcam end with it
                return std::nullopt;
            }
            const auto vb = killcamRigStep(v);
            const KillcamRig& r = killcamRig;
            const game::MpViewState& vs = v.view;
            const std::string& vw = vs.viewWeapon.empty() ? v.weapon : vs.viewWeapon;
            auto num = [&](const char* k) { return killcamNumber(vw, k); };
            c.pos = {v.eye[0], v.eye[1], v.eye[2]};
            c.pitch = v.angles[0];
            c.yaw = v.angles[1];
            c.roll = 0;
            // The view bob (0x4ae290): up by the bob wave, sideways along the right vector; the aim bob turns the view.
            {
                const Vec3 rgt = c.right();
                c.pos = c.pos + Vec3{0, 0, vb[0]} + rgt * vb[1];
            }
            c.pitch += r.recoil.viewKick()[0]; // the view kick spring (0x4ae327)
            c.yaw += r.recoil.viewKick()[1];
            const std::string ret = killcamField(vw, "adsOverlayReticle");
            const bool reticle = !ret.empty() && ret != "none";
            if (vs.adsFrac != 0) { // scoped view bob (0x4ae210)
                const game::V3 svb = game::scopedViewBob(r.rawGunBobPrev, vs.adsFrac, reticle);
                c.pitch += svb[0];
                c.yaw += svb[1];
                c.roll += svb[2];
            }
            c.pitch += r.bobAngles[0]; // ADS view bob (0x4ae4a9)
            c.yaw += r.bobAngles[1];
            // The aim zoom (0x4ae7d9) from the recorded aim fraction and its direction.
            c.fovX43 = game::adsFov(80.0f, num("adsZoomFov") > 0 ? num("adsZoomFov") : 80.0f, num("adsZoomInFrac"), num("adsZoomOutFrac"),
                                    vs.adsFrac, vs.adsHeld);
            static const bool trace = std::getenv("IW2_KILLCAM_TRACE") != nullptr;
            if (trace)
                std::printf("kctrace view t=%lld lerp=%.2f frame=%lld ft=%lld killer=%d eye=%.1f,%.1f,%.1f ang=%.1f,%.1f fire=%d fireAge=%d clip=%d ads=%.2f state=%d anim=%s/%d\n",
                            static_cast<long long>(world->timeMs()), double(frac), static_cast<long long>(v.frame), static_cast<long long>(v.frameTimeMs), // [server time]
                            v.killer, double(v.eye[0]), double(v.eye[1]), double(v.eye[2]), double(v.angles[0]), double(v.angles[1]), v.fireCount,
                            v.fireAgeMs, v.clip, double(vs.adsFrac), vs.weaponState, vs.animKey.c_str(), vs.animMs);
            return v;
        };
        // The killcam's scope overlay alpha (0x4bc8a0 on the copied player state): the followed weapon's overlay at the
        // recorded aim fraction; 0 without a killcam.
        auto killcamScopeAlpha = [&]() -> float {
            if (!killcamRig.active) return 0.0f;
            const game::MpViewState& vs = killcamRig.view.view;
            const std::string& vw = vs.viewWeapon.empty() ? killcamRig.view.weapon : vs.viewWeapon;
            const std::string shader = killcamField(vw, "adsOverlayShader"), reticle = killcamField(vw, "adsOverlayReticle");
            if (shader.empty() && (reticle.empty() || reticle == "none")) return 0.0f;
            return game::scopeOverlayAlpha(true, vs.adsFrac, killcamNumber(vw, vs.adsHeld ? "adsZoomInFrac" : "adsZoomOutFrac"));
        };
        // The followed client's view weapon through the shared poser: the recorded animation at the rig's clock (the
        // idle loops), the recorded aim fraction with the rig's aim cross-fade, the rig's offsets, bob, kick and flash.
        auto addKillcamViewModel = [&](std::vector<DrawItem>& items, const Camera& c, const game::World::MpKillcamView& v) {
            const game::MpViewState& vs = v.view;
            const std::string& vw = vs.viewWeapon.empty() ? v.weapon : vs.viewWeapon;
            if (!world->weapon(vw)) return;
            KillcamRig& r = killcamRig;
            ViewWeaponSource s;
            s.field = [&, vw](const char* k) { return killcamField(vw, k); };
            auto xm = [](std::string m) { return m.empty() || m.rfind("xmodel/", 0) == 0 ? m : "xmodel/" + m; };
            // The hands: the client's setviewmodel hands (as the own view, the character's hands also during a throw).
            s.handsName = xm(!v.hands.empty() ? v.hands : killcamField(vw, "handModel"));
            s.gunName = xm(killcamField(vw, "gunModel"));
            s.animName = killcamField(vw, vs.animKey.empty() ? "idleAnim" : vs.animKey.c_str());
            if (s.animName.empty()) s.animName = killcamField(vw, "idleAnim");
            if (const assets::XAnim* an = viewAnim(vfs, s.animName)) {
                const float last = float(std::max(0, an->numFrames - 1));
                const float f = r.animMs * 0.001f * float(an->frameRate);
                const bool loops = vs.weaponState == 0 || vs.animKey.empty() || vs.animKey == "idleAnim" || vs.animKey == "emptyIdleAnim";
                s.animFrame = loops && last > 0 ? std::fmod(f, last) : std::min(f, last);
            }
            if (std::getenv("IW2_VM_TRACE")) std::cout << "killcam vm hands '" << s.handsName << "' gun '" << s.gunName << "' anim " << s.animName << " frame " << s.animFrame << " ads " << vs.adsFrac << "\n";
            s.adsFrac = vs.adsFrac;
            s.aimUp = r.aim.up();
            s.aimDown = r.aim.down();
            s.move = r.motion.move();
            s.rot = r.motion.rot();
            for (int k = 0; k < 3; ++k) s.rot[size_t(k)] += r.gunBob[size_t(k)];
            {
                const auto gk = r.recoil.gunKick();
                for (int k = 0; k < 3; ++k) s.rot[size_t(k)] += gk[size_t(k)];
            }
            s.flash = r.flash;
            s.skinKey = -2;
            // Under the scope overlay the gun is posed but not drawn, as the own view's (0x4b6d7c).
            if (killcamScopeAlpha() > 0) {
                std::vector<DrawItem> scratch;
                poseViewWeapon(scratch, c, s, false);
            } else {
                poseViewWeapon(items, c, s, true);
            }
        };
        // gatherItems with the archived frame swapped into the entities; the live interpolation state does not apply to it.
        auto gatherKillcamItems = [&](const Camera& c, float frac) {
            if (!world->mpKillcamView(frac).active) return gatherItems(c); // network killcam: the server sent the archived frame
            world->mpKillcamApply(frac);
            std::map<int, std::pair<script::Vec3, script::Vec3>> savedLerp;
            std::map<int, game::AnimState> savedAnim;
            savedLerp.swap(lerpPrev);
            savedAnim.swap(prevAnim);
            // The archived frame before the shown one is the "before the server frame" state the bodies are advanced from.
            for (const auto& e : world->entities())
                if (const game::AnimState* pa = world->mpKillcamPrevAnim(e->number)) prevAnim.emplace(e->number, *pa);
            auto items = gatherItems(c);
            savedLerp.swap(lerpPrev);
            savedAnim.swap(prevAnim);
            world->mpKillcamRestore();
            return items;
        };

        // HUD: script elements, crosshair and ammo counter, laid out on a 640x480 virtual screen
        // (alignment rules are inferred; see docs/spec/ui.md).
        assets::Localization loc;
        loc.load(vfs, language);
        for (const auto& [k, t] : ui::mpext::localizedStrings(language)) loc.add(k, t); // the extension panel's texts (docs/spec/mp-ui.md)
        // Log lines of server texts: localized as the message window shows them, in UTF-8 (the strings are code-page text).
        const int logCodePage = util::codePageForLanguage(language);
        const auto logText = [&](const std::string& msg) {
            return util::codePageToUtf8(game::localizeTextMessage(msg, [&](const std::string& k) { return loc.find(k); }), logCodePage);
        };
        std::map<std::string, std::optional<assets::Font>> fonts;
        auto font = [&](const std::string& name) -> const assets::Font* {
            auto [it, inserted] = fonts.try_emplace(name);
            if (inserted) {
                try {
                    // Languages with their own code page have fonts under fonts/<language>/.
                    auto d = vfs.read("fonts/" + language + "/" + name);
                    if (!d) d = vfs.read("fonts/" + name);
                    if (d) it->second = assets::parseFont(*d);
                } catch (const std::exception& e) {
                    std::cerr << "warning: font " << name << ": " << e.what() << "\n";
                }
            }
            return it->second ? &*it->second : nullptr;
        };
        // Level material aliases: at the start of a map load (0x40ce90, also 0x4c4030 / 0x4c6356) the client reads maps/<map>.csv
        // (0x4d0770, "^3WARNING: Could not find '%s'." when missing); each row "key,material" registers the material under the
        // lower-cased name "$key" (0x4d0700: format "$%s", refexport +0x... alias). The menus use them as "$levelBriefing",
        // "$victoryBackdrop" (briefing, pregame, victoryscreen).
        std::map<std::string, std::string> materialAliases;
        auto loadMapAliases = [&](const std::string& map) {
            materialAliases.clear();
            const auto csv = vfs.read("maps/" + map + ".csv");
            if (!csv) { std::cout << "WARNING: Could not find 'maps/" << map << ".csv'.\n"; return; }
            std::istringstream in(std::string(csv->begin(), csv->end()));
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                const size_t comma = line.find(',');
                if (comma == std::string::npos || comma == 0) continue;
                std::string key = "$" + line.substr(0, comma);
                for (auto& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                materialAliases[key] = line.substr(comma + 1);
            }
        };
        auto materialTexture = [&](const std::string& name) {
            Uploader up(dev);
            if (!name.empty() && name[0] == '$') {
                std::string key = name;
                for (auto& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (const auto a = materialAliases.find(key); a != materialAliases.end())
                    return sceneMaterial(scene, dev, up, vfs, a->second).texture;
            }
            return sceneMaterial(scene, dev, up, vfs, name).texture;
        };
        // The death screen's quote item comes from the HUD menus (ui/hud.txt → ui/hud.menu, menu "deadscreen").
        ui::MenuLibrary hudMenus(vfs);
        hudMenus.loadList("ui/hud.txt");
        const ui::Item* deadQuoteItem = nullptr;
        if (const ui::Menu* m = hudMenus.find("deadscreen"))
            for (const auto& it : m->items)
                if (it->name == "quote") deadQuoteItem = it.get();
        // The mantle hint item (menu "MantleHint", ownerdraw CG_MANTLE_HINT 80) and whether pmove offers a mantle
        // (ps mantle flags & 8).
        const ui::Item* mantleHintItem = nullptr;
        if (const ui::Menu* m = hudMenus.find("MantleHint"))
            for (const auto& it : m->items)
                if (it->name == "chRect") mantleHintItem = it.get();
        const ui::Item* invalidCmdItem = nullptr; // menu "InvalidCmdHint" (ownerdraw 110)
        if (const ui::Menu* m = hudMenus.find("InvalidCmdHint"))
            for (const auto& it : m->items)
                if (it->name == "chRect") invalidCmdItem = it.get();
        int invalidCmdState = 0;       // 0xf79694: 1 no ammo, 2 no frag grenade, 3 no smoke grenade, 4 stand blocked, 5 crouch blocked
        int64_t invalidCmdStartMs = 0; // 0xf79698
        auto setInvalidCmd = [&](int state, int64_t nowMs) { // 0x4ba860: restarts only when the state changes
            if (invalidCmdState != state) invalidCmdState = state, invalidCmdStartMs = nowMs;
        };
        // Objectives screen (menu "objectiveinfo", ui/objective_info.menu): ownerdraw 97 CG_MISSION_OBJECTIVE_LIST (item
        // "objectivecheckbox") and 96 CG_MISSION_OBJECTIVE_HEADER (item "objectiveheader"). The menu rect (-2, -2) is added to the
        // item rects (LEFT / TOP aligned).
        const ui::Item* objListItem = nullptr;
        const ui::Item* objHeaderItem = nullptr;
        float objMenuX = 0, objMenuY = 0;
        if (const ui::Menu* m = hudMenus.find("objectiveinfo")) {
            objMenuX = m->rect.x, objMenuY = m->rect.y;
            for (const auto& it : m->items) {
                if (it->name == "objectivecheckbox") objListItem = it.get();
                if (it->name == "objectiveheader") objHeaderItem = it.get();
            }
        }
        const ui::Item* holdBreathItem = nullptr; // menu "HoldBreathHint" (ownerdraw 71)
        if (const ui::Menu* m = hudMenus.find("HoldBreathHint"))
            for (const auto& it : m->items)
                if (it->name == "chRect") holdBreathItem = it.get();
        // Script menus (openmenu, docs/spec/ui.md): loaded from "ui/scriptmenus/<name>.menu" on the first "popupopen" (0x4c3ad0).
        ui::MenuLibrary scriptMenus(vfs);
        float menuMx = 320.0f, menuMy = 240.0f; // cursor in the 640x480 virtual screen
        // Client-side cvars of the menus (persist across levels; the running world's cvars take precedence while it exists).
        std::string pendingShotPng; // debug token shot:<file.png> of IW2_MENU_KEYS (docs/debug-flags.md)
        bool pendingDisconnect = false; // "disconnect" (player_profile_ingame_warning "yes"): leave the level, back to the front end
        std::optional<std::pair<std::string, bool>> pendingLevel; // map to load (from "map X" or a level change), failed flag
        // The pending load comes from a persistent changelevel (0x1c60ff0): game[] and the player's weapons survive it (0x4d9d3c ->
        // 0x46e1c0 frees the game object 0x9bef30 otherwise; 0x4d9dd1 -> 0x4e6c20 writes the client into a memory save).
        bool pendingPersistent = false;
        std::string deferredExec; // console text after a `wait`, run on the next frame
        std::optional<ui::MpLoadScreenText> mpLoadText; // the MP load screen's two lines while "connect" is up
        int loadScreenPhase = 0; // 1: the briefing load screen is up for one frame before the synchronous load (UI_SetActiveMenu(13))
        std::optional<std::string> pendingLoad;                   // savegame to load ("loadgame X", loadgame_continue)
        // The save reload after a death (loadgame_continue from the dead screen): no briefing frame and no pregame prompt, the
        // level resumes at once. The exe opens the load screen only once a load has lasted 100 ms of real time (the progress
        // callback 0x4a2fcf / 0x4b91d0 first stores the start time and then calls 0x4022b0 at most every 100 ms) and opens
        // pregame at the end only when a menu is open (0x449a2c tests the menu count 0x10fed5c), so a quick reload shows neither.
        // openiw2 loads synchronously, so the 100 ms condition is never met and the screen is not shown here.
        bool quickReload = false;
        // loadgame_continue (death / mission failure): when the named save cannot be loaded (missing file, old format, damaged) the level
        // restarts instead of staying in the dead world with the death screen up (docs/spec/campaign.md "Death screen").
        bool continueRestart = false;
        // Script menu environment (src/ui/menurun.h): the menu's commands act on the game cvars and send responses back.
        ui::MenuEnv menuEnv;
        // MP obituary (0x4e03f0, docs/spec/mp-hud.md): a game-message row with the kill icon between the team-coloured names, and
        // the local client's centre print. startNs / atMs: when the event arrived (front-end clock / world time).
        auto pushObituary = [&](const game::MpObituary& ev, uint64_t startNs, int64_t atMs) {
            const game::ObituaryLine l = world->mpObituaryLine(ev);
            auto ob = std::make_shared<ScreenMessage::Obit>();
            ob->attacker = l.attacker;
            ob->victim = l.victim;
            ob->icon = l.icon;
            ob->iconW = l.iconW;
            ob->iconH = l.iconH;
            ob->flip = l.flip;
            const auto ac = game::mpTeamColor(l.attackerTeam, menuEnv.getCvar("g_TeamColor_Axis"), menuEnv.getCvar("g_TeamColor_Allies"));
            const auto vc = game::mpTeamColor(l.victimTeam, menuEnv.getCvar("g_TeamColor_Axis"), menuEnv.getCvar("g_TeamColor_Allies"));
            for (int k = 0; k < 3; ++k) ob->attackerRgb[k] = ac[size_t(k)], ob->victimRgb[k] = vc[size_t(k)];
            screenMessages.push_back({"", startNs + game::kGameMessageNs, false, startNs, startNs + game::kGameMessageNs, ob});
            if (!l.centerText.empty()) {
                mpCenterText = l.centerText;
                mpCenterStartMs = atMs;
            }
        };
        // Multiplayer front end (openiw2 extension, docs/spec/mp-ui.md): the original game starts a separate multiplayer executable
        // from the SP menu (uiScript startMultiplayer); iw2view swaps its menu set to ui_mp/menus.txt instead and runs the
        // create-server screen in the same process. The switch waits for the next frame (the running menu script owns the items).
        bool mpFrontEnd = false;
        std::optional<bool> pendingFrontEnd; // true: to multiplayer, false: back to single player
        ui::MpServerSetup mpSetup;
        menuEnv.getCvar = [&](const std::string& n) {
            if (n == "sv_running") return std::string(world ? "1" : "0"); // dvar registered at 0x42df10 (default 0, flags 0x1040); the writers are not traced
            if (n == "cl_ingame") return std::string(world ? "1" : "0");  // MP menus: connected to a running game. TODO(spec): exe cvar
            if (world) {
                if (const std::string* v = world->mpLocalCvar(n)) return *v; // setClientCvar of the local client (docs/spec/mp.md)
                const auto it = world->cvars.find(n);
                if (it != world->cvars.end()) return it->second;
            }
            const auto u = uiCvars.find(n);
            if (u != uiCvars.end()) return u->second;
            if (mpFrontEnd || (world && world->multiplayer())) // the MP executable's registered defaults (docs/spec/mp-ui.md)
                if (std::string d = ui::mpRegisteredDefault(n); !d.empty()) return d;
            return ui::registeredDefault(n);
        };
        menuEnv.setCvar = [&](const std::string& n, const std::string& v) {
            const auto store = [&](const std::string& name) {
                if (ui::isArchivedCvar(name) && uiCvars[name] != v) profileDirty = true;
                uiCvars[name] = v;
                if (world) world->cvars[name] = v;
            };
            store(n);
            if (const std::string direct = ui::immediateMenuDvar(n); !direct.empty()) store(direct);
        };
        menuEnv.ingameActive = [&] {
            return ui::ingameCloseActive(world && world->multiplayer(), world != nullptr, std::atoi(menuEnv.getCvar("cl_paused").c_str()) != 0);
        };
        menuEnv.multiplayer = [&] { return world && world->multiplayer(); };
        menuEnv.scriptMenuResponse = [&](const std::string& menu, const std::string& response) {
            // "cmd mr <menu index> <response>" (0x4c8e20); the index is the menu's precache slot.
            if (!world) return;
            if (world->multiplayer()) { // MP: "cmd mr <serverId> <index> <response>" -> the client's menuresponse (docs/spec/mp.md)
                if (netClient) { // network game: to the server (docs/spec/net.md section 10)
                    // The index is the server's precache slot, read from its menu configstrings: the client's own world
                    // precaches its menus with its own cvars, so its order can differ from the server's.
                    for (int i = 0; i < 32; ++i) {
                        const auto cs = netClient->configstrings.find(netgame::kMenuConfigstrings + i);
                        if (cs != netClient->configstrings.end() && cs->second == menu) {
                            netClient->addReliableCommand("mr " + std::to_string(netClient->serverId) + " " + std::to_string(i) + " " + response);
                            break;
                        }
                    }
                    return;
                }
                world->mpMenuResponse(world->mpLocalClient(), menu, response);
                return;
            }
            const auto& names = world->menuNames();
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == menu) world->menuResponse(int(i), response);
        };
        menuEnv.play = [&](const std::string& alias) {
            float duration = 0;
            mixer.play(alias, std::nullopt, &duration); // interface sounds are 2D
        };
        // Console text run by menu "exec" (src/ui/frontend_exec.h): set, vstr, map, difficulty*, ';' separated.
        std::function<void(const std::string&)> connectText; // set below, with the join code
        std::function<void(const std::string&)> execText = [&](const std::string& text) {
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find(';', start);
                if (end == std::string::npos) end = text.size();
                std::istringstream in(text.substr(start, end - start));
                start = end + 1;
                std::string c, a1, a2;
                in >> c >> a1;
                std::getline(in >> std::ws, a2);
                for (auto& ch : c) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                while (!a2.empty() && std::isspace(static_cast<unsigned char>(a2.back()))) a2.pop_back();
                if (c == "vstr") execText(menuEnv.getCvar(a1));
                // Chat in a network game: "say" / "say_team" go to the server as client commands (docs/spec/net.md section 10).
                else if (c == "cmd" && netClient) netClient->addReliableCommand(a1 + (a2.empty() ? "" : " " + a2)); // forwarded to the server (MP console command "cmd")
                else if ((c == "say" || c == "say_team") && netClient) netClient->addReliableCommand(c + " " + a1 + (a2.empty() ? "" : " " + a2));
                // "setfromdvar dst src" (console command, ui/options_graphics.cfg) copies the value of src (registered default if unset).
                // TODO(spec): [command not located] exe address of the command and its behaviour for an unknown src.
                else if (c == "setfromdvar") menuEnv.setCvar(a1, menuEnv.getCvar(a2));
                // "exec <file>" runs a shipped cfg (menu onOpen of options_graphics: ui/options_graphics.cfg), one command per line.
                else if (c == "exec" && !a1.empty()) {
                    if (const auto cfg = vfs.read(a1)) {
                        std::string t(cfg->begin(), cfg->end());
                        for (auto& ch : t) if (ch == '\r' || ch == '\n') ch = ';';
                        execText(t);
                    }
                }
                // "fade <r> <g> <b> <a> <seconds>" (cgame console command, table 0x56e4d0 -> 0x4a4d60): needs all 5 arguments; only the alpha is
                // kept: the screen fade (the scr_fade state 0xf79a50..5c drawn by 0x4bda70) goes to atoi(a) / 255 from the current alpha over
                // atoi(seconds) * 1000 ms from cg.time, at once when that is 0 (0x4a4ea7). r, g, b are parsed and dropped.
                else if (c == "fade") {
                    std::istringstream fa(a1 + " " + a2);
                    std::vector<std::string> t;
                    for (std::string x; fa >> x;) t.push_back(x);
                    if (t.size() >= 5) {
                        const int64_t now = cgNowMs();
                        screenFade.command(float(std::atoi(t[3].c_str())) * 0.003921568859368563f, int(now), std::atoi(t[4].c_str()) * 1000, now);
                    }
                }
                else if (c == "set" || c == "seta" || c == "sets") menuEnv.setCvar(a1, a2);
                // In the multiplayer front end (openiw2 extension, docs/spec/mp-ui.md) "map X" / "devmap X" start the listen server
                // on mp/X, the iw2view multiplayer mode.
                else if ((c == "map" || c == "devmap") && mpFrontEnd && a1.rfind("mp/", 0) != 0)
                    pendingLevel = std::make_pair("mp/" + a1, false), pendingPersistent = false;
                else if (c == "map" || c == "spmap" || c == "devmap" || c == "spdevmap") pendingLevel = std::make_pair(a1, false), pendingPersistent = false;
                // nextlevel (console 0x446fa0 / menu command 0x4c8e10 -> 0x4d9f00): `spmap <stored map>` (`spdevmap` with sv_cheats) or,
                // without a stored map, `disconnect`.
                else if (c == "nextlevel") {
                    if (world && !world->storedNextMap().empty())
                        pendingLevel = std::make_pair(world->storedNextMap(), false), pendingPersistent = world->storedPersistent();
                    else pendingDisconnect = world != nullptr;
                }
                // "openmenu <name>" (console command registered with closemenu, string 0x581368).
                else if (c == "openmenu") menuEnv.open(a1);
                else if (c == "wait") { // the command buffer's wait: the rest of the text runs on the next frame (after a disconnect)
                    deferredExec = start <= text.size() ? text.substr(start) : std::string();
                    return;
                }
                // Exe commands 0x446860..0x4468c0 (difficultyEasy/Medium/Hard/Fu) call 0x432570 with 0/1/2/3 and then re-apply the skill.
                else if (c == "difficultyeasy") menuEnv.setCvar("g_gameskill", "0");
                else if (c == "difficultymedium") menuEnv.setCvar("g_gameskill", "1");
                else if (c == "difficultyhard") menuEnv.setCvar("g_gameskill", "2");
                else if (c == "difficultyfu") menuEnv.setCvar("g_gameskill", "3");
                else if (c == "disconnect") pendingDisconnect = world != nullptr;
                else if (c == "connect" && !a1.empty()) connectText(a1); // docs/spec/mp-ui.md "Addresses and name lookup"
                else if (c == "quit") std::exit(0);
                // "closemenu name" (console command registered at 0x4c3a80, string 0x58135c) closes that menu; used by difficulty.menu's ui_selected_map.
                else if (c == "closemenu") menuEnv.close(a1);
                // TODO(spec): [handler not read] fast_restart (reloads the current map here).
                else if (c == "fast_restart" && !currentMap.empty()) pendingLevel = std::make_pair(currentMap, false);
                // vid_restart (0x40cd10) / snd_restart (0x40ce30) while a level runs: queue the save internal/<cmd> (type 0, flags 4);
                // once written, "disconnect;<cmd>;loadgame internal/<cmd>" runs (0x44a050). Without a level: the restart itself, which
                // openiw2 does not implement (TODO: renderer / mixer re-init), so it only logs.
                else if (c == "vid_restart" || c == "snd_restart") {
                    if (world && !pendingDisconnect) world->queueSave("internal/" + c, "", "", 0, 4);
                    else if (c == "vid_restart") presentModeDirty = true; // the renderer part openiw2 restarts: the present mode
                    else std::cout << c << ": no subsystem restart in openiw2\n";
                }
                // "loadgame <name>" (exe "You must specify a savegame to load", "Can't find savegame %s") and loadgame_continue
                // (0x447190: the save named by sv_lastSaveGame) load save/<name>.svg (docs/spec/savegame.md section 5).
                else if (c == "loadgame") {
                    if (a1.empty()) std::cout << "You must specify a savegame to load\n";
                    else pendingLoad = a1;
                } else if (c == "loadgame_continue") {
                    const std::string last = menuEnv.getCvar("sv_lastSaveGame");
                    if (!last.empty()) pendingLoad = last, continueRestart = !currentMap.empty();
                    else if (!currentMap.empty()) pendingLevel = std::make_pair(currentMap, true); // no save yet: restart the level
                }
            }
        };
        int menuLoopHandle = 0;
        menuEnv.startLoop = [&](const std::string& alias) {
            if (menuLoopHandle) mixer.stop(menuLoopHandle);
            float d = 0;
            menuLoopHandle = mixer.play(alias, std::nullopt, &d);
            std::cout << "menu loop " << alias << (menuLoopHandle ? " started\n" : " not started\n");
        };
        menuEnv.stopLoop = [&] { if (menuLoopHandle) mixer.stop(menuLoopHandle); menuLoopHandle = 0; };
        menuEnv.exec = execText;
        // uiScript dispatcher (0x4c5b9b..): "Quit" (string 0x580e60) runs the console command "quit" (0x41b0a0 with 0x571858).
        // TODO(spec): [remaining names not walked in 0x4c5b9b] the other uiScripts (loadControls, openMenuOnDvar, loadMissionDifficultyOffset, Loadgame, profiles ...).
        std::array<std::time_t, 4> lastServerRefresh{};
        ui::MenuStack* frontPtr = nullptr; // set once frontStack exists (the scripts select list rows through it)
        auto saveProfileCfg = [&] {
            if (!profileDirty) return;
            profileDirty = false;
            std::error_code ec;
            std::filesystem::create_directories(profileCfg.parent_path(), ec);
            if (std::ofstream of(profileCfg); of) of << ui::formatConfig(uiCvars) << input.formatBindings();
        };
        auto profileCountChanged = [&] { uiCvars["ui_playerProfileCount"] = std::to_string(profiles.count()); };
        auto selectProfileRow = [&](int row) { // Menu_SetFeederSelection(0x18, row); an empty list clears the selection
            if (row < 0 || row >= profiles.count()) { uiCvars["ui_playerProfileSelected"] = ""; return; }
            if (frontPtr) frontPtr->setFeederSelection(0x18, row);
            else uiCvars["ui_playerProfileSelected"] = profiles.name(row);
        };
        uiCvars["com_playerProfile"] = profileName;
        struct SaveRow { std::string name; ui::SaveSortKey key; ui::SaveInfo info; };
        std::vector<SaveRow> saveRows;
        uint64_t saveRowsAt = 0;
        auto saveLoc = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
        std::string saveDetail;   // UI state +0x41e: detail text of the selected savegame (drawn by textsavegame items)
        std::string saveSelName;  // UI state +0x31e: entry +0 of the selected savegame (ownerdraw UI_SAVEGAMENAME 262)
        int saveSel = -1;         // selected row (ownerdraw UI_SAVEGAME_SHOT 258 reads its image name, entry +8)
        // Savegame directory (docs/debug-flags.md): the app's preference folder however the game was started, so a Terminal start of
        // the app binary or a build-folder run finds the app's saves instead of writing <main>/save into the game folder. IW2_SAVE_DIR
        // overrides it; <main>/save stays only when the preference folder cannot be obtained.
        std::filesystem::path savePref = app ? app->pref : std::filesystem::path();
        if (!app) {
            if (char* pref = SDL_GetPrefPath("OpenIW2", "OpenIW2")) {
                savePref = pref;
                SDL_free(pref);
            }
        }
        const std::filesystem::path saveDir = ui::saveDirectory(std::getenv("IW2_SAVE_DIR"), savePref, args[0]);
        // Join a server: from the join list, a favorite, "connect" and +connect (name lookup first, docs/spec/mp-ui.md).
        net::Resolver pendingConnect;
        auto joinAddress = [&](const net::NetAddress& to) {
            std::cout << "joining " << to.toString() << "\n";
            auto nc = std::make_unique<net::GameClient>();
            nc->name = ui::profilePlayerName(playersDir, menuEnv.getCvar("name")); // the multiplayer options name, else the profile
            if (const char* n = std::getenv("IW2_NET_NAME")) nc->name = n;
            nc->password = menuEnv.getCvar("password"); // ui_mp/password.menu
            if (netClient) netClient->disconnect(); // a connection still held goes first
            if (!nc->connect(to, netNowMs())) { std::cerr << "error: " << nc->error << "\n"; return; }
            nc->waitForHandshake(netNowMs, 20, [&] { if (listenServer) listenServer->poll(netNowMs(), 0); });
            if (nc->state() == net::ClientState::Disconnected) {
                std::cerr << "error: " << nc->error << "\n";
                netError = netgame::errorDialog(nc->dropFromServer, nc->dropReason);
                return;
            }
            const std::string map = net::infoValueForKey(nc->configstrings[0], "mapname");
            std::cout << "connected as client " << nc->clientNum << ", map " << map << "\n";
            netClient = std::move(nc);
            ++netConnection;
            if (frontPtr) frontPtr->closeAll();
            execText("map " + map);
        };
        connectText = [&](const std::string& text) { pendingConnect.start(text, net::kDefaultPort, netNowMs()); };
        // Once per frame: a lookup that finished joins, one that failed shows the error popup.
        auto pumpConnect = [&] {
            const auto st = pendingConnect.poll(netNowMs());
            if (st == net::Resolver::Status::Done) { const auto to = pendingConnect.address(); pendingConnect.reset(); joinAddress(to); }
            else if (st == net::Resolver::Status::Failed) {
                std::cerr << "error: bad address " << pendingConnect.text() << "\n";
                pendingConnect.reset();
                netError = netgame::errorDialog(false, "\x14" "EXE_BADSERVERADDRESS");
            }
        };
        net::FavoriteList favorites; // per profile, next to its config.cfg
        auto favoritesFile = [&] { return profileCfg.parent_path() / "favorites.txt"; };
        auto loadFavorites = [&] {
            favorites.clear();
            if (std::ifstream ff(favoritesFile()); ff) favorites.parse(std::string(std::istreambuf_iterator<char>(ff), {}), net::kDefaultPort);
        };
        loadFavorites();
        auto saveFavorites = [&] {
            std::error_code ec;
            std::filesystem::create_directories(favoritesFile().parent_path(), ec);
            if (std::ofstream of(favoritesFile()); of) of << favorites.format();
        };
        auto refreshBrowser = [&] {
            const int source = std::atoi(menuEnv.getCvar("ui_netSource").c_str());
            const char* extra = std::getenv("IW2_LAN_EXTRA"); // openiw2: also ask this host:port (tests, no broadcast)
            std::vector<net::NetAddress> ex;
            if (net::NetAddress a; extra && net::NetAddress::parse(extra, a, net::kDefaultPort)) ex.push_back(a);
            lanBrowser.setSource(source);
            lanBrowser.setFavorites(favorites.items());
            lanBrowser.refresh(netNowMs(), ex);
            lastServerRefresh[source & 3] = std::time(nullptr);
            std::cout << "lan browser: refresh\n";
        };
        menuEnv.uiScript = [&](const std::string& name, const ui::Command& cmd) {
            (void)cmd;
            if (name == "quit") execText("quit");
            else if (name == "startmultiplayer") pendingFrontEnd = true;   // SP ui/main.menu (multi_popmenu): see mpFrontEnd
            else if (name == "startsingleplayer") pendingFrontEnd = false; // MP ui_mp/single_player.menu
            // loadArenas (MP exe 0x5352ce): arenas, gametype from g_gametype, the map filter, row 0 of FEEDER_ALLMAPS. The
            // extension panel opens with it (createserver's onOpen runs loadArenas).
            // Join-server list (ui_mp/joinserver.menu; uiScript dispatch MP 0x53588c): RefreshServers broadcasts getinfo
            // (docs/spec/net.md section 12), JoinServer connects to the selected row and loads its map as a network client.
            else if (name == "refreshservers" || name == "refreshfilter" || name == "updatefilter") {
                // UpdateFilter (the join screen's onOpen) refreshes the local source; openiw2 also asks the favorites, which have no
                // stored answers here (docs/spec/mp-ui.md "Join server").
                const int source = std::atoi(menuEnv.getCvar("ui_netSource").c_str());
                if (name != "updatefilter" || source == net::kSourceLocal || source == net::kSourceFavorites) refreshBrowser();
                else lanBrowser.select(-1);
            }
            else if (name == "joinserver") {
                const net::LanServer* row = lanBrowser.rowServer(lanBrowser.selected >= 0 ? lanBrowser.selected : 0);
                if (!row) return;
                if (row->favorite && row->address.ip == 0) connectText(row->addressText); // its name was not looked up yet
                else joinAddress(row->address);
            }
            else if (name == "serversort") {
                if (cmd.size() > 2) lanBrowser.sortBy(std::atoi(cmd[2].c_str()));
            }
            else if (name == "closejoin" || name == "stoprefresh") {} // the refresh is over within its timeout (kRefreshMs)
            else if (name == "serverstatus") lanBrowser.queryStatus(netNowMs());
            else if (name == "addfavorite" || name == "createfavorite") {
                const bool create = name == "createfavorite";
                const int source = std::atoi(menuEnv.getCvar("ui_netSource").c_str());
                if (create != (source == net::kSourceFavorites)) return; // the dispatcher's source test (MP 0x534d60)
                std::string nm, ad;
                if (create) nm = menuEnv.getCvar("ui_favoriteName"), ad = menuEnv.getCvar("ui_favoriteAddress");
                else if (const net::LanServer* r = lanBrowser.rowServer(lanBrowser.selected >= 0 ? lanBrowser.selected : 0)) nm = r->hostname(), ad = r->address.toString();
                menuEnv.setCvar("ui_favorite_message", net::favoriteMessage(favorites, nm.substr(0, 31), ad.substr(0, 31), net::kDefaultPort));
                saveFavorites();
                if (create) refreshBrowser();
            }
            else if (name == "deletefavorite") {
                if (std::atoi(menuEnv.getCvar("ui_netSource").c_str()) != net::kSourceFavorites) return;
                if (const net::LanServer* r = lanBrowser.rowServer(lanBrowser.selected)) {
                    const std::string ad = r->addressText;
                    favorites.remove(ad);
                    saveFavorites();
                    refreshBrowser();
                }
            }
            else if (name == "clearerror") menuEnv.setCvar("com_errorMessage", ""); // error_popmenu onClose (docs/spec/net.md section 14)
            else if (name == "loadarenas") {
                const auto get = menuEnv.getCvar;
                const auto set = menuEnv.setCvar;
                mpSetup.loadArenas(vfs, get, set);
                if (frontPtr) frontPtr->setFeederSelection(ui::MpServerSetup::kFeederAllMaps, 0);
                ui::mpext::initDefaults(get, set);
                menuEnv.open("iw2_createserver_extbutton");
            }
            // StartServer (MP exe 0x534d93): cvars and "wait ; wait ; map <map>"; the extension adds the weapon classes and bots.
            else if (name == "startserver") {
                // The weapon panel's list must leave every nation a weapon to choose (a player without one cannot join): refused
                // with a message instead of starting a server nobody can join (docs/spec/mp-ui.md "Weapon classes").
                {
                    game::MpWeaponSelection sel;
                    const std::string list = ui::mpext::weaponList(menuEnv.getCvar);
                    if (!game::mpParseWeaponList(list, sel).empty()) {
                        const bool noClass = [&] {
                            for (const auto& c : ui::mpext::weaponClasses())
                                if (std::string(c.id) != "grenades" && list.find(c.id) != std::string::npos) return false;
                            return true;
                        }();
                        const auto find = [&](const std::string& k) { return loc.find(k); };
                        menuEnv.setCvar("com_errorTitle", game::localizeTextMessage("\x14MENU_ERROR", find));
                        const std::string* msg = loc.find(noClass ? "IW2_ERR_NO_WEAPON" : "IW2_ERR_NATION_WEAPON");
                        menuEnv.setCvar("com_errorMessage", msg ? *msg : list);
                        std::cout << "start server refused: weapon classes '" << list << "'\n";
                        if (frontPtr) frontPtr->errorKeepsScreen(true);
                        menuEnv.open("error_popmenu");
                        return;
                    }
                }
                const std::string cmd = mpSetup.startServer(menuEnv.getCvar, menuEnv.setCvar);
                if (cmd.empty()) return;
                // A server started from the menu takes LAN clients, as the original's listen server does (docs/spec/net.md section 11);
                // IW2_LISTEN=0 turns the listen server off.
                listenWanted = ui::menuStartListens(std::getenv("IW2_LISTEN"));
                ui::mpext::applyWeaponClasses(menuEnv.getCvar, menuEnv.setCvar);
                // The dedicated choice (dvar "dedicated" 1 LAN / 2 internet; MP 0x434dc0 -> CL_Shutdown 0x4116d0): no local game;
                // the window closes and the process becomes the server (docs/spec/mp-ui.md "Dedicated start").
                if (netgame::menuStartIsDedicated(menuEnv.getCvar("dedicated"))) {
                    auto menuCvars = uiCvars;
                    for (const char* k : {"sv_hostname", "sv_maxclients", "g_password", "g_gametype"}) menuCvars[k] = menuEnv.getCvar(k);
                    const char* lp = std::getenv("IW2_LISTEN_PORT");
                    const std::string np = menuEnv.getCvar("net_port");
                    dedicatedStart = netgame::dedicatedFromMenu(menuCvars, cmd, ui::mpext::botCount(menuEnv.getCvar),
                                                                ui::mpext::botSkill(menuEnv.getCvar),
                                                                lp ? std::atoi(lp) : !np.empty() ? std::atoi(np.c_str()) : 28960);
                    std::cout << "dedicated server: " << dedicatedStart->gametype << ", " << dedicatedStart->map << "  bots "
                              << dedicatedStart->bots << " skill " << dedicatedStart->botSkill << "\n";
                    return; // the main loop ends at its next turn
                }
                mpMenuBots = std::make_pair(ui::mpext::botCount(menuEnv.getCvar), ui::mpext::botSkill(menuEnv.getCvar));
                std::cout << "start server: " << menuEnv.getCvar("g_gametype") << ", " << cmd.substr(cmd.rfind(' ') + 1) << "  bots "
                          << mpMenuBots->first << " skill " << mpMenuBots->second << "\n";
                execText(cmd);
            }
            // "playerstart" (0x4c5ab3 -> 0x4c54c0): cl_paused 0, console "fade 0 0 0 0 3" (0x581044), close every menu.
            else if (name == "playerstart") { menuEnv.setCvar("cl_paused", "0"); execText("fade 0 0 0 0 3"); if (frontPtr) frontPtr->closeAll(); }
            // "Loadgame" uiScript: "loadgame %s" of ui_savegame (0x580f98 / 0x580fa8).
            else if (name == "loadgame") { const std::string sv = menuEnv.getCvar("ui_savegame"); if (!sv.empty()) execText("loadgame " + sv); }
            // "DelSavegame": deletes save/<ui_savegame>.svg ("Deleted savegame: %s.svg" / "Unable to delete savegame: %s.svg", 0x5812d0 / 0x5812a0).
            else if (name == "delsavegame") {
                const std::string sv = menuEnv.getCvar("ui_savegame");
                std::error_code ec;
                if (!sv.empty() && std::filesystem::remove(saveDir / (sv + ".svg"), ec)) std::cout << "Deleted savegame: " << sv << ".svg\n";
                else std::cout << "Unable to delete savegame: " << sv << ".svg\n";
                saveRowsAt = 0;
            }
            // Profile scripts (exe 0x4c4b80 addPlayerProfiles, 0x4c5a06 sortPlayerProfiles, 0x4c4a70 selectActive, 0x4c4d20 create,
            // 0x4c4e50 delete); details in src/ui/profilelist.h.
            else if (name == "addplayerprofiles") { profiles.refresh(); profileCountChanged(); selectProfileRow(0); }
            else if (name == "sortplayerprofiles") { profiles.toggleSort(); selectProfileRow(0); }
            else if (name == "selectactiveplayerprofile") selectProfileRow(std::max(0, profiles.rowOf(menuEnv.getCvar("com_playerProfile"))));
            else if (name == "createplayerprofile") {
                const std::string nm = menuEnv.getCvar("ui_playerProfileNameNew");
                int row = 0;
                if (nm.empty()) return;
                const auto r = profiles.create(nm, row);
                uiCvars["ui_playerProfileNameNew"] = "";
                if (r == ui::ProfileList::Create::TooMany) menuEnv.open("profile_create_too_many_popmenu");
                else if (r == ui::ProfileList::Create::Exists) menuEnv.open("profile_exists_popmenu");
                else if (r == ui::ProfileList::Create::Failed) menuEnv.open("profile_create_fail_popmenu");
                else if (r == ui::ProfileList::Create::Ok) { profileCountChanged(); selectProfileRow(row); }
            } else if (name == "deleteplayerprofile") {
                const std::string sel = menuEnv.getCvar("ui_playerProfileSelected");
                int row = 0;
                if (!profiles.remove(sel, row)) menuEnv.open("profile_delete_fail_popmenu");
                profileCountChanged();
                if (!profiles.exists(sel) && menuEnv.getCvar("com_playerProfile") == sel) uiCvars["com_playerProfile"] = "";
                selectProfileRow(row);
            } else if (name == "loadplayerprofile") {
                // loadPlayerProfile (0x4c4f10 -> 0x4274a0, docs/spec/savegame.md section 4): nothing for an empty selection, the
                // active profile (case-insensitive) or a profile that does not exist; else players/active.txt, `disconnect`, dvar
                // reset, the profile's config.cfg, and snd_restart only when a dvar's current value differs from its latched one.
                // openiw2 has no latched dvars, so that condition never holds and no snd_restart is issued (no vid_restart either).
                const std::string sel = menuEnv.getCvar("ui_playerProfileSelected");
                if (sel.empty() || !profiles.exists(sel)) return;
                if (strcasecmp(sel.c_str(), profileName.c_str()) == 0) return;
                if (world) pendingDisconnect = true; // console "disconnect" (0x42750e)
                saveProfileCfg();
                profileName = sel;
                profileCfg = playersDir / profileName / "config.cfg";
                std::error_code ec;
                std::filesystem::create_directories(playersDir, ec);
                if (std::ofstream af(playersDir / "active.txt"); af) af << profileName << "\n";
                std::map<std::string, std::string> fresh;
                if (std::ifstream cf(profileCfg); cf) {
                    const std::string cfgText(std::istreambuf_iterator<char>(cf), {});
                    fresh = ui::parseConfig(cfgText);
                    if (cfgText.find("unbindall") != std::string::npos) input.loadConfig(cfgText);
                }
                moveScreenshotKeyOffF12();
                for (const char* keep : {"ui_playerProfileCount", "ui_playerProfileSelected", "ui_playerProfileNameNew", "ui_playerProfileAlreadyChosen"})
                    fresh[keep] = uiCvars[keep];
                fresh["com_playerProfile"] = profileName;
                uiCvars = fresh;
                loadFavorites();
            }
        };
        // Savegame list (FEEDER_SAVEGAMES 0x10): save/*.svg whose header is listable, newest first (0x4c44a0).
        // TODO(spec): [header fields not decoded] the level name / time / health / difficulty columns come from header fields that are not decoded yet.
        auto refreshSaves = [&] {
            if (saveRowsAt && SDL_GetTicksNS() - saveRowsAt < 1000000000ull) return;
            saveRowsAt = SDL_GetTicksNS() | 1;
            saveRows.clear();
            std::error_code ec;
            // save/*.svg and, with ui_savegameListAutosave set, save/autosave/*.svg listed as "autosave/<name>" (entry +0 is "%s/%s"
            // of directory and name for a sub directory, 0x4c44a0). TODO(spec): [walk not read] the exe's directory walk and the toggle's exact test.
            std::vector<std::pair<std::filesystem::path, std::string>> files;
            for (const auto& de : std::filesystem::directory_iterator(saveDir, ec)) files.emplace_back(de.path(), "");
            if (menuEnv.getCvar("ui_savegameListAutosave") != "" && menuEnv.getCvar("ui_savegameListAutosave") != "0")
                for (const auto& de : std::filesystem::directory_iterator(saveDir / "autosave", ec)) files.emplace_back(de.path(), "autosave/");
            for (const auto& [path, prefix] : files) {
                if (path.extension() != ".svg") continue;
                const std::filesystem::directory_entry de(path);
                std::ifstream f(de.path(), std::ios::binary);
                std::vector<uint8_t> hdr(ui::kSaveHeaderSize);
                f.read(reinterpret_cast<char*>(hdr.data()), std::streamsize(hdr.size()));
                if (size_t(f.gcount()) != hdr.size() || !ui::saveHeaderListable(hdr)) continue;
                SaveRow r;
                r.name = prefix + de.path().stem().string();
                if (!ui::saveSortKey(hdr, r.key) || !ui::saveHeaderInfo(hdr, r.info)) continue;
                size_t at = saveRows.size();
                for (size_t i = 0; i < saveRows.size(); ++i)
                    if (ui::saveListBefore(r.key, saveRows[i].key)) { at = i; break; }
                saveRows.insert(saveRows.begin() + std::ptrdiff_t(at), r);
                if (saveRows.size() >= ui::kSaveListMax) break;
            }
        };
        auto readBrowserFilter = [&] {
            net::BrowserFilter f;
            const auto i = [&](const char* k) { return std::atoi(menuEnv.getCvar(k).c_str()); };
            f.showFull = i("ui_browserShowFull"), f.showEmpty = i("ui_browserShowEmpty"), f.showPassword = i("ui_browserShowPassword");
            f.showNoPassword = i("ui_browserShowNoPassword"), f.showPure = i("ui_browserShowPure"), f.showDedicated = i("ui_browserShowDedicated");
            f.mod = i("ui_browserMod"), f.friendlyFire = i("ui_browserFriendlyfire"), f.killcam = i("ui_browserKillcam"), f.punkBuster = i("ui_browserShowPunkBuster");
            const int g = i("ui_joinGametype");
            if (g > 0 && size_t(g - 1) < mpSetup.gametypes().size()) f.gametype = mpSetup.gametypes()[size_t(g - 1)].name;
            return f;
        };
        std::map<std::string, ui::MpLoadScreenText> browserTextCache;
        auto browserText = [&](const std::string& gt, const std::string& map) {
            const std::string k = gt + "/" + map;
            auto it = browserTextCache.find(k);
            if (it == browserTextCache.end()) it = browserTextCache.emplace(k, ui::mpLoadScreenText(vfs, gt, map)).first;
            return it->second;
        };
        menuEnv.feederCount = [&](int feeder) {
            if (feeder == 0x18) return profiles.count();
            if (feeder == net::LanBrowser::kFeederServers) { lanBrowser.setFilter(readBrowserFilter()); lanBrowser.poll(netNowMs()); return lanBrowser.rowCount(); }
            if (feeder == net::LanBrowser::kFeederServerStatus) { lanBrowser.poll(netNowMs()); return lanBrowser.statusCount(); }
            if (feeder == ui::MpServerSetup::kFeederAllMaps && mpFrontEnd) return mpSetup.mapCount();
            if (feeder == 0x10) { refreshSaves(); return int(saveRows.size()); }
            return 0;
        };
        menuEnv.feederText = [&](int feeder, int row, int col) {
            if (feeder == 0x18 && col == 0) return profiles.name(row);
            if (feeder == net::LanBrowser::kFeederServers) {
                if (col == 3 || col == 5) { // the map's long name and the gametype's display string (MP 0x537640, 0x532cd0, 0x532d90)
                    const net::LanServer* s = lanBrowser.rowServer(row);
                    if (!s || s->info.empty()) return std::string();
                    const auto t = browserText(s->gametype(), s->map());
                    return col == 3 ? t.map : saveLoc(t.gametype);
                }
                return lanBrowser.column(row, col);
            }
            if (feeder == net::LanBrowser::kFeederServerStatus) return lanBrowser.statusColumn(row, col);
            if (feeder == ui::MpServerSetup::kFeederAllMaps && mpFrontEnd) return mpSetup.mapText(row); // 0x537550: long name
            // exe 0x4c5f60: column 0 = entry +4 (the file name), column 1 = entry +0x14 (the date text)
            if (feeder == 0x10 && row >= 0 && size_t(row) < saveRows.size() && col <= 1) {
                if (col == 0) return saveRows[size_t(row)].name;
                return ui::saveDateText(saveRows[size_t(row)].info, [&](const std::string& k) { return saveLoc(k); });
            }
            return std::string();
        };
        menuEnv.feederSelect = [&](int feeder, int row) {
            if (feeder == 0x18) uiCvars["ui_playerProfileSelected"] = profiles.name(row); // 0x4c6050
            if (feeder == net::LanBrowser::kFeederServers) lanBrowser.select(row);
            if (feeder == ui::MpServerSetup::kFeederAllMaps && mpFrontEnd) mpSetup.selectMap(row, menuEnv.setCvar); // 0x537b00
            // 0x4c6050 for feeder 0x10: dvar ui_savegame = file name; the name and the detail text go to the UI state (0x59b724 +0x31e / +0x41e).
            if (feeder == 0x10 && row >= 0 && size_t(row) < saveRows.size()) {
                uiCvars["ui_savegame"] = saveRows[size_t(row)].name;
                saveSelName = saveRows[size_t(row)].name;
                saveSel = row;
                saveDetail = ui::saveDetailText(saveRows[size_t(row)].info, [&](const std::string& k) { return saveLoc(k); });
            }
        };
        // savegameshow / savegamehide: the save named by sv_lastSaveGame is loadable (0x4e06e0: a whole 0x4d8-byte header with
        // version 0x99). TODO(spec): [caller not read] that menurun's show/hide rule matches the exe's caller of 0x4e06e0 exactly.
        menuEnv.savegameExists = [&] {
            const std::string last = menuEnv.getCvar("sv_lastSaveGame");
            if (last.empty()) return false;
            std::ifstream f(saveDir / (last + ".svg"), std::ios::binary);
            std::vector<uint8_t> hdr(ui::kSaveHeaderSize);
            f.read(reinterpret_cast<char*>(hdr.data()), std::streamsize(hdr.size()));
            return size_t(f.gcount()) == hdr.size() && ui::saveHeaderLoadable(hdr);
        };
        menuEnv.restartAvailable = [&] { return bool(world); };
        // UI_NETGAMETYPE (245): the key handler (MP exe 0x533b70) cycles the gametype and refilters the maps; the text is the
        // gametype's display name.
        menuEnv.ownerDrawKeyed = [&](int od) { return mpFrontEnd && (od == ui::kOwnerDrawNetSource || od == ui::kOwnerDrawJoinGametype); };
        menuEnv.ownerDrawFlagShown = [&](int flag) { return ui::mpOwnerDrawFlagShown(flag, std::atoi(menuEnv.getCvar("ui_netSource").c_str())); };
        menuEnv.ownerDrawKey = [&](int od, int step) {
            if (!mpFrontEnd) return false;
            if (od == ui::kOwnerDrawNetSource) { // MP 0x533cb0: 0..2 wrapping; a refresh unless the new source is internet
                const int v = ui::mpCycleNetSource(std::atoi(menuEnv.getCvar("ui_netSource").c_str()), step);
                menuEnv.setCvar("ui_netSource", std::to_string(v));
                if (v != net::kSourceInternet) refreshBrowser();
                else { lanBrowser.setSource(v); lanBrowser.refresh(netNowMs()); }
                return true;
            }
            if (od == ui::kOwnerDrawJoinGametype) { // "All" and the gametypes, wrapping
                const int n = int(mpSetup.gametypes().size()) + 1;
                const int v = (std::atoi(menuEnv.getCvar("ui_joinGametype").c_str()) + step + n) % n;
                menuEnv.setCvar("ui_joinGametype", std::to_string(v));
                return true;
            }
            if (od != ui::MpServerSetup::kOwnerDrawNetGametype) return false;
            const int row = mpSetup.cycleGametype(step, menuEnv.getCvar, menuEnv.setCvar);
            if (row >= 0 && frontPtr) frontPtr->setFeederSelection(ui::MpServerSetup::kFeederAllMaps, row);
            return true;
        };
        menuEnv.ownerDrawText = [&](int od) {
            if (!mpFrontEnd) return std::string();
            if (od == ui::MpServerSetup::kOwnerDrawNetGametype) return mpSetup.gametypeLabel(menuEnv.getCvar);
            if (od == ui::kOwnerDrawNetSource) return ui::mpNetSourceLabel(std::atoi(menuEnv.getCvar("ui_netSource").c_str()));
            if (od == ui::kOwnerDrawJoinGametype) {
                const int g = std::atoi(menuEnv.getCvar("ui_joinGametype").c_str());
                return g > 0 && size_t(g - 1) < mpSetup.gametypes().size() ? mpSetup.gametypes()[size_t(g - 1)].display : std::string("EXE_ALL");
            }
            if (od == ui::kOwnerDrawServerRefreshDate) {
                const std::time_t t = lastServerRefresh[std::atoi(menuEnv.getCvar("ui_netSource").c_str()) & 3];
                if (!t) return std::string();
                char buf[32];
                std::strftime(buf, sizeof buf, "%d-%m-%Y %H:%M", std::localtime(&t));
                std::string fmt = saveLoc("EXE_REFRESHTIME");
                if (const size_t p = fmt.find("&&1"); p != std::string::npos) fmt.replace(p, 3, buf);
                return fmt;
            }
            return std::string();
        };
        ui::initUiCvars(menuEnv); // ui_mousePitch from m_pitch (0x4c30db)
        ui::MenuStack frontStack(scriptMenus, menuEnv); // installs open/close/focus/show hooks into menuEnv
        frontStack.setTextCodePage(logCodePage); // typed characters become bytes of the language's code page
        frontPtr = &frontStack;
        // Multiplayer / single player menu set (openiw2 extension, docs/spec/mp-ui.md), applied outside of any running menu script.
        auto applyFrontEnd = [&] {
            if (!pendingFrontEnd) return;
            mpFrontEnd = *pendingFrontEnd;
            pendingFrontEnd.reset();
            frontStack.reset();
            scriptMenus.clear();
            if (mpFrontEnd) {
                scriptMenus.loadList("ui_mp/menus.txt"); // the MP UI's menu list (MP exe 0x538109)
                scriptMenus.parseSource(ui::mpext::menuSource(), "openiw2/createserver_ext.menu");
                for (const auto& e : mpSetup.loadGametypes(vfs)) std::cout << e << "\n";
            } else {
                scriptMenus.loadList("ui/menus.txt");
                scriptMenus.loadList("ui/ingame.txt");
            }
            for (const auto& e : scriptMenus.errors()) std::cerr << "menu: " << e << "\n";
            for (const auto& e : scriptMenus.warnings()) if (e.rfind("menu file not found: ", 0) == 0) std::cerr << "WARNING: " << e << "\n";
            menuEnv.setCvar("ui_multiplayer", mpFrontEnd ? "1" : "0");
            frontStack.open("main");
        };
        // The error dialog of a network disconnect (docs/spec/net.md section 14, 0x4320d0): com_errorTitle /
        // com_errorMessage, then error_popmenu over the MP main menu, once no level is up.
        auto showNetError = [&] {
            if (!netError || world || pendingLevel) return;
            const auto find = [&](const std::string& k) { return loc.find(k); };
            menuEnv.setCvar("com_errorTitle", game::localizeTextMessage("\x14" + netError->titleKey, find));
            menuEnv.setCvar("com_errorMessage", game::localizeTextMessage(netError->message, find));
            std::cout << "error dialog: " << menuEnv.getCvar("com_errorTitle") << ": " << menuEnv.getCvar("com_errorMessage") << "\n";
            netError.reset();
            if (!mpFrontEnd) pendingFrontEnd = true;
            applyFrontEnd();
            frontStack.errorKeepsScreen(false);
            frontStack.closeAll();
            frontStack.open("main");
            frontStack.open("error_popmenu");
        };
        // Extra debug tokens shared by IW2_MENU_KEYS / IW2_SHOT_KEYS (docs/debug-flags.md): text:abc, bs, del, home, end, ins, pgup, pgdn,
        // tab, wheelup, wheeldown, dbl:X/Y, ms:N (advance the UI clock), uiscript:name.
        auto menuExtraToken = [&](const std::string& k) {
            using K = ui::MenuStack::Key;
            static const std::map<std::string, K> keys = {{"bs", K::Backspace}, {"del", K::Delete}, {"home", K::Home}, {"end", K::End}, {"ins", K::Insert},
                {"pgup", K::PageUp}, {"pgdn", K::PageDown}, {"tab", K::Tab}, {"wheelup", K::WheelUp}, {"wheeldown", K::WheelDown}};
            if (const auto it = keys.find(k); it != keys.end()) { frontStack.key(it->second); return true; }
            if (k.rfind("text:", 0) == 0) { frontStack.textInput(k.substr(5)); return true; }
            if (k.rfind("dbl:", 0) == 0) {
                const size_t sl = k.find('/');
                menuMx = float(std::atof(k.substr(4, sl - 4).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                frontStack.mouseMove(menuMx, menuMy);
                for (int i = 0; i < 2; ++i) { frontStack.tick(int(SDL_GetTicks())); frontStack.click(menuMx, menuMy); frontStack.release(); }
                return true;
            }
            if (k.rfind("uiscript:", 0) == 0) { std::string n = k.substr(9); for (auto& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch))); menuEnv.uiScript(n, ui::Command{}); return true; }
            return false;
        };
        { // the UI loads ui/menus.txt at start regardless of a level (the in-level ESC opens "main" as the pause menu)
            scriptMenus.loadList("ui/menus.txt");
            scriptMenus.loadList("ui/ingame.txt"); // pregame, victoryscreen (exe 0x581314 list)
            for (const auto& e : scriptMenus.errors()) std::cerr << "menu: " << e << "\n";
            for (const auto& e : scriptMenus.warnings()) if (e.rfind("menu file not found: ", 0) == 0) std::cerr << "WARNING: " << e << "\n";
            ui::addOpenIw2Options(scriptMenus); // the openiw2 option rows (hit marker) in the game options menu
        }
        // A level started from the command line ends its load the same way (0x449a2c): "pregame" unless ui_autoContinue (not for --shot).
        // Not in MP: the MP game has no pause and opens its own script menus.
        if (world && !world->multiplayer() && shot.empty() && std::atoi(menuEnv.getCvar("ui_autoContinue").c_str()) == 0) {
            menuEnv.setCvar("cl_paused", "1");
            frontStack.open("pregame");
        }
        // End of a level load (0x449340: the first cgame frame 0x402ac0 starts the sounds the level script queued, then cl_paused
        // is decided, client state 4): the pause state reaches the mixer before the held voices are released, so a pregame
        // keeps them where they started; the next frame then counts from here (0x449340 resets the Com_Frame clock 0x74a92c).
        auto endLoad = [&]() {
            mixer.setPaused(menuEnv.getCvar("cl_paused") == "1");
            mixer.setLoading(false);
            traceFramesAfterLoad = 0;
            firstFramePending = true;
            trace(menuEnv.getCvar("cl_paused") == "1" ? "load ended (pregame, paused)" : "load ended (running)", world ? world->timeMs() : 0); // [server time]
        };
        endLoad();
        // Server commands popupopen <index> [1] / popupclose (0x4a7094): the menu file is "ui/scriptmenus/<name>.menu" (0x4c3ad0).
        // The "1" (openmenunomouse) variant would keep the cursor hidden; here the cursor is always shown. No shipped script calls
        // openmenunomouse (every .gsc of the game files was searched), so that path is unreachable and left as is.
        // Chat server commands (switch 0x4d1bb8): `h` unless cg_teamChatsOnly, `i` always; localized (0x4ab700), cut and cleaned
        // (0x4d1550), added to the chat lines (0x4d0e10, cg_chatHeight / cg_chatTime) and printed to the console.
        auto cvarInt = [&](const char* n, int def) { const std::string v = menuEnv.getCvar(n); return v.empty() ? def : std::atoi(v.c_str()); };
        auto chatCommand = [&](const std::string& name, const std::string& arg) {
            if (name == "h" && cvarInt("cg_teamChatsOnly", 0) != 0) return;
            const std::string text = game::mpChatReceived(game::localizeTextMessage(arg, [&](const std::string& k) { return loc.find(k); }));
            chatBuffer.add(text, cgNowMs(), cvarInt("cg_chatHeight", 8), cvarInt("cg_chatTime", 12000));
            std::cout << "[chat] " << game::mpCleanString(text) << "\n";
        };
        auto menuCommand = [&](const std::string& name, const std::string& arg) {
            if (name == "popupclose") {
                if (const ui::Menu* t = frontStack.top()) frontStack.close(t->name);
                return;
            }
            const int idx = std::atoi(arg.c_str());
            if (!world || idx < 0) return;
            std::string menuName;
            if (netClient) { // network game: the server's precache slot, from its menu configstrings (net.md section 10)
                const auto cs = netClient->configstrings.find(netgame::kMenuConfigstrings + idx);
                if (cs != netClient->configstrings.end()) menuName = cs->second;
            } else if (size_t(idx) < world->menuNames().size()) menuName = world->menuNames()[size_t(idx)];
            if (menuName.empty()) return;
            if (world->multiplayer() && ui::menuIsHidden(menuName)) return; // a hidden entry (src/ui/menuhide.h): the server's request is ignored
            // The MP client loads them from "ui_mp/scriptmenus/" (0x5326d0).
            if (!scriptMenus.find(menuName)) scriptMenus.loadFile(std::string(world->multiplayer() ? "ui_mp" : "ui") + "/scriptmenus/" + menuName + ".menu");
            menuMx = 320.0f, menuMy = 240.0f;
            if (!frontStack.open(menuName)) std::cerr << "warning: script menu " << menuName << " not found\n";
        };
        // MP togglemenu (client 0x4107b0, also ESC in a running level): the script main menu the server named with
        // setClientCvar g_scriptMainMenu (the client keeps it in its own buffer, 0x4d1b3e) opens; when it is not a loaded
        // menu, "main". The MP game is not paused by it.
        auto mpToggleMenu = [&]() {
            if (!frontStack.empty()) {
                if (const ui::Menu* t = frontStack.top()) frontStack.close(t->name);
                return;
            }
            menuMx = 320.0f, menuMy = 240.0f;
            // menuEnv.getCvar: the local server's setClientCvar, or on a network client the "v" server command (menuEnv.setCvar).
            const std::string sm = menuEnv.getCvar("g_scriptMainMenu");
            if (!sm.empty() && !scriptMenus.find(sm)) scriptMenus.loadFile("ui_mp/scriptmenus/" + sm + ".menu");
            frontStack.open(ui::toggleMenuTarget(sm, !sm.empty() && scriptMenus.find(sm)));
        };
        // Bound console commands of the MP bindings (togglemenu, openscriptmenu <menu> <response>).
        auto runConsoleCommands = [&]() {
            for (const std::string& line : input.takeConsoleCommands()) {
                std::istringstream in(line);
                std::string c, menu, response;
                in >> c >> menu >> response;
                for (auto& ch : c) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (!world || !world->multiplayer()) continue;
                if (c == "togglemenu") mpToggleMenu();
                // chatmodepublic / chatmodeteam (0x4039d0 / 0x403a20): clear the field, set its kind, toggle the message catcher.
                else if (c == "chatmodepublic" || c == "chatmodeteam") {
                    chatField.team = c == "chatmodeteam";
                    chatField.text.clear();
                    chatField.open = !chatField.open;
                }
                // mp_QuickMessage (0x4c1680): the UI menu "quickmessage" (ui_mp/wm_quickmessage.menu, listed in ui_mp/ingame.txt), only
                // with the local player's pm_flags 0x800000 (playing or dead and not following, 0x501068). The port loads the menu
                // and its three script menus on first use.
                else if (c == "mp_quickmessage") {
                    const game::Entity* lp = world->mpClientEntity(world->mpLocalClient());
                    const std::string ss = lp ? lp->obj->get("sessionstate").toString() : std::string();
                    if ((ss == "playing" || ss == "dead") && frontStack.empty()) {
                        if (!scriptMenus.find("quickmessage")) scriptMenus.loadFile("ui_mp/wm_quickmessage.menu");
                        for (const char* m : {"quickcommands", "quickstatements", "quickresponses"})
                            if (!scriptMenus.find(m)) scriptMenus.loadFile(std::string("ui_mp/scriptmenus/") + m + ".menu");
                        frontStack.open("quickmessage");
                    }
                }
                else if (c == "openscriptmenu") {
                    if (response.empty()) std::cout << "USAGE: openscriptmenu <parent menu name> <script menu response>\n";
                    else world->mpOpenScriptMenu(menu, response);
                }
            }
        };
        bool mantleHint = false;
        float hudAimSpread = 255.0f, hudViewHeight = 60.0f; // ps+0x5b0 and the view height for the crosshair
        float aimSpreadBeforeCmd = 255.0f;
        game::GunAim drawnGunAim;     // the pose stored by the last drawn frame (cgame 0x4b6d40), for the next command
        game::GunAimDelay gunAimDelay; // the staleness of that pose for a shot (docs/spec/weapons.md)
        // Damage direction icons (cgame 0x4ba2a0 → 8 slots at 0xf79704: start, duration, yaw).
        struct DamageIcon { int64_t start = -1000000; int duration = 0; float yaw = 0; };
        std::array<DamageIcon, 8> damageIcons{};
        int lastDamageEvent = 0;
        // Menu "weaponinfo" (hud.menu:184): the ammo counter backdrop `ammocounterback` (ownerdraw CG_PLAYER_AMMO_BACKDROP 6), rect
        // -100 -50 100 50 from the bottom right of the 4:3 area. Ownerdraw 6 (0x4a7810): hidden in a vehicle or without a weapon, alpha the ammo fade, red at low stock.
        const ui::Item* ammoBackItem = nullptr;
        const ui::Item* fireModeItem = nullptr; // item "weaponfiremode" (ownerdraw 83), rect -115 -35 20 20
        const ui::Item* weapNameItem = nullptr; // item "weapname" (ownerdraw 81), rect -385 -15 290 40
        const ui::Item* ammoTextItem = nullptr; // item "ammotex" (ownerdraw CG_PLAYER_AMMO_VALUE 5), rect -85 -15 70 40
        if (const ui::Menu* m = hudMenus.find("weaponinfo"))
            for (const auto& it : m->items) {
                if (it->name == "ammocounterback") ammoBackItem = it.get();
                else if (it->name == "ammotex") ammoTextItem = it.get();
                else if (it->name == "weaponfiremode") fireModeItem = it.get();
                else if (it->name == "weapname") weapNameItem = it.get();
            }
        const ui::Item* cursorHintItem = nullptr; // menu "Cursorhints" (ownerdraw CG_CURSORHINT)
        if (const ui::Menu* m = hudMenus.find("Cursorhints"))
            for (const auto& it : m->items)
                if (it->name == "chRect") cursorHintItem = it.get();
        // Menu "Compass" (hud.menu:315): rect -20 -140 160 160 LEFT/BOTTOM; items compassback/compasshighlight (ownerdraw 85),
        // compassface (84), compasspointers (86). 87/88 (friendly actors, tanks) are not drawn.
        const ui::Menu* compassMenu = hudMenus.find("Compass");
        // Scope overlay (cgame 0x4bc8a0): the weapon has an adsOverlayShader or adsOverlayReticle and the aim fraction
        // is not 0; alpha = (fraction − (1 − adsZoomInFrac)) / adsZoomInFrac while zooming in (adsZoomOutFrac while
        // zooming out), shown above 0.01 and capped at 1.
        auto scopeAlpha = [&]() -> float {
            if (killcamRig.active) return killcamScopeAlpha(); // the killcam: the followed weapon's overlay at its recorded aim
            if (!pweapon.equipped() && !pweapon.binocularsUp()) return 0;
            const std::string shader = pweapon.field("adsOverlayShader"), reticle = pweapon.field("adsOverlayReticle");
            if (shader.empty() && (reticle.empty() || reticle == "none")) return 0;
            return game::scopeOverlayAlpha(true, pweapon.adsFrac(), pweapon.number(pweapon.adsZoomingIn() ? "adsZoomInFrac" : "adsZoomOutFrac"));
        };
        auto hudText = [&](std::vector<Quad2D>& out, const std::string& text, float x, float y, float scale, const assets::Font& f,
                           const uint8_t rgba[4], float alignX, float alignY, int style = 0) {
            // alignX/alignY: 0 = left/top, 0.5 = centre, 1 = right/bottom of the text box at (x, y).
            // style: the menu item's textstyle (itemDef +0x290, parsed at exe 0x4d2370), which the exe passes unchanged
            // as the last argument of the renderer's text command (0x4c3790 → refexport 0x1000c070, cmd 0x15 +0x1c).
            // The renderer (0x100329a0) draws, for ITEM_TEXTSTYLE_SHADOWED (3) / ITEM_TEXTSTYLE_SHADOWEDMORE (6),
            // each glyph first 1 / 2 screen pixels right and down (0x10032b3d) in black with the text colour's alpha
            // (colour & 0xff000000, 0x100329d7; colour codes do not change it, 0x10032c51), then the glyph itself.
            // It tests no other style.
            const float shadow = style == 3 ? 1.0f : style == 6 ? 2.0f : 0.0f;
            const int tex = materialTexture(f.material);
            std::vector<std::string> lines(1);
            for (char c : text) {
                if (c == '\n') lines.emplace_back();
                else lines.back() += c;
            }
            const float lineH = float(f.pixelHeight) * scale;
            float ty = y - alignY * lineH * float(lines.size());
            // Colour codes (renderer 0x100329a0, table 0x10197f9c): "^" + digit 0..9 is not drawn and takes no width; 0..6 select black, red, green,
            // yellow, blue, cyan, magenta (the base alpha is kept), 7 restores the item colour. TODO(spec): [run-time values unknown] 8 and 9 read two dwords set at run time
            // (0x1070c4c4 / 0x1070c4c0, zero in the file); here they keep the current colour.
            static const uint8_t kCodes[7][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {0, 255, 255}, {255, 0, 255}};
            auto isCode = [](const std::string& l, size_t i) { return l[i] == '^' && i + 1 < l.size() && l[i + 1] >= '0' && l[i + 1] <= '9'; };
            for (const auto& line : lines) {
                float width = 0;
                for (size_t i = 0; i < line.size(); ++i) {
                    if (isCode(line, i)) { ++i; continue; }
                    if (const auto* g = f.find((unsigned char)line[i])) width += float(g->dx) * scale;
                }
                float pen = x - alignX * width;
                uint8_t cur[4];
                std::memcpy(cur, rgba, 4);
                for (size_t ci = 0; ci < line.size(); ++ci) {
                    if (isCode(line, ci)) {
                        const int n = line[ci + 1] - '0';
                        if (n == 7) std::memcpy(cur, rgba, 4);
                        else if (n < 7) std::memcpy(cur, kCodes[n], 3);
                        // MP: the client hands the renderer g_TeamColor_Allies / g_TeamColor_Axis for 8 / 9 (0x403441, defaults
                        // 0.5 0.5 1 and 1 0.5 0.5; docs/spec/mp-chat.md).
                        else if (world && world->multiplayer()) {
                            static const uint8_t kTeam[2][3] = {{128, 128, 255}, {255, 128, 128}};
                            std::memcpy(cur, kTeam[n - 8], 3);
                        }
                        ++ci;
                        continue;
                    }
                    const unsigned char c = (unsigned char)line[ci];
                    const auto* g = f.find(c);
                    if (!g) continue;
                    if (g->width && g->height) {
                        Quad2D q{pen + float(g->x0) * scale, ty + lineH + float(g->y0) * scale, 0, 0, g->s0, g->t0, g->s1, g->t1};
                        q.x1 = q.x0 + float(g->width) * scale;
                        q.y1 = q.y0 + float(g->height) * scale;
                        std::memcpy(q.rgba, cur, 4);
                        q.texture = tex;
                        if (shadow > 0) {
                            Quad2D s = q;
                            s.x0 += shadow, s.x1 += shadow, s.y0 += shadow, s.y1 += shadow;
                            s.rgba[0] = s.rgba[1] = s.rgba[2] = 0;
                            out.push_back(s);
                        }
                        out.push_back(q);
                    }
                    pen += float(g->dx) * scale;
                }
                ty += lineH;
            }
        };
        // Menu item text: localized string; the font and scale follow menuFont below (exe 0x4c38e0).
        auto menuLocalized = [&](const std::string& raw) {
            std::string key = raw;
            if (!key.empty() && key[0] == '@') key.erase(0, 1);
            const std::string* t = loc.find(key);
            return t ? *t : key;
        };
        // Menu text font (exe 0x4c38e0, handles loaded at 0x4c3487..0x4c34fe): the item's textfont 2 (UI_FONT_BIG) = $bigfont, 3 (SMALL) =
        // $smallfont, 5 (CONSOLE) = $consolefont; 4 (BOLD) and 0/1 choose by scale = textscale * (screen height / 480) (0x793944)
        // against ui_smallFont (0.25), ui_bigFont (0.4) and ui_extraBigFont (0.55): <= small $smallfont; generic: < big $font,
        // < extraBig $bigfont, else $extrabigfont; BOLD: < extraBig $font, else $boldfont.
        float menuScreenH = 480.0f; // set by the HUD builder each frame
        auto menuFont = [&](float ts, int textFont) {
            if (textFont == 2) return font("bigFont");
            if (textFont == 3) return font("smallFont");
            if (textFont == 5) return font("consoleFont");
            const float s = ts * menuScreenH / 480.0f;
            if (s <= 0.25f) return font("smallFont");
            if (textFont == 4) return font(s < 0.55f ? "normalFont" : "boldFont");
            return font(s >= 0.55f ? "extraBigFont" : s < 0.4f ? "normalFont" : "bigFont");
        };
        menuEnv.bindKey = [&](const std::string& command, const std::string& key) {
            input.rebind(command, key);
            profileDirty = true;
            if (world) { // getkeybinding follows the new bindings
                world->keyBindings.clear();
                for (const auto& [k, c] : input.bindings()) world->keyBindings[c].push_back(k);
            }
        };
        // The renderer's enum dvars (r_mode, r_displayRefresh: the D3D adapter's mode lists) have no equivalent here: empty domain.
                // dvarEnumList domains (exe 0x4cd720: the domain string of the named enum dvar; the D3D adapter modes behind r_mode and
        // r_displayRefresh are registered by the renderer DLL). No D3D adapter here, so by design the primary display's SDL fullscreen modes are listed:
        // r_mode = distinct "WxH" sizes, r_displayRefresh = distinct "N Hz" rates.
        menuEnv.enumValues = [](const std::string& dvar) {
            std::vector<std::string> out;
            const bool mode = dvar == "r_mode", rate = dvar == "r_displayRefresh";
            if (!mode && !rate) return out;
            int n = 0;
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
            for (int i = 0; modes && i < n; ++i) {
                char buf[64];
                if (mode) std::snprintf(buf, sizeof buf, "%dx%d", modes[i]->w, modes[i]->h);
                else std::snprintf(buf, sizeof buf, "%d Hz", int(modes[i]->refresh_rate + 0.5f));
                if (std::find(out.begin(), out.end(), buf) == out.end()) out.push_back(buf);
            }
            SDL_free(modes);
            return out;
        };
        menuEnv.language = [&] { // ids of the ui_language list in options_game.menu
            static const char* const names[] = {"english", "french", "german", "italian", "spanish", "british", "russian", "polish", "korean", "taiwanese", "japanese", "chinese", "thai"};
            for (int i = 0; i < 13; ++i)
                if (language == names[i]) return i;
            return 0;
        };
        menuEnv.textWidth = [&](const ui::Item& it) {
            const assets::Font* f = menuFont(it.textScale, it.textFont);
            if (!f) return 0.0f;
            const float gs = it.textScale * 48.0f / float(f->pixelHeight);
            float width = 0;
            for (unsigned char ch : menuLocalized(it.text))
                if (const auto* g = f->find(ch)) width += float(g->dx) * gs;
            return width;
        };
        // MP scoreboard (0x4dabf0): layout from src/game/mpscoreboard.cpp, drawn on the 640x480 screen of the 4:3 area.
        auto drawMpScoreboard = [&](std::vector<Quad2D>& out, uint32_t w, uint32_t h) {
            if (!world || !world->multiplayer() || menuEnv.getCvar("cl_paused") == "1") return;
            const int64_t nowMs = cgNowMs();
            const bool held = hudFade.objectivesHeld || mpIntermissionView;
            if (held && !mpSb.shown) mpSb.scroll = 0;
            if (!held && mpSb.shown) mpSb.hiddenAt = nowMs;
            mpSb.shown = held;
            float alpha = 1.0f;
            if (!held) {
                const int64_t el = nowMs - mpSb.hiddenAt;
                if (el < 0 || el >= 100) return;
                alpha = float(100 - el) / 100.0f;
            }
            // A new level starts a new scoreboard: no list and no request time of the previous map.
            bool newLevel = false;
            if (netClient && mpSb.request.due(nowMs, worldGeneration, &newLevel)) netClient->addReliableCommand("score");
            if (newLevel) mpSb.netScores.clear();
            const float sc = float(h) / 480.0f, left43 = (float(w) - 640.0f * sc) * 0.5f;
            auto pickFont = [&](float s) { const float fs = s * float(h) / 480.0f; return font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont"); };
            auto tw = [&](const std::string& t, float s) {
                const assets::Font* f = pickFont(s);
                if (!f) return 0.0f;
                float x = 0;
                for (size_t i = 0; i < t.size(); ++i) {
                    if (t[i] == '^' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9') { ++i; continue; }
                    if (const auto* g = f->find((unsigned char)t[i])) x += float(g->dx) * s * 48.0f / float(f->pixelHeight);
                }
                return std::nearbyint(x);
            };
            auto locKey = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
            const auto& mps = world->mpState();
            // Listen server: the scores this server would send; network game: the last "b" command.
            const std::string cmd = netClient ? mpSb.netScores : world->mpScoresCommand();
            // Network game: the score lines carry the server's client numbers; names and teams come from the snapshot's
            // client states (every client, also one whose entity is not in the snapshot), not from the remote world's slots.
            const net::ClientSnapshot* sbSnap = netClient ? netClient->latest() : nullptr;
            const auto teamOf = [&](int cl) {
                std::string n;
                int t = 0;
                if (sbSnap && netgame::RemoteView::clientInfo(*sbSnap, cl, n, t)) return t;
                game::Entity* e = world->mpClientEntity(netClient ? netgame::RemoteView::worldSlot(cl, netClient->clientNum) : cl);
                return e ? world->mpTeamOf(e) : 0;
            };
            const game::MpScoreData data = game::mpParseScores(cmd.size() > 2 ? cmd.substr(2) : std::string(), teamOf);
            game::MpScoreboardInput in;
            in.data = &data;
            in.localClient = netClient ? netClient->clientNum : world->mpLocalClient();
            in.alpha = alpha;
            in.nameOf = [&](int cl) {
                std::string n;
                int t = 0;
                if (sbSnap && netgame::RemoteView::clientInfo(*sbSnap, cl, n, t)) return n;
                game::Entity* e = world->mpClientEntity(netClient ? netgame::RemoteView::worldSlot(cl, netClient->clientNum) : cl);
                return e ? e->obj->get("name").toString() : std::string();
            };
            in.clientValid = [&](int cl) { return netClient || (cl >= 0 && size_t(cl) < mps.clients.size() && mps.clients[size_t(cl)].state != game::MpClientState::Free); };
            in.localize = [&](const std::string& k) {
                std::string key = k;
                if (key.size() > 1 && key[0] == '@') key = key.substr(1);
                const std::string* t = loc.find(key);
                return t ? *t : k;
            };
            in.textWidth = tw;
            in.textHeight = [](float s) { return 48.0f * s; }; // TODO(spec): the renderer's font height query (0x68a318)
            in.statusIconMaterial = [&](int i) { return i >= 1 && size_t(i) <= mps.statusIcons.size() ? mps.statusIcons[size_t(i) - 1] : std::string(); };
            // Network game: the server info's g_gametype (configstring 0), as the load screen reads it.
            const std::string gt = netClient ? net::infoValueForKey(netClient->configstrings[0], "g_gametype") : menuEnv.getCvar("g_gametype");
            std::string gtName = gt;
            if (const auto txt = vfs.read("maps/mp/gametypes/" + gt + ".txt")) {
                std::istringstream gin(std::string(txt->begin(), txt->end()));
                std::string tok;
                if (gin >> tok) gtName = tok;
                if (gtName.size() >= 2 && gtName.front() == '"' && gtName.back() == '"') gtName = gtName.substr(1, gtName.size() - 2);
            }
            in.gametypeName = locKey(gtName);
            in.mapName = menuEnv.getCvar("mapname"); // TODO(spec): the display name lookup of 0x532cd0
            in.hostName = menuEnv.getCvar("sv_hostname");
            in.objectiveText = world->mpLocalCvar("cg_objectiveText") ? *world->mpLocalCvar("cg_objectiveText") : std::string();
            {   // setClientCvar joins a localized string and its arguments with spaces ("KEY 100"): the key's text with &&1.. filled.
                std::istringstream oin(in.objectiveText);
                std::string key, arg;
                oin >> key;
                if (const std::string* t = loc.find(key)) {
                    std::string txt = *t;
                    for (int k = 1; oin >> arg; ++k) {
                        const std::string mark = "&&" + std::to_string(k);
                        if (const size_t at = txt.find(mark); at != std::string::npos) txt.replace(at, mark.size(), arg);
                    }
                    in.objectiveText = txt;
                }
            }
            auto cv = [&](const char* n, const char* def) { const std::string v = menuEnv.getCvar(n); return v.empty() ? std::string(def) : v; };
            in.teamNameAxis = cv("g_TeamName_Axis", "GAME_AXIS");
            in.teamNameAllies = cv("g_TeamName_Allies", "GAME_ALLIES");
            in.bannerNone = cv("g_ScoresBanner_None", "mpflag_none");
            in.bannerAxis = cv("g_ScoresBanner_Axis", "mpflag_german");
            in.bannerAllies = cv("g_ScoresBanner_Allies", "mpflag_american");
            in.bannerSpectators = cv("g_ScoresBanner_Spectators", "mpflag_spectator");
            in.colorAxis = game::mpTeamColor(1, menuEnv.getCvar("g_TeamColor_Axis"), menuEnv.getCvar("g_TeamColor_Allies"));
            in.colorAllies = game::mpTeamColor(2, menuEnv.getCvar("g_TeamColor_Axis"), menuEnv.getCvar("g_TeamColor_Allies"));
            in.bannerHeight = std::clamp(cvarInt("cg_scoreboardBannerHeight", 24), 1, 100);
            in.scroll = mpSb.scroll;
            in.firstTeam = mpSb.firstTeam;
            const game::MpScoreboardOutput lay = game::mpScoreboardLayout(in);
            mpSb.scroll = lay.scroll;
            mpSb.firstTeam = lay.firstTeam;
            auto col = [](const std::array<float, 4>& c, uint8_t o[4]) { for (int k = 0; k < 4; ++k) o[k] = uint8_t(std::clamp(c[size_t(k)], 0.0f, 1.0f) * 255.0f); };
            for (const game::SbItem& it : lay.items) {
                if (!it.isText) {
                    const game::SbRect& r = it.rect;
                    Quad2D q{left43 + r.x * sc, r.y * sc, left43 + (r.x + r.w) * sc, (r.y + r.h) * sc};
                    col(r.rgba, q.rgba);
                    if (r.material != "white") q.texture = materialTexture(r.material);
                    if (r.material == "black") { q.rgba[0] = q.rgba[1] = q.rgba[2] = 0; q.texture = 0; }
                    out.push_back(q);
                    continue;
                }
                const game::SbText& t = it.text;
                const assets::Font* f = pickFont(t.scale);
                if (!f || t.text.empty()) continue;
                uint8_t c[4];
                col(t.rgba, c);
                hudText(out, t.text, left43 + t.x * sc, t.y * sc, t.scale * 48.0f / float(f->pixelHeight) * sc, *f, c, 0.0f, 1.0f, t.style);
            }
        };
        // Killcam (docs/spec/mp-hud.md "Killcam"): the followed client's archived player state is the HUD's player state, so
        // the ammo counter, weapon name, firemode icon and health bar show the killer's (set before buildHud).
        std::optional<game::World::MpKillcamView> hudKillcam;
        // blurWorld of the open menus (docs/spec/ui.md "blurWorld"): only over a running game (the client gate at 0x4cf910
        // needs the UI key catcher and a connection); the front end without a world is not blurred.
        auto worldMenuBlur = [&]() {
            if (!world) return 0.0f;
            std::vector<float> b;
            for (const ui::Menu* m : frontStack.menus()) b.push_back(m->blurWorld);
            return game::menuBlurRadius(b);
        };
        bool showFps = false; // F12, openiw2 feature: frame counter in the top right corner
        int fpsShown = 0;
        // The screenshotJpeg / screenshot commands (docs/spec/controls.md, Screenshots): set by a key event, taken after the
        // frame was drawn, so the file holds the frame as displayed (scene and HUD, at the window size).
        enum class Shot { None, Jpeg, Png };
        Shot pendingShot = Shot::None;
        auto takeShot = [&](Shot s, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
            const std::string ext = s == Shot::Jpeg ? "jpg" : "png";
            std::error_code ec;
            std::filesystem::create_directories(shotBase / "screenshots", ec);
            const std::filesystem::path path = util::screenshotPath(shotBase / "screenshots", ext);
            bool ok = false;
            if (s == Shot::Jpeg) ok = iw2view::writeJpegRgba(path, px.data(), int(w), int(h), 95);
            else {
                try {
                    util::writePngRgba(path, px.data(), int(w), int(h));
                    ok = true;
                } catch (const std::exception&) {
                }
            }
            // "Wrote %s" / "ScreenShot: Couldn't create a file": the renderer DLL's own messages.
            if (ok) std::cout << "Wrote " << path.string() << "\n";
            else std::cout << "ScreenShot: Couldn't create a file\n";
        };
        auto buildHud = [&](uint32_t w, uint32_t h, const Camera& c) {
            menuScreenH = float(h);
            if (world && world->multiplayer() && !netClient) mpIntermissionView = world->mpSessionState(world->mpLocalClient()) == 3;
            std::vector<Quad2D> out, front; // front: foreground script elements, drawn last
            std::vector<Quad2D> scopeQuads; // scope / binocular overlay: drawn below the script HUD elements and subtitles (see below)
            size_t hudElemStart = 0;
            const float sc = float(h) / 480.0f;               // virtual pixel to screen pixel
            const float left43 = (float(w) - 640.0f * sc) * 0.5f; // left edge of the 4:3 area
            auto field = [](const script::ObjectPtr& o, const char* k, float def) {
                const script::Value v = o->get(k);
                return v.isNumber() ? v.asFloat() : def;
            };
            auto lerpOver = [&](const script::ObjectPtr& o, const char* startKey, const char* timeKey) {
                const script::Value st = o->get(startKey), tm = o->get(timeKey);
                if (!st.isNumber() || !tm.isNumber() || tm.asFloat() <= 0) return 1.0f;
                return std::clamp((float(cgNowMs()) - st.asFloat()) / tm.asFloat(), 0.0f, 1.0f);
            };
            // MP world sprites (docs/spec/mp-hud.md): script head icons (0x4dbcc0 -> 0x4dbba0) and waypoint HUD elements (type 13,
            // 0x4ddd30 -> 0x4ddcb0) are sprites at a world point, radius in world units. Here they are projected and drawn over
            // the scene. TODO(spec): the sprite's depth test, the screen unit of the constant size (× 0.0043) and of the head
            // icon minimum screen radius (taken as fractions of the screen height).
            if (world && world->multiplayer()) {
                const Vec3 f = c.forward(), r = c.right(), u = cross(r, f);
                const float t = 1.0f / std::tan(std::atan(std::tan(c.fovX43 * float(M_PI) / 360.0f) * 0.75f));
                auto sprite = [&](const script::Vec3& p, float radius, bool constant, float minScreen, const std::string& mat, float alpha) {
                    const Vec3 d{p[0] - c.pos.x, p[1] - c.pos.y, p[2] - c.pos.z};
                    const float z = dot(d, f);
                    if (z < 1.0f) return;
                    const float k = t * float(h) * 0.5f / z;
                    const float sx = float(w) * 0.5f + dot(d, r) * k, sy = float(h) * 0.5f - dot(d, u) * k;
                    float rp = constant ? radius * 0.0043f * float(h) : radius * k;
                    rp = std::max(rp, minScreen * float(h));
                    Quad2D q{sx - rp, sy - rp, sx + rp, sy + rp};
                    q.rgba[3] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
                    q.texture = materialTexture(mat);
                    out.push_back(q);
                };
                const std::string mr = menuEnv.getCvar("cg_headIconMinScreenRadius");
                const float minR = mr.empty() ? 0.02f : float(std::atof(mr.c_str()));
                const bool constHead = menuEnv.getCvar("cg_constantSizeHeadIcons") == "1";
                for (const auto& hi : world->mpHeadIcons()) sprite(hi.pos, hi.size, constHead, minR, hi.material, 1.0f);
                for (const auto& o : world->hudElems()) {
                    if (!o->get("$waypoint").truthy() || !world->mpHudElemVisible(o) || !o->get("$shader").isString()) continue;
                    const script::Value z = o->get("z");
                    const script::Vec3 p{o->get("x").isNumber() ? o->get("x").asFloat() : 0.0f, o->get("y").isNumber() ? o->get("y").asFloat() : 0.0f,
                                         z.isNumber() ? z.asFloat() : 0.0f};
                    const float wd = o->get("$width").isNumber() ? o->get("$width").asFloat() : 0.0f;
                    const float al = o->get("alpha").isNumber() ? o->get("alpha").asFloat() : 1.0f;
                    if (wd != 0.0f && al > 0.0f) sprite(p, wd, o->get("$waypointconst").asInt() != 0, 0.0f, o->get("$shader").asString(), al);
                }
            }
            if (world) {
                hudElemStart = out.size();
                std::vector<script::ObjectPtr> elems = world->hudElems();
                std::stable_sort(elems.begin(), elems.end(), [&](const auto& a, const auto& b) { return field(a, "sort", 0) < field(b, "sort", 0); });
                for (const auto& o : elems) {
                    if (!world->mpHudElemVisible(o)) continue; // another client's element (newclienthudelem / newteamhudelem)
                    if (o->get("$waypoint").truthy()) continue; // a world sprite (setWayPoint), drawn above
                    // Elements with `foreground` set draw above the game's own HUD (crosshair, ammo); the intro
                    // screen's black backdrop uses it (inferred; docs/spec/ui.md).
                    std::vector<Quad2D>& dst = o->get("foreground").truthy() ? front : out;
                    float alpha = field(o, "alpha", 1);
                    script::Vec3 color = o->get("color").isVector() ? o->get("color").asVector() : script::Vec3{1, 1, 1};
                    if (const float t = lerpOver(o, "$fadestart", "$fadetime"); t < 1) {
                        alpha = field(o, "$fadealpha", alpha) + (alpha - field(o, "$fadealpha", alpha)) * t;
                        if (o->get("$fadecolor").isVector())
                            for (int k = 0; k < 3; ++k) color[k] = o->get("$fadecolor").asVector()[k] + (color[k] - o->get("$fadecolor").asVector()[k]) * t;
                    }
                    if (alpha <= 0.001f) continue;
                    float x = field(o, "x", 0), y = field(o, "y", 0);
                    if (const float t = lerpOver(o, "$movestart", "$movetime"); t < 1) {
                        x = field(o, "$movex", x) + (x - field(o, "$movex", x)) * t;
                        y = field(o, "$movey", y) + (y - field(o, "$movey", y)) * t;
                    }
                    std::string ha = o->get("horzalign").toString(), va = o->get("vertalign").toString();
                    // "center_safearea" (the MP scripts, e.g. _killcam.gsc:102) centres on the safe area; with no safe-area
                    // inset on a PC screen it is the centre. TODO(spec): the safe-area margins.
                    if (ha == "center_safearea") ha = "center";
                    if (va == "center_safearea") va = "middle";
                    const std::string ax = o->get("alignx").toString(), ay = o->get("aligny").toString();
                    const bool fullX = ha == "fullscreen", fullY = va == "fullscreen";
                    const float sx = fullX ? float(w) / 640.0f : sc, sy = fullY ? float(h) / 480.0f : sc;
                    const float baseX = fullX ? 0.0f : ha == "center" ? float(w) * 0.5f : ha == "right" ? left43 + 640.0f * sc
                                      : ha == "subleft" ? 0.0f : ha == "subright" ? float(w) : left43;
                    const float baseY = fullY ? 0.0f : va == "middle" ? float(h) * 0.5f : va == "bottom" ? float(h) : 0.0f;
                    const float px = baseX + x * sx, py = baseY + y * sy;
                    const float fx = ax == "center" ? 0.5f : ax == "right" ? 1.0f : 0.0f;
                    const float fy = ay == "middle" ? 0.5f : ay == "bottom" ? 1.0f : 0.0f;
                    const uint8_t rgba[4] = {uint8_t(std::clamp(color[0], 0.0f, 1.0f) * 255), uint8_t(std::clamp(color[1], 0.0f, 1.0f) * 255),
                                             uint8_t(std::clamp(color[2], 0.0f, 1.0f) * 255), uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255)};
                    if (o->get("$shader").isString()) {
                        float ww = field(o, "$width", 0), hh = field(o, "$height", 0);
                        if (const float t = lerpOver(o, "$scalestart", "$scaletime"); t < 1) {
                            ww = field(o, "$scalew", ww) + (ww - field(o, "$scalew", ww)) * t;
                            hh = field(o, "$scaleh", hh) + (hh - field(o, "$scaleh", hh)) * t;
                        }
                        const std::string shader = o->get("$shader").asString();
                        Quad2D q{px - fx * ww * sx, py - fy * hh * sy, 0, 0};
                        q.x1 = q.x0 + ww * sx;
                        q.y1 = q.y0 + hh * sy;
                        std::memcpy(q.rgba, rgba, 4);
                        q.texture = materialTexture(shader);
                        if (shader == "black") q.rgba[0] = q.rgba[1] = q.rgba[2] = 0;
                        dst.push_back(q);
                        if (o->get("$clockdur").isNumber()) {
                            // Clock needle (cgame 0x4c2c30): material + "needle" over the same box, rotated by the angle of the
                            // remaining (or elapsed) time: R * 360 / duration (or one turn per minute without a duration), in 1/65536 turns.
                            const float endMs = o->get("$timerend").asFloat();
                            float r = o->get("$clockup").asInt() ? float(cgNowMs()) - endMs : endMs - float(cgNowMs());
                            r = std::max(0.0f, r);
                            const float dur = o->get("$clockdur").asFloat();
                            const int a16 = (dur != 0.0f ? int(r * 360.0f / dur * 182.04444885f) : int(r * 1.0922666f)) & 0xffff;
                            Quad2D n = q;
                            n.texture = materialTexture(shader + "needle");
                            n.rotDeg = float(a16) * 0.0054931640625f; // TODO(spec): [call site not read] rotation sign / pivot of 0x4b0310 as called by 0x4c2d7a
                            dst.push_back(n);
                        }
                        continue;
                    }
                    // Text: label, set text, value or timer.
                    auto str = [&](const script::Value& v) -> std::string {
                        if (std::holds_alternative<script::LocalizedString>(v.v)) {
                            const auto& key = std::get<script::LocalizedString>(v.v).key;
                            const std::string* t = loc.find(key);
                            return t ? *t : key;
                        }
                        return v.isDefined() ? v.toString() : std::string();
                    };
                    std::string text = str(o->get("$text"));
                    if (o->get("$timerend").isNumber()) {
                        const bool up = o->get("$timerup").isNumber(); // 0x4c1f6a: now - start
                        const int secs = std::max(0, up ? int((float(cgNowMs()) - o->get("$timerend").asFloat()) / 1000.0f)
                                                        : int(std::ceil((o->get("$timerend").asFloat() - float(cgNowMs())) / 1000.0f)));
                        text = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
                    } else if (o->get("$value").isDefined()) {
                        const script::Value v = o->get("$value");
                        text = v.isNumber() && v.asFloat() == float(int(v.asFloat())) ? std::to_string(int(v.asFloat())) : v.toString();
                    }
                    text = game::hudElemText(str(o->get("label")), text); // 0x4dd330 (MP), src/game/textmessage.h
                    // Key-name placeholders "[{command}]" (0x4a36b0, found by "[{" / "}]"): replaced by the key(s) bound
                    // to the command (0x4cdb30: up to two, joined by " <KEY_OR> "), shown by their key.str name (0x498a20);
                    // an unbound command shows KEY_UNBOUND. The surrounding "[" "]" of "[[{+x}]]" stay.
                    // 0x4c6a20 is the generic string translator (a leading 0x15 byte marks a literal; otherwise 0x498a20 does an exact
                    // map find; a miss gives "^1UNLOCALIZED(^7" s "^1)^7"); it has no KEY_ special case. The "KEY_" in the refs comes from
                    // the string file name (key.str -> KEY_<REFERENCE>), as with loc.find("KEY_USE").
                    for (size_t b; (b = text.find("[{")) != std::string::npos;) {
                        const size_t e = text.find("}]", b);
                        if (e == std::string::npos) break;
                        const auto it = world->keyBindings.find(text.substr(b + 2, e - b - 2));
                        auto name = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
                        std::string keys = name("UNBOUND");
                        if (it != world->keyBindings.end() && !it->second.empty()) {
                            keys = name(it->second[0]);
                            if (it->second.size() > 1) keys += " " + name("OR") + " " + name(it->second[1]);
                        }
                        text = text.substr(0, b) + keys + text.substr(e + 2);
                    }
                    if (text.empty()) continue;
                    // Font and size (cgame 0x4c27b8, hudelem font +0x14): 0 "default": textScale = fontScale * 0.25 (0x591fe0),
                    // s = textScale * h / 480 (0x793944); s <= ui_smallFont $smallfont, >= ui_extraBigFont $extrabigfont,
                    // < ui_bigFont $font, else $bigfont. 1 "bigfixed": textScale = fontScale * 0.5 (0x591e10); s <= ui_smallFont
                    // $smallfont, < ui_bigFont $font, else $boldfont. 2 "smallfixed": $consolefont, textScale = fontScale / 3
                    // (0x592250). Handles: 0x1107448 $bigfont, 0x110744c $smallfont, 0x1107450 $consolefont, 0x1107454 $boldfont,
                    // 0x1107458 $font, 0x110745c $extrabigfont (0x4c3493..0x4c34fe). Glyphs: textScale * 48 / font height virtual
                    // units per font pixel, as menu text. Before, every hudelem used $font stretched (blurry intro text).
                    const std::string fontName = o->get("font").toString();
                    const int fontType = fontName == "bigfixed" ? 1 : fontName == "smallfixed" ? 2 : 0;
                    auto uiF = [&](const char* k, float def) {
                        auto it = world->cvars.find(k);
                        return it == world->cvars.end() ? def : float(std::atof(it->second.c_str()));
                    };
                    const float fsc = field(o, "fontscale", 1);
                    const float textScale = fsc * (fontType == 0 ? 0.25f : fontType == 1 ? 0.5f : 1.0f / 3.0f);
                    const float s = textScale * sc;
                    const char* hudFont = "consoleFont";
                    if (fontType == 0)
                        hudFont = s <= uiF("ui_smallFont", 0.25f) ? "smallFont" : s >= uiF("ui_extraBigFont", 0.55f) ? "extraBigFont"
                                : s < uiF("ui_bigFont", 0.4f) ? "normalFont" : "bigFont";
                    else if (fontType == 1)
                        hudFont = s <= uiF("ui_smallFont", 0.25f) ? "smallFont" : s < uiF("ui_bigFont", 0.4f) ? "normalFont" : "boldFont";
                    const assets::Font* f = font(hudFont);
                    if (f) hudText(dst, text, px, py, textScale * 48.0f / float(f->pixelHeight) * sc, *f, rgba, fx, fy);
                }
            }
            {
                const uint64_t now = SDL_GetTicksNS();
                game::expireScreenMessages(screenMessages, now);
                // Game messages (client commands obj_update / obj_complete, 0x4a6c7f): the string "^2%s" (0x57f6a0, ^2 = green
                // (0,1,0,1) in the colour table at 0x56cd98) goes to the game-message window (type 1; 0x404830 → 0x403aa0
                // → window base of 0x5cc2b0), shown for con_gamemessagetime 5 s (0x571118; 0x403890 case 1), wrapped at
                // cg_gameMessageWidth 70 characters (0x4a2026). Drawn each frame by 0x4bf1c0 → 0x406b60 → 0x4068e0 → 0x4065e0
                // with x 6, y 100, character height 12, horizontal code 1 (LEFT edge of the window, 0x440ff4[1]) and vertical
                // code 1 (TOP of the window, 0x441014[1]), style 3: the rows of the window go bottom-up, the newest row's top
                // at y − 12 and each older row 12 higher (0x4066e3 loop). Same font rule as subtitles (0x4062d0, flag 0).
                // TODO(spec): [arguments of 0x404830 not read] the exact message text for
                // obj_update when the objective has no string, the flag (4th) argument of 0x404830.
                // Rows (oldest first) of the window of one message type; "\x1f" separates the "&&N" arguments of iprintln.
                struct WindowRow { std::string text; uint64_t startNs, endNs; const ScreenMessage::Obit* obit = nullptr; };
                auto windowRows = [&](bool bold, size_t width, size_t lines) {
                    std::vector<WindowRow> rows;
                    for (const auto& m : screenMessages) {
                        if (m.bold != bold) continue;
                        if (m.obit) { // one row: attacker, icon, victim (0x405530)
                            rows.push_back({"", m.startNs, m.endNs, m.obit.get()});
                            continue;
                        }
                        // SEH_LocalizeTextMessage (0x496e80): 0x14 / 0x15 segments, localized references substituted into &&N.
                        const std::string text = game::localizeTextMessage(m.text, [&](const std::string& k) { return loc.find(k); });
                        std::string cur, code; // the last ^N colour code carries over to the next row (rows keep per-character colours)
                        std::istringstream words(text);
                        for (std::string wd; words >> wd;) {
                            if (!cur.empty() && cur.size() + 1 + wd.size() > width) { rows.push_back({cur, m.startNs, m.endNs}); cur = code; }
                            cur += (cur.empty() || cur == code ? "" : " ") + wd;
                            for (size_t k = 0; k + 1 < wd.size(); ++k)
                                if (wd[k] == '^' && wd[k + 1] >= '0' && wd[k + 1] <= '9') code = wd.substr(k, 2);
                        }
                        if (!cur.empty()) rows.push_back({cur, m.startNs, m.endNs});
                    }
                    if (rows.size() > lines) rows.erase(rows.begin(), rows.end() - std::ptrdiff_t(lines));
                    return rows;
                };
                // Window row drawing (0x4065e0): each row has a start and an end time (row fields [8] and [4]). Fade: alpha ×
                // age / fadeIn while age < window +0x1c (250 ms), else × remaining / fadeOut while remaining < +0x20 (500 ms)
                // (0x406758 / 0x40677b); no row is drawn before its start or after its end. Scroll (first loop 0x406628): every
                // row younger than +0x18 (250 ms) moves the whole window down by (1 − age / 250) × the row pitch, so the
                // rows slide up when a new row is added.
                auto drawWindow = [&](const std::vector<WindowRow>& rows, float x, float yBottom, float pitch, const assets::Font& f,
                                      const uint8_t base[4], float alignX, float charH = 12.0f, float glyphScale = 1.0f) {
                    auto ms = [&](uint64_t a, uint64_t b) { return a >= b ? 0.0f : float(b - a) / 1.0e6f; };
                    float shift = 0;
                    for (const auto& r : rows) {
                        const float age = r.startNs ? ms(r.startNs, now) : 1e9f;
                        if (age < 250.0f) shift += (1.0f - age / 250.0f) * pitch;
                    }
                    float ry = yBottom - pitch + std::floor(shift + 0.5f);
                    for (auto it = rows.rbegin(); it != rows.rend(); ++it, ry -= pitch) {
                        float a = 1.0f;
                        if (it->startNs) {
                            if (it->startNs > now || it->endNs <= now) continue;
                            const float age = ms(it->startNs, now), left = ms(now, it->endNs);
                            if (age < 250.0f) a = age / 250.0f;
                            else if (left < 500.0f) a = left / 500.0f;
                        }
                        const uint8_t col[4] = {base[0], base[1], base[2], uint8_t(base[3] * a)};
                        if (const ScreenMessage::Obit* ob = it->obit) {
                            // Obituary row: the attacker name and the victim name in their team colours with the kill icon between
                            // them, iconH x iconW line heights, mirrored with flipKillIcon. TODO(spec): the exact gaps of the
                            // console glyph run (0x405530).
                            const float gs = charH * glyphScale / float(f.pixelHeight) * sc;
                            auto textW = [&](const std::string& t) {
                                float wsum = 0;
                                for (size_t k = 0; k < t.size(); ++k) {
                                    if (t[k] == '^' && k + 1 < t.size() && t[k + 1] >= '0' && t[k + 1] <= '9') { ++k; continue; }
                                    if (const auto* g = f.find((unsigned char)t[k])) wsum += float(g->dx) * gs;
                                }
                                return wsum;
                            };
                            auto rgb = [&](const float c3[3], uint8_t o[4]) {
                                for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::clamp(c3[k], 0.0f, 1.0f) * float(base[k]));
                                o[3] = col[3];
                            };
                            const float space = textW(" ");
                            float px = x;
                            if (!ob->attacker.empty()) {
                                uint8_t ac[4];
                                rgb(ob->attackerRgb, ac);
                                hudText(out, ob->attacker, px, ry * sc, gs, f, ac, 0, 0, 3);
                                px += textW(ob->attacker) + space;
                            }
                            const float ih = ob->iconH * charH * sc, iw = ob->iconW * charH * sc;
                            const float iy = ry * sc + (charH * sc - ih) * 0.5f;
                            Quad2D q{px, iy, px + iw, iy + ih};
                            if (ob->flip) q.s0 = 1, q.s1 = 0;
                            q.rgba[0] = q.rgba[1] = q.rgba[2] = 255;
                            q.rgba[3] = col[3];
                            q.texture = materialTexture(ob->icon);
                            out.push_back(q);
                            px += iw + space;
                            uint8_t vc[4];
                            rgb(ob->victimRgb, vc);
                            hudText(out, ob->victim, px, ry * sc, gs, f, vc, 0, 0, 3);
                            continue;
                        }
                        hudText(out, it->text, x, ry * sc, charH * glyphScale / float(f.pixelHeight) * sc, f, col, alignX, 0, 3);
                    }
                };
                // The rows keep per-character colours from the ^ codes; plain text is white (only obj_update / obj_complete are
                // formatted "^2%s" by the client handler, 0x57f6a0), so pickup and iprintln messages are white.
                // The game-message and bold-message windows (0x4bf1c0 / 0x4bf210) draw only with cl_paused == 0 and with alpha
                // 1 - f, f = 0x4bfd00: 1 while the objectives screen is held (0xf791fc), else the 300 ms release fade of touch 5
                // (0xf79200, 0x4b0900), 0 without a touch; they are skipped when 1 - f == 0. The subtitles (0x4bf2e0) ignore f.
                // 0x1040c4c is the bool dvar cg_drawpaused (name 0x57103c, default 1, flags 0x1000): when it is 0, f = 0.
                float objFade = 0.0f;
                if (menuEnv.getCvar("cg_drawpaused") == "0") objFade = 0.0f;
                else if (hudFade.objectivesHeld) objFade = 1.0f;
                else if (world && hudFade.objectives != 0) {
                    const int64_t el = cgNowMs() - hudFade.objectives;
                    if (el < 300) objFade = el <= 0 ? 1.0f : float(300 - el) / 300.0f;
                }
                const float msgAlpha = menuEnv.getCvar("cl_paused") == "1" ? 0.0f : 1.0f - objFade;
                const uint8_t white[4] = {255, 255, 255, 255};
                const uint8_t whiteMsg[4] = {255, 255, 255, uint8_t(std::clamp(msgAlpha, 0.0f, 1.0f) * 255.0f)};
                const float msgScale = 12.0f / 48.0f * float(h) / 480.0f;
                if (const assets::Font* f = font(msgScale <= 0.25f ? "smallFont" : msgScale >= 0.55f ? "extraBigFont" : msgScale < 0.4f ? "normalFont" : "bigFont")) {
                    // MP: anchored above the compass (game::mpGameMessageWindowY), bottom-relative; SP: y 100.
                    float msgY = 100.0f;
                    if (world && world->multiplayer()) {
                        const ui::Rect cr = compassMenu ? compassMenu->rect : ui::Rect{-20, -140, 160, 160};
                        const auto cv = world->cvars.find("cg_hudCompassSize");
                        const float csz = cv == world->cvars.end() || cv->second.empty() ? 1.0f : float(std::atof(cv->second.c_str()));
                        msgY = 480.0f + game::mpGameMessageWindowY(cr.y, cr.h, csz);
                    }
                    if (msgAlpha > 0) drawWindow(windowRows(false, 70, 9), left43 + 6.0f * sc, msgY, 12.0f, *f, whiteMsg, 0);
                }
                // Bold messages (client command gmb = iprintlnbold, 0x4a6c3f → 0x4a2750 → window type 2 at 0x5cc2b0+0x6a0,
                // 9 lines (struct 0x5cc954, +0x10 = 0x5cc964; corrected: 0x5ccff4 is the subtitle window), cg_gameBoldMessageWidth 60, con_boldgamemessagetime 8 s): drawn by 0x4bf210 →
                // 0x4068e0 mode 3 at x 0, y 180, character height 12, horizontal code 7 (the centre of the window box),
                // vertical code 1, white; the bold flag adds 4 to the row pitch (0x4065e0) and selects the flag path of
                // the font choice (0x4062d0: scale <= 0.25 small, < ui_bigFont 0.4 normal, else bold font).
                // Re-checked: 0x4bf210 passes only the horizontal code 7 and x 0; 0x440e60 code 7 gives (0x79397c + 0x793974) / 2
                // + x·scale and 0x4062d0 draws the text from that point (0x617bf0), so each row STARTS at the centre of the box
                // (no centring in 0x4062d0 / 0x4065e0 / 0x403b90).
                {
                    const float scale = 12.0f / 48.0f * float(h) / 480.0f;
                    if (const assets::Font* f = font(scale <= 0.25f ? "smallFont" : scale < 0.4f ? "normalFont" : "boldFont")) {
                        if (msgAlpha > 0) drawWindow(windowRows(true, 60, 9), left43 + 320.0f * sc, 180.0f, 16.0f, *f, whiteMsg, 0);
                    }
                }
                // MP centre print (0x4c8360): the text localized, at most 75 characters per row, scale ftol(9.6) / 32, shadowed,
                // each row centred on the screen centre at y = cg_centerPrintY (default -60) - rows × row height × 1.2 below the
                // centre; shown 2000 ms with the alpha of 0x4c2860 (100 ms fade). TODO(spec): the alignment codes of 0x5322c0
                // and the start / fade arguments of 0x4c2860 here.
                if (world && world->multiplayer() && mpCenterStartMs >= 0) {
                    const int64_t el = cgNowMs() - mpCenterStartMs;
                    if (el < 0 || el >= 2000) mpCenterStartMs = -1;
                    else {
                        const float a = 2000 - el < 100 ? float(2000 - el) / 100.0f : 1.0f;
                        const std::string text = game::localizeTextMessage(mpCenterText, [&](const std::string& k) { return loc.find(k); });
                        const float scale = 9.0f / 32.0f;
                        const float vscale = scale * float(h) / 480.0f;
                        if (const assets::Font* f = font(vscale <= 0.25f ? "smallFont" : vscale < 0.4f ? "normalFont" : "bigFont")) {
                            const float gs = scale * 48.0f / float(f->pixelHeight) * sc;
                            const float rowH = float(f->pixelHeight) * scale * 48.0f / float(f->pixelHeight) * 1.2f;
                            const std::string cy = menuEnv.getCvar("cg_centerPrintY");
                            const float y0 = 240.0f + (cy.empty() ? -60.0f : float(std::atof(cy.c_str()))) - rowH;
                            const uint8_t col[4] = {255, 255, 255, uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f)};
                            hudText(out, text.substr(0, 75), float(w) * 0.5f, y0 * sc, gs, *f, col, 0.5f, 0, 3);
                        }
                    }
                }
                // MP crosshair names (0x4c97c0 / 0x4c9700): the trace each frame, the teammate's name 150 ms after the last hit
                // with a 100 ms fade, at (cg_drawCrosshairNamesPosX 300, cg_drawCrosshairNamesPosY 180 + 16), scale 1/3,
                // shadowed, white. TODO(spec): the alignment codes of 0x5322c0 (here the left of the 4:3 area / top).
                if (world && world->multiplayer() && menuEnv.getCvar("cg_drawCrosshairNames") != "0") {
                    const Vec3 fw = c.forward();
                    const int hit = world->mpCrosshairClient({c.pos.x, c.pos.y, c.pos.z}, {fw.x, fw.y, fw.z});
                    if (hit >= 0 && hit != world->mpFollowing(world->mpLocalClient())) mpCrossClient = hit, mpCrossMs = cgNowMs();
                    const float a = mpCrossMs >= 0 ? game::mpCrosshairNameAlpha(cgNowMs() - mpCrossMs) : -1.0f;
                    game::Entity* te = a > 0 ? world->mpClientEntity(mpCrossClient) : nullptr;
                    if (te && game::mpCrosshairNameVisible(world->mpLocalTeam(), world->mpTeamOf(te))) {
                        const std::string nm = world->mpState().clients[size_t(mpCrossClient)].name;
                        auto num = [&](const char* n, float def) { const std::string v = menuEnv.getCvar(n); return v.empty() ? def : float(std::atof(v.c_str())); };
                        const float scale = 1.0f / 3.0f, vscale = scale * float(h) / 480.0f;
                        if (const assets::Font* f = font(vscale <= 0.25f ? "smallFont" : vscale < 0.4f ? "normalFont" : "bigFont")) {
                            const uint8_t col[4] = {255, 255, 255, uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f)};
                            hudText(out, nm, left43 + num("cg_drawCrosshairNamesPosX", 300.0f) * sc, (num("cg_drawCrosshairNamesPosY", 180.0f) + 16.0f) * sc,
                                    scale * 48.0f / float(f->pixelHeight) * sc, *f, col, 0, 0, 3);
                        }
                    }
                }
                // Subtitles (alias subtitle column, 0x4a3540 -> 0x401d60 -> 0x404830 into the third text window, struct
                // 0x5cc2b0 + 0xd40 with 10 lines and fade times 250 / 250 / 500 ms, init 0x403c00; drawn each frame by 0x4bf2e0 ->
                // 0x406c60 -> 0x4068e0 with the same row drawer as the game messages, horizontal and vertical code 1 = left / top
                // of the 4:3 area). Draw gate (0x4bf2e0): cl_paused (0x104309c) == 0, the objectives screen flag (0xf791fc) clear
                // and cg_subtitles (0xf569b8, default 1) set. Position / height from the dvars cg_subtitlePosX / PosY /
                // CharHeight (0xf48cf0 / 0xf48cf4 / 0x1040c78, defaults 120, 400, 12; value read at draw time), width wrap from
                // cg_subtitleWidthWidescreen when the aspect (0xf56a90) > 1.3333 else cg_subtitleWidthStandard (0x4a3540), both
                // default 80; duration max(sound, cg_subtitleMinTime default 3 s). Colour: the row drawer starts from white
                // (alpha = fade), like the game messages. con_subtitleScale (0x5710ac, default 1; 0x406c60 passes it as the last
                // argument of 0x4068e0 -> argument 7 of 0x4062d0, which multiplies the font-selection scale at 0x4062fc) scales
                // the glyphs and the font choice, not the row pitch.
                {
                    auto cv = [&](const char* n, float def) {
                        const std::string v = menuEnv.getCvar(n);
                        return v.empty() ? def : float(std::atof(v.c_str()));
                    };
                    const bool gate = menuEnv.getCvar("cl_paused") != "1" && !input.objectivesHeld() && cv("cg_subtitles", 1) != 0.0f;
                    const float minT = std::max(0.0f, cv("cg_subtitleMinTime", 3.0f));
                    std::erase_if(subtitles, [&](const Subtitle& st) { return st.startNs + uint64_t(std::max(st.soundSec, minT) * 1e9f) < now; });
                    const float charH = std::clamp(cv("cg_subtitleCharHeight", 12.0f), 1.0f, 480.0f);
                    const float conScale = cv("con_subtitleScale", 1.0f);
                    const float subScale = charH / 48.0f * float(h) / 480.0f * conScale;
                    const size_t width = size_t(std::clamp(cv(float(w) / float(h) > 1.3333f ? "cg_subtitleWidthWidescreen" : "cg_subtitleWidthStandard", 80.0f), 20.0f, 256.0f));
                    if (const assets::Font* f = gate ? font(subScale <= 0.25f ? "smallFont" : subScale >= 0.55f ? "extraBigFont" : subScale < 0.4f ? "normalFont" : "bigFont") : nullptr) {
                        std::vector<WindowRow> rows;
                        for (const auto& st : subtitles) {
                            const std::string* t = loc.find(st.key);
                            const std::string text = t ? *t : st.key; // an untranslated key is shown as it is
                            const uint64_t end = st.startNs + uint64_t(std::max(st.soundSec, minT) * 1e9f);
                            std::string cur;
                            std::istringstream words(text);
                            for (std::string wd; words >> wd;) {
                                if (!cur.empty() && cur.size() + 1 + wd.size() > width) { rows.push_back({cur, st.startNs, end}); cur.clear(); }
                                cur += (cur.empty() ? "" : " ") + wd;
                            }
                            if (!cur.empty()) rows.push_back({cur, st.startNs, end});
                        }
                        if (rows.size() > 10) rows.erase(rows.begin(), rows.end() - 10);
                        drawWindow(rows, left43 + cv("cg_subtitlePosX", 120.0f) * sc, cv("cg_subtitlePosY", 400.0f), charH, *f, white, 0, charH, conScale);
                    }
                }
            }
            // Scope overlay (0x4bcdd0 → 0x4bca90): the weapon's adsOverlayShader, white with the overlay alpha. Wider than
            // 320 or taller than 240 (adsOverlayWidth / Height, virtual units): one quad of that size centred on the
            // view; else four quads of that size around the centre, mirrored (u and/or v flipped) towards the right and
            // bottom. Outside it (0x4bc980) the rest of the screen is filled with the same material: left and right
            // strips with u = 0 (t 0..1), top and bottom with v = 0 (s 0..1). TODO(spec): [0x4bc6e0 not implemented, needs weapon angles] the centre follows the gun
            // sway (0x4bc6e0 projects the weapon angles); adsOverlayReticle drawing.
            // On a turret 0x4bd760 draws only the turret reticle, so the overlay is not drawn (the aim state is kept).
            if (const float sa = world ? game::scopeOverlayDrawAlpha(scopeAlpha(), world->playerOnTurret()) : 0.0f; sa > 0) {
                // The overlay's weapon: the client's own, or in a killcam the followed client's recorded view weapon.
                auto overlayField = [&](const char* k) -> std::string {
                    if (!killcamRig.active) return pweapon.field(k);
                    const game::MpViewState& vs = killcamRig.view.view;
                    return killcamField(vs.viewWeapon.empty() ? killcamRig.view.weapon : vs.viewWeapon, k);
                };
                const std::string shader = overlayField("adsOverlayShader");
                const int tex = shader.empty() ? -1 : materialTexture(shader);
                if (tex >= 0) {
                    const float ow = float(std::atof(overlayField("adsOverlayWidth").c_str())), oh = float(std::atof(overlayField("adsOverlayHeight").c_str()));
                    const float cx = left43 + 320.0f * sc, cy = 240.0f * sc;
                    const uint8_t col[4] = {255, 255, 255, uint8_t(sa * 255.0f)};
                    auto quad = [&](float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
                        Quad2D q{x0, y0, x1, y1, s0, t0, s1, t1};
                        std::memcpy(q.rgba, col, 4);
                        q.texture = tex;
                        scopeQuads.push_back(q);
                    };
                    float l, r, t, b;
                    if (ow > 320.0f || oh > 240.0f) {
                        l = cx - ow * 0.5f * sc; t = cy - oh * 0.5f * sc; r = l + ow * sc; b = t + oh * sc;
                        quad(l, t, r, b, 0, 0, 1, 1);
                    } else {
                        l = cx - ow * sc; t = cy - oh * sc; r = cx + ow * sc; b = cy + oh * sc;
                        quad(l, t, cx, cy, 0, 0, 1, 1);
                        quad(cx, t, r, cy, 1, 0, 0, 1);
                        quad(l, cy, cx, b, 0, 1, 1, 0);
                        quad(cx, cy, r, b, 1, 1, 0, 0);
                    }
                    const float sw = float(w), sh = float(h);
                    if (l > 0) quad(0, 0, l, sh, 0, 0, 0, 1);
                    if (r < sw) quad(r, 0, sw, sh, 0, 0, 0, 1);
                    if (t > 0) quad(l, 0, r, t, 0, 0, 1, 0);
                    if (b < sh) quad(l, b, r, sh, 0, 0, 1, 0);
                }
            }
            // Cursor hint (ownerdraw CG_CURSORHINT, 0x4a9b50) for what the use key would use. Icon (0xf5b4a8 by hint,
            // registered at 0x4a2ca6…): 2 gfx/icons/hint_usable, 3 hint_health, 4 hint_friendly, a weapon hint the
            // weapon's hudIcon (else hint_usable, 0x4b3cb7), twice as wide with wideListIcon. Text: the hint string
            // localized ("Hint String", 0x4a9ac0) with &&1 = the key bound to +activate (else KEY_USE); weapon hints
            // only for turret-class weapons. Rect: menu "Cursorhints" "0 70 40 40" centred → (320, 310) virtual.
            // With text: x = rect.x − (icon width + text width)/2, text baseline rect.y + text height/2, icon right of
            // it at y = rect.y − h/2; without: icon at x = rect.x − icon width/2, y = rect.y. cg_cursorHints default
            // 3 (no pulse; registered 0x4a1123 with 3, 0, 4). Fade (0x4a9b50 → 0x4b0900 with 100 ms): while a hint is
            // set (0x4a9960) its start time is the current time and the duration cg_hintFadeTime (int, default 100 ms,
            // 0 .. INT_MAX, registered 0x4a1163); once it is gone the last hint stays for that duration and its
            // colour alpha is remaining / 100 (1 while more than 100 ms remain), then the hint is cleared. The colour
            // (1, 1, 1, alpha) goes to the icon draw (0x4d4790 colour argument); the text takes it too (assumed, the
            // 0x4c3790 colour argument was not traced). HINT_HEALTH without a string index (0x4a9cc1..0x4a9ce5) uses the localized
            // PLATFORM_PICKUPHEALTH with the +activate key substituted (same path as a hint string).
            float hintFade = 1.0f;
            if (world) {
                const auto& cur = world->playerUseHint();
                hintFade = cursorHint.update({cur.hint, cur.weapon, cur.text}, cgNowMs());
                if (static const bool hintLog = std::getenv("IW2_HINT_LOG") != nullptr; hintLog) { // debug: one line per change of the drawn hint
                    static std::string lastLine;
                    const std::string line = hintFade < 0 ? std::string("none") : "hint " + std::to_string(cursorHint.held().hint) + " weapon '" + cursorHint.held().weapon +
                                             "' text '" + cursorHint.held().text + "'" + (cur.hint != 0 || !cur.weapon.empty() ? "" : " (fading)");
                    if (line != lastLine) std::cout << "[hint] t=" << cgNowMs() << " " << line << "\n";
                    lastLine = line;
                }
            } else cursorHint.reset();
            if (world && cursorHintItem && !world->playerOnTurret() && hintFade >= 0.0f) {
                const auto& uh = cursorHint.held();
                std::string icon;
                bool wide = false;
                std::string text = uh.text;
                if (text.empty() && uh.weapon.empty() && uh.hint == 3 && loc.find("PLATFORM_PICKUPHEALTH")) text = "PLATFORM_PICKUPHEALTH";
                if (!uh.weapon.empty()) {
                    const auto* wd = world->weapon(uh.weapon);
                    icon = wd && wd->count("hudicon") && !wd->at("hudicon").empty() ? wd->at("hudicon") : "gfx/icons/hint_usable";
                    wide = wd && wd->count("widelisticon") && wd->at("widelisticon") != "0";
                } else if (uh.hint == 2) icon = "gfx/icons/hint_usable";
                else if (uh.hint == 3) icon = "hint_health";
                else if (uh.hint == 4) icon = "hint_friendly";
                if (!icon.empty()) {
                    const float rx = 320.0f, ry = 310.0f, rw = 40.0f, rh = 40.0f;
                    const float iw = (wide ? 2.0f : 1.0f) * rw;
                    const uint8_t white[4] = {255, 255, 255, uint8_t(hintFade * 255.0f)};
                    float ix = rx - iw * 0.5f, iy = ry;
                    if (!text.empty()) {
                        text = game::localizeTextMessage(text, [&](const std::string& k) { return loc.find(k); }); // 0x14 / 0x15 pieces (0x4eb9b0)
                        auto keys = input.keysFor("+activate");
                        std::string key = keys.empty() ? std::string() : keys.front();
                        if (key.empty()) { const std::string* k = loc.find("KEY_USE"); key = k ? *k : "KEY_USE"; }
                        if (const auto at = text.find("&&1"); at != std::string::npos) text = text.substr(0, at) + key + text.substr(at + 3);
                        const float ts = cursorHintItem->textScale;
                        const float fs = ts * float(h) / 480.0f;
                        if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                            const float gs = ts * 48.0f / float(f->pixelHeight);
                            float tw = 0;
                            for (unsigned char ch : text)
                                if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                            const float textH = float(int(ts * 48.0f));
                            const float x0 = rx - (iw + tw) * 0.5f;
                            hudText(out, text, left43 + x0 * sc, (textH * 0.5f + ry) * sc, gs * sc, *f, white, 0.0f, 1.0f,
                                    cursorHintItem->textStyle); // style: the item's (0x4a9d38)
                            ix = x0 + tw;
                            iy = ry - rh * 0.5f;
                        }
                    }
                    Quad2D q{left43 + ix * sc, iy * sc, 0, 0};
                    q.x1 = q.x0 + iw * sc;
                    q.y1 = q.y0 + rh * sc;
                    std::memcpy(q.rgba, white, 4);
                    q.texture = materialTexture(icon);
                    out.push_back(q);
                }
            }
            // Damage direction icons (0x4bdcb0): for each slot alive (0 < elapsed < cg_hudDamageIconTime 2000), the
            // material hit_direction as a quad of cg_hudDamageIconWidth 128 × cg_hudDamageIconHeight 64 screen pixels,
            // cg_hudDamageIconOffset 128 below the screen centre, turned clockwise by view yaw − icon yaw about the
            // centre (0x4b0490); alpha 2 − 2 · elapsed / duration, at most 1. Not under a scope overlay
            // (cg_hudDamageIconInScope 0). The corner-to-UV order of the gfx quad draw (0x617b68, render command 0x12, handler
            // 0x1002f280) is (0,0) (1,0) (1,1) (0,1) for the corners in the order given.
            if (world && !(scopeAlpha() > 0)) {
                const int64_t nowMs = cgNowMs();
                const float W = 128.0f, H = 64.0f, O = 128.0f;
                const float px[4] = {W * 0.5f, -W * 0.5f, -W * 0.5f, W * 0.5f}, py[4] = {O + H, O + H, O, O};
                const float cxs = left43 + 320.0f * sc, cys = 240.0f * sc;
                for (const auto& di : damageIcons) {
                    const int64_t el = nowMs - di.start;
                    if (!(el > 0 && el < di.duration)) continue;
                    float a = 2.0f - float(el + el) / float(di.duration);
                    if (a - 1.0f >= 0) a = 1.0f;
                    const float ang = (c.yaw - di.yaw) * float(M_PI) / 180.0f, cs = std::cos(ang), sn = std::sin(ang);
                    Quad2D q{0, 0, 0, 0};
                    q.free = true;
                    for (int k = 0; k < 4; ++k) {
                        q.qx[k] = cs * px[k] + cxs - sn * py[k];
                        q.qy[k] = cs * py[k] + sn * px[k] + cys;
                    }
                    q.rgba[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                    q.texture = materialTexture("hit_direction");
                    out.push_back(q);
                }
            }
            // Mantle hint (ownerdraw CG_MANTLE_HINT, 0x4a9f90): PLATFORM_MANTLE with &&1 = the key bound to +gostand (else
            // +moveup, 0x4cdbf0), then the hint_mantle icon right of it; both centred on the item rect's x. The rect is
            // the menu's "0 130 40 40" with HORIZONTAL_ALIGN_CENTER / VERTICAL_ALIGN_CENTER: (320, 370) virtual, 40×40.
            // Text at x = rect.x − (text width + rect.w)/2 with its baseline at rect.y + text height/2; the icon at
            // (that x + text width, rect.y − rect.h/2). The item's textstyle goes to the text draw (see hudText).
            // TODO(spec): [hint gate not fully read] the dvars it also tests.
            if (world && mantleHint && mantleHintItem && !input.objectivesHeld() && menuEnv.getCvar("cg_drawpaused") != "0" /* 0x4a9fb1 */) { // 0xf791fc (+scores held, 0x4a4d40) hides it (0x4a9fc2)
                const float ts = mantleHintItem->textScale;
                const float fs = ts * float(h) / 480.0f;
                const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont");
                const std::string* t = loc.find("PLATFORM_MANTLE");
                std::string text = t ? *t : "PLATFORM_MANTLE";
                auto keys = input.keysFor("+gostand");
                if (keys.empty()) keys = input.keysFor("+moveup");
                if (const auto at = text.find("&&1"); at != std::string::npos)
                    text = text.substr(0, at) + (keys.empty() ? std::string() : keys.front()) + text.substr(at + 3);
                if (f) {
                    const float gs = ts * 48.0f / float(f->pixelHeight);
                    float tw = 0;
                    for (unsigned char ch : text)
                        if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                    const float textH = float(int(ts * 48.0f));
                    const float rx = 320.0f, ry = 370.0f, rw = 40.0f, rh = 40.0f;
                    const float x0 = rx - (tw + rw) * 0.5f;
                    const uint8_t white[4] = {255, 255, 255, 255};
                    hudText(out, text, left43 + x0 * sc, (textH * 0.5f + ry) * sc, gs * sc, *f, white, 0.0f, 1.0f,
                            mantleHintItem->textStyle); // style: the item's (0x4aa05b)
                    Quad2D q{left43 + (x0 + tw) * sc, (ry - rh * 0.5f) * sc, 0, 0};
                    q.x1 = q.x0 + rw * sc;
                    q.y1 = q.y0 + rh * sc;
                    std::memcpy(q.rgba, white, 4);
                    q.texture = materialTexture("hint_mantle");
                    out.push_back(q);
                }
            }
            // Hold-breath hint (ownerdraw CG_HOLD_BREATH_HINT 71, 0x4a9e30; menu "HoldBreathHint" "0 -130 0 0" centred =
            // (320, 110) virtual): shown with cg_drawBreathHint (default 1), the 2D hud on, no objectives screen, not paused,
            // pm_flags 0x4000 clear, the current weapon's weaponClass != item (9) with an adsOverlayReticle (def +0x278 != 0)
            // and the aim fraction (0xf709a0) exactly 1.0. Text PLATFORM_HOLD_BREATH with &&1 = the key bound to +holdbreath,
            // else +melee_breath, else +breath_binoculars (0x4cdbf0), white, the item's textscale / textstyle; x = rect.x −
            // int(width / 2), baseline y = rect.y (no half-height offset, unlike the mantle hint). Hidden in a vehicle
            // (the weapon lookup 0x4dc6a0 -> 0x4dc660 then finds a turret-type weapon only). pm_flags 0x4000 is the
            // hold-breath flag of PM_UpdateHoldBreath 0x4dd350 (PlayerState::holdingBreath): the hint is hidden while it is set.
            if (world && world->player() && holdBreathItem && !input.objectivesHeld() /* 0xf791fc, 0x4a9e5d */ && menuEnv.getCvar("cg_drawpaused") != "0" /* 0x4a9e4c */ && menuEnv.getCvar("cl_paused") != "1" /* 0x104309c */ && !(world->playerPmFlags() & 0x20000) && !(world->playerPmFlags() & 0x4000) && pweapon.adsFrac() == 1.0f) {
                const std::string reticle = pweapon.field("adsOverlayReticle");
                if (!reticle.empty() && reticle != "none" && pweapon.field("weaponClass") != "item") {
                    const float ts = holdBreathItem->textScale;
                    const float fs = ts * float(h) / 480.0f;
                    if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                        const std::string* t = loc.find("PLATFORM_HOLD_BREATH");
                        std::string text = t ? *t : "PLATFORM_HOLD_BREATH";
                        auto keys = input.keysFor("+holdbreath");
                        if (keys.empty()) keys = input.keysFor("+melee_breath");
                        if (keys.empty()) keys = input.keysFor("+breath_binoculars");
                        if (const auto at = text.find("&&1"); at != std::string::npos)
                            text = text.substr(0, at) + (keys.empty() ? std::string() : keys.front()) + text.substr(at + 3);
                        const float gs = ts * 48.0f / float(f->pixelHeight);
                        float tw = 0;
                        for (unsigned char ch : text)
                            if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                        const uint8_t white[4] = {255, 255, 255, 255};
                        hudText(out, text, left43 + (320.0f - float(int(tw * 0.5f))) * sc, 110.0f * sc, gs * sc, *f, white, 0.0f, 1.0f,
                                holdBreathItem->textStyle);
                    }
                }
            }
            // Invalid-command hint (ownerdraw CG_INVALID_CMD_HINT 110, 0x4aa550; menu "InvalidCmdHint" "0 -130 0 0" centred =
            // (320, 110), textscale .35 shadowed): state 0xf79694 set by 0x4ba860 (restarts the time 0xf79698 only on a change) is
            // reset to 0 once cg_invalidCmdHintDuration (0xf56984, default 1800 ms) has passed. 1 WEAPON_NO_AMMO, 2
            // WEAPON_NO_FRAG_GRENADE, 3 WEAPON_NO_SMOKE_GRENADE, 4 GAME_STAND_BLOCKED, 5 GAME_CROUCH_BLOCKED (localized). Alpha =
            // (elapsed mod b) / b with b = cg_invalidCmdHintBlinkInterval (0xf5bcdc, default 600 ms, min 1): a sawtooth, written into
            // the item colour's alpha. Text x = rect.x - round(int(width) / 2), baseline y = rect.y. Set by player events 0xc9 / 0xca
            // (no offhand of the class, 0x4baf57 / 0x4baf7f). TODO(spec): [needs PM_Weapon and stance-trace events] state 1 (event 0x92 handler 0x4baeb6, raised by PM_Weapon
            // 0x4de4c0 / 0x4deb70 on an empty weapon, not modelled in PlayerWeapon) and 4 / 5 (events 0x8d / 0x8e with parm 1 / 2,
            // 0x4bac40 / 0x4bac60; the server stance trace 0x4fc000 is not modelled).
            if (world && invalidCmdItem && invalidCmdState != 0) {
                const int64_t nowMs = cgNowMs();
                if (nowMs > invalidCmdStartMs + 1800) invalidCmdState = 0;
                else {
                    static const char* const keys[] = {"", "WEAPON_NO_AMMO", "WEAPON_NO_FRAG_GRENADE", "WEAPON_NO_SMOKE_GRENADE", "GAME_STAND_BLOCKED", "GAME_CROUCH_BLOCKED"};
                    const std::string key = keys[invalidCmdState];
                    const std::string* t = loc.find(key);
                    const std::string text = t ? *t : key;
                    const float ts = invalidCmdItem->textScale;
                    const float fs = ts * float(h) / 480.0f;
                    if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                        const int64_t blink = 600;
                        const float alpha = float((nowMs - invalidCmdStartMs) % blink) / float(blink);
                        const float gs = ts * 48.0f / float(f->pixelHeight);
                        float tw = 0;
                        for (unsigned char ch : text)
                            if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                        const float half = std::nearbyint(float(int(tw)) * 0.5f + 9.3e-10f);
                        uint8_t col[4];
                        for (int k = 0; k < 3; ++k) col[k] = uint8_t(std::clamp(invalidCmdItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                        col[3] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
                        hudText(out, text, left43 + (320.0f - half) * sc, 110.0f * sc, gs * sc, *f, col, 0.0f, 1.0f, invalidCmdItem->textStyle);
                    }
                }
            }
            // Objectives screen (menu "objectiveinfo"; ownerdraws 97 list 0x4c0400 and 96 header 0x4bfdd0). Shown while +scores
            // is held (cgame 0xf791fc, set by 0x4a4d40, cleared by 0x4a4d20; both stamp the touch time 0xf79200 = cg.time); after
            // the release the alpha is (300 - elapsed) / 300 (0x4b0900 with 300 ms), nothing once elapsed >= 300 or without a touch.
            // Both menu items are LEFT / TOP aligned (0x440e60 code 1: x * sx, y * sy). Both ownerdraws test the bool dvar
            // cg_drawpaused (0x1040c4c, default 1) and draw nothing when it is 0. TODO(spec): the multi-byte (Asian) wrap path of 0x4bffb0 (cvar 0xc4d2f8).
            // MP scoreboard (docs/spec/mp-scoreboard.md, src/game/mpscoreboard.cpp): drawn while +scores is held (TAB / F4 in
            // default_mp.cfg) with a 100 ms fade after the release, and forced during intermission (pm_type 5, 0x4cbca0 ->
            // 0x4cb810), where it is the only thing drawn (below, at the end of the HUD).
            if (world && world->multiplayer() && !mpIntermissionView) drawMpScoreboard(out, w, h);
            // Following another client (pm_flags 0x400000, 0x4cba90): CGAME_FOLLOWING at y 40 and the target's name at y 65,
            // scale 1/3, centred, while the scoreboard is not shown. TODO(spec): the colour 0x59d038 (white here) and the
            // flag 0x151ad94 that also hides it.
            if (world && world->multiplayer() && !netClient && !mpSb.shown) {
                const int fol = world->mpFollowing(world->mpLocalClient());
                game::Entity* te = fol >= 0 ? world->mpClientEntity(fol) : nullptr;
                const float s3 = 1.0f / 3.0f, fs = s3 * float(h) / 480.0f;
                if (te) if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                    const std::string* lab = loc.find("CGAME_FOLLOWING");
                    const uint8_t white[4] = {255, 255, 255, 255};
                    const float gs = s3 * 48.0f / float(f->pixelHeight) * sc;
                    hudText(out, lab ? *lab : std::string("CGAME_FOLLOWING"), float(w) * 0.5f, 40.0f * sc, gs, *f, white, 0.5f, 1.0f, 3);
                    hudText(out, te->obj->get("name").toString(), float(w) * 0.5f, 65.0f * sc, gs, *f, white, 0.5f, 1.0f, 3);
                }
            }
            // MP chat lines (0x4c7760) and the message field (0x406d2d), docs/spec/mp-chat.md. Positions are the vec2 dvars
            // cg_hudChatPosition (5 150) and cg_hudSayPosition (5 126) in the 640x480 screen, text scale 0.2083.
            if (world && world->multiplayer()) {
                const float ts = 0.2083333f, fs = ts * float(h) / 480.0f;
                if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                    const float gs = ts * 48.0f / float(f->pixelHeight) * sc;
                    auto vec2 = [&](const char* n, float dx, float dy) {
                        std::istringstream in(menuEnv.getCvar(n));
                        float x = dx, y = dy;
                        if (!(in >> x >> y)) x = dx, y = dy;
                        return std::array<float, 2>{x, y};
                    };
                    auto textW = [&](const std::string& t) { // virtual units
                        float tw = 0;
                        for (size_t i = 0; i < t.size(); ++i) {
                            if (t[i] == '^' && i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9') { ++i; continue; }
                            if (const auto* g = f->find((unsigned char)t[i])) tw += float(g->dx) * ts * 48.0f / float(f->pixelHeight);
                        }
                        return std::nearbyint(tw);
                    };
                    const auto cp = vec2("cg_hudChatPosition", 5.0f, 150.0f);
                    const auto lines = chatBuffer.frame(cgNowMs(), cvarInt("cg_chatHeight", 8), cvarInt("cg_chatTime", 12000));
                    for (size_t i = 0; i < lines.size(); ++i) {
                        const auto& l = lines[i];
                        const float y = game::mpChatLineTop(cp[1], int(i));
                        // Strip colour: the line's first ^N (0x4032e0: 8 g_TeamColor_Allies, 9 g_TeamColor_Axis), rgb x 0.25, alpha x 0.6.
                        static const float kTab[10][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}, {0.5f, 0.5f, 1}, {1, 0.5f, 0.5f}};
                        const float* col = l.code >= 0 ? kTab[l.code] : kTab[7];
                        Quad2D bar{left43 + cp[0] * sc, y * sc, left43 + (cp[0] + textW(l.text) + 24.0f) * sc, (y + 10.0f) * sc};
                        bar.texture = materialTexture("hudcolorbar");
                        for (int k = 0; k < 3; ++k) bar.rgba[k] = uint8_t(std::clamp(col[k] * 0.25f, 0.0f, 1.0f) * 255.0f);
                        bar.rgba[3] = uint8_t(std::clamp(l.alpha * 0.6f, 0.0f, 1.0f) * 255.0f);
                        out.push_back(bar);
                        const uint8_t white[4] = {255, 255, 255, uint8_t(std::clamp(l.alpha, 0.0f, 1.0f) * 255.0f)};
                        hudText(out, l.text, left43 + cp[0] * sc, (y + 9.0f) * sc, gs, *f, white, 0.0f, 1.0f, 3);
                    }
                    // The field: "<EXE_SAY>: " or "<EXE_SAYTEAM>: " and the text, below the history (docs/spec/mp-chat.md). TODO(spec): the label colour, the cursor.
                    if (chatField.open) {
                        const auto sp = vec2("cg_hudSayPosition", 5.0f, 126.0f);
                        const std::string* lab = loc.find(chatField.team ? "EXE_SAYTEAM" : "EXE_SAY");
                        const std::string label = (lab ? *lab : std::string(chatField.team ? "EXE_SAYTEAM" : "EXE_SAY")) + ": ";
                        const uint8_t white[4] = {255, 255, 255, 255};
                        const float fieldY = game::mpChatFieldBaseline(sp[1], ts * float(f->pixelHeight));
                        hudText(out, label + chatField.text + "_", left43 + sp[0] * sc, fieldY * sc, gs, *f, white, 0.0f, 1.0f, 3);
                    }
                }
            }
            if (world && !world->multiplayer() && objListItem && objHeaderItem && menuEnv.getCvar("cg_drawpaused") != "0") {
                float objAlpha = 0.0f;
                if (hudFade.objectivesHeld) objAlpha = 1.0f;
                else if (hudFade.objectives != 0) {
                    const int64_t el = cgNowMs() - hudFade.objectives;
                    if (el < 300) objAlpha = el <= 0 ? 1.0f : float(300 - el) / 300.0f;
                }
                if (objAlpha > 0.0f) {
                    auto b8 = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                    auto rnd = [](float v) { return std::nearbyint(v + 9.3e-10f); }; // fistp of (v + 2^-31), 0x591f20
                    const float sr = rnd(sc);
                    const auto fontFor = [&](float ts) -> const assets::Font* {
                        const float fs = ts * float(h) / 480.0f;
                        return font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont");
                    };
                    // List (0x4c0400): the rows run down from the item's y (a baseline); icon 16 x 16 above it.
                    {
                        const ui::Item& it = *objListItem;
                        const float rx = objMenuX + it.rect.x, ry = objMenuY + it.rect.y, rw = it.rect.w, rh = it.rect.h;
                        if (const assets::Font* f = fontFor(it.textScale)) {
                            const float gs = it.textScale * 48.0f / float(f->pixelHeight);
                            const float fontH = rnd(it.textScale * 48.0f);
                            const float rowH = std::max(fontH, rh);
                            const int wrapPx = float(w) / float(h) <= 4.0f / 3.0f + 1e-4f ? 600 : 530; // cg_objectiveListWrapCountStandard / Widescreen
                            const auto textW = [&](const std::string& t) {
                                float tw = 0;
                                for (unsigned char ch : t)
                                    if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                                return int(rnd(tw));
                            };
                            float y = ry, lastIconTop = 0;
                            std::array<uint8_t, 4> lastCol{255, 255, 255, 255};
                            for (const auto& ob : world->objectives()) {
                                if (ob.state < 1 || ob.state > 5) continue;
                                if (ob.state == 2) { y += rowH + 4.0f; continue; } // invisible: only the row step (0x4c0733)
                                std::array<uint8_t, 4> col{};
                                std::string mat;
                                switch (ob.state) {
                                case 1: col = {b8(0.6f), b8(0.6f), b8(0.6f), b8(objAlpha)}; mat = ob.icon.empty() ? "checkbox_active" : ob.icon; break;
                                case 3: col = {b8(0.35f), b8(0.35f), b8(0.35f), b8(objAlpha)}; mat = "checkbox_done"; break;
                                case 4: col = {255, 255, 255, b8(objAlpha)}; mat = ob.icon.empty() ? "checkbox_current" : ob.icon; break;
                                default: col = {b8(0.4f), b8(0.3f), b8(0.3f), b8(objAlpha)}; mat = "ui/assets/checkbox_fail"; break;
                                }
                                lastCol = col;
                                {
                                    Quad2D q{rx * sc, (y - rh) * sc, (rx + rw) * sc, (y - rh) * sc + rh * sc};
                                    std::memcpy(q.rgba, col.data(), 4);
                                    q.texture = materialTexture(mat);
                                    out.push_back(q);
                                    lastIconTop = (y - rh) * sc;
                                }
                                // Text: the localized reference with its "&&N" arguments (after \x1f, see objective_string).
                                std::string text = ob.text.substr(0, ob.text.find('\x1f'));
                                if (const std::string* t = loc.find(text)) text = *t;
                                for (size_t at = ob.text.find('\x1f'), n = 1; at != std::string::npos; ++n) {
                                    const size_t next = ob.text.find('\x1f', at + 1);
                                    const std::string mark = "&&" + std::to_string(n);
                                    if (const size_t k = text.find(mark); k != std::string::npos)
                                        text.replace(k, mark.size(), ob.text.substr(at + 1, next == std::string::npos ? next : next - at - 1));
                                    at = next;
                                }
                                const float tx = rnd((rx + rw + 12.0f) * sc);
                                const auto drawLine = [&](const std::string& ln) {
                                    hudText(out, ln, tx, rnd(((rowH - rh) * 0.5f + y) * sc), gs * sc, *f, col.data(), 0.0f, 1.0f, it.textStyle);
                                    y += rowH + 4.0f;
                                };
                                // 0x4bffb0: while the width exceeds the wrap count, break at the last space whose prefix fits
                                // (scanning back from the end); no fitting space leaves the text as it is.
                                while (textW(text) > wrapPx) {
                                    size_t cut = std::string::npos;
                                    for (size_t n = text.size(); n-- > 1;)
                                        if (text[n] == ' ' && textW(text.substr(0, n)) <= wrapPx) { cut = n; break; }
                                    if (cut == std::string::npos) break;
                                    drawLine(text.substr(0, cut));
                                    text = text.substr(cut + 1);
                                }
                                drawLine(text);
                            }
                            // Connector line (0x4d4990 twice; horizontal code 5 = already screen pixels, vertical code 1):
                            // from the first row's top down to the last icon's top, in the gap between the icons and the text.
                            const float yTop = ry - fontH;
                            const float D = lastIconTop - yTop;
                            if (lastIconTop != 0.0f && D != 0.0f) {
                                const float X = ((12.0f - sr) * 0.5f + rw + rx) * sc;
                                Quad2D sh{X + sr, (yTop + 1.0f) * sc, X + sr + 1.0f, (yTop + 1.0f) * sc + D * sc};
                                sh.rgba[0] = sh.rgba[1] = sh.rgba[2] = 0, sh.rgba[3] = b8(objAlpha);
                                out.push_back(sh);
                                Quad2D ln{X, yTop * sc, X + sr, yTop * sc + D * sc};
                                std::memcpy(ln.rgba, lastCol.data(), 4);
                                out.push_back(ln);
                            }
                        }
                    }
                    // Header (0x4bfdd0): CGAME_MISSIONOBJECTIVES at the item's x / baseline y, the item colour; below it a one pixel
                    // underline (thickness round(sy)) from x to the mirrored right margin with a black copy one pixel right / down.
                    {
                        const ui::Item& it = *objHeaderItem;
                        const float rx = objMenuX + it.rect.x, ry = objMenuY + it.rect.y;
                        if (const assets::Font* f = fontFor(it.textScale)) {
                            const std::string* t = loc.find("CGAME_MISSIONOBJECTIVES");
                            const uint8_t col[4] = {b8(it.foreColor[0]), b8(it.foreColor[1]), b8(it.foreColor[2]), b8(objAlpha)};
                            hudText(out, t ? *t : std::string("CGAME_MISSIONOBJECTIVES"), rnd(rx * sc), rnd(ry * sc),
                                    it.textScale * 48.0f / float(f->pixelHeight) * sc, *f, col, 0.0f, 1.0f, it.textStyle);
                            const float t0 = rx * sc, W = -rx * sc + float(w) - t0, y0 = ry * sc;
                            Quad2D sh{t0 + sr, y0 + 2.0f * sr, t0 + sr + W, y0 + 2.0f * sr + 1.0f};
                            sh.rgba[0] = sh.rgba[1] = sh.rgba[2] = 0, sh.rgba[3] = b8(objAlpha);
                            out.push_back(sh);
                            Quad2D ln{t0, y0 + sr, t0 + W, y0 + sr + sr};
                            std::memcpy(ln.rgba, col, 4);
                            out.push_back(ln);
                        }
                    }
                }
            }
            // Dead quote (ownerdraw CG_DEADQUOTE, 0x4aa2a0): after opendeadscreen the ui_deadquote dvar (a leading '@'
            // skipped, localized) fades in over hud_deathQuoteFadeTime ms (default 1000) of client time. 0x4cc6a0 wraps
            // it at spaces to the item width, centres each line on rect x + textalignx, line step text height + 5;
            // the glyph scale is textscale × 48 / font pixel height (gfx refexport +0x128, 0x1000bfa0). The item's
            // textstyle reaches the text draw (menu 0x4cec99 → 0x4c4070 → 0x4aa690 → 0x4aa2a0 → 0x4cc6a0 → 0x4c3790),
            // so ITEM_TEXTSTYLE_SHADOWED gets the renderer's shadow (see hudText). TODO(spec): [argument mapping not read] which ownerdraw
            // arguments carry textalignx/textaligny (from the menu's "point to center around" comments).
            if (world && deadQuoteItem && deadScreenMs && deadScreenMs < cgNowMs()) {
                const auto qv = world->cvars.find("ui_deadquote");
                // Font for textfont 0 (0x4c38e0): textscale × (screen height / 480) ≤ ui_smallFont (0.25) → $smallfont,
                // ≥ ui_extraBigFont (0.55) → $extrabigfont, < ui_bigFont (0.4) → $font, else $bigfont. The aliases
                // are the assetGlobalDef fonts of ui/main.menu (smallFont, font = normalFont, bigFont, extraBigFont).
                auto cvarF = [&](const char* k, float def) {
                    auto it = world->cvars.find(k);
                    return it == world->cvars.end() ? def : float(std::atof(it->second.c_str()));
                };
                const float fs = deadQuoteItem->textScale * float(h) / 480.0f;
                const char* fontName = fs <= cvarF("ui_smallFont", 0.25f) ? "smallFont" : fs >= cvarF("ui_extraBigFont", 0.55f) ? "extraBigFont"
                                     : fs < cvarF("ui_bigFont", 0.4f) ? "normalFont" : "bigFont";
                const assets::Font* f = font(fontName);
                if (qv != world->cvars.end() && f) {
                    int fadeMs = 1000;
                    if (auto it = world->cvars.find("hud_deathQuoteFadeTime"); it != world->cvars.end()) fadeMs = std::atoi(it->second.c_str());
                    const int64_t since = cgNowMs() - deadScreenMs;
                    const float alpha = fadeMs > 0 && since <= fadeMs ? float(since) / float(fadeMs) : 1.0f;
                    std::string key = qv->second;
                    if (!key.empty() && key[0] == '@') key.erase(0, 1);
                    const std::string* t = loc.find(key);
                    const std::string text = t ? *t : key;
                    const ui::Item& qi = *deadQuoteItem;
                    const float gs = qi.textScale * 48.0f / float(f->pixelHeight); // virtual units per font pixel
                    auto width = [&](const std::string& str) {
                        float wd = 0;
                        for (unsigned char ch : str)
                            if (const auto* g = f->find(ch)) wd += float(g->dx) * gs;
                        return wd;
                    };
                    std::vector<std::string> lines;
                    std::string line, word;
                    auto flushWord = [&] {
                        if (!line.empty() && width(line + " " + word) > qi.rect.w) { lines.push_back(line); line.clear(); }
                        line += (line.empty() ? "" : " ") + word;
                        word.clear();
                    };
                    for (char ch : text) {
                        if (ch == ' ' || ch == '\t') { if (!word.empty()) flushWord(); }
                        else if (ch == '\n') { if (!word.empty()) flushWord(); lines.push_back(line); line.clear(); }
                        else word += ch;
                    }
                    if (!word.empty()) flushWord();
                    if (!line.empty()) lines.push_back(line);
                    const uint8_t rgba[4] = {uint8_t(qi.foreColor[0] * 255), uint8_t(qi.foreColor[1] * 255), uint8_t(qi.foreColor[2] * 255),
                                             uint8_t(std::clamp(qi.foreColor[3] * alpha, 0.0f, 1.0f) * 255)};
                    const float step = float(int(qi.textScale * 48.0f) + 5);
                    float y = qi.rect.y + qi.textAlignY;
                    for (const auto& l : lines) {
                        hudText(out, l, left43 + (qi.rect.x + qi.textAlignX) * sc, y * sc, gs * sc, *f, rgba, 0.5f, 1.0f, qi.textStyle);
                        y += step;
                    }
                }
            }
            // Crosshair (cgame 0x4bd760). On a turret (eFlags & 0x300) only the turret's: with cg_drawTurretCrosshair (1)
            // and cg_crosshairAlpha (1) ≥ 0.01, its weapon's reticleCenter, reticleCenterSize virtual units square,
            // centred (0x4bcef0). In a vehicle the vehicle's turret weapon (0x4bd000). Otherwise the hand weapon's
            // with alpha (1 − scope overlay alpha) · cg_crosshairAlpha (0x4bce70) when ≥ 0.01, not at full aim
            // (cg_drawGun 1), and not in weapon states 1, 2, 5, 10, 11 (0x4bd110, cg_drawCrosshair 1).
            // The colours (0x4bce70, verified): see below. The sway projection (0x4bc6e0) is (0, 0) except with
            // cg_crosshairDynamic 1 or cg_drawGun 0, so nothing to draw by default.
            const bool onTurretHud = world && world->playerOnTurret();
            const bool inVehicleHud = world && (world->playerPmFlags() & 0x20000) && !(world->playerPmFlags() & 0x80000);
            // Paused with cg_drawpaused set hides the turret reticle (0x4bcef0, 0x4bcf03..0x4bcf1b) and the hand crosshair (0x4bd110,
            // 0x4bd11c); the vehicle weapon reticle (0x4bd000) has no such test.
            const bool pausedHide = menuEnv.getCvar("cl_paused") == "1" && menuEnv.getCvar("cg_drawpaused") != "0";
            if ((onTurretHud && !pausedHide) || (inVehicleHud && !onTurretHud)) {
                // 0x4bd000: the vehicle's weapon reticle, drawn unconditionally (the 0x4bd110 conditions are not checked there).
                const std::string tw = onTurretHud ? world->playerTurretWeapon() : world->playerVehicleTurretWeapon();
                const auto* td = tw.empty() ? nullptr : world->weapon(tw);
                const std::string reticle = td && td->count("reticlecenter") ? td->at("reticlecenter") : std::string();
                const float size = td && td->count("reticlecentersize") ? float(std::atof(td->at("reticlecentersize").c_str())) : 0.0f;
                if (!reticle.empty() && size > 0) {
                    Quad2D q{left43 + (320.0f - size * 0.5f) * sc, (240.0f - size * 0.5f) * sc, 0, 0};
                    q.x1 = q.x0 + size * sc;
                    q.y1 = q.y0 + size * sc;
                    q.texture = materialTexture(reticle);
                    out.push_back(q);
                }
            }
            // The weapon the crosshair reads: the client's own, or in a killcam the followed client's recorded one (the copied
            // player state's weapon, state and aim fraction; docs/spec/mp-hud.md "The recorded view state").
            struct HudWeapon {
                bool equipped = false;
                std::string name;
                int state = 0;
                float adsFrac = 0, aimSpread = 255, viewHeight = 60;
                bool zoomingIn = true, cooking = false;
                int fuseMs = 0;
                std::function<std::string(const char*)> field;
            } hw;
            if (killcamRig.active) {
                const game::MpViewState& vs = killcamRig.view.view;
                const std::string vw = vs.viewWeapon.empty() ? killcamRig.view.weapon : vs.viewWeapon;
                hw.equipped = !vw.empty() && world && world->weapon(vw);
                hw.name = vw;
                hw.state = vs.weaponState;
                hw.adsFrac = vs.adsFrac;
                hw.aimSpread = vs.aimSpread;
                hw.viewHeight = vs.viewHeight;
                hw.zoomingIn = vs.adsHeld;
                hw.field = [&, vw](const char* k) { return killcamField(vw, k); };
            } else {
                hw.equipped = pweapon.equipped();
                hw.name = pweapon.name();
                hw.state = pweapon.stateNumber();
                hw.adsFrac = pweapon.adsFrac();
                hw.aimSpread = hudAimSpread;
                hw.viewHeight = hudViewHeight;
                hw.zoomingIn = pweapon.adsZoomingIn();
                hw.cooking = pweapon.cooking();
                hw.fuseMs = pweapon.fuseMs();
                hw.field = [&](const char* k) { return pweapon.field(k); };
            }
            const int wst = hw.state;
            const float crossAlpha = 1.0f - (world ? scopeAlpha() : 0.0f);
            // cg_drawCrosshair (bool, default 1; 0x4bd129) off: no hand crosshair (the turret and vehicle reticles do not test it).
            const bool crossOn = menuEnv.getCvar("cg_drawCrosshair") != "0" && menuEnv.getCvar("cg_drawcrosshair") != "0";
            const bool drawCross = crossOn && !pausedHide && !onTurretHud && !inVehicleHud && crossAlpha >= 0.01f && hw.adsFrac != 1.0f &&
                                   wst != 1 && wst != 2 && wst != 5 && wst != 10 && wst != 11;
            // Look (0x4bd760 → 0x4bd2c0 centre, 0x4bd520 sides). Aiming (0x4bd230, fraction past 1 − adsCrosshairInFrac
            // while zooming in, adsCrosshairOutFrac out): scale 1 − f/2 and the centre moved down by 480 / fov_y ·
            // adsAimPitch · f. cg_crosshairDynamic (0) keeps it otherwise at the screen centre. Alpha (0x4bd450):
            // (1 − aim spread / 255) · cg_crosshairAlpha (1) · (1 − scope alpha), at least cg_crosshairAlphaMin (0).
            // Centre: reticleCenter, reticleCenterSize · scale (+ fuse % 1000 · 0.01 while cooking a cookOffHold
            // grenade). Sides: reticleSide, reticleSideSize · scale square, at d = max(reticleMinOfs,
            // tan(spread · scale) / tan(fov_y / 2) · 240) − hipReticleSidePos · size from the centre, spread = hip min
            // + (max − min) · aim spread / 255 (0x4bd3a0, 0x4dceb0); top 0°, right 90°, bottom 180°, left 270°, with
            // the 0.125 · size placement terms of 0x4bd4a0.
            if (hw.equipped && drawCross) {
                auto num = [&](const char* k) { return float(std::atof(hw.field(k).c_str())); };
                float scale = 1.0f, offY = 0.0f;
                const float fovY = 2.0f * std::atan(std::tan(c.fovX43 * 0.5f * float(M_PI) / 180.0f) * 0.75f) * 180.0f / float(M_PI);
                if (const float af = hw.adsFrac; af != 0) {
                    const float cf = num(hw.zoomingIn ? "adsCrosshairInFrac" : "adsCrosshairOutFrac");
                    float t = af - (1.0f - cf);
                    if (t > 0 && cf != 0) {
                        t /= cf;
                        scale = 1.0f - 0.5f * t;
                        offY = 480.0f / fovY * num("adsAimPitch") * t;
                    }
                }
                const float alpha = std::max((1.0f - hw.aimSpread / 255.0f) * 1.0f * crossAlpha, 0.0f);
                const float cx = 320.0f, cy = 240.0f + offY;
                // Colour (0x4bce70): pm_flags 0x100000 → (0.25, 1, 0.25); 0x200000 with cg_crosshairEnemyColor (1) →
                // (1, 0.25, 0.25); else white.
                const uint32_t chf = world ? world->playerCrosshairFlags() : 0;
                const bool friendly = chf & 0x100000u, enemy = !friendly && (chf & 0x200000u);
                const uint8_t lo = uint8_t(0.25f * 255.0f);
                const uint8_t col[4] = {friendly ? lo : uint8_t(255), enemy ? lo : uint8_t(255), friendly || enemy ? lo : uint8_t(255),
                                        uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
                auto pic = [&](float x, float y, float sz, const std::string& mat, int rot) {
                    Quad2D q{left43 + x * sc, y * sc, 0, 0};
                    q.x1 = q.x0 + sz * sc;
                    q.y1 = q.y0 + sz * sc;
                    std::memcpy(q.rgba, col, 4);
                    q.texture = materialTexture(mat);
                    q.rot = rot;
                    out.push_back(q);
                };
                const std::string center = hw.field("reticleCenter"), side = hw.field("reticleSide");
                if (!center.empty()) {
                    float cs = num("reticleCenterSize");
                    if (hw.cooking && hw.fuseMs != 0)
                        cs += float(hw.fuseMs % 1000) * 0.01f;
                    cs *= scale;
                    pic(cx - cs * 0.5f, cy - cs * 0.5f, cs, center, 0);
                }
                if (!side.empty()) {
                    const float size = num("reticleSideSize") * scale;
                    if (const auto* d = world ? world->weapon(hw.name) : nullptr) {
                        auto fld = [&](const char* k) { auto it = d->find(k); return it == d->end() ? 0.0f : float(std::atof(it->second.c_str())); };
                        game::HipSpread hs;
                        hs.standMin = fld("hipspreadstandmin"); hs.duckedMin = fld("hipspreadduckedmin"); hs.proneMin = fld("hipspreadpronemin");
                        hs.max = fld("hipspreadmax"); hs.duckedMax = fld("hipspreadduckedmax"); hs.proneMax = fld("hipspreadpronemax");
                        const auto range = game::hipSpreadRange(hs, hw.viewHeight);
                        const float spread = range[0] + (range[1] - range[0]) * hw.aimSpread / 255.0f;
                        float px = std::tan(spread * scale * float(M_PI) / 180.0f) / std::tan(fovY * float(M_PI) / 180.0f * 0.5f) * 240.0f;
                        px = std::max(px, num("reticleMinOfs"));
                        const float sp = px - num("hipReticleSidePos") * size, k = size * 0.125f;
                        pic(cx - size * 0.5f, cy - size - sp - k, size, side, 0);
                        pic(sp + cx, cy - size * 0.5f, size, side, 1);
                        pic(cx - size * 0.5f - k, sp + cy, size, side, 2);
                        pic(cx - size - sp - k, cy - size * 0.5f - k, size, side, 3);
                    }
                }
            }
            // Grenade indicator (cgame 0x4be210, after the crosshair, only with pm_type < 4): for each grenade at rest (|trDelta|^2
            // <= 1) within range (src/game/grenadeindicator.h) a hud_grenadepointer box and a hud_grenadeicon box around the screen centre,
            // colour white with the pulsing alpha. Not under a scope overlay unless cg_hudGrenadeIconInScope (0).
            // TODO(spec): [needs trajectory state in the port] the exe reads the entity state's pos.trBase / pos.trDelta (a flying grenade keeps its launch / last
            // bounce base) and the 0x793940 / 0x793944 scales are taken as the uniform virtual-unit scale used elsewhere here.
            if (world && world->player() && (!(scopeAlpha() > 0) || std::atof(menuEnv.getCvar("cg_hudGrenadeIconInScope").c_str()) != 0.0) &&
                !(world->player()->obj->get("health").isNumber() && world->player()->obj->get("health").asInt() <= 0)) {
                const game::GrenadeIndicatorCfg gcfg;
                const std::array<float, 3> pOrigin{c.pos.x, c.pos.y, c.pos.z - hudViewHeight};
                const float ga = game::grenadeIndicatorAlpha(gcfg, cgNowMs());
                for (const auto& g : world->hudGrenades()) {
                    const std::array<float, 3> gb{g.base[0], g.base[1], g.base[2]}, gd{g.delta[0], g.delta[1], g.delta[2]};
                    if (!game::grenadeIndicatorVisible(gcfg, gb, gd, pOrigin, hudViewHeight)) continue;
                    const float ang = game::grenadeIndicatorAngle(gb[0] - pOrigin[0], gb[1] - pOrigin[1], c.yaw);
                    const auto lay = game::grenadeIndicatorLayout(gcfg, float(w) * 0.5f, float(h) * 0.5f, sc, sc, ang);
                    const struct { const game::GrenadeIndicatorQuad* quad; const char* mat; } parts[2] = {{&lay.pointer, "hud_grenadepointer"}, {&lay.icon, "hud_grenadeicon"}};
                    for (const auto& part : parts) {
                        Quad2D q{0, 0, 0, 0};
                        q.free = true;
                        for (int k = 0; k < 4; ++k) {
                            q.qx[k] = part.quad->xy[size_t(k) * 2];
                            q.qy[k] = part.quad->xy[size_t(k) * 2 + 1];
                        }
                        q.rgba[3] = uint8_t(std::clamp(ga, 0.0f, 1.0f) * 255.0f);
                        q.texture = materialTexture(part.mat);
                        out.push_back(q);
                    }
                }
            }
            // Hit marker (openiw2 option iw2_hitMarker, src/game/hitmarker.h): the multiplayer damage feedback material at the screen centre,
            // 24 x 24, fading over a second after each hit on an enemy actor. Drawn here, after the script elements and the scope overlay
            // (which is spliced in below them), so it shows over a scope as the multiplayer script element does. No sound. Hidden with
            // menus open, while paused and while the player is dead.
            if (world && world->player() && frontStack.empty() && menuEnv.getCvar("cl_paused") != "1" &&
                game::hitMarkerMode(float(std::atof(menuEnv.getCvar("iw2_hitMarker").c_str()))) != game::HitMarkerMode::Off &&
                !(world->player()->obj->get("health").isNumber() && world->player()->obj->get("health").asInt() <= 0)) {
                const game::HitMarkerLook look;
                if (const float ha = game::hitMarkerAlpha(cgNowMs(), world->playerHitMarkerMs(), look); ha > 0) {
                    Quad2D q{left43 + (320.0f + look.offset) * sc, (240.0f + look.offset) * sc, 0, 0};
                    q.x1 = q.x0 + look.size * sc;
                    q.y1 = q.y0 + look.size * sc;
                    const auto col = game::hitMarkerColor(game::hitMarkerMode(float(std::atof(menuEnv.getCvar("iw2_hitMarker").c_str()))),
                                                          world->playerHitMarkerMs(), world->playerHitMarkerKillMs());
                    std::copy(col.begin(), col.end(), q.rgba);
                    q.rgba[3] = uint8_t(std::clamp(ha, 0.0f, 1.0f) * 255.0f + 0.5f);
                    q.texture = materialTexture("damage_feedback");
                    out.push_back(q);
                }
            }
            // Crosshair target names (cgame 0x4bf8b0, drawn when cl_paused is 0): the server's configstrings 0xb / 0xc (0x534aa0:
            // the target actor's name and weapon display name) as localized text at CENTER/CENTER alignment (0x440e60 mode 2):
            // line 1 at (+25, -2) in green (0 1 0 1), line 2 at (+25, +18) in white alpha 0.7, textscale 1/3, style 3
            // (shadowed), baseline at y, left aligned. The font is the generic branch of 0x4c38e0, inlined at 0x4bf913..0x4bf99b (scale = screen
            // height / 480 * textscale against ui_smallFont, ui_extraBigFont, ui_bigFont: the same thresholds as menuFont). TODO(spec): [fallback not read] the
            // \x15 localization fallback.
            if (world && !world->crosshairNameLine().empty()) {
                const float ts = 1.0f / 3.0f, fs = ts * float(h) / 480.0f;
                if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                    const float gs = ts * 48.0f / float(f->pixelHeight) * sc;
                    const float cx = float(w) * 0.5f + 25.0f * sc, cy = float(h) * 0.5f;
                    auto loca = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
                    const uint8_t green[4] = {0, 255, 0, 255}, white7[4] = {255, 255, 255, uint8_t(0.7f * 255.0f)};
                    hudText(out, loca(world->crosshairNameLine()), cx, cy - 2.0f * sc, gs, *f, green, 0.0f, 1.0f, 3);
                    if (!world->crosshairWeaponLine().empty())
                        hudText(out, loca(world->crosshairWeaponLine()), cx, cy + 18.0f * sc, gs, *f, white7, 0.0f, 1.0f, 3);
                }
            }
            // Ammo counter. Menu "weaponinfo" has rect 0 0 0 0 HORIZONTAL_ALIGN_RIGHT VERTICAL_ALIGN_BOTTOM and its items
            // give no alignment of their own, so they inherit RIGHT/BOTTOM: the negative rects are offsets from the
            // viewable right/bottom edge (exe 0x440e60: RIGHT = x*sx + 0x79397c, the right edge of the window, set up
            // by 0x440a40 from the real resolution; BOTTOM likewise), i.e. NOT from the 4:3 box on a wide screen.
            // The ammo ownerdraws (0x4a7818 …) special-case only vehicles (pm_flags 0x20000), not turrets, so the hand
            // weapon's ammo stays on a turret. Text rule (handler 0x4a7930, both clip and stock valid): clip "%2i"
            // left at rect.x, stock "%3i" right-aligned at rect.x + rect.w, "|" centred in the rect minus 5 (0x591ef0);
            // baseline at rect.y (4c3790 gets the item rect y unchanged). Low-ammo red, the fade alpha and the firemode icon are
            // below. 0x4a76b0 reads maxAmmo from the def registered for the weapon's ammo index (0x1c64750[def +0x1c8] +0x1d4);
            // here the current weapon's own maxAmmo is used. The owner of a shared ammo index is the first registered weapon with that
            // ammoName (0x4dbdc0) and the exe prints a message (0x4dbed7) when sharers differ in maxAmmo; no shipped SP weapon file differs (test_ammoshare),
            // so the two readings give the same number.
            // Fade (0x4a7600 / 0x4b0900): dvar seconds 0 -> 1; else touch == 0 or elapsed >= ticks -> 0, else min(1, (ticks - elapsed) / 700).
            // Ammo backdrop / text use hud_fade_ammodisplay (default 2 s, 0x10438d8) from the ammo touch time; the firemode icon the same dvar
            // but a fixed 0x708 = 1800 ms window from the weapon change time (0xf796e4); the compass hud_fade_compass (default 8 s).
            // cg_drawpaused == 0 forces alpha 0 first (0x4a7600, and inline in the compass / stance handlers 0x4a90e2, 0x4a9222, 0x4a8214).
            const bool drawPausedOn = menuEnv.getCvar("cg_drawpaused") != "0";
            auto fadeAlpha = [&](int64_t touch, float dvarSeconds, int ticksMs) {
                if (!drawPausedOn) return 0.0f;
                if (dvarSeconds == 0.0f) return 1.0f;
                const int64_t now = cgNowMs(), el = now - touch;
                if (touch == 0 || el >= ticksMs) return 0.0f;
                const int64_t rem = ticksMs - el;
                return rem >= 700 ? 1.0f : float(rem) / 700.0f;
            };
            // MP registers hud_fade_ammodisplay / compass / offhand with 0 s (0x4c3430 in the MP executable): those never fade there.
            const bool mpHud = world && world->multiplayer();
            const float ammoFadeSec = game::hudFadeDefaultSeconds(game::HudFadeDvar::AmmoDisplay, mpHud);
            const float ammoAlpha = fadeAlpha(hudFade.ammo, ammoFadeSec, 3000) /* SP: hud_fade_ammodisplay default 3.0 (0x4a728a) */;
            const bool inVeh = world && (world->playerPmFlags() & 0x20000); // the ammo ownerdraws draw nothing in a vehicle (0x4a7818)
            // Low stock (0x4a76b0): stock <= 0.2 * maxAmmo (def +0x1d4, clamped to 999; 0 when negative); low clip (0x4a7760): clip <= 0.33 * clipSize
            // (def +0x1d8; false when clipSize <= 0). Both tint red (0.89 0.18 0.01).
            // def +0x1d4 / +0x1d8 are maxAmmo / clipSize: weapon field table 0x5a7e80 (docs/spec/inventory.md; the table is in the
            // packed .data, read at run time), and givemaxammo (0x538020) reads +0x1d4 at 0x538109 for the stock limit.
            // The weapon the weapon-info ownerdraws read (ammo 0x4a79b0 / 0x4a7760 / 0x4a76b0, name 0x4a7da0, firemode icon 0x4a8040): the selected weapon
            // cg.weaponSelect (0xf796e0) when the player owns it (bitset 0xf70e00, bounds-checked against the weapon count), else the player state's
            // weapon (0xf70998). So the display follows the key press at once instead of the end of the drop. While an offhand grenade is out the
            // primary stays shown (the offhand is not the player state's weapon). In a killcam the followed client's archived weapon is the player state's (hudKillcam).
            const std::map<std::string, std::string>* hwDef = hudKillcam && world ? world->weapon(hudKillcam->weapon) : nullptr;
            std::string selKey;
            if (!hudKillcam && world && world->player()) {
                auto lower = [](std::string v) {
                    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    return v;
                };
                const std::string cur = lower(pweapon.name()), sel = lower(weaponSelect);
                const game::Inventory& inv = world->inventory(*world->player());
                selKey = game::hudShownWeapon(sel, cur, [&](const std::string& n) { return inv.owned.count(n) != 0; });
                if (selKey != cur) hwDef = world->weapon(selKey);
                else selKey.clear();
                // A grenade being cooked or thrown is the offhand, not the player state's weapon: the ownerdraws keep
                // showing the selected (primary) weapon's name and ammo (ps.weapon stays the primary, ps+208 is the offhand).
                if (!hwDef && pweapon.offhandActive()) hwDef = world->weapon(cur);
            }
            auto hwField = [&](const char* k) -> std::string {
                if (!hwDef) return pweapon.hudField(k);
                std::string key = k;
                for (char& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                const auto it = hwDef->find(key);
                return it == hwDef->end() ? std::string() : it->second;
            };
            auto hwNum = [&](const char* k) { return hwDef ? float(std::atof(hwField(k).c_str())) : pweapon.hudNumber(k); };
            const bool hwEquipped = hudKillcam ? hwDef != nullptr : pweapon.equipped();
            const std::string hwName = hudKillcam ? hudKillcam->weapon : !selKey.empty() ? selKey : pweapon.name();
            int hwClip = hudKillcam ? hudKillcam->clip : pweapon.clip(), hwStockRaw = hudKillcam ? hudKillcam->stock : pweapon.stock();
            if (!hudKillcam && hwDef) { // the selected weapon's own clip and stock
                game::Inventory& inv = world->inventory(*world->player());
                hwClip = inv.clip[hwField("clipName")];
                hwStockRaw = inv.ammo[hwField("ammoName")];
            }
            const float maxAmmo = std::min(hwNum("maxAmmo"), 999.0f), clipSize = std::min(hwNum("clipSize"), 999.0f);
            // clipOnly weapons (def +0x340): the clip is "invalid" for the ownerdraw (0x4a7a13) and the displayed stock is the
            // clip count itself (0x4dcd60 without a shared-ammo group returns ps.ammoclip[clipIndex] for them, ps.ammo[ammoIndex]
            // otherwise); the shared-group sum (def +0x1e4 >= 0) is World::hudStock.
            const bool clipOnly = hwNum("clipOnly") != 0.0f;
            int dispStock = clipOnly ? hwClip : hwStockRaw;
            // Shared cap group (def +0x1e4 >= 0, field sharedAmmoCapName): the stock shown is the group sum (0x4dcd60, World::hudStock).
            if (world && !hwName.empty() && !hudKillcam) { // TODO(spec): the killer's shared-group sum
                std::string k = hwName;
                for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (const int g = world->hudGroupStock(k); g >= 0) dispStock = g;
            }
            const bool stockLow = maxAmmo >= 0 && float(std::min(dispStock, 999)) <= maxAmmo * 0.2f;
            const bool clipLow = !clipOnly && clipSize > 0 && float(std::min(hwClip, 999)) <= clipSize * 0.33f;
            if (hwEquipped && !inVeh && ammoAlpha > 0.0f) {
                const float rightEdge = float(w), bottomEdge = float(h); // RIGHT / BOTTOM alignment, safe area 0
                if (ammoBackItem && !ammoBackItem->background.empty()) {
                    const auto& r = ammoBackItem->rect;
                    Quad2D q{rightEdge + r.x * sc, bottomEdge + r.y * sc, 0, 0};
                    q.x1 = q.x0 + r.w * sc;
                    q.y1 = q.y0 + r.h * sc;
                    for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(ammoBackItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                    if (stockLow) q.rgba[0] = uint8_t(0.89f * 255.0f), q.rgba[1] = uint8_t(0.18f * 255.0f), q.rgba[2] = uint8_t(0.01f * 255.0f);
                    q.rgba[3] = uint8_t(std::clamp(ammoAlpha, 0.0f, 1.0f) * 255.0f);
                    q.texture = materialTexture(ammoBackItem->background);
                    out.push_back(q);
                }
                if (ammoTextItem) {
                    const auto& r = ammoTextItem->rect;
                    const float ts = ammoTextItem->textScale;
                    const float fs = ts * float(h) / 480.0f; // font choice as for the cursor hint (same item defaults)
                    if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                        const float gs = ts * 48.0f / float(f->pixelHeight) * sc;
                        uint8_t col[4];
                        for (int k = 0; k < 4; ++k) col[k] = uint8_t(std::clamp(ammoTextItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                        auto fmt = [](const char* pat, int v) { char b[16]; std::snprintf(b, sizeof b, pat, std::min(v, 999)); return std::string(b); };
                        auto width = [&](const std::string& t) {
                            float tw = 0;
                            for (unsigned char ch : t)
                                if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                            return tw;
                        };
                        const float x0 = rightEdge + r.x * sc, y0 = bottomEdge + r.y * sc, w0 = r.w * sc;
                        const std::string clip = fmt("%2i", hwClip), stock = fmt("%3i", dispStock);
                        const int st = ammoTextItem->textStyle;
                        // Colours (0x4a7930): alpha = the fade; the clip keeps the item colour, "|" and the stock turn red when the
                        // stock is low; a low clip is overdrawn in red with a sawtooth alpha (800 ms, 1 -> 0, restarting),
                        // limited by the fade alpha (0x4a7aab..0x4a7b09, state 0xf79690).
                        col[3] = uint8_t(std::clamp(ammoAlpha, 0.0f, 1.0f) * 255.0f);
                        uint8_t col2[4] = {col[0], col[1], col[2], col[3]};
                        if (stockLow) col2[0] = uint8_t(0.89f * 255.0f), col2[1] = uint8_t(0.18f * 255.0f), col2[2] = uint8_t(0.01f * 255.0f);
                        if (clipOnly) {
                            // Clip invalid, stock valid (0x4a7d28): the stock text alone, centred in the rect, colour as above.
                            hudText(out, stock, x0 + (w0 - width(stock)) * 0.5f, y0, gs, *f, col2, 0.0f, 1.0f, st);
                        } else {
                            hudText(out, clip, x0, y0, gs, *f, col, 0.0f, 1.0f, st);
                            if (clipLow) {
                                const int64_t now = cgNowMs();
                                if (hudFade.flashStart > now || hudFade.flashStart + 800 < now) hudFade.flashStart = now;
                                const float saw = float(hudFade.flashStart - now + 800) * 0.00125f;
                                const uint8_t flash[4] = {uint8_t(0.89f * 255.0f), uint8_t(0.18f * 255.0f), uint8_t(0.01f * 255.0f),
                                                          uint8_t(std::clamp(std::min(saw, ammoAlpha), 0.0f, 1.0f) * 255.0f)};
                                hudText(out, clip, x0, y0, gs, *f, flash, 0.0f, 1.0f, st);
                            }
                            hudText(out, "|", x0 + (w0 - width("|")) * 0.5f - 5.0f * sc, y0, gs, *f, col2, 0.0f, 1.0f, st);
                            hudText(out, stock, x0 + w0, y0, gs, *f, col2, 1.0f, 1.0f, st);
                        }
                    }
                }
            }
            // Weapon name (ownerdraw 81, 0x4a7da0): not in a vehicle (pm_flags 0x20000); same fade as the firemode icon (hud_fade_ammodisplay,
            // 1800 ms from the weapon change time 0xf796e4, 0x4a7600). Text = localized displayName (client weapon info +0xb4), plus
            // " / " + localized modeName (+0xb8, def +0x70) when the weapon has a modeName ("%s / %s" 0x57fb54, else "%s" 0x56ca4c).
            // Drawn at x = rect.x + rect.w - textWidth - 28 (0x592084), y = rect.y, item colour with the fade alpha, item text style,
            // font by the HUD rule.
            if (weapNameItem && hwEquipped && !inVeh) {
                const float a = fadeAlpha(hudFade.weaponChange, ammoFadeSec, 1800);
                const float ts = weapNameItem->textScale, fs = ts * float(h) / 480.0f;
                if (a > 0.0f)
                    if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                        auto name = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
                        std::string text = name(hwField("displayName"));
                        const std::string mode = hwField("modeName");
                        if (!mode.empty()) text += " / " + name(mode);
                        const float gs = ts * 48.0f / float(f->pixelHeight) * sc;
                        float tw = 0;
                        for (unsigned char ch : text)
                            if (const auto* g = f->find(ch)) tw += float(g->dx) * gs;
                        const auto& r = weapNameItem->rect;
                        uint8_t col[4];
                        for (int k = 0; k < 4; ++k) col[k] = uint8_t(std::clamp(weapNameItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                        col[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                        hudText(out, text, float(w) + (r.x + r.w - 28.0f) * sc - tw, float(h) + r.y * sc, gs, *f, col, 0.0f, 1.0f, weapNameItem->textStyle);
                    }
            }
            // Offhand items (menu "offhandinfo"; ownerdraws 100 / 101 icon, 102 / 103 ammo, 104 / 105 name for class 1 frag / 2 smoke,
            // 0x4b84f0 / 0x4b8850 / 0x4b8990): hidden in a vehicle (pm_flags 0x20000 without 0x80000), until the offhand touch
            // (0xf79a48) and when the player owns no weapon of the class; alpha = hud_fade_offhand (3 s, 0x10438cc). Icon = hudIcon of the
            // first owned weapon of the class with rounds (0x4dc6d0), item colour. Ammo = "%i" of the rounds of the class' weapons
            // (0x4b8690), red (0.89 0.18 0.01) at 0, else the item colour. Name = localized WEAPON_FRAGGRENADE / WEAPON_SMOKEGRENADE (table
            // 0x5a4d58). TODO(spec): [helpers not read] the [0xf708ec] < 4 test, 0x4dc740 (class owned without rounds is assumed), 0x4b8490, flash helper 0x4b8620.
            if (world && world->player() && !(world->playerPmFlags() & 0x20000 && !(world->playerPmFlags() & 0x80000))) {
                const float oa = fadeAlpha(hudFade.offhand, game::hudFadeDefaultSeconds(game::HudFadeDvar::Offhand, mpHud), 3000);
                if (oa > 0.0f)
                    if (const ui::Menu* om = hudMenus.find("offhandinfo"))
                        for (const auto& itp : om->items) {
                            const ui::Item& it = *itp;
                            if (it.ownerDraw < 100 || it.ownerDraw > 105) continue;
                            const int cls = (it.ownerDraw - 100) % 2 + 1, kind = (it.ownerDraw - 100) / 2; // 0 icon, 1 ammo, 2 name
                            const std::string clsName = cls == 1 ? "Frag Grenade" : "Smoke Grenade";
                            auto lower = [](std::string v) { for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch))); return v; };
                            auto wf = [&](const std::string& n, const char* k) { const auto* d = world->weapon(n); return d && d->count(k) ? d->at(k) : std::string(); };
                            int rounds = 0;
                            bool owned = false;
                            auto& inv = world->inventory(*world->player());
                            for (const std::string& wn : inv.owned)
                                if (lower(wf(wn, "offhandclass")) == lower(clsName)) {
                                    owned = true;
                                    rounds += inv.clip[wf(wn, "clipname")] + inv.ammo[wf(wn, "ammoname")];
                                }
                            const std::string wep = world->playerOffhandFor(clsName);
                            if (wep.empty() && !owned) continue;
                            const auto& r = it.rect;
                            uint8_t col[4];
                            for (int k = 0; k < 4; ++k) col[k] = uint8_t(std::clamp(it.foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                            col[3] = uint8_t(std::clamp(oa, 0.0f, 1.0f) * 255.0f);
                            if (kind == 0) {
                                if (wep.empty()) continue;
                                const std::string icon = wf(wep, "hudicon");
                                if (icon.empty()) continue;
                                Quad2D q{float(w) + (om->rect.x + r.x) * sc, float(h) + (om->rect.y + r.y) * sc, 0, 0};
                                q.x1 = q.x0 + r.w * sc;
                                q.y1 = q.y0 + r.h * sc;
                                std::memcpy(q.rgba, col, 4);
                                q.texture = materialTexture(icon);
                                out.push_back(q);
                                continue;
                            }
                            std::string text;
                            if (kind == 1) {
                                text = std::to_string(rounds);
                                if (rounds == 0) { col[0] = 227; col[1] = 46; col[2] = 3; }
                            } else {
                                const std::string key = cls == 1 ? "WEAPON_FRAGGRENADE" : "WEAPON_SMOKEGRENADE";
                                const std::string* t = loc.find(key);
                                text = t ? *t : key;
                            }
                            const float ts = it.textScale, fs = ts * float(h) / 480.0f;
                            if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                                const float gs = ts * 48.0f / float(f->pixelHeight) * sc;
                                hudText(out, text, float(w) + (om->rect.x + r.x) * sc, float(h) + (om->rect.y + r.y) * sc, gs, *f, col, 0.0f, 1.0f, it.textStyle);
                            }
                        }
            }
            // Firemode icon (ownerdraw 83, 0x4a8040): not in a vehicle; alpha = fade over a fixed 1800 ms (0x708) from the weapon
            // change time (0xf796e4); the material is the weapon's modeIcon (client weapon info +0x140; only weapons with a non-empty
            // modeIcon key draw one), item colour, rect from the item (RIGHT / BOTTOM aligned like the other weaponinfo items).
            if (fireModeItem && hwEquipped && !inVeh) {
                const std::string icon = hwDef ? hwField("modeIcon") : pweapon.field("modeIcon");
                const float a = fadeAlpha(hudFade.weaponChange, ammoFadeSec, 1800);
                if (!icon.empty() && a > 0.0f) {
                    const auto& r = fireModeItem->rect;
                    Quad2D q{float(w) + r.x * sc, float(h) + r.y * sc, 0, 0};
                    q.x1 = q.x0 + r.w * sc;
                    q.y1 = q.y0 + r.h * sc;
                    for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(fireModeItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                    q.rgba[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                    q.texture = materialTexture(icon);
                    out.push_back(q);
                }
            }
            // Health bar (menu "Health": back + cross = ownerdraw 95 (0x4a8ab0), bar = ownerdraw 79 (0x4a8800)). Fade: hud_fade_healthbar
            // (2 s, touch 0xf79a3c, touched by a damage event 0x4ba537). Fraction (0x4a8790) = health / maxhealth clamped to 0..1 (1 when
            // either is 0). Bar: item rect with width * fraction, texcoords s 0..fraction, item colour with alpha = fade, adjusted by
            // the fraction c: c > 0.5 -> r and b scaled by 2 * (1 - c); else g = (c + 0.2) * g + 0.3. A red (1 0 0) trail (0xf796a8)
            // covers fraction..trail: it follows the fraction up at once and falls by 0.0012 per ms (0x592080) after a damage frame.
            // Back (95): background drawn with the item colour and the fade alpha; below hud_health_startpulse_injured (1.0) the same
            // background is drawn again in (0.89 0.18 0.01) with alpha (timer - now + period) / period, capped by the fade, period =
            // hud_health_pulserate_injured (1.0 s), below hud_health_startpulse_critical (0.33) pulserate_critical (0.5 s).
            // Both ownerdraws draw nothing unless the bool dvar cg_drawHealth (0xf48cf8, registered at 0x4a125d with default 0, flags
            // 0x1080; no shipped default.cfg / config sets it, only developer cfgs) is on, so the stock SP HUD has no health bar.
            // Fraction (0x4a8790): health / maxhealth (cg snapshot ps +0x108 / +0x110 = copies of the entity's health /
            // maxhealth, world.h) clamped to 0..1, and 0 (not 1) when either is 0 or pm_type is 4 (spectator; the port
            // has no pm_type 4 for the player). Per-frame touch (0x4be720): below hud_health_startpulse_injured (1.0)
            // the health fade is touched whenever its touch time is older than now (and the Health menu flagged), at
            // full health, once hud_fade_healthbar (2 s) has passed since the touch, the touch is cleared. The trail
            // quad (0x4a8a0f) spans x = fraction .. trail with texcoords s fraction .. trail, t 0 .. 1, colour (1, 0, 0,
            // fade alpha) as implemented.
            if (const ui::Menu* hm = hudMenus.find("Health"); hm && world && world->player() && std::atof(menuEnv.getCvar("cg_drawHealth").c_str()) != 0.0) {
                const int64_t now = cgNowMs();
                const int dtMs = hudFade.lastHudMs > 0 && now > hudFade.lastHudMs ? int(now - hudFade.lastHudMs) : 0;
                hudFade.lastHudMs = now;
                const auto hv = world->player()->obj->get("health"), mv = world->player()->obj->get("maxhealth");
                int hp = hv.isNumber() ? hv.asInt() : 0, maxHp = mv.isNumber() ? mv.asInt() : 100; // entity maxhealth (+0x140)
                if (hudKillcam) hp = hudKillcam->health, maxHp = hudKillcam->maxHealth; // the followed player state's copies
                const float frac = game::hudHealthFraction(hp, maxHp); // 0x4a8790
                if (frac < 1.0f) {
                    if (hudFade.health < now) hudFade.health = now;
                } else if (hudFade.health != 0 && now - hudFade.health > 2000) {
                    hudFade.health = 0; // hud_fade_healthbar 2.0 s (0x4a7300)
                }
                const float fa = fadeAlpha(hudFade.health, 2.0f, 2000);
                if (fa > 0.0f) {
                    // trail state machine (0x4a8988..0x4a8a00)
                    if (frac >= hudFade.healthTrail) {
                        hudFade.healthTrail = frac;
                        hudFade.healthTrailTimer = 1;
                    } else if (hudFade.healthTrailTimer != 0) {
                        hudFade.healthTrailTimer -= dtMs;
                        if (hudFade.healthTrailTimer < 0) hudFade.healthTrailTimer = 0;
                    } else {
                        hudFade.healthTrail = std::max(frac, hudFade.healthTrail - float(dtMs) * 0.0012f);
                    }
                    for (const auto& itp : hm->items) {
                        const ui::Item& it = *itp;
                        if (it.ownerDraw != 79 && it.ownerDraw != 95) continue;
                        const float rx = float(w) + (hm->rect.x + it.rect.x) * sc, ry = float(h) + (hm->rect.y + it.rect.y) * sc;
                        const float rw = it.rect.w * sc, rh = it.rect.h * sc;
                        float col[4] = {it.foreColor[0], it.foreColor[1], it.foreColor[2], fa};
                        auto put = [&](float x0, float x1, float s0, float s1, const float* cl, const std::string& mat) {
                            Quad2D q{x0, ry, x1, ry + rh};
                            q.s0 = s0, q.s1 = s1;
                            for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(cl[k], 0.0f, 1.0f) * 255.0f);
                            q.texture = materialTexture(mat);
                            out.push_back(q);
                        };
                        if (it.ownerDraw == 95) {
                            if (it.background.empty()) continue;
                            put(rx, rx + rw, 0, 1, col, it.background);
                            if (frac == 0.0f) continue;
                            int period = 0;
                            if (frac < 0.33f) period = int(0.5f * 1000.0f + 0.5f);       // hud_health_startpulse_critical / pulserate_critical
                            else if (frac < 1.0f) period = int(1.0f * 1000.0f + 0.5f);   // hud_health_startpulse_injured / pulserate_injured
                            if (period == 0) continue;
                            if (hudFade.healthPulse > now || hudFade.healthPulse + period < now) hudFade.healthPulse = now;
                            const float pa = float(hudFade.healthPulse - now + period) / float(period);
                            const float pc[4] = {0.89f, 0.18f, 0.01f, std::min(pa, fa)};
                            put(rx, rx + rw, 0, 1, pc, it.background);
                        } else if (frac > 0.0f && !it.background.empty()) {
                            if (frac > 0.5f) { col[0] = 2.0f * (1.0f - frac) * col[0]; col[2] = 2.0f * (1.0f - frac) * col[2]; }
                            else col[1] = (frac + 0.2f) * col[1] + 0.3f;
                            put(rx, rx + rw * frac, 0, frac, col, it.background);
                            if (hudFade.healthTrail > frac) {
                                const float tc[4] = {1.0f, 0.0f, 0.0f, fa};
                                put(rx + rw * frac, rx + rw * hudFade.healthTrail, frac, hudFade.healthTrail, tc, it.background);
                            }
                        }
                    }
                }
            }
            // Stance (menu "stance", ownerdraw 20, 0x4a81e0): alpha = fade of hud_fade_stance (default 1.7 s, 0x10438e4, ticks = dvar * 1000,
            // touch 0xf79a44); nothing while the alpha is 0, nor in a vehicle (pm_flags 0x20000 without 0x80000). The stance change
            // time (0xf796c8) is set to now when it lies in the future or the stance (pm_flags & 3) differs from the stored one
            // (0xf796c4). Icon: bit 0 stance_prone, else bit 1 stance_crouch, else stance_stand; then, for 1000 ms after the change,
            // the stance_flash overlay with the cg_hudStanceFlash colour (default white) and alpha (changeTime + 1000 - now) * 0.001 * 0.8
            // (0x591e30, 0x591f94) capped by the fade alpha. Rect: menu 90 -70 LEFT / BOTTOM + item 60x60.
            // TODO(spec): [scale constants not read] the cg_hudStanceHintPrints text lines (default 0, 0xf569c0), the position scale factors (0xf5bcc8, 0x591f30) and
            // the bool dvar 0x1040c4c checked in the fade.
            if (const ui::Menu* sm = hudMenus.find("stance"); sm && world) {
                const int64_t now = cgNowMs();
                const uint32_t st = world->playerPmFlags() & 3u;
                if (hudFade.stanceChange > now || hudFade.stanceState != st) hudFade.stanceChange = now;
                hudFade.stanceState = st;
                const float sa = fadeAlpha(hudFade.stance, 1.7f, 1700);
                const bool stanceInVeh = (world->playerPmFlags() & 0x20000) && !(world->playerPmFlags() & 0x80000);
                if (sa > 0.0f && !stanceInVeh)
                    for (const auto& itp : sm->items) {
                        if (itp->ownerDraw != 20) continue;
                        const auto& r = itp->rect;
                        Quad2D q{(sm->rect.x + r.x) * sc, float(h) + (sm->rect.y + r.y) * sc, 0, 0};
                        q.x1 = q.x0 + r.w * sc;
                        q.y1 = q.y0 + r.h * sc;
                        for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(itp->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                        q.rgba[3] = uint8_t(std::clamp(sa, 0.0f, 1.0f) * 255.0f);
                        q.texture = materialTexture((st & 1u) ? "stance_prone" : (st & 2u) ? "stance_crouch" : "stance_stand");
                        out.push_back(q);
                        if (hudFade.stanceChange + 1000 > now) {
                            const float fa = std::min(sa, float(hudFade.stanceChange + 1000 - now) * 0.001f * 0.8f);
                            Quad2D f = q;
                            // Colour: the dvar cg_hudStanceFlash (default 1 1 1 1); its rgb, the alpha from the fade above.
                            float flash[4] = {1, 1, 1, 1};
                            if (const std::string fs = menuEnv.getCvar("cg_hudStanceFlash"); !fs.empty())
                                std::sscanf(fs.c_str(), "%f %f %f %f", &flash[0], &flash[1], &flash[2], &flash[3]);
                            for (int k = 0; k < 3; ++k) f.rgba[k] = uint8_t(std::clamp(flash[k], 0.0f, 1.0f) * 255.0f);
                            f.rgba[3] = uint8_t(std::clamp(fa, 0.0f, 1.0f) * 255.0f);
                            f.texture = materialTexture("stance_flash");
                            out.push_back(f);
                        }
                    }
            }
            // Tank status (menu "tankstatus", ownerdraws 90 body 0x4aa1b0 / 91 barrel 0x4aa360): only in a vehicle (pm_flags 0x20000, not
            // 0x80000). Both are LEFT / BOTTOM aligned like the stance icon. angle = wrap180(view yaw (0xf79194) - yaw), yaw = the
            // vehicle's angles[1] (centity +0x158) for the body, the tag_barrel forward yaw (0x4ab9e0) for the barrel. The body quad
            // turns about its centre (0x4b0310); the barrel quad (x ± w/2, y - 0.75 h .. y + 0.25 h; constants 0x591e38, 0x591fe0) turns
            // about the item's top-left corner (0x4b0490), which is the body's centre. The x offset (cg_hudCompassSize - 1) * 0xf5bcc8 * 0.7
            // is 0 at the default size 1. The centity needs eFlags & 0x10000 (set at vehicle spawn for info type 1, "tank", see
            // World::playerVehicleHudYaws). The quad draw 0x617b68 queues render command 0x12 (cgame 0x10021940, handler
            // 0x1002f280): its four corners get texcoords (0,0) (1,0) (1,1) (0,1) in the order given.
            if (const ui::Menu* tm = hudMenus.find("tankstatus"); tm && world) {
                float bodyYaw = 0, barrelYaw = 0;
                bool hasBarrel = false;
                if (!(world->playerPmFlags() & 0x80000) && world->playerVehicleHudYaws(bodyYaw, barrelYaw, hasBarrel))
                    for (const auto& itp : tm->items) {
                        if (itp->ownerDraw != 90 && itp->ownerDraw != 91) continue;
                        if (itp->ownerDraw == 91 && !hasBarrel) continue;
                        const auto& r = itp->rect;
                        float dlt = c.yaw - (itp->ownerDraw == 90 ? bodyYaw : barrelYaw);
                        while (dlt > 180.0f) dlt -= 360.0f;
                        while (dlt < -180.0f) dlt += 360.0f;
                        const float x = (tm->rect.x + r.x) * sc, y = float(h) + (tm->rect.y + r.y) * sc, rw = r.w * sc, rh = r.h * sc;
                        Quad2D q{x, y, x + rw, y + rh};
                        for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(itp->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                        q.texture = materialTexture(itp->background);
                        if (itp->ownerDraw == 90) {
                            q.rotDeg = dlt;
                        } else {
                            const float px[4] = {rw * 0.5f, -rw * 0.5f, -rw * 0.5f, rw * 0.5f};
                            const float py[4] = {-rh * 0.75f, -rh * 0.75f, rh * 0.25f, rh * 0.25f};
                            const float ang = dlt * float(M_PI) / 180.0f, cs = std::cos(ang), sn = std::sin(ang);
                            q.free = true;
                            for (int k = 0; k < 4; ++k) {
                                q.qx[k] = cs * px[k] - sn * py[k] + x;
                                q.qy[k] = sn * px[k] + cs * py[k] + y;
                            }
                        }
                        out.push_back(q);
                    }
            }
            // Compass (ownerdraw 85/84/86, exe 4a91f0 / 4a90b0 / 4a9310). LEFT/BOTTOM alignment is relative to the window
            // edges (0x440e60), like the ammo counter. The compass fade (0x4a90f3, 0x4a9375: hud_fade_compass default 8 s, touch
            // 0xf79a38, CG_FadeColor 0x4b0900 with a 700 ms span) is written into the item colour alpha; the touches are spawn
            // (0x4a6220), the console command → 0x4a6130 case 2 and the objective ring below.
            // Objective ring pulse touch (0x4a9775..0x4a97b8): while a pointer's ring is active (cg_hudObjectiveNumRings 10 > 0,
            // cg_hudObjectiveRingTime 10000 > 0, start >= 0, start + time > now) the compass is touched every frame.
            constexpr int ringNum = 10, ringTime = 10000;
            auto ringElapsed = [&](const game::Objective& ob) -> int64_t {
                if (!world || ob.state != 4 || ob.ringStartMs < 0 || ob.ringStartMs + ringTime <= cgNowMs()) return -1;
                return cgNowMs() - ob.ringStartMs;
            };
            if (world)
                for (const auto& ob : world->objectives())
                    if (ringElapsed(ob) >= 0 && ob.positions[0] != std::array<float, 3>{0, 0, 0}) hudFade.compass = cgNowMs();
            const float compassAlpha = fadeAlpha(hudFade.compass, game::hudFadeDefaultSeconds(game::HudFadeDvar::Compass, mpHud), 8000);
            if (compassMenu && world && compassAlpha > 0.0f) {
                // Compass dvars from the world's dvar table (maps set cg_hudCompassMaxRange with setsavedcvar: demolition 700/400,
                // downtown_assault 1000, moscow 900, libya 7000), else the exe defaults: cg_hudCompassSize 1 (0x1040c38),
                // MaxRange 1500 (0xf48b54), MinRange 0 (0xf569dc), MinRadius 0 (0x1040b8c), SoundPingFadeTime 2 s (0xf48ce4).
                auto compassDvar = [&](const char* n, float def) {
                    const auto it = world->cvars.find(n);
                    return it == world->cvars.end() || it->second.empty() ? def : float(std::atof(it->second.c_str()));
                };
                const float csize = compassDvar("cg_hudCompassSize", 1.0f);
                const float cMaxRange = compassDvar("cg_hudCompassMaxRange", 1500.0f), cMinRange = compassDvar("cg_hudCompassMinRange", 0.0f);
                const float cMinRadius = compassDvar("cg_hudCompassMinRadius", 0.0f), pingFade = compassDvar("cg_hudCompassSoundPingFadeTime", 2.0f);
                // Radius factor of an actor / tank at horizontal distance d (0x4c1148..0x4c11e4): d clamped to MaxRange (beyond it
                // on the rim, no culling), t = (d - MinRange) / (MaxRange - MinRange), r = (1 - MinRadius) * t + MinRadius.
                auto compassRadius = [&](float d) {
                    const float t = (std::min(d, cMaxRange) - cMinRange) / (cMaxRange - cMinRange);
                    return (1.0f - cMinRadius) * t + cMinRadius;
                };
                const float cyaw = c.yaw, north = world->northYaw();
                auto wrap360 = [](float a) { a = std::fmod(a, 360.0f); return a < 0 ? a + 360.0f : a; };
                const auto& mr = compassMenu->rect;
                for (const auto& itp : compassMenu->items) {
                    const ui::Item& it = *itp;
                    if (it.ownerDraw != 84 && it.ownerDraw != 85 && it.ownerDraw != 86 && it.ownerDraw != 87 && it.ownerDraw != 88) continue;
                    const float rx = mr.x + it.rect.x, ry = mr.y + it.rect.y, rw = it.rect.w, rh = it.rect.h;
                    const float bx = rx * sc;                                  // LEFT: window left edge
                    const float by = float(h) + (ry - (csize - 1.0f) * rh) * sc; // BOTTOM: window bottom edge
                    const float qw = rw * csize * sc, qh = rh * csize * sc;
                    uint8_t col[4];
                    for (int k = 0; k < 4; ++k) col[k] = uint8_t(std::clamp(it.foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                    col[3] = uint8_t(std::clamp(compassAlpha, 0.0f, 1.0f) * 255.0f);
                    if (it.ownerDraw == 87 || it.ownerDraw == 88) {
                        // Friendly actors (4c0f20): R = size * r * 43.75 (double @0x591e90), icon size*10 (0x591e8c), r from
                        // compassRadius; placed at the compass centre - icon/2 - (sin, cos)(th) * R with th = yaw to the actor -
                        // view yaw; the arrow turns by (view yaw - actor yaw). 88 tanks (4c1700): the same formulas over the vehicles
                        // with addvehicletocompass, icon 16 (0x591e84). Entry age: an entry is drawn while its stamp >= now - 800 ms (0x4c10e8;
                        // refreshed each frame for snapshot actors, so only a removed actor would linger 800 ms: not implemented). The springy
                        // yaw (cg_hudCompassSpringyPointers, 0xf569ac, default 0) swaps the view yaw for the 0x4b0ce0 spring state 0xf79a30;
                        // implemented below as pyaw.
                        if (it.background.empty()) continue;
                        const bool tanks = it.ownerDraw == 88;
                        // 0x4b0ce0 (run at 4c1056): with cg_hudCompassSpringyPointers set the placement angle is the spring state
                        // (0xf79a30) fed by the view yaw instead of the raw yaw; the arrow rotation (4c1265) keeps the raw yaw.
                        // The spring is the 0x4b09a0 algorithm (5 ms steps, 500 ms reset) with its own angle/velocity; the exe shares
                        // the time word 0xf79a20 between both springs (not reproduced).
                        const float pyaw = std::atof(menuEnv.getCvar("cg_hudCompassSpringyPointers").c_str()) != 0.0 ? pointerSpring.update(int(cgNowMs()), cyaw) : cyaw;
                        const float ccx = bx + qw * 0.5f, ccy = by + qh * 0.5f, icon = (tanks ? 16.0f : 10.0f) * csize * sc;
                        auto place = [&](float px, float py, float& ix, float& iy) {
                            const float dx = px - c.pos.x, dy = py - c.pos.y;
                            const float th = wrap360(std::atan2(dy, dx) * 180.0f / float(M_PI) - pyaw) * 0.0174532925f;
                            const float rad = csize * compassRadius(std::sqrt(dx * dx + dy * dy)) * 43.75f * sc;
                            ix = ccx - icon * 0.5f - std::sin(th) * rad, iy = ccy - icon * 0.5f - std::cos(th) * rad;
                        };
                        const int tex = materialTexture(it.background);
                        const int64_t now = cgNowMs();
                        for (const auto& m : tanks ? world->compassVehicles() : world->compassFriendlies()) {
                            float ix, iy;
                            place(m.pos[0], m.pos[1], ix, iy);
                            // Friendly sound ping (0x4c1279..0x4c1371): while active, alpha = min(ping alpha, compass alpha); at full
                            // ping alpha the arrow itself is skipped, otherwise drawn first; then compassping_friendlyfiring
                            // (0xf5bca0) in the same rotated rect with the item colour and the ping alpha. Tanks (4c1700) have none.
                            const float pa = tanks ? -1.0f : game::compassPingAlpha(m.pingEndMs, now, pingFade);
                            const float pingA = std::min(pa, compassAlpha);
                            const float rot = wrap360(cyaw - m.yaw);
                            if (pa < 0.0f || pingA != 1.0f) {
                                Quad2D q{ix, iy, ix + icon, iy + icon};
                                std::memcpy(q.rgba, col, 4);
                                q.texture = tex;
                                q.rotDeg = rot;
                                out.push_back(q);
                            }
                            if (pa >= 0.0f) {
                                Quad2D q{ix, iy, ix + icon, iy + icon};
                                std::memcpy(q.rgba, col, 4);
                                q.rgba[3] = uint8_t(std::clamp(pingA, 0.0f, 1.0f) * 255.0f);
                                q.texture = materialTexture("compassping_friendlyfiring");
                                q.rotDeg = rot;
                                out.push_back(q);
                            }
                        }
                        // Enemy sound pings (second pass 0x4c1480..0x4c16e6, ownerdraw 87 only): every entry with the enemy byte and an
                        // active ping, at the position stored by the shot, icon size*10, alpha = min(ping alpha, compass alpha), item
                        // colour, material compassping_enemyfiring (0xf5bca8, a red dot), drawn unrotated (0x4d4790).
                        if (!tanks)
                            for (const auto& p : world->compassPings().activeEnemyPings(now, pingFade)) {
                                float ix, iy;
                                place(p.pos[0], p.pos[1], ix, iy);
                                Quad2D q{ix, iy, ix + icon, iy + icon};
                                std::memcpy(q.rgba, col, 4);
                                const float ea = std::min(game::compassPingAlpha(p.endMs, now, pingFade), compassAlpha);
                                q.rgba[3] = uint8_t(std::clamp(ea, 0.0f, 1.0f) * 255.0f);
                                q.texture = materialTexture("compassping_enemyfiring");
                                out.push_back(q);
                            }
                        continue;
                    }
                    if (it.ownerDraw != 86) {
                        if (it.background.empty()) continue;
                        Quad2D q{bx, by, bx + qw, by + qh};
                        std::memcpy(q.rgba, col, 4);
                        q.texture = materialTexture(it.background);
                        if (it.ownerDraw == 84) q.rotDeg = compassSpring.update(int(cgNowMs()), wrap360(cyaw - north)); // 0x4b09a0
                        out.push_back(q);
                        continue;
                    }
                    // Pointers: centre (x + w*size/2, y + h*size/2 - (size-1)*h); K = 43.75 (double @0x591e90); icon 16*size.
                    const float ccx = bx + qw * 0.5f, ccy = by + qh * 0.5f;
                    const float maxR = cMaxRange, minR = cMinRange, minRad = cMinRadius;
                    const float objMax = compassDvar("cg_hudObjectiveMaxRange", 2048.0f), minAlpha = compassDvar("cg_hudObjectiveMinAlpha", 1.0f);
                    const float icon = 16.0f * csize * sc;
                    for (const auto& ob : world->objectives()) {
                        // An objective without an icon uses the default material "objective" (0xf5b6cc, registered at 0x4a2d62
                        // from the string 0x57e3f4; used at 0x4a96da when the icon lookup 0x4a8fc0 fails): the star on the compass.
                        if (ob.state != 4) continue;
                        for (const auto& p : ob.positions) {
                            if (p[0] == 0 && p[1] == 0 && p[2] == 0) continue;
                            const float dx = p[0] - c.pos.x, dy = p[1] - c.pos.y, d = std::sqrt(dx * dx + dy * dy);
                            const float a = wrap360(std::atan2(dy, dx) * 180.0f / float(M_PI) - cyaw) * 0.0174532925f;
                            // Pointer alpha (0x4a953f..0x4a958c): d clamped to at most objMax then at least maxR; 0 when the two
                            // are equal. The drawn alpha is the smaller of it and the compass fade (0x4a96e0), not the product.
                            float alpha = 0.0f;
                            if (objMax != maxR) {
                                const float dc = std::max(std::min(d, objMax), maxR);
                                alpha = (minAlpha - 1.0f) * ((dc - maxR) / (objMax - maxR)) + 1.0f;
                            }
                            const float ptrAlpha = std::min(compassAlpha, alpha);
                            const float t = (std::clamp(d, minR, maxR) - minR) / (maxR - minR);
                            const float rad = csize * ((1.0f - minRad) * t + minRad) * 43.75f * sc;
                            const float ix = ccx - icon * 0.5f - std::sin(a) * rad, iy = ccy - icon * 0.5f - std::cos(a) * rad;
                            Quad2D q{ix, iy, ix + icon, iy + icon};
                            std::memcpy(q.rgba, col, 4);
                            q.rgba[3] = uint8_t(std::clamp(ptrAlpha, 0.0f, 1.0f) * 255.0f);
                            q.texture = materialTexture(ob.icon.empty() ? std::string("objective") : ob.icon);
                            out.push_back(q);
                            // Ring pulse (0x4a9748..0x4a9915): period = RingTime / NumRings; phase = (elapsed mod period) / period;
                            // alpha = triangle(phase) clamped to the pointer alpha; size = phase * cg_hudObjectiveRingSize (64) *
                            // compass size, halved once a full period has passed; material "objective_ring", or the icon when the
                            // objective has one.
                            if (const int64_t el = ringElapsed(ob); el >= 0) {
                                const float period = float(ringTime) / float(ringNum);
                                float rem = float(el);
                                bool later = false;
                                while (rem > period) { rem -= period; later = true; }
                                const float ph = rem / period;
                                const float tri = ph > 0.5f ? 1.0f - (ph - 0.5f) * 2.0f : ph * 2.0f;
                                float rs = ph * 64.0f * (later ? 0.5f : 1.0f) * csize * sc;
                                const float rx2 = ccx - rs * 0.5f - std::sin(a) * rad, ry2 = ccy - rs * 0.5f - std::cos(a) * rad;
                                Quad2D rq{rx2, ry2, rx2 + rs, ry2 + rs};
                                std::memcpy(rq.rgba, col, 4);
                                rq.rgba[3] = uint8_t(std::clamp(std::min(tri, compassAlpha) /* 0x4a981f: the ring is limited by the fade only */, 0.0f, 1.0f) * 255.0f);
                                rq.texture = materialTexture(ob.icon.empty() ? std::string("objective_ring") : ob.icon);
                                out.push_back(rq);
                            }
                        }
                    }
                }
            }
            // The fade to black belongs to the cgame's 2D pass (CG_DrawActive 0x4bfaa4: 0x4bda70 after the HUD); the UI menus are drawn
            // after the cgame (the victory screen over the level-end fade), so with menus open it goes below them.
            bool fadeDrawn = false;
            auto drawScreenFade = [&] {
                screenFade.update(cgNowMs(), int64_t(SDL_GetTicks()));
                if (screenFade.alpha() > 0) {
                    Quad2D q{0, 0, float(w), float(h)};
                    q.rgba[0] = q.rgba[1] = q.rgba[2] = 0;
                    q.rgba[3] = uint8_t(std::min(screenFade.alpha(), 1.0f) * 255.0f + 0.5f);
                    out.push_back(q);
                }
                fadeDrawn = true;
            };
            if (!frontStack.empty()) {
                out.insert(out.end(), front.begin(), front.end());
                front.clear();
                drawScreenFade();
            }
            if (!frontStack.empty()) {
                // Menus on the stack (front end and script menus), items laid out relative to the menu rect in the 640x480 virtual
                // screen. Window style 1 (FILLED) draws backColor, 3 (SHADER) the background material, 6 (DVAR_SHADER) the material
                // named by the item's dvar; text items draw their (localized) text, the focused item in the menu's focusColor.
                // TODO(spec): [generic Item_Paint order not walked] item/menu alignment, draw order (the font is menuFont, exe 0x4c38e0).
                auto col8 = [](const ui::Color& c) { return std::array<uint8_t, 4>{uint8_t(std::clamp(c[0], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[1], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[2], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[3], 0.0f, 1.0f) * 255)}; };
                std::vector<const ui::Menu*> order;
                // No forced background: the menu "bg" is an ordinary menu that "main" opens and closes in its own onOpen / onClose
                // (docs/spec/ui.md "Background menu"), so in-game script menus draw over the world only.
                for (const ui::Menu* m : frontStack.menus()) order.push_back(m);
                for (const ui::Menu* mp : order) {
                    const ui::Menu& menu = *mp;
                    // HORIZONTAL_ALIGN_FULLSCREEN (4) menus span the whole window; the others the centred 4:3 area.
                    const bool full = menu.rect.horzAlign == 4;
                    const float xs = full ? float(w) / 640.0f : sc, x0 = full ? 0.0f : left43;
                    auto quad = [&](float rx, float ry, float rw, float rh, const std::array<uint8_t, 4>& c, const std::string& mat) {
                        Quad2D q{x0 + rx * xs, ry * sc, x0 + (rx + rw) * xs, (ry + rh) * sc};
                        std::memcpy(q.rgba, c.data(), 4);
                        q.texture = mat.empty() ? 0 : materialTexture(mat);
                        out.push_back(q);
                    };
                    // Window_Paint FILLED (MP 0x539f9c..0x53a081): with a background material that material is drawn in the back
                    // colour (the MP load screen "connect": $levelBriefing), else the back colour fills the rect.
                    if (menu.style == 1) quad(menu.rect.x, menu.rect.y, menu.rect.w, menu.rect.h, col8(menu.backColor), menu.background);
                    else if (menu.style == 3 && !menu.background.empty()) quad(menu.rect.x, menu.rect.y, menu.rect.w, menu.rect.h, col8(menu.foreColor), menu.background);
                    for (const auto& itp : menu.items) {
                        const ui::Item& it = *itp;
                        if (!frontStack.itemVisible(it)) continue;
                        const float ix = menu.rect.x + it.rect.x, iy = menu.rect.y + it.rect.y;
                        // Window_Paint FILLED (0x4c725c): with a background material the back colour's alpha fades (fadein / fadeout).
                        if (it.style == 1 && !it.background.empty()) frontStack.paintFade(menu, it, true);
                        if (it.style == 1 && !frontStack.itemVisible(it)) continue; // the fade-out just ended
                        // An item rect with HORIZONTAL_ALIGN_FULLSCREEN (4) spans the whole window like such a menu (victoryscreen, briefing bars).
                        auto itemQuad = [&](float rx, float ry, float rw, float rh, const std::array<uint8_t, 4>& c, const std::string& mat) {
                            if (it.rect.horzAlign != 4 || full) return quad(rx, ry, rw, rh, c, mat);
                            const float fx = float(w) / 640.0f, lx = rx - menu.rect.x; // the item's own x, in full-width units
                            Quad2D q{lx * fx, ry * sc, (lx + rw) * fx, (ry + rh) * sc};
                            std::memcpy(q.rgba, c.data(), 4);
                            q.texture = mat.empty() ? 0 : materialTexture(mat);
                            out.push_back(q);
                        };
                        if (it.style == 1) itemQuad(ix, iy, it.rect.w, it.rect.h, col8(frontStack.backColor(it)), "");
                        else if (it.style == 3 || it.style == 6) {
                            std::string mat = it.background;
                            if (it.style == 6 || (mat.empty() && !it.dvar.empty())) mat = menuEnv.getCvar(it.dvar);
                            if (!mat.empty()) itemQuad(ix, iy, it.rect.w, it.rect.h, col8(frontStack.foreColor(it)), mat);
                        }
                        for (const ui::BorderBar& b : ui::borderBars(it.border, ix, iy, it.rect.w, it.rect.h, it.borderSize))
                            itemQuad(b.x, b.y, b.w, b.h, col8(frontStack.borderColor(it)), "");
                        if (it.type == 6) { // LISTBOX paint, exe 0x4ce280 (vertical, text columns)
                            const assets::Font* lf = menuFont(it.textScale, it.textFont);
                            const auto white = std::array<uint8_t, 4>{255, 255, 255, 255};
                            const float xa = ix + it.rect.w - 17.0f;
                            quad(xa, iy + 1.0f, 16.0f, 16.0f, white, "ui/assets/scrollbar_arrow_up_a.tga");
                            quad(xa, iy + 16.0f, 16.0f, it.rect.h - 31.0f, white, "ui/assets/scrollbar.tga");
                            quad(xa, iy + it.rect.h - 16.0f, 16.0f, 16.0f, white, "ui/assets/scrollbar_arrow_dwn_a.tga");
                            quad(xa, std::min(frontStack.listThumbY(menu, it), iy + it.rect.h - 17.0f), 16.0f, 16.0f, white, "ui/assets/scrollbar_thumb.tga");
                            const auto lay = frontStack.listLayout(it);
                            const int cursor = frontStack.listState(it).cursor;
                            const int total = menuEnv.feederCount ? menuEnv.feederCount(it.feeder) : 0;
                            for (int r = 0; r < lay.rows && lay.first + r < total; ++r) {
                                const float rowTop = iy + 1.0f + float(r) * it.elementHeight;
                                if (lay.first + r == cursor)
                                    quad(ix + 3.0f, rowTop + 2.0f, it.rect.w - 16.0f - 4.0f, it.elementHeight, col8(it.outlineColor), "white");
                                if (!lf) continue;
                                const float lgs = it.textScale * 48.0f / float(lf->pixelHeight);
                                auto lc = col8(it.foreColor);
                                const size_t ncol = std::max<size_t>(1, it.columns.size());
                                for (size_t ci = 0; ci < ncol; ++ci) {
                                    std::string cell = menuEnv.feederText(it.feeder, lay.first + r, int(ci));
                                    const int maxc = it.columns.empty() ? 0 : it.columns[ci].maxChars;
                                    if (maxc > 0 && cell.size() > size_t(maxc)) cell.resize(size_t(maxc));
                                    const float cx = ix + 1.0f + (it.columns.empty() ? 0.0f : it.columns[ci].pos) + 4.0f;
                                    if (!cell.empty() && !(ci < it.columns.size() && it.columns[ci].hidden))
                                        hudText(out, cell, x0 + cx * xs, (rowTop + it.elementHeight) * sc, lgs * sc, *lf, lc.data(), 0.0f, 1.0f, it.textStyle);
                                }
                            }
                            continue;
                        }
                        // UI_SAVEGAME_SHOT (258, 0x4c3c00): the material named by the selected entry's image (+8 = header +0x320,
                        // script default "$default"); no selection, an empty name or a material that does not load -> "unknownsave".
                        // Drawn over the item rect (0x4d4790). TODO(spec): the colour argument of the ownerdraw call (item fore colour here).
                        // UI_STARTMAPCINEMATIC (255) on the create-server screen: the selected map's load screen image (arena entry +8,
                        // "loadscreen_<map>"). TODO(spec): the ownerdraw's cinematic path; only the still image is drawn.
                        if (it.ownerDraw == ui::kOwnerDrawNetMapCinematic && mpFrontEnd) { // the selected server's map (MP UI_NETMAPCINEMATIC)
                            const net::LanServer* srv = lanBrowser.rowServer(lanBrowser.selected);
                            const std::string mat = srv && !srv->map().empty() ? "loadscreen_" + srv->map() : std::string();
                            if (!mat.empty() && vfs.exists("materials/" + mat)) quad(ix, iy, it.rect.w, it.rect.h, std::array<uint8_t, 4>{255, 255, 255, 255}, mat);
                            continue;
                        }
                        if (it.ownerDraw == ui::MpServerSetup::kOwnerDrawStartMapCinematic && mpFrontEnd) {
                            const std::string mat = mpSetup.loadScreenOfCurrent(menuEnv.getCvar);
                            if (!mat.empty() && vfs.exists("materials/" + mat)) quad(ix, iy, it.rect.w, it.rect.h, std::array<uint8_t, 4>{255, 255, 255, 255}, mat);
                            continue;
                        }
                        if (it.ownerDraw == 258) {
                            std::string mat = "unknownsave";
                            if (saveSel >= 0 && size_t(saveSel) < saveRows.size() && !saveRows[size_t(saveSel)].info.image.empty() &&
                                (vfs.exists("materials/" + saveRows[size_t(saveSel)].info.image)))
                                mat = saveRows[size_t(saveSel)].info.image;
                            quad(ix, iy, it.rect.w, it.rect.h, col8(it.foreColor), mat);
                            continue;
                        }
                        // UI_SAVEGAMENAME (262) shows UI state +0x31e, a textsavegame item UI state +0x41e (0x4c6050 fills both).
                        // TODO(spec): [handlers not read] the ownerdraw / textsavegame draw handlers are not decoded; drawn with the item's text settings.
                        const bool saveText = it.ownerDraw == 262 || it.textSaveGame;
                        if (saveText && (it.ownerDraw == 262 ? saveSelName : saveDetail).empty()) continue;
                        // Item_Text_Paint (0x4ccf60): an item without text shows its dvar's value (0x2c0 -> 0x4314b0), localized when it
                        // starts with '@' (0x4c6a20); e.g. victoryscreen's "quote" (dvar ui_victoryquote).
                        const bool dvarText = !saveText && it.text.empty() && !it.dvar.empty() && it.type != 4 && it.type != 9 && it.type != 16 && it.type != 11 &&
                                              it.type != 12 && it.type != 13 && it.type != 14 && it.type != 17;
                        const std::string ownerText = it.type == 0 && it.ownerDraw && mpFrontEnd ? menuEnv.ownerDrawText(it.ownerDraw) : std::string();
                        if (!saveText && !dvarText && ownerText.empty() && it.text.empty() && it.type != 4 && it.type != 9 && it.type != 16 && it.type != 11 && it.type != 12 && it.type != 13 && it.type != 14 && it.type != 17) continue;
                        const std::string dv = dvarText ? menuEnv.getCvar(it.dvar) : std::string();
                        const std::string text = saveText ? (it.ownerDraw == 262 ? saveSelName : saveDetail)
                                                 : !ownerText.empty() ? (it.text.empty() ? std::string() : menuLocalized(it.text) + " ") + menuLocalized(ownerText)
                                                 : dvarText ? (!dv.empty() && dv[0] == '@' ? menuLocalized(dv) : dv) : menuLocalized(it.text);
                        if (dvarText && text.empty()) continue;
                        frontStack.paintFade(menu, it, false); // text fade on the fore colour (0x4cd025 -> 0x4cc450)
                        const float ts = it.textScale;
                        const assets::Font* f = menuFont(ts, it.textFont);
                        if (!f) continue;
                        const float gs = ts * 48.0f / float(f->pixelHeight);
                        auto c = col8(&it == frontStack.focus() ? menu.focusColor : frontStack.foreColor(it));
                        if (it.type == 14 && &it == frontStack.focus()) {
                            c = col8(ui::focusPulseColor(menu.focusColor, &it == frontStack.captureItem(), SDL_GetTicks()));
                        }
                        const float ax = it.textAlign == 1 || it.textAlign == 3 ? 0.5f : it.textAlign == 2 ? 1.0f : 0.0f;
                        if (it.autoWrapped) {
                            // exe 0x4cc950 (item flag 0x800000, keyword "autowrapped"): the text is broken at spaces, tabs and LF so that
                            // each line fits the item rect width; each line is drawn by textalign one line height + 5 lower than the
                            // previous. TODO(spec): [0x4c3790 result is in screen units, conversion unread] the exact break rule and line step of 0x4cc950 (taken from the dead quote's 0x4cc6a0).
                            auto width = [&](const std::string& str) {
                                float wd = 0;
                                for (unsigned char ch : str)
                                    if (const auto* g = f->find(ch)) wd += float(g->dx) * gs;
                                return wd;
                            };
                            std::vector<std::string> lines;
                            std::string line, word;
                            auto flushWord = [&] {
                                if (!line.empty() && width(line + " " + word) > it.rect.w) { lines.push_back(line); line.clear(); }
                                line += (line.empty() ? "" : " ") + word;
                                word.clear();
                            };
                            for (char ch : text) {
                                if (ch == ' ' || ch == '\t') { if (!word.empty()) flushWord(); }
                                else if (ch == '\n') { if (!word.empty()) flushWord(); lines.push_back(line); line.clear(); }
                                else word += ch;
                            }
                            if (!word.empty()) flushWord();
                            if (!line.empty()) lines.push_back(line);
                            float ly = iy + it.textAlignY;
                            for (const auto& l : lines) {
                                hudText(out, l, x0 + (ix + it.textAlignX) * xs, ly * sc, gs * sc, *f, c.data(), ax, 1.0f, it.textStyle);
                                ly += float(int(ts * 48.0f) + 5);
                            }
                        } else if (it.wrapped) {
                            // exe 0x4ccc70 (item flag 0x400000, keyword "wrapped"): split only at CR (0xd), each line drawn by textalign; the
                            // next line starts one line height + 5 lower. TODO(spec): [0x4c3790 result is in screen units, conversion unread] exact line step (the exe adds 5 to the result of 0x4c3790).
                            float ly = iy + it.textAlignY;
                            size_t pos = 0;
                            while (true) {
                                const size_t e = text.find('\r', pos);
                                hudText(out, text.substr(pos, e == std::string::npos ? e : e - pos), x0 + (ix + it.textAlignX) * xs, ly * sc, gs * sc, *f, c.data(), ax, 1.0f, it.textStyle);
                                if (e == std::string::npos) break;
                                pos = e + 1;
                                ly += ts * 48.0f + 5.0f;
                            }
                        } else
                        hudText(out, text, x0 + (ix + it.textAlignX) * xs, (iy + it.textAlignY) * sc, gs * sc, *f, c.data(), ax, 1.0f, it.textStyle);
                        if (it.type == 4 || it.type == 9 || it.type == 16) { // edit field value (exe 0x4cd0e0, the item window is painted above); colour codes: hudText
                            const std::string val = menuEnv.getCvar(it.dvar);
                            const int off = frontStack.editItem() == &it ? frontStack.editOffset() : 0;
                            std::string shown = off < int(val.size()) ? val.substr(size_t(off)) : std::string();
                            if (it.maxPaintChars > 0 && shown.size() > size_t(it.maxPaintChars)) shown.resize(size_t(it.maxPaintChars));
                            const float vx = frontStack.valueTextX(menu, it);
                            auto widthOf = [&](const std::string& str) {
                                float wd = 0;
                                for (size_t k = 0; k < str.size(); ++k) { // "^N" colour codes take no width (renderer 0x100329a0)
                                    if (str[k] == '^' && k + 1 < str.size() && str[k + 1] >= '0' && str[k + 1] <= '9') { ++k; continue; }
                                    if (const auto* g = f->find((unsigned char)str[k])) wd += float(g->dx) * gs;
                                }
                                return wd;
                            };
                            const auto vc = &it == frontStack.focus() ? col8(ui::focusPulseColor(menu.focusColor, false, SDL_GetTicks())) : c;
                            hudText(front, shown, x0 + vx * xs, (iy + it.textAlignY) * sc, gs * sc, *f, vc.data(), 0.0f, 1.0f, it.textStyle);
                            if (frontStack.editItem() == &it) { // cursor glyph: '_' overstrike, '|' insert (drawn over the character)
                                const int rel = std::clamp(frontStack.editCursor() - off, 0, int(shown.size()));
                                hudText(front, frontStack.editOverstrike() ? "_" : "|", x0 + (vx + widthOf(shown.substr(0, size_t(rel)))) * xs,
                                        (iy + it.textAlignY) * sc, gs * sc, *f, c.data(), 0.0f, 1.0f, it.textStyle);
                            }
                        }
                        if (it.type == 14) { // key names (exe 0x4cd980): "k1 KEY_OR k2", KEY_UNBOUND; x after the text + 8 (0x592010)
                            auto name = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
                            const auto keys = input.keysFor(it.dvar);
                            std::string label = name("UNBOUND");
                            if (!keys.empty()) {
                                label = name(keys[0]);
                                if (keys.size() > 1) label += " " + name("OR") + " " + name(keys[1]);
                            }
                            hudText(front, label, x0 + frontStack.valueTextX(menu, it) * xs, (iy + it.textAlignY) * sc, gs * sc, *f, c.data(), 0.0f, 1.0f, it.textStyle);
                        }
                        if (it.type == 11 || it.type == 12 || it.type == 13 || it.type == 17) { // value text, exe 0x4cd3a0 / 0x4cd570: same font, colour and baseline
                            const std::string label = frontStack.valueLabel(it);
                            if (!label.empty())
                                hudText(front, menuLocalized(label), x0 + frontStack.valueTextX(menu, it) * xs,
                                        (iy + it.textAlignY) * sc, gs * sc, *f, c.data(), 0.0f, 1.0f, it.textStyle);
                        }
                    }
                    // Sliders: track ui/assets/slider2 (96x16) and thumb ui/assets/sliderbutt_1 (10x20), exe 0x4cdd20..0x4cded0.
                    for (const auto& itp : menu.items) {
                        const ui::Item& it = *itp;
                        if (it.type != 10 || !it.hasRange || !frontStack.itemVisible(it)) continue;
                        const float tx = frontStack.sliderTrackX(menu, it), ty = menu.rect.y + it.rect.y;
                        const auto white = std::array<uint8_t, 4>{255, 255, 255, 255};
                        quad(tx, ty, 96.0f, 16.0f, white, "ui/assets/slider2.tga");
                        quad(tx + 6.0f + frontStack.sliderFraction(it) * 84.0f - 5.0f, ty - 2.0f, 10.0f, 20.0f, white, "ui/assets/sliderbutt_1");
                    }
                    // MP load screen text (0x5390c0, docs/spec/mp-ui.md "Load screen"): the gametype's display string and the map's long
                    // name, centred at x 320, scale 0.5, white, text style 6, over the connect menu.
                    if (menu.name == "connect" && mpLoadText) {
                        const auto white = std::array<uint8_t, 4>{255, 255, 255, 255};
                        if (const assets::Font* f = menuFont(ui::kMpLoadScreenTextScale, 0)) {
                            const float gs = ui::kMpLoadScreenTextScale * 48.0f / float(f->pixelHeight) * sc;
                            hudText(front, menuLocalized(mpLoadText->gametype), left43 + 320.0f * sc, ui::kMpLoadScreenGametypeY * sc, gs, *f, white.data(), 0.5f, 1.0f, 6);
                            hudText(front, mpLoadText->map, left43 + 320.0f * sc, ui::kMpLoadScreenMapY * sc, gs, *f, white.data(), 0.5f, 1.0f, 6);
                        }
                    }
                }
                {   // the cursor
                    Quad2D q{left43 + (menuMx - 8.0f) * sc, (menuMy - 8.0f) * sc, left43 + (menuMx + 8.0f) * sc, (menuMy + 8.0f) * sc};
                    std::memset(q.rgba, 255, 4);
                    q.texture = materialTexture("ui/assets/3_cursor3");
                    front.push_back(q);
                }
            }
            if (const char* key = std::getenv("IW2_HUD_TEXT")) { // debug: show a localized string
                const std::string* t = loc.find(key);
                const uint8_t yellow[4] = {255, 230, 120, 255};
                if (const assets::Font* f = font("bigfont")) hudText(out, t ? *t : key, float(w) * 0.5f, 80 * sc, 0.8f * sc, *f, yellow, 0.5f, 0);
            }
            // Draw order of CG_DrawActive (0x4bfa70): damage icons, crosshair + scope overlay (0x4bd760 -> 0x4bcdd0), then the hud
            // elements of both layers (0x4c3030), the HUD menus, the subtitles and the rest: the overlay lies below all of them.
            game::insertBelow(out, hudElemStart, scopeQuads);
            // Intermission (pm_type 5): the 2D pass draws only the forced scoreboard (0x4cbca0 -> 0x4cb810); open menus stay.
            if (world && world->multiplayer() && mpIntermissionView) {
                out.clear();
                drawMpScoreboard(out, w, h);
            }
            out.insert(out.end(), front.begin(), front.end());
            // Without menus the fade to black is drawn last, over the HUD (CG_DrawActive 0x4bfaa4: 0x4bda70 after the 2D elements).
            if (!fadeDrawn) drawScreenFade();
            if (showFps)
                if (const assets::Font* f = font("normalFont")) {
                    const float fpsSc = float(h) / 480.0f;
                    const uint8_t yellow[4] = {255, 255, 0, 255};
                    hudText(out, std::to_string(fpsShown) + " FPS", float(w) - 8.0f * fpsSc, 20.0f * fpsSc, 0.3f * 48.0f / float(f->pixelHeight) * fpsSc, *f, yellow, 1.0f, 1.0f, 3);
                }
            (void)c;
            return out;
        };
        Camera cam = scene.start;
        if (pos) cam.pos = *pos;
        if (yaw) cam.yaw = *yaw;
        if (pitch) cam.pitch = *pitch;

        // Debug: IW2_SHOT_ALIASES=<map> loads that map's material aliases (maps/<map>.csv, "$levelBriefing" ...) for a --shot; a level
        // shot loads its own map's.
        if (!shot.empty() && (std::getenv("IW2_SHOT_ALIASES") || !currentMap.empty()))
            loadMapAliases(std::getenv("IW2_SHOT_ALIASES") ? std::string(std::getenv("IW2_SHOT_ALIASES")) : currentMap);
        else if (!currentMap.empty()) loadMapAliases(currentMap); // the first map's load (0x40ce90)
        showNetError(); // a +connect that failed at start-up
        if (!shot.empty() && (menuMode || std::getenv("IW2_SHOT_MENU"))) { // debug: IW2_SHOT_MENU=name[,name...] opens menus (and IW2_SHOT_SET=dvar=value,... sets cvars) before the shot
            if (const char* sets = std::getenv("IW2_SHOT_SET")) {
                std::string rest = sets;
                while (!rest.empty()) {
                    const size_t comma = rest.find(',');
                    const std::string kv = rest.substr(0, comma);
                    rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                    if (const size_t eq = kv.find('='); eq != std::string::npos) menuEnv.setCvar(kv.substr(0, eq), kv.substr(eq + 1));
                }
            }
            std::string rest = std::getenv("IW2_SHOT_MENU") ? std::getenv("IW2_SHOT_MENU") : "main";
            while (!rest.empty()) {
                const size_t comma = rest.find(',');
                const std::string name = rest.substr(0, comma);
                // "connect": the MP load screen of the IW2_SHOT_ALIASES map (mp/<map>) with IW2_MP_GAMETYPE (default dm).
                if (name == "connect") {
                    if (!mpFrontEnd) pendingFrontEnd = true, applyFrontEnd(); // the MP menu set (the SP set has its own "connect")
                    frontStack.closeAll();
                    frontStack.open(name);
                    const std::string m = std::getenv("IW2_SHOT_ALIASES") ? std::getenv("IW2_SHOT_ALIASES") : "";
                    mpLoadText = ui::mpLoadScreenText(vfs, std::getenv("IW2_MP_GAMETYPE") ? std::getenv("IW2_MP_GAMETYPE") : "dm",
                                                      m.rfind("mp/", 0) == 0 ? m.substr(3) : m);
                } else frontStack.open(name);
                rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
            }
            menuMx = 320.0f, menuMy = 240.0f;
            if (const char* keys = std::getenv("IW2_SHOT_KEYS")) { // debug: the IW2_MENU_KEYS tokens (open:, click:X/Y, up, down, enter, esc, left, right) run before the shot
                std::string kr = keys;
                while (!kr.empty()) {
                    const size_t comma = kr.find(',');
                    const std::string k = kr.substr(0, comma);
                    kr = comma == std::string::npos ? std::string() : kr.substr(comma + 1);
                    using K = ui::MenuStack::Key;
                    if (menuExtraToken(k)) applyFrontEnd();
                    else if (k.rfind("open:", 0) == 0) frontStack.open(k.substr(5));
                    else if (k.rfind("click:", 0) == 0) {
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                        frontStack.click(menuMx, menuMy);
                        frontStack.release();
                        applyFrontEnd();
                    } else if (k.rfind("hover:", 0) == 0) {
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                    } else if (k.rfind("bindkey:", 0) == 0) {
                        frontStack.captureKey(k.substr(8));
                    } else frontStack.key(k == "up" ? K::Up : k == "down" ? K::Down : k == "esc" ? K::Escape : k == "left" ? K::Left : k == "right" ? K::Right : K::Enter);
                }
            }
            frontStack.settleFades(); // a single-frame shot shows the menus after their fades
        }
        if (!shot.empty()) {
            if (walkSeconds > 0) {
                // Drop the player at the camera position and walk forward, then take the shot from the eyes.
                const game::MoveParams mp;
                game::PlayerState ps;
                ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - ps.viewHeight + 8.0f};
                game::PlayerInput in;
                in.yaw = cam.yaw;
                for (int i = 0; i < 20; ++i) game::playerMove(*scene.collision, ps, in, 0.05f, mp);
                in.forward = 1;
                for (int i = 0; i < int(walkSeconds * 60); ++i) game::playerMove(*scene.collision, ps, in, 1.0f / 60.0f, mp);
                cam.pos = {ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeight};
                std::cout << "walked to " << cam.pos.x << ", " << cam.pos.y << ", " << cam.pos.z << "\n";
            }
            if (world) {
                std::array<game::Vec3, 3> rPrev{};
                bool rHave = false, rInit = false;
                struct ShotMsg { std::string text; int64_t at; bool bold; };
                std::vector<ShotMsg> shotMsgs; // game messages with their world time
                std::vector<std::pair<game::MpObituary, int64_t>> shotObits; // MP obituaries with their world time
                bool killedVehicles = false;
                for (int f = 0; f < int(simSeconds * 20); ++f) {
                    // Debug: IW2_DEBUG_ITEM=name[@seconds] drops a weapon item on the player (touch pickup, owned or not).
                    if (const char* di = std::getenv("IW2_DEBUG_ITEM")) {
                        std::string spec = di;
                        const auto at = spec.find('@');
                        const float when = at == std::string::npos ? 0.0f : std::stof(spec.substr(at + 1));
                        if (f == int(when * 20) && world->player()) {
                            for (auto& kv : world->inventory(*world->player()).ammo) kv.second = 0; // so the item has room
                            for (auto& kv : world->inventory(*world->player()).clip) kv.second = 0;
                            world->debugSpawnWeaponItem(spec.substr(0, at));
                            std::cout << "debug-item " << spec.substr(0, at) << " at frame " << f << "\n";
                        }
                        if (f == int(when * 20) + 20 && world->player()) {
                            const game::Inventory& inv = world->inventory(*world->player());
                            std::cout << "debug-item owned:";
                            for (const auto& o : inv.owned) std::cout << " " << o;
                            std::cout << " | slots " << inv.slot[1] << "," << inv.slot[2];
                            for (const auto& [a, n] : inv.ammo) std::cout << " ammo[" << a << "]=" << n;
                            for (const auto& [a, n] : inv.clip) std::cout << " clip[" << a << "]=" << n;
                            int items = 0;
                            for (const auto& e : world->entities())
                                if (e->classname.rfind("weapon_", 0) == 0 && !e->obj->removed) ++items;
                            std::cout << " items-left " << items << "\n";
                        }
                    }
                    const bool rideNow = (world->playerPmFlags() & 0x20000) != 0;
                    if (!world->playerLinked() && !rideNow) world->setPlayerOrigin({cam.pos.x, cam.pos.y, cam.pos.z - 60.0f}, {0, cam.yaw, 0});
                    {
                        // The view angles feed the crosshair target trace (0x535070) as in play.
                        game::World::PlayerCommand pc;
                        pc.viewAngles = {cam.pitch, cam.yaw, 0.0f};
                        if (rideNow && rideDrive && float(f) / 20.0f >= rideAfter) {
                            pc.forward = int(rideDrive->at(0) * 127.0f);
                            pc.right = int(rideDrive->at(1) * 127.0f);
                            pc.buttons = rideButtons;
                        }
                        world->setPlayerCommand(pc);
                    }
                    // Debug: IW2_FIRE_VEHICLES=1 sends the script notify "turret_fire" (the AI tank fire loop) to every vehicle in the
                    // last 3 simulation frames, so their muzzle flashes are in the shot.
                    if (std::getenv("IW2_FIRE_VEHICLES") && f >= int(simSeconds * 20) - 3)
                        for (const auto& ve : world->entities())
                            if (ve->vehicle && !ve->obj->removed) world->vm().notify(ve->obj, "turret_fire", {});
                    if (const char* kv = std::getenv("IW2_KILL_VEHICLES"); kv && !killedVehicles && float(f) / 20.0f >= float(std::atof(kv))) {
                        // Debug: IW2_KILL_VEHICLES=<s> deals 100000 damage (an explosion without attacker) to every vehicle at sim time s.
                        killedVehicles = true;
                        for (const auto& ve : world->entities())
                            if (ve->vehicle && !ve->obj->removed) world->debugDamage(*ve, 100000);
                    }
                    if (const char* ft = std::getenv("IW2_FIRE_TURRETS"); ft && f >= int(simSeconds * 20) - 3) {
                        // Debug: IW2_FIRE_TURRETS=N fires every turret entity in the last 3 frames and puts the camera 150 units
                        // beside and 60 above the Nth one (muzzle flashes in the shot).
                        world->debugFireTurrets();
                    }
                    // Debug: IW2_SHOT_MP_RESPONSES=t:menu:response,... sends the local client's menu responses at sim time t (MP);
                    // a menu "-" runs the bound command text instead (e.g. 1:-:openscriptmenu ingame changeweapon, 2:-:togglemenu).
                    if (const char* mr = std::getenv("IW2_SHOT_MP_RESPONSES"); mr && world->multiplayer()) {
                        std::string rest = mr;
                        while (!rest.empty()) {
                            const size_t comma = rest.find(',');
                            const std::string e = rest.substr(0, comma);
                            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                            const size_t c1 = e.find(':'), c2 = c1 == std::string::npos ? c1 : e.find(':', c1 + 1);
                            if (c2 == std::string::npos || f != int(std::atof(e.substr(0, c1).c_str()) * 20)) continue;
                            const std::string menu = e.substr(c1 + 1, c2 - c1 - 1), resp = e.substr(c2 + 1);
                            if (menu == "-") {
                                input.command(resp);
                                runConsoleCommands();
                            } else world->mpMenuResponse(world->mpLocalClient(), menu, resp);
                        }
                    }
                    // Debug: IW2_SHOT_MP_KILL=t:victim:attacker[:mod],... damages client victim by 1000 from client attacker (-1 the world)
                    // through the MP damage path at sim time t (kill feed / centre print shots).
                    if (const char* mk = std::getenv("IW2_SHOT_MP_KILL"); mk && world->multiplayer()) {
                        std::istringstream all(mk);
                        for (std::string e; std::getline(all, e, ',');) {
                            std::vector<std::string> p;
                            std::istringstream one(e);
                            for (std::string x; std::getline(one, x, ':');) p.push_back(x);
                            if (p.size() < 3 || f != int(std::atof(p[0].c_str()) * 20)) continue;
                            world->mpDebugDamage(std::atoi(p[1].c_str()), std::atoi(p[2].c_str()), 1000, p.size() > 3 ? p[3] : "MOD_RIFLE_BULLET");
                        }
                    }
                    // Debug: IW2_SHOT_MP_THROW=t:client[:weapon[:view]],... client throws a grenade (default frag_grenade_german_mp) from
                    // its eye toward the local client (with view: along its own facing) at sim time t (killcam grenade shots, docs/spec/mp-hud.md).
                    if (const char* mt = std::getenv("IW2_SHOT_MP_THROW"); mt && world->multiplayer()) {
                        std::istringstream all(mt);
                        for (std::string e; std::getline(all, e, ',');) {
                            std::vector<std::string> p;
                            std::istringstream one(e);
                            for (std::string x; std::getline(one, x, ':');) p.push_back(x);
                            if (p.size() < 2 || f != int(std::atof(p[0].c_str()) * 20)) continue;
                            game::Entity* thrower = world->mpClientEntity(std::atoi(p[1].c_str()));
                            game::Entity* target = world->mpClientEntity(world->mpLocalClient());
                            if (!thrower || !target) continue;
                            const auto o = thrower->obj->get("origin").asVector(), t = target->obj->get("origin").asVector();
                            const game::Vec3 eye{o[0], o[1], o[2] + 60.0f};
                            // "view": along the thrower's own facing (what its killcam view shows), else toward the local client.
                            const bool view = p.size() > 3 && p[3] == "view";
                            const auto ta = thrower->obj->get("angles");
                            const float yaw = view && ta.isVector() ? ta.asVector()[1] : std::atan2(t[1] - o[1], t[0] - o[0]) * 57.29578f;
                            world->clientThrowGrenade(*thrower, p.size() > 2 && !p[2].empty() ? p[2] : "frag_grenade_german_mp", eye, {-25.0f, yaw, 0.0f}, {0, 0, 0});
                            std::cout << "shot: client " << p[1] << " throws a grenade at yaw " << yaw << "\n";
                        }
                    }
                    // Debug: IW2_SHOT_MP_FOLLOW=t:dir,... runs the local spectator's follow cycle (dir 1 / -1, as an attack / 0x1000
                    // press) at sim time t; dir 0 stops following (docs/spec/mp-spectate.md).
                    if (const char* mf = std::getenv("IW2_SHOT_MP_FOLLOW"); mf && world->multiplayer()) {
                        std::istringstream all(mf);
                        for (std::string e; std::getline(all, e, ',');) {
                            const size_t c1 = e.find(':');
                            if (c1 == std::string::npos || f != int(std::atof(e.substr(0, c1).c_str()) * 20)) continue;
                            const int dir = std::atoi(e.substr(c1 + 1).c_str());
                            if (dir == 0) world->mpStopFollow(world->mpLocalClient());
                            else std::cout << "shot: follow cycle " << dir << " -> " << (world->mpFollowCycle(world->mpLocalClient(), dir) ? world->mpFollowing(world->mpLocalClient()) : -1) << "\n";
                        }
                    }
                    // Debug: IW2_SHOT_LEVEL_CALL=<s>:file.gsc:function starts that function as a thread on level at sim time <s>
                    // (e.g. 2:maps/_endmission.gsc:nextmission for the victory screen).
                    if (const char* lc = std::getenv("IW2_SHOT_LEVEL_CALL")) {
                        const std::string spec = lc;
                        const size_t c1 = spec.find(':'), c2 = spec.rfind(':');
                        if (c1 != std::string::npos && c2 > c1 && f == int(std::atof(spec.substr(0, c1).c_str()) * 20))
                            world->vm().start(spec.substr(c1 + 1, c2 - c1 - 1), spec.substr(c2 + 1), {}, script::Value(world->vm().level()));
                    }
                    world->frame();
                    if (auto change = world->takeDueLevelChange()) { // a --shot stops at a level change; the victory screen is shown
                        std::cout << "shot: level change to " << change->map << (change->victoryScreen ? " (victory screen)" : "") << " at "
                                  << cgNowMs() << " ms\n";
                        if (change->victoryScreen) {
                            for (const auto& cmd : world->takeClientCommands()) if (cmd.name == "openvictoryscreen") break;
                            menuMx = 320.0f, menuMy = 448.0f;
                            frontStack.open("victoryscreen");
                        }
                        break;
                    }
                    // This server frame's effect events are played now and the effects updated at the frame time, as the client
                    // does per frame (before, every event of the simulation was played at the shot time, all at age 0).
                    fxSystem.viewOrigin = {cam.pos.x, cam.pos.y, cam.pos.z};
                    fxSystem.viewValid = true;
                    setFxFrustum(fxSystem, cam, float(width) / float(height));
                    playWorldFx();
                    (void)fxSystem.sprites(cgNowMs());
                    if ((world->playerPmFlags() & 0x20000) && rideDrive) { // the vehicle view as in play (cgame 0x4aeb60): pitch follows tag_body, yaw while pm_flags 0x40000
                        std::array<game::Vec3, 3> body;
                        if (world->playerVehicleBodyAxis(body)) {
                            if (!rInit) { cam.yaw = std::atan2(body[0][1], body[0][0]) * 180.0f / float(M_PI); cam.pitch = 0; rInit = true; }
                            if (!rHave) rPrev = body;
                            float d[3][3], v[3][3], m[3][3];
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j) {
                                    d[i][j] = 0;
                                    for (int k = 0; k < 3; ++k) d[i][j] += rPrev[size_t(k)][size_t(i)] * body[size_t(k)][size_t(j)];
                                }
                            const Vec3 fw = cam.forward(), rt = cam.right(), up = cross(rt, fw);
                            const float rows[3][3] = {{fw.x, fw.y, fw.z}, {-rt.x, -rt.y, -rt.z}, {up.x, up.y, up.z}};
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j) { v[i][j] = rows[i][j]; m[i][j] = 0; }
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j)
                                    for (int k = 0; k < 3; ++k) m[i][j] += v[i][k] * d[k][j];
                            cam.pitch = std::clamp(-std::atan2(m[0][2], std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1])) * 180.0f / float(M_PI), -89.0f, 89.0f);
                            if (world->playerPmFlags() & 0x40000) cam.yaw = std::atan2(m[0][1], m[0][0]) * 180.0f / float(M_PI);
                            rPrev = body;
                            rHave = true;
                        }
                        const script::Vec3 po = world->player()->obj->get("origin").asVector();
                        cam.pos = {po[0], po[1], po[2] + 60.0f};
                        if (f % 20 == 0 || std::getenv("IW2_RIDE_TRACE")) std::cout << "ride f=" << f << " cam " << cam.pos.x << "," << cam.pos.y << "," << cam.pos.z << " yaw " << cam.yaw << " pitch " << cam.pitch;
                        if (std::getenv("IW2_RIDE_TRACE"))
                            for (const auto& pv : world->entities()) if (pv->number == world->player()->usingVehicle && pv->vehicle) {
                                const script::Vec3 vo = pv->obj->get("origin").asVector(), va = pv->obj->get("angles").asVector();
                                std::cout << " | veh " << vo[0] << "," << vo[1] << "," << vo[2] << " ang " << va[0] << "," << va[1] << "," << va[2] << " spd " << pv->vehicle->forwardSpeed;
                            }
                        std::cout << "\n";
                    }
                    if (world->multiplayer())
                        for (const auto& ev : world->takeMpObituaries()) shotObits.push_back({ev, world->timeMs()}); // [server time]
                    for (const auto& cmd : world->takeClientCommands()) // --shot shows the death screen and script menus too
                        if (cmd.name == "opendeadscreen") deadScreenMs = cgNowMs();
                        else if (cmd.name == "popupopen" || cmd.name == "popupclose") menuCommand(cmd.name, cmd.arg);
                        else if (cmd.name == "fog") {
                            std::istringstream a(cmd.arg);
                            float nearD = 0, farD = 0, one = 1, r = 0, g = 0, b = 0, ms = 0;
                            a >> nearD >> farD >> one >> r >> g >> b >> ms;
                            if (one >= 1.0f) world->clientFog.command(nearD, farD, r, g, b, int(ms), cgNowMs());
                            else world->clientFog.commandExp(one, r, g, b, int(ms), cgNowMs());
                        }
                        else if (cmd.name == "obj_update" || cmd.name == "obj_complete" || cmd.name == "gm")
                            shotMsgs.push_back({cmd.name == "gm" ? cmd.arg : "^2\x14" + cmd.arg, cgNowMs(), false}); // obj_*: "^2%s" (0x57f6a0)
                        else if (cmd.name == "gmb") shotMsgs.push_back({cmd.arg, cgNowMs(), true});
                        else if (cmd.name == "h" || cmd.name == "i") chatCommand(cmd.name, cmd.arg);
                }
                frontStack.settleFades(); // a single-frame shot shows the menus after their fades
                // Messages still on screen (con_gamemessagetime 5 s, con_boldgamemessagetime 8 s) at the end of the simulation.
                for (const auto& m : shotMsgs)
                    if (cgNowMs() - m.at < (m.bold ? 8000 : 5000)) { const uint64_t nowNs = SDL_GetTicksNS(), st = nowNs - uint64_t(cgNowMs() - m.at) * 1000000ull;
                        screenMessages.push_back({m.text, nowNs + 600000000000ull, m.bold, st, st + (m.bold ? 8000000000ull : 5000000000ull)}); }
                for (const auto& [ev, at] : shotObits)
                    if (world->timeMs() - at < 5000) pushObituary(ev, SDL_GetTicksNS() - uint64_t(world->timeMs() - at) * 1000000ull, at); // [server time]
                // Debug: IW2_BOLD_MESSAGE="<text or localized key>" shows an iprintlnbold message in the shot.
                if (const char* bm = std::getenv("IW2_BOLD_MESSAGE")) screenMessages.push_back({bm, SDL_GetTicksNS() + 600000000000ull, true, SDL_GetTicksNS() - 1000000000ull, SDL_GetTicksNS() + 600000000000ull});
                // Debug: IW2_SHOT_CHATFIELD="<text>" shows the open message field with that text (docs/spec/mp-chat.md).
                if (const char* cf = std::getenv("IW2_SHOT_CHATFIELD")) chatField = {true, false, cf};
                // Debug: IW2_SHOT_HINT=<state> shows an invalid-command hint state 300 ms into its blink cycle.
                if (const char* hs = std::getenv("IW2_SHOT_HINT")) setInvalidCmd(std::atoi(hs), cgNowMs() - 300);
                // Debug: IW2_SHOT_OBJECTIVES=1 shows the objectives screen as if +scores were held.
                if (std::getenv("IW2_SHOT_OBJECTIVES")) hudFade.objectivesHeld = true;
                // Debug: IW2_SHOT_HEALTH=<hp or fraction of maxhealth when <= 1>[,<trail fraction>[,<pulse phase ms>]] sets the player health, the red trail of the health bar
                // (the fraction it just fell from) and how far into its pulse period the health background is.
                if (const char* hs = std::getenv("IW2_SHOT_HEALTH"); hs && world->player()) {
                    float v[3] = {100, -1, 0};
                    std::sscanf(hs, "%f,%f,%f", &v[0], &v[1], &v[2]);
                    if (v[0] <= 1.0f) { // a fraction of the player's maxhealth
                        const auto mv = world->player()->obj->get("maxhealth");
                        v[0] *= mv.isNumber() ? float(mv.asInt()) : 100.0f;
                    }
                    world->player()->obj->set("health", script::Value(int32_t(v[0])));
                    if (v[1] >= 0) hudFade.healthTrail = v[1];
                    hudFade.healthPulse = cgNowMs() - int64_t(v[2]);
                }
                // A still shot has no input history: show the faded HUD blocks as just touched (IW2_HUD_FADED=1 keeps them untouched).
                if (!std::getenv("IW2_HUD_FADED")) hudFade.compass = hudFade.ammo = hudFade.weaponChange = hudFade.stance = hudFade.health = hudFade.offhand = cgNowMs();
                // A script link carries the player: shoot from there (--yaw / --pitch kept); IW2_SHOT_FREE_CAM=1 keeps --pos.
                if (world->playerLinked() && !std::getenv("IW2_SHOT_FREE_CAM")) {
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    cam.pos = {po[0], po[1], po[2] + 60.0f};
                }
                // A following MP spectator (docs/spec/mp-spectate.md) sees the target's view.
                if (world->multiplayer()) {
                    script::Vec3 eye, ang;
                    const int fol = world->mpFollowing(world->mpLocalClient());
                    if (fol >= 0 && world->mpClientView(fol, eye, ang)) {
                        cam.pos = {eye[0], eye[1], eye[2]};
                        cam.pitch = ang[0];
                        cam.yaw = ang[1];
                    }
                }
                if (const char* ft = std::getenv("IW2_FIRE_TURRETS")) { // IW2_FIRE_TURRETS=N: camera beside the Nth turret
                        int n = 0;
                        for (const auto& te : world->entities())
                            if (te->turret && !te->obj->removed && n++ == std::atoi(ft)) {
                                const script::Vec3 to = te->obj->get("origin").asVector();
                                const float yr = (te->turret->yaw + 100.0f) * float(M_PI) / 180.0f;
                                cam.pos = {to[0] + std::cos(yr) * 150.0f, to[1] + std::sin(yr) * 150.0f, to[2] + 60.0f};
                                cam.yaw = std::atan2(to[1] - cam.pos.y, to[0] - cam.pos.x) * 180.0f / float(M_PI);
                                cam.pitch = 15.0f;
                            }
                }
                if (const char* se = std::getenv("IW2_SHOT_ENT")) { // IW2_SHOT_ENT=N[,dist,yaw,height]: camera aimed at entity N
                    float v[4] = {-1, 120, 90, 50};
                    std::sscanf(se, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
                    for (const auto& te : world->entities())
                        if (te->number == int(v[0]) && te->obj->get("origin").isVector()) {
                            const script::Vec3 to = te->obj->get("origin").asVector();
                            const script::Value ta = te->obj->get("angles");
                            const float yr = ((ta.isVector() ? ta.asVector()[1] : 0.0f) + v[2]) * float(M_PI) / 180.0f;
                            cam.pos = {to[0] + std::cos(yr) * v[1], to[1] + std::sin(yr) * v[1], to[2] + v[3]};
                            cam.yaw = std::atan2(to[1] - cam.pos.y, to[0] - cam.pos.x) * 180.0f / float(M_PI);
                            cam.pitch = std::atan2(v[3] - 10.0f, v[1]) * 180.0f / float(M_PI);
                        }
                }
                if (forceTurret || forceJolt)
                    for (const auto& e : world->entities())
                        if (e->vehicle) {
                            if (forceTurret) e->vehicle->turretPitch = (*forceTurret)[0], e->vehicle->turretYaw = (*forceTurret)[1];
                            if (forceJolt) e->vehicle->joltPitch = (*forceJolt)[0], e->vehicle->joltRoll = (*forceJolt)[1];
                        }
                if (followVehicle >= -1) {
                    int n = 0;
                    std::cout << "player pm_flags 0x" << std::hex << world->playerPmFlags() << std::dec << "\n";
                    for (const auto& e : world->entities()) {
                        if (!e->vehicle || e->obj->removed) continue;
                        const script::Vec3 o = e->obj->get("origin").asVector(), an = e->obj->get("angles").asVector();
                        std::cout << "vehicle " << n << " (" << e->obj->get("targetname").toString() << "): " << e->obj->get("model").toString() << " at " << o[0] << "," << o[1] << "," << o[2]
                                  << " angles " << an[0] << "/" << an[1] << "/" << an[2] << " turret p/y " << e->vehicle->turretPitch << "/" << e->vehicle->turretYaw
                                  << " jolt " << e->vehicle->joltPitch << "/" << e->vehicle->joltRoll << " speed " << e->vehicle->speed
                                  << "/" << e->vehicle->forwardSpeed << " scroll " << e->vehicle->texScroll << " compass " << e->vehicle->onCompass
                                  << " lookat '" << e->vehicle->lookAtText << "'/'" << e->vehicle->lookAtText2 << "'\n";
                        if (n == followVehicle) {
                            const float yr = (an[1] + followYaw) * float(M_PI) / 180.0f;
                            const float pr = followPitch * float(M_PI) / 180.0f;
                            cam.pos = {o[0] + std::cos(yr) * followDist * std::cos(pr), o[1] + std::sin(yr) * followDist * std::cos(pr), o[2] + 60.0f + followDist * std::sin(pr)};
                            cam.yaw = std::atan2(o[1] - cam.pos.y, o[0] - cam.pos.x) * 180.0f / float(M_PI);
                            cam.pitch = followPitch;
                        }
                        ++n;
                    }
                }
                if (shotUse) {
                    game::World::PlayerCommand pc;
                    pc.viewAngles = {cam.pitch, cam.yaw, 0};
                    for (int f = 0; f < 3; ++f) {
                        pc.buttons = f == 1 ? 8u : 0u;
                        world->setPlayerCommand(pc);
                        world->frame();
                    }
                    if (world->playerOnTurret()) {
                        world->clampPlayerView(cam.pitch, cam.yaw);
                        pc.viewAngles = {cam.pitch, cam.yaw, 0};
                        pc.buttons = 0;
                        world->setPlayerCommand(pc);
                        world->frame();
                        game::Vec3 eye;
                        float tp = 0, ty = 0;
                        if (world->playerTurretAnglesFor({cam.pitch, cam.yaw, 0}, tp, ty)) {
                            playerTurretPose = std::make_pair(tp, ty);
                            if (world->playerTurretEyeAt(tp, ty, eye)) cam.pos = {eye[0], eye[1], eye[2] + 60.0f - shotViewHeight};
                        }
                    }
                    std::cout << "use: on turret " << world->playerOnTurret() << "\n";
                }
                std::cout << "script errors: " << world->vm().errors().size() << ", live threads: " << world->vm().liveThreadCount()
                          << ", hud elements: " << world->hudElems().size() << "\n";
            }
            if (world && shootMarks > 0) {
                for (int i = 0; i < shootMarks; ++i) {
                    const float y = (cam.yaw + float(i % 5 - 2)) * float(M_PI) / 180.0f, pt = (cam.pitch + float(i / 5 - 2)) * float(M_PI) / 180.0f;
                    world->playerFire({cam.pos.x, cam.pos.y, cam.pos.z}, {std::cos(pt) * std::cos(y), std::cos(pt) * std::sin(y), -std::sin(pt)});
                }
                std::cout << "shoot: " << world->stats.bulletImpacts << " impacts\n";
                atExitMarks = true;
            }
            renderer.init(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, aaSamples());
            syncWeapon();
            pweapon.update(1.0f, {});
            game::PlayerWeapon::Input shotIn;
            shotIn.fire = fireSeconds > 0;
            shotIn.ads = shotAds;
            if (pweapon.update(fireSeconds, shotIn) > 0) viewShot();
            shotIn.fire = false;
            shotIn.frag = std::getenv("IW2_SHOT_FRAG") != nullptr; // debug: hold +frag for the shot (offhand view model)
            // Debug: IW2_SHOT_SCRIPT=t:action,... (reload+/-, fire+/-, ads+/-, frag+/-, smoke+/-, melee+/-, weapon=name) runs
            // IW2_SHOT_SECONDS (default 3) of weapon time before the shot and logs the weapon state each 0.25 s.
            std::vector<std::pair<float, std::string>> shotScript;
            if (const char* sc = std::getenv("IW2_SHOT_SCRIPT")) {
                std::stringstream ss(sc);
                for (std::string tok; std::getline(ss, tok, ',');)
                    if (const auto c = tok.find(':'); c != std::string::npos) shotScript.push_back({std::stof(tok.substr(0, c)), tok.substr(c + 1)});
            }
            const int shotSteps = shotScript.empty() ? 100 : int(50.0f * (std::getenv("IW2_SHOT_SECONDS") ? std::stof(std::getenv("IW2_SHOT_SECONDS")) : 3.0f));
            // `turn=<deg/s>` turns the view each 20 ms step. A script with it also runs the effects per step like client frames:
            // every shot plays the view flash / brass at that step's time with that step's gun pose, and the particles are updated
            // each step, so firing while turning can be checked headlessly (bolted particles must stay on tag_flash).
            float shotTurnRate = 0;
            const bool shotTurnSim = world && std::any_of(shotScript.begin(), shotScript.end(), [](const auto& a) { return a.second.rfind("turn=", 0) == 0; });
            for (int i = 0; i < shotSteps; ++i) {
                const float tNow = float(i) * 0.02f;
                for (auto& [t, act] : shotScript) {
                    if (t < 0 || tNow < t) continue;
                    t = -1;
                    if (act == "reload+" || act == "reload-") shotIn.reload = act == "reload+";
                    else if (act == "fire+" || act == "fire-") shotIn.fire = act == "fire+";
                    else if (act == "ads+" || act == "ads-") shotIn.ads = act == "ads+";
                    else if (act == "frag+" || act == "frag-") shotIn.frag = act == "frag+";
                    else if (act == "smoke+" || act == "smoke-") shotIn.smoke = act == "smoke+";
                    else if (act == "melee+" || act == "melee-") shotIn.melee = act == "melee+";
                    else if (act == "binoc+" || act == "binoc-") shotIn.binoculars = act == "binoc+";
                    else if (act == "use+" || act == "use-") shotIn.use = act == "use+";
                    else if (act.rfind("weapon=", 0) == 0) shotIn.weapon = act.substr(7);
                    else if (act.rfind("turn=", 0) == 0) shotTurnRate = std::stof(act.substr(5));
                }
                cam.yaw += shotTurnRate * 0.02f;
                game::PlayerWeapon::Input stepIn = shotIn;
                if (pweapon.stateNumber() == 0x13 || pweapon.stateNumber() == 0x14) stepIn.ads = true; // forced aim (0x409af0)
                const int shotsFired = pweapon.update(0.02f, stepIn);
                if (world && world->multiplayer()) { // the host's recorded view state of the shot (its view height, standing still)
                    game::PlayerState shotPs;
                    shotPs.viewHeight = shotViewHeight;
                    shotPs.viewHeightTarget = int(shotViewHeight);
                    world->mpSetLocalViewState(game::mpViewStateOf(pweapon, shotPs));
                }
                if (shotsFired > 0 && (shotScript.empty() || shotTurnSim || i >= shotSteps - 5)) viewShot();
                if (!shotScript.empty()) {
                    for (const auto& e : pweapon.takeEvents()) {
                        const game::Vec3 eye{cam.pos.x, cam.pos.y, cam.pos.z}, angles{cam.pitch, cam.yaw, 0};
                        if (e.kind == game::PlayerWeapon::Event::ReloadBegin) input.clearAds();
                        else if (e.kind == game::PlayerWeapon::Event::EjectBrass) { if (shotTurnSim || i >= shotSteps - 5) viewFlash->brassPending = true; }
                        else if (e.kind >= game::PlayerWeapon::Event::BinocularEnter && e.kind <= game::PlayerWeapon::Event::BinocularDrop && world) {
                            static const char* const kN[] = {"binocular_enter", "binocular_exit", "binocular_fire", "binocular_release", "binocular_drop"};
                            world->notifyPlayer(kN[e.kind - game::PlayerWeapon::Event::BinocularEnter]);
                        }
                        else if (e.kind == game::PlayerWeapon::Event::MeleeHit && world) world->playerMelee(eye, angles);
                        else if (e.kind == game::PlayerWeapon::Event::OffhandThrow && world) world->playerThrowGrenade(e.weapon, eye, angles, {0, 0, 0}, pweapon.takeFuseMs());
                        std::cout << "shot-script t=" << tNow << " event " << int(e.kind) << " " << e.weapon << "\n";
                    }
                    if (i % 13 == 0)
                        std::cout << "shot-script t=" << tNow << " weapon " << pweapon.name() << " state " << pweapon.stateNumber() << " anim " << pweapon.anim()
                                  << " clip " << pweapon.clip() << " stock " << pweapon.stock() << " ads " << pweapon.adsFrac() << "\n";
                }
                if (aimBlendWeapon != pweapon.name()) { aimBlendWeapon = pweapon.name(); aimBlend.reset(); }
                aimBlend.update(pweapon.viewAiming(), 0.02f);
                vmMotion.update([&](const char* k) { return pweapon.number(k); }, 20, 0.0f, 190.0f, shotViewHeight, pweapon.adsFrac(), shotLean);
                if (shotTurnSim) {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    vmMotion.sway([&](const char* k) { return pweapon.number(k); }, 20, cam.pitch, cam.yaw, pweapon.adsFrac(), 0, !ret.empty() && ret != "none");
                    // This step as a client frame: the camera as the shot will place it, the view model posed first (plays the
                    // pending flash at this step's time), then the effect update.
                    Camera stepCam = cam;
                    stepCam.pos.z += shotViewHeight - 60.0f;
                    const game::Vec lo = game::leanOffset(stepCam.yaw, shotLean);
                    stepCam.pos = stepCam.pos + Vec3{lo[0], lo[1], lo[2]};
                    fxTimeOverride = world->timeMs() - int64_t(shotSteps - 1 - i) * 20; // [server time]
                    std::vector<DrawItem> scratch;
                    addViewModel(scratch, stepCam);
                    (void)fxSystem.sprites(*fxTimeOverride);
                    if (shotsFired > 0) std::cout << "shot-script t=" << tNow << " turn flash yaw " << cam.yaw << " tag " << viewFlash->pos[0] << "," << viewFlash->pos[1] << "," << viewFlash->pos[2] << "\n";
                    fxTimeOverride.reset();
                }
            }
            if (shotIn.frag) std::cout << "shot weapon state " << pweapon.stateNumber() << " anim " << pweapon.anim() << " t " << pweapon.animTime() << "\n";
            cam.pos.z += shotViewHeight - 60.0f;
            {
                const game::Vec lo = game::leanOffset(cam.yaw, shotLean);
                cam.pos = cam.pos + Vec3{lo[0], lo[1], lo[2]};
            }
            cam.fovX43 = game::playerViewFov(game::adsFov(80.0f, pweapon.number("adsZoomFov") > 0 ? pweapon.number("adsZoomFov") : 80.0f,
                                      pweapon.number("adsZoomInFrac"), pweapon.number("adsZoomOutFrac"), pweapon.adsFrac(), pweapon.adsZoomingIn()),
                                             world && world->playerOnTurret());
            const auto killcamView = killcamCamera(cam, 1.0f); // killcam: the killer's archived view
            if (killcamView && world->player())
                std::cout << "shot killcam: follows entity " << killcamView->killer << " weapon " << killcamView->weapon << ", spectatorclient "
                          << world->player()->obj->get("spectatorclient").toString() << ", last attacker "
                          << (world->player()->lastAttacker ? world->player()->lastAttacker->number : -1) << ", state "
                          << world->player()->obj->get("sessionstate").toString() << " archivetime " << world->player()->obj->get("archivetime").toString()
                          << " eye " << killcamView->eye[0] << " " << killcamView->eye[1] << " " << killcamView->eye[2] << " cam " << cam.pos.x << " " << cam.pos.y << "\n";
            fxRight = cam.right();
            fxUp = cross(cam.right(), cam.forward());
            // The per-element spawnRange / cullrange gate (0x485300): view origin and tan(fovX / 2) / tan(40 deg) (0x486e00).
            fxSystem.viewOrigin = {cam.pos.x, cam.pos.y, cam.pos.z};
            fxSystem.viewScale = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) / std::tan(0.6981317f);
            fxSystem.viewValid = true;
            setFxFrustum(fxSystem, cam, float(width) / float(height));
            setFxFar(fxSystem, cam, (world ? world->clientFog.current(cgNowMs()) : game::FogParams{}), world ? world->cullDist() : 0.0f);
            if (const char* test = std::getenv("IW2_FX_TEST"); test && world) {
                // Debug: play an effect 150 units ahead and show it after IW2_FX_MS milliseconds.
                const float dist = std::getenv("IW2_FX_TEST_DIST") ? float(std::atof(std::getenv("IW2_FX_TEST_DIST"))) : 150.0f;
                const Vec3 at = cam.pos + cam.forward() * dist;
                const int64_t t0 = world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 200); // [server time]
                fxSystem.play(efx(test), {at.x, at.y, at.z}, {0, 0, 1}, t0);
                // IW2_FX_STEP=<ms>: update the effects every <ms> from the play time to the shot, like client frames (element
                // updates such as emitFx and bounces then run with that frame time instead of one large step).
                if (const char* st = std::getenv("IW2_FX_STEP"); st && std::atoi(st) > 0)
                    for (int64_t t = t0; t < cgNowMs(); t += std::atoi(st)) (void)fxSystem.sprites(t);
            }
            if (const char* tt = std::getenv("IW2_TRACER_TEST"); tt && world) {
                // Debug: N tracer attempts (cg_tracerchance applies) from beside the view towards 1500 units ahead, aged IW2_FX_MS.
                const Vec3 from = cam.pos + cam.right() * 30.0f + Vec3{0, 0, -15.0f};
                const Vec3 to = cam.pos + cam.forward() * 1500.0f;
                for (int i = 0; i < std::atoi(tt); ++i)
                    fxSystem.addTracer({from.x, from.y, from.z}, {to.x, to.y + float(i) * 10.0f, to.z}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 100)); // [server time]
            }
            if (const char* as = std::getenv("IW2_AIM_SPREAD")) hudAimSpread = float(std::atof(as)); // debug: aim spread for the shot
            // Pose the view model before the effects are drawn, as the live loop does (plays a pending view flash, refreshes the
            // tag the bolted particles follow).
            std::vector<DrawItem> shotVmItems;
            if (!(world && world->playerOnTurret()) && !(world && scopeAlpha() > 0)) addViewModel(shotVmItems, cam);
            else if ((viewFlash->pending || viewFlash->brassPending) && !(world && world->playerOnTurret())) {
                std::vector<DrawItem> scratch;
                addViewModel(scratch, cam, false);
            }
            if (killcamView) {
                shotVmItems.clear();
                addKillcamViewModel(shotVmItems, cam, *killcamView);
            }
            auto shotItems = killcamView ? gatherKillcamItems(cam, 1.0f) : gatherItems(cam);
            shotItems.insert(shotItems.end(), shotVmItems.begin(), shotVmItems.end());
            std::cout << "effect particles: " << fxSystem.count() << "\n";
            for (const auto& l : fxSystem.lights()) std::cout << "fx light " << l.origin[0] << "," << l.origin[1] << "," << l.origin[2] << " radius " << l.radius << " rgb " << l.rgb[0] << "," << l.rgb[1] << "," << l.rgb[2] << "\n"; // dynamic lights submitted this frame (not rendered yet)
            if (atExitMarks) std::cout << "marks: " << scene.marks.marks().size() << "\n";
            updateWaterMaps(scene, dev, world ? float(cgNowMs()) / 1000.0f : 0.0f);
            renderer.fog = (world ? world->clientFog.consts(cgNowMs()) : game::FogConsts{});
            applyTextureFilter(renderer, uiCvars);
            renderer.dlights.clear(); for (const auto& d : selectDlights(fxSystem.lights(), cam.pos, dlightLimitFrom(uiCvars), fxSystem.viewPlanes)) renderer.dlights.push_back({{d[0], d[1], d[2]}, d[3], {d[4], d[5], d[6]}});
            hudKillcam = killcamView;
            forcedBlur = std::max(forcedBlur, worldMenuBlur());
            renderer.gamma = gammaFrom(uiCvars), renderer.zFeather = zFeatherFrom(uiCvars);
            screenshot(dev, renderer, scene, cam, width, height, shot, shotItems, std::getenv("IW2_NO_HUD") ? std::vector<Quad2D>{} : buildHud(width, height, cam), forcedBlur); // IW2_NO_HUD: debug, no script HUD / fade
            Uploader::releasePool(dev);
            SDL_DestroyGPUDevice(dev);
            SDL_Quit();
            return 0;
        }

        SDL_Window* window = SDL_CreateWindow(app ? "OpenIW2" : ("iw2view - " + (menuMode ? std::string("menu") : args[1])).c_str(), int(width), int(height),
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) sdlFail("SDL_CreateWindow");
        if (!SDL_ClaimWindowForGPUDevice(dev, window)) sdlFail("SDL_ClaimWindowForGPUDevice");
        renderer.init(dev, SDL_GetGPUSwapchainTextureFormat(dev, window), aaSamples());
        SDL_SetWindowRelativeMouseMode(window, true);

        if (menuMode) frontStack.open("main"); // UI_SetActiveMenu(MAIN) opens "main" (0x4c6520)
        uint64_t last = SDL_GetTicksNS();
        bool running = true;
        bool walk = true;
        uint32_t prevButtons = 0;
        int64_t lastWeaponCycleMs = 0; // 0xf796e4, zeroed by CG_Init on every map load
        std::string cgOffhand; // 0xf79700: the offhand as of the last frame, which a pickup event still sees
        float gameClock = 0;
        // The camera of a player placed by a link or a vehicle is drawn interpolated like the entities (they lag one server frame): the
        // player origin before the last server frame and whether the camera follows the link this frame. TODO(spec): [no original rule found yet] the original's rule.
        script::Vec3 prevPlayerO{0, 0, 0};
        // The view turn a player link made in the last server frame, shown over the server interval (linkview.h).
        game::LinkViewSmoother linkView;
        bool camLinkLerp = false;
        float camLinkHeight = 60.0f;
        game::PlayerState ps;
        ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - 60.0f + 8.0f};
        game::markSpawned(ps);
        bool wasDead = false;
        std::array<float, 2> bobAngles{0, 0};
        float deadYaw = 0, deadPitch = 0;
        game::MoveRecorder moveRecorder;
        bool fellOut = false;
        std::array<game::Vec3, 3> prevBody{};
        bool havePrevBody = false;
        // The driven vehicle's body axes at the previous server frame (set with prevPlayerO): the client draws the vehicle and
        // the player from the two last snapshots by the frame fraction (CG_InterpolatePlayerState 0x4b9940, entity lerp
        // 0x4a4c10), so the driver's view origin and body turn are interpolated too instead of stepping at 20 Hz.
        std::array<game::Vec3, 3> tickBodyPrev{};
        bool haveTickBodyPrev = false;
        const game::MoveParams mp;
        game::MantleAnims mantleAnims;
        for (int i = 1; i < 11; ++i) mantleAnims.anims[size_t(i)] = viewAnim(vfs, game::mantleAnimName(i));
        // Window pixels to the 640x480 virtual screen (4:3 area centred horizontally, as the HUD).
        auto toVirtual = [&](float wx, float wy) {
            int pw = 0, ph = 0, ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            SDL_GetWindowSize(window, &ww, &wh);
            const float sc = float(ph) / 480.0f;
            const float left = (float(pw) - 640.0f * sc) * 0.5f;
            return std::array<float, 2>{(wx * float(pw) / float(std::max(ww, 1)) - left) / sc, wy * float(ph) / float(std::max(wh, 1)) / sc};
        };
        // Cinematic playback (docs/spec/video.md), modelled on CL_PlayCinematic_f (0x49e690) and the per-frame run (0x49dcc0): modal; the
        // movie fills the 640x480 area (letterbox: y 105, height 270, 0x49e722); the clock is wall time with frozen hitches (a frame delta
        // over 100 ms moves the start time, 0x49dd91), the shown frame is elapsed * fps / 1000 and late frames are decoded and dropped;
        // audio is queued when its frame is decoded. Any key or mouse button skips it when skippable (`cinematic` sets the skippable
        // byte 0x59a0d0, `unskippablecinematic` clears it). `loop` restarts it at the end (the command's flag '2', 0x49ded8).
        // Returns false when the window was closed.
        // The client's stop-all (0x444730 with flags 0, docs/spec/sound.md "Stopping sounds"); defined once the voice handles exist.
        std::function<void()> stopAllSounds;
        auto playCinematic = [&](const std::string& rawName, bool skippable, bool letterbox, bool loop) {
            std::string name = rawName;
            for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            const auto data = vfs.read("video/" + name + ".roq");
            if (!data) { std::cerr << "cinematic: no video/" << name << ".roq\n"; return true; }
            std::unique_ptr<video::RoqDecoder> dec;
            try { dec = std::make_unique<video::RoqDecoder>(*data); } catch (const std::exception& e) { std::cerr << "cinematic " << name << ": " << e.what() << "\n"; return true; }
            // CL_PlayCinematic_f (0x49e690): once the movie opened, every playing sound ends (0x49e752) and the master fade returns to
            // full (0x49e75d..0x49e775) - a level's music does not resume behind or after the movie.
            stopAllSounds();
            mixer.fade(1.0f, 1);
            std::cout << "cinematic " << name << (skippable ? "" : " (unskippable)") << (letterbox ? " letterbox" : "") << (loop ? " loop" : "") << "\n";
            const bool stats = std::getenv("IW2_CINEMATIC_STATS") != nullptr;
            Scene movieScene = emptyScene(dev);
            SDL_GPUTexture* tex = nullptr;
            int tw = 0, th = 0;
            SDL_AudioStream* snd = nullptr;
            if (audioOn) {
                const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, video::RoqDecoder::kAudioRate};
                snd = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (snd) SDL_ResumeAudioStreamDevice(snd);
            }
            // The mixer's own stream must not talk over the movie.
            if (audioStream) SDL_PauseAudioStreamDevice(audioStream);
            const uint64_t t0 = SDL_GetTicksNS();
            uint64_t lastNs = t0, startShiftNs = 0;
            bool more = true, skipped = false;
            int64_t shown = -1, dropped = 0, audioPut = 0;
            double syncSum = 0, syncMax = 0, syncMin = 1e9;
            int64_t syncN = 0;
            while (more && !skipped && running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT) running = false;
                    else if (skippable && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) skipped = true;
                }
                const uint64_t nowNs = SDL_GetTicksNS();
                if (const char* lim = std::getenv("IW2_EXIT_AFTER"))
                    if (nowNs > uint64_t(std::atof(lim) * 1e9)) { running = false; break; } // debug: leave after N seconds, movie included
                if (nowNs - lastNs > 100000000ull) startShiftNs += nowNs - lastNs; // a hitch over 100 ms freezes the movie clock (0x49dd91)
                lastNs = nowNs;
                const double elapsed = double(nowNs - t0 - startShiftNs) * 1e-9;
                const int64_t due = int64_t(elapsed * dec->fps()) + 1; // frames that should have been shown
                bool fresh = false;
                while (more && dec->frameIndex() < due) {
                    const bool hadFrame = dec->frameIndex() > 0;
                    more = dec->nextFrame();
                    if (!more) break;
                    fresh = true;
                    if (hadFrame && dec->frameIndex() < due) ++dropped;
                    auto pcm = dec->takeAudio();
                    if (snd && !pcm.empty()) {
                        if (dec->audioChannels() == 1) {
                            std::vector<int16_t> st(pcm.size() * 2);
                            for (size_t i = 0; i < pcm.size(); ++i) st[i * 2] = st[i * 2 + 1] = pcm[i];
                            pcm = std::move(st);
                        }
                        SDL_PutAudioStreamData(snd, pcm.data(), int(pcm.size() * sizeof(int16_t)));
                        audioPut += int64_t(pcm.size() / 2);
                    }
                }
                if (!more && loop && !skipped) { // restart (0x49d490)
                    dec = std::make_unique<video::RoqDecoder>(*data);
                    more = true;
                    continue;
                }
                if (dec->width() == 0) break;
                if (!tex || tw != dec->width() || th != dec->height()) {
                    if (tex) SDL_ReleaseGPUTexture(dev, tex);
                    tw = dec->width(), th = dec->height();
                    tex = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, uint32_t(tw), uint32_t(th), 1);
                    movieScene.textures.resize(1);
                    movieScene.textures.push_back(tex);
                    fresh = true;
                }
                if (fresh) {
                    Uploader up(dev, false);
                    up.texture(tex, 0, 0, uint32_t(tw), uint32_t(th), dec->rgba().data(), uint32_t(dec->rgba().size()));
                    shown = dec->frameIndex();
                }
                if (stats && snd && audioPut > 0) {
                    // Sync measurement: the picture time (shown frame / fps) against the audio actually played by the device stream
                    // (samples put minus samples still queued), both in seconds since the start.
                    const double played = double(audioPut - SDL_GetAudioStreamQueued(snd) / 4) / video::RoqDecoder::kAudioRate;
                    const double picture = double(std::max<int64_t>(shown, 0)) / dec->fps();
                    if (elapsed > 1.0) { // the first second includes the device start-up
                        const double d = picture - played; // > 0: the picture is ahead of the sound
                        syncSum += d, syncMax = std::max(syncMax, d), syncMin = std::min(syncMin, d), ++syncN;
                    }
                }
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
                SDL_GPUTexture* swap = nullptr;
                uint32_t w = 0, h = 0;
                if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h) && swap) {
                    const float sc = float(h) / 480.0f, left = (float(w) - 640.0f * sc) * 0.5f;
                    const float y0 = letterbox ? 105.0f : 0.0f, y1 = letterbox ? 105.0f + 270.0f : 480.0f;
                    Quad2D q{left, y0 * sc, left + 640.0f * sc, y1 * sc};
                    q.texture = 1;
                    renderer.fog = {};
                    std::vector<Quad2D> quads;
                    if (letterbox) { // black bars outside the picture
                        Quad2D bar{0, 0, float(w), float(h)};
                        bar.rgba[0] = bar.rgba[1] = bar.rgba[2] = 0;
                        quads.push_back(bar);
                    }
                    quads.push_back(q);
                    renderer.gamma = gammaFrom(uiCvars), renderer.zFeather = zFeatherFrom(uiCvars);
                    renderer.draw(cmd, swap, w, h, movieScene, cam, {}, quads, 0.0f);
                }
                SDL_SubmitGPUCommandBuffer(cmd);
            }
            if (stats) {
                const double wall = double(SDL_GetTicksNS() - t0) * 1e-9;
                std::cout << "cinematic stats: " << name << " " << tw << "x" << th << " " << dec->fps() << " fps; wall " << wall << " s, last frame "
                          << shown << " (" << double(shown) / dec->fps() << " s), dropped " << dropped << ", audio " << double(audioPut) / video::RoqDecoder::kAudioRate
                          << " s queued"
                          << (syncN ? "; picture minus audio played (after 1 s) mean " + std::to_string(syncSum / double(syncN) * 1000.0) + " ms, min " + std::to_string(syncMin * 1000.0) + ", max " + std::to_string(syncMax * 1000.0) + " ms over " + std::to_string(syncN) + " draws" : std::string())
                          << (skipped ? "; skipped" : "") << "\n";
            }
            if (snd) SDL_DestroyAudioStream(snd);
            stopAllSounds(); // the stop routine of a cinematic (0x49e800 -> 0x49e816) ends all sounds again
            if (audioStream) SDL_ResumeAudioStreamDevice(audioStream);
            SDL_WaitForGPUIdle(dev);
            releaseScene(dev, movieScene);
            return running;
        };
        game::ShockEndSound shockEnd; // shellshock_end / shellshock_end_abort (0x4b1f90, 0x4b1e90)
        // Hold-breath sounds (0x4b6840) and the hold_breath channel volumes (cgame registration 0x4a3140 loads
        // shock/hold_breath.shock into record 16, 0xf5741c; its volumes drive sound volume slot 2 via 0x4b6700).
        game::BreathSounds breathSnd;
        game::ShockRecord holdBreathRec;
        // Sound volume slot 1 (setchannelvol, docs/spec/campaign.md "Victory screen"): the channel volumes of shock/victoryscreen.shock,
        // reached linearly over `ms` (slot fade 0x4431f0: a per-channel step of (target - current output) / max(ms, 1), advanced by the elapsed ms and clamped at the target; the start is the current output of the highest slot, which is full volume here).
        audio::ChannelVolumeSlots volSlots; // slot 1 doubles as the victory-screen volume (cleared on level load, 0x444730)
        struct { game::ShockRecord rec; uint64_t startNs = 0; int ms = 1; } victoryVol;
        // 0x444730 with flags 0: every voice, the music and both ambient slots included, ends at once; reverb layers 1 and 2 and the
        // volume slots 1..4 are cleared. The handles the client code holds for those voices go with them.
        stopAllSounds = [&] {
            clientSnd.stopAll(mixer); // Mixer::stopAll(0) and the handles of the music, ambient, shell shock and heartbeat voices
            volSlots.clearAll();
            menuLoopHandle = 0;
            frontStack.soundsStopped();
        };
        if (const auto hb = vfs.read("shock/hold_breath.shock")) holdBreathRec = game::parseShockFile(std::string(hb->begin(), hb->end()));
        else std::cerr << "Couldn't find shock file [hold_breath.shock]\n"; // 0x57e240 (the exe then raises an error)
        float frameDt = 0.016f; // last frame time, for the shell shock mouse speed limit
        float mouseAccX = 0, mouseAccY = 0, mousePrevX = 0, mousePrevY = 0; // CL_MouseMove slots (0x621538 / 0x621540)
        uint64_t fpsSince = SDL_GetTicksNS();
        int fpsFrames = 0;
        // The scripted `cinematic <name> 0` (flags 0): skippable, no letterbox, no loop.
        auto playScripted = [&](const std::string& n) { return playCinematic(n, true, false, false); };
        if (const char* mv = std::getenv("IW2_PLAY_CINEMATIC")) { // debug: play video/<name>.roq (e.g. "iw_logo" or "legal!" for unskippable) first
            std::string n = mv;
            const bool unskippable = !n.empty() && n.back() == '!';
            if (unskippable) n.pop_back();
            playCinematic(n, !unskippable, std::getenv("IW2_CINEMATIC_LETTERBOX") != nullptr, std::getenv("IW2_CINEMATIC_LOOP") != nullptr);
        }
        // Start-up movie chain of the front end (Com_Init 0x42dee0, 0x42e7d5): `unskippablecinematic legal`, then, while com_introPlayed is
        // 0, `cinematic atvi; set nextmap "cinematic IW_logo; set nextmap cinematic cod_intro"`. A movie that is missing or ends runs
        // nextmap; a skip stops the current movie only. The dvar pointer has three references in the exe (registration, its store, this read) and its name occurs once, so
        // nothing in the exe sets com_introPlayed; only developer .cfg files of the game files mention it, none is auto-executed.
        if (menuMode && running && !std::getenv("IW2_SKIP_INTRO") && !skipIntro) {
            playCinematic("legal", false, false, false);
            for (const char* m : {"atvi", "IW_logo", "cod_intro"})
                if (running && !playCinematic(m, true, false, false)) break;
        }
        // IW2_PROF=N: print per-frame CPU timings (ms) every N frames and quit after the second report.
        const int profEvery = std::getenv("IW2_PROF") ? std::atoi(std::getenv("IW2_PROF")) : 0;
        double profWater = 0, profDraw = 0, profFrame = 0, profWait = 0;
        int profFrames = 0, profReports = 0;
        uint64_t profT0 = SDL_GetTicksNS();
        int binocDamageEvent = 0; // ps+0x104 != 0 for the frame that took damage (0x4e711a)
        // The client's state that belongs to one level or one connection, and its one reset. A disconnect (the player's own, a server
        // drop or an error), a connect to another server and a level load all end the level the state was built in: cgame is shut down,
        // the client state cleared and every sound stopped (CL_Disconnect 0x40cd10 and the map load both call the cgame shutdown 0x4b2490
        // and the sound stop-all 0x44f940 with no keep flags; docs/spec/net.md section 14). Times in here are the old level's
        // clock, which restarts with the next one: nothing may survive but what is kept across levels on purpose (the menu cvars, the
        // profile, the loaded assets). The World itself is freed by the caller. Returns the number of voices it stopped.
        netgame::AmbientConfigTracker netAmbient; // configstring 3 of the server (docs/spec/sound.md "Ambient on a network client")
        auto resetClientState = [&](const char* why) {
            const int voices = mixer.active();
            // Sound: the stop-all (every voice: music, ambient, loops, the menu loop; the handles to them; the volume slots), then
            // the rest of the per-level mixer state: the reverb, the pause flags (0x40cc80) and the channel scales.
            stopAllSounds();
            shockEnd = {};
            mixer.reverb = audio::EnvReverb{};
            mixer.resetPausable();
            for (float& v : mixer.channelScale) v = 1;
            mixer.fade(1.0f, 0);
            victoryVol = {};
            breathSnd.reset();
            // Screen and HUD: the old level's fades, blur, messages and hint hold.
            screenFade.clear();
            screenBlur.clear();
            cursorHint.reset();
            hudFade = HudFade{};
            compassSpring = {};
            pointerSpring = {};
            subtitles.clear();
            screenMessages.clear();
            mpCenterText.clear();
            mpCenterStartMs = -1;
            mpCrossClient = -1;
            mpCrossMs = -1;
            chatBuffer = {};
            chatField = {};
            mpSb = {};
            mpIntermissionView = false;
            invalidCmdState = 0;
            invalidCmdStartMs = 0;
            mantleHint = false;
            hudAimSpread = 255.0f;
            hudViewHeight = 60.0f;
            damageIcons = {};
            lastDamageEvent = 0;
            lastLandSeq = 0;
            lastEventSeq = 0;
            binocDamageEvent = 0;
            deadScreenMs = 0;
            slowStartNs = slowEndNs = 0;
            slowFrom = slowTo = slowmo = 1;
            // The first-person view: weapon, motion, springs, impulses, effects. CG_Init (0x4a5c10) zeroes 0xf5bd00..0xf7eb4c on every
            // map load: the last weapon cycle time 0xf796e4 among them (kept from the previous map, whose clock ran far ahead, it blocked
            // weapnext / weapprev until the new map's clock caught up), the selected offhand 0xf79700, the HUD fade touches and the
            // invalid-command hint (a touch time from the previous map would be "in the future" of the new map's clock).
            pweapon = game::PlayerWeapon{};
            weaponSelect.clear();
            weaponWritten.clear();
            prevSelect.clear();
            lastWeaponCycleMs = 0;
            cgOffhand.clear();
            drawnGunAim = {};
            gunAimDelay.reset();
            netSelectUntilMs = 0;
            hostWeaponStatePrev = 0;
            vmMotion = game::ViewWeaponMotion{};
            recoil.reset();
            viewShake.reset();
            aimBlend.reset();
            aimBlendWeapon.clear();
            viewImp.reset();
            gunBobAngles = {0, 0, 0};
            rawGunBob = rawGunBobPrev = {0, 0, 0};
            breathWeapon = false;
            *viewFlash = ViewFlash{};
            killcamRig = KillcamRig{};
            hudKillcam.reset();
            input.clearAds(); // the toggled aim ends with the level (0x40e821)
            input.setStance(0);
            menuEnv.setCvar("cl_paused", "0");
            menuEnv.setCvar("cl_bypassMouseInput", "0");
            // The network client's view of the server it left.
            netAmbient.reset(); // a level's track starts again from the configstring of the next level
            netPredictor.reset();
            netPredictedWeapon.clear();
            netWeaponSnapshot = -1;
            netPredictorFor = {nullptr, -1};
            netClockOffset.reset();
            netKillcam = {};
            remoteView = netgame::RemoteView{};
            // Effects and the interpolation caches of the old entities.
            endWorldFx();
            lerpPrev.clear();
            prevAnim.clear();
            prevTurret.clear();
            prevTexScroll.clear();
            gameClock = 0;
            lerpFrac = 0.0f;
            wasDead = false;
            bobAngles = {0, 0};
            havePrevBody = false;
            camLinkLerp = false;
            std::cout << "[reset] client state (" << why << "): " << voices << " voice(s) stopped\n";
            return voices;
        };
        while (running) {
            if (dedicatedStart) break; // StartServer with the dedicated choice: the client ends here
            if (profEvery > 0 && ++profFrames > profEvery) {
                const double n = double(profFrames - 1);
                std::cout << "prof: frame " << profFrame / n << " ms, waterUpdate " << profWater / n << ", draw " << profDraw / n
                          << ", swapchainWait " << profWait / n << ", wall " << double(SDL_GetTicksNS() - profT0) * 1e-6 / n << " ms/frame\n";
                profFrames = 1; profWater = profDraw = profFrame = profWait = 0; profT0 = SDL_GetTicksNS();
                if (++profReports >= 2) running = false;
            }
            const uint64_t profFrameStart = SDL_GetTicksNS();
            // Network client (docs/spec/net.md section 10): packets, server commands, the other players from the snapshots.
            if (listenWanted && world && world->multiplayer() && !world->mpRemote()) {
                if (!listenServer) {
                    listenGame = std::make_unique<netgame::WorldGame>(nullptr, "", "", 0, 64);
                    listenGame->worldOwnedByHost = true;
                    listenServer = std::make_unique<net::Server>(*listenGame, uint32_t(netNowMs()));
                    listenGame->sendCommand = [s = listenServer.get()](int slot, const std::string& text) { s->sendServerCommand(slot, text); };
                    listenGame->clientPing = [s = listenServer.get()](int slot) { return s->client(slot).ping; };
                    listenGame->onRestart = [s = listenServer.get()] { s->snapFlags ^= 4; };
                    // The game's password check (docs/spec/dedicated.md section 5) against the host's g_password.
                    listenServer->connectFilter = [&](const std::string& userinfo) {
                        return net::gamePasswordRejection(world ? world->cvars["g_password"] : std::string(), userinfo);
                    };
                    const char* lp = std::getenv("IW2_LISTEN_PORT");
                    const std::string np = menuEnv.getCvar("net_port");
                    const int port = lp ? std::atoi(lp) : !np.empty() ? std::atoi(np.c_str()) : 28960;
                    const std::string mc = world->cvars["sv_maxclients"];
                    listenServer->maxClients = mc.empty() ? 8 : std::clamp(std::atoi(mc.c_str()), 1, 64);
                    // NET_OpenIP (docs/spec/net.md section 11, "Server socket"): the next free port of ten, written back to net_port.
                    if (const uint16_t got = listenServer->openFirstFree(uint16_t(port))) {
                        menuEnv.setCvar("net_port", std::to_string(got));
                        std::cout << "listen server on UDP port " << got << "\n";
                    } else std::cerr << "listen server: cannot open UDP ports " << port << ".." << port + 9 << "\n";
                }
                // A new map: the clients get its gamestate. Compared by generation, not by address: the next level's
                // World can be allocated where the previous one was, and then the clients never got the new map.
                if (listenWorldGeneration != worldGeneration) {
                    listenWorldGeneration = worldGeneration;
                    game::World* const listenWorld = world.get();
                    const std::string map = world->cvars["mapname"], gt = world->mpState().gametype;
                    const int serverId = int((netNowMs() + 1 + listenServer->serverId) & 0xff);
                    listenGame->setWorld(listenWorld, map, gt, serverId);
                    listenGame->hostname = menuEnv.getCvar("sv_hostname").empty() ? "openiw2" : menuEnv.getCvar("sv_hostname");
                    listenServer->info().mapname = map;
                    listenServer->info().gametype = gt;
                    listenServer->info().hostname = listenGame->hostname;
                    listenServer->newMap(serverId);
                    for (int s = 0; s < listenServer->maxClients; ++s)
                        if (listenServer->client(s).state != net::ServerClientState::Free) listenGame->clientConnect(s, listenServer->client(s).userinfo);
                }
                listenServer->poll(netNowMs(), 0);
            }
            // Network killcam / follow: the followed client's eye and view angles the server sent (RemoteView::followView).
            bool netFollow = false;
            float netFollowEye[3] = {0, 0, 0}, netFollowAngles[3] = {0, 0, 0};
            if (netClient && world) {
                const int64_t nowNet = netNowMs();
                netClient->poll(nowNet);
                // A new gamestate after the one the level was loaded from (the server changed or restarted the map, e.g.
                // map rotation): the client loads the map it names (docs/spec/net.md section 10, "Map change").
                // A gamestate sent again for the same server id (the client was busy loading) is no new map.
                static std::tuple<int, int, int> loadedGamestate{0, 0, 0};
                if (std::get<0>(loadedGamestate) != netConnection)
                    loadedGamestate = {netConnection, netClient->gamestates, netClient->serverId};
                if (netClient->gamestates > std::get<1>(loadedGamestate) && netClient->serverId != std::get<2>(loadedGamestate) &&
                    !pendingLevel) {
                    loadedGamestate = {netConnection, netClient->gamestates, netClient->serverId};
                    const std::string map = net::infoValueForKey(netClient->configstrings[0], "mapname");
                    std::cout << "new gamestate, map " << map << "\n";
                    if (!map.empty()) pendingLevel = std::make_pair("mp/" + map, false);
                }
                if (const char* aj = std::getenv("IW2_MP_AUTOJOIN"); aj && std::atoi(aj) != 0 && netClient->latest()) {
                    // debug: the team menu's autoassign once per connection and gamestate, as the local autojoin does
                    static std::pair<int, int> joinedFor{0, 0};
                    bool joined = joinedFor == std::make_pair(netConnection, netClient->gamestates);
                    for (int i = netgame::kMenuConfigstrings; !joined && netClient->configstrings.count(i); ++i)
                        if (netClient->configstrings[i].rfind("team_", 0) == 0) {
                            netClient->addReliableCommand("mr " + std::to_string(netClient->serverId) + " " +
                                                          std::to_string(i - netgame::kMenuConfigstrings) + " autoassign");
                            joined = true;
                            joinedFor = {netConnection, netClient->gamestates};
                        }
                }
                for (const std::string& sc : netClient->takeServerCommands()) {
                    const auto tok = net::tokenize(sc);
                    if (tok.empty() || tok[0].size() != 1) continue;
                    const char letter = tok[0][0];
                    if (letter == 't' && tok.size() >= 2) {
                        // Debug: IW2_MP_AUTOJOIN=1 answers the server's team and weapon menus as the local autojoin does
                        // (autoassign, the side's default weapon) instead of opening them.
                        const char* aj = std::getenv("IW2_MP_AUTOJOIN");
                        const auto cs = netClient->configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                        const std::string mn = cs != netClient->configstrings.end() ? cs->second : std::string();
                        if (aj && std::atoi(aj) != 0 && (mn.rfind("team_", 0) == 0 || mn.rfind("weapon_", 0) == 0)) {
                            const std::string resp = mn.rfind("team_", 0) == 0 ? "autoassign"
                                                     : std::getenv("IW2_MP_WEAPON") ? std::string(std::getenv("IW2_MP_WEAPON")) // debug: as the local autojoin
                                                     : mn == "weapon_german" ? "mp40_mp" : mn == "weapon_russian" ? "ppsh_mp" : "thompson_mp";
                            netClient->addReliableCommand("mr " + std::to_string(netClient->serverId) + " " + tok[1] + " " + resp);
                        } else menuCommand("popupopen", tok[1]);
                    }
                    else if (letter == 'u') menuCommand("popupclose", ""); // TODO(spec): the close-menu letter
                    else if (letter == 'v' && tok.size() >= 3) menuEnv.setCvar(tok[1], tok[2]);
                    else if (letter == 's' && tok.size() >= 2) { // playLocalSound: sound index, configstring 590 + index
                        const auto cs = netClient->configstrings.find(netgame::kSoundConfigstrings - 1 + std::atoi(tok[1].c_str()));
                        if (cs != netClient->configstrings.end()) world->mpRemoteLocalSound(cs->second);
                    }
                    else if (letter == 'b') mpSb.netScores = sc; // scores (0x4d02b0, docs/spec/mp-scoreboard.md)
                    else if ((letter == 'h' || letter == 'i') && tok.size() >= 2) chatCommand(std::string(1, letter), tok[1]); // chat lines (docs/spec/mp-chat.md)
                    else if ((letter == 'e' || letter == 'f' || letter == 'c' || letter == 'g') && tok.size() >= 2) {
                        std::cout << "[print] " << logText(game::printMessageText(tok[1])) << "\n";
                        game::addScreenMessage(screenMessages, game::printMessageText(tok[1]), false, SDL_GetTicksNS());
                    }
                }
                // The level ambient: configstring 3 (the gamestate's and its later changes), once the level is up.
                if (const net::ClientSnapshot* amb = netClient->latest(); amb && world && !pendingLevel) {
                    const auto it = netClient->configstrings.find(netgame::kAmbientConfigstring);
                    if (const auto c = netAmbient.update(it != netClient->configstrings.end() ? it->second : std::string(), amb->serverTime)) {
                        if (c->play) clientSnd.playAmbient(mixer, c->alias, c->fadeMs);
                        else clientSnd.stopAmbient(mixer, c->fadeMs);
                    }
                }
                if (netClient->state() == net::ClientState::Disconnected) {
                    std::cerr << "disconnected: " << netClient->error << "\n";
                    netError = netgame::errorDialog(netClient->dropFromServer, netClient->dropReason);
                    pendingDisconnect = true; // the level ends below; netClient goes with it
                }
                // The synced dvars of the gamestate become the client's dvars when they change (docs/spec/net.md
                // section 11): the serverinfo menu's score and time limits, the team names and colours.
                {
                    static std::map<std::string, std::string> appliedDvars;
                    for (const auto& [n, val] : netgame::RemoteView::syncedDvars(netClient->configstrings)) {
                        auto& last = appliedDvars[n];
                        if (last == val && world->cvars.count(n)) continue;
                        last = val;
                        menuEnv.setCvar(n, val);
                    }
                }
                const net::ClientSnapshot *snapA = nullptr, *snapB = nullptr;
                float snapFrac = 0;
                // Self: the client number of the gamestate (the server's MP client number, docs/spec/net.md section 11).
                if (netClient->interpolation(netClient->renderTime(nowNet), snapA, snapB, snapFrac)) {
                    world->mpSetRemotePlayers(netgame::RemoteView::players(*snapA, *snapB, snapFrac, netClient->clientNum, netClient->configstrings));
                    netFollow = netgame::RemoteView::followView(*snapA, *snapB, snapFrac, netFollowEye, netFollowAngles);
                    netKillcam = remoteView.killcamView(*snapA, *snapB, snapFrac, netClient->configstrings, cgNowMs());
                } else {
                    netKillcam = {};
                }
                if (const net::ClientSnapshot* last = netClient->latest()) {
                    // The server times of the snapshot move to the client clock: clock at the snapshot's time.
                    const int64_t clockOffset = game::heldClockOffset(netClockOffset, cgNowMs() - netClient->renderTime(nowNet));
                    remoteView.apply(*world, *last, netClient->configstrings, netClient->clientNum, last->serverTime + clockOffset,
                                     &netPredictedWeapon.ammo);
                    // Grenades, rockets and falling weapons move along their trajectory in the drawn time (docs/spec/net.md
                    // section 11 "Trajectories"); evaluated from the older snapshot of the pair, as the cgame does.
                    if (snapA && snapB) netgame::RemoteView::placeTrajectories(*world, *snapA, *snapB, netClient->renderTime(nowNet));
                    // Other players' shots, reloads and throws: sounds, impacts, animations (docs/spec/net.md section 13).
                    remoteView.playerEvents(*world, *last, netClient->configstrings, netClient->clientNum);
                    // The followed client's shots in a killcam (the player state's event ring).
                    remoteView.followEvents(*world, *last, netClient->configstrings, cgNowMs());
                    // Debug: IW2_KILLCAM_TRACE=1, one line per snapshot with the bodies and the own puppet a network client holds.
                    static const bool netKcTrace = std::getenv("IW2_KILLCAM_TRACE") != nullptr;
                    static int netKcSnapshot = -1;
                    if (netKcTrace && last->messageNum != netKcSnapshot) {
                        netKcSnapshot = last->messageNum;
                        std::string bodies;
                        for (const auto& e : world->entities())
                            if (!e->obj->removed && e->obj->get("$deathanim").isDefined()) {
                                const auto o = e->obj->get("origin").asVector();
                                char b[96];
                                std::snprintf(b, sizeof b, " %d@%.0f,%.0f,%.0f", e->number, double(o[0]), double(o[1]), double(o[2]));
                                bodies += b;
                            }
                        const game::Entity* own = world->mpClientEntity(netgame::RemoteView::kOwnBodySlot);
                        std::printf("kctrace net t=%d follow=%d own=%d bodies=%zu%s\n", last->serverTime, int(netgame::RemoteView::following(*last)),
                                    own != nullptr, size_t(std::count(bodies.begin(), bodies.end(), '@')), bodies.c_str());
                    }
                }
            }
            // Text events only while a menu edit field has the focus (both the app start and --menu run this loop).
            if ((frontStack.wantsTextInput() || chatField.open) != SDL_TextInputActive(window)) {
                if (frontStack.wantsTextInput() || chatField.open) SDL_StartTextInput(window);
                else SDL_StopTextInput(window);
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                // F12 works everywhere and needs no cheats; only a BIND item waiting for a key takes it first.
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_F12 && !frontStack.capturing()) {
                    showFps = !showFps;
                    continue;
                }
                // The message field (0x40ade0) takes the keys: Escape closes it, Enter sends `say` / `say_team` "\x15<text>" when the
                // field is not empty, both clear it; other keys edit it. Key releases still reach the bindings.
                if (chatField.open && ev.type == SDL_EVENT_KEY_DOWN) {
                    const auto sc2 = ev.key.scancode;
                    if (sc2 == SDL_SCANCODE_ESCAPE || sc2 == SDL_SCANCODE_RETURN || sc2 == SDL_SCANCODE_KP_ENTER) {
                        // A network client sends the line to the server (docs/spec/net.md section 10), a local game says it.
                        if (sc2 != SDL_SCANCODE_ESCAPE && !chatField.text.empty() && netClient)
                            netClient->addReliableCommand(netgame::sayCommand(chatField.team, chatField.text));
                        else if (sc2 != SDL_SCANCODE_ESCAPE && !chatField.text.empty() && world && world->multiplayer())
                            world->mpSay(world->mpLocalClient(), chatField.team ? 1 : 0, "\x15" + chatField.text);
                        chatField.open = false;
                        chatField.text.clear();
                    } else if (sc2 == SDL_SCANCODE_BACKSPACE && !chatField.text.empty()) {
                        chatField.text.pop_back();
                    }
                    continue;
                }
                if (chatField.open && ev.type == SDL_EVENT_TEXT_INPUT) {
                    // The field takes any character code >= 0x20 as one byte (0x40a25e, the buffer holds 255); the keyboard text is
                    // converted from UTF-8 to the language's code page, as the localized strings and the fonts use it.
                    for (const unsigned char t : util::utf8ToCodePage(ev.text.text, logCodePage))
                        if (t >= 0x20 && t != 0x7f && chatField.text.size() < 255) chatField.text += char(t);
                    continue;
                }
                // A menu that leaves the input to the game (cl_bypassMouseInput 1: the quick-message menus) takes only its
                // execKey keys and Escape (0x5345b0); everything else goes on to the bindings below.
                if (!frontStack.empty() && !frontStack.takesAllKeys() && menuEnv.getCvar("cl_bypassMouseInput") == "1") {
                    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                        if (ev.key.scancode == SDL_SCANCODE_ESCAPE) { frontStack.key(ui::MenuStack::Key::Escape); continue; }
                        if (ev.key.key < 0x80 && frontStack.execKey(char(ev.key.key))) continue;
                    }
                } else if (!frontStack.empty()) {
                    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key < 0x80 && frontStack.execKey(char(ev.key.key))) continue;
                    // The menus own the mouse (cursor), the keyboard navigation and ESC; key releases still reach the bindings.
                    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                        const auto v = toVirtual(ev.motion.x, ev.motion.y);
                        menuMx = v[0], menuMy = v[1];
                        frontStack.mouseMove(menuMx, menuMy);
                        continue;
                    }
                    if (frontStack.capturing()) { // a BIND item waits for a key (exe 0x4ce100)
                        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) frontStack.captureKey(keyName(ev.key.scancode));
                        else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                            frontStack.captureKey(ev.button.button == SDL_BUTTON_RIGHT ? "MOUSE2" : ev.button.button == SDL_BUTTON_MIDDLE ? "MOUSE3" : "MOUSE" + std::to_string(int(ev.button.button)));
                        else if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) frontStack.captureKey(ev.wheel.y > 0 ? "MWHEELUP" : "MWHEELDOWN");
                        continue;
                    }
                    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                            const auto v = toVirtual(ev.button.x, ev.button.y);
                            menuMx = v[0], menuMy = v[1];
                            frontStack.click(menuMx, menuMy);
                        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                            const auto v = toVirtual(ev.button.x, ev.button.y);
                            menuMx = v[0], menuMy = v[1];
                            frontStack.clickBack(menuMx, menuMy);
                        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                            frontStack.release();
                        }
                        continue;
                    }
                    if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                        using K = ui::MenuStack::Key;
                        const auto sc2 = ev.key.scancode;
                        if (sc2 == SDL_SCANCODE_ESCAPE) frontStack.key(K::Escape);
                        else if (sc2 == SDL_SCANCODE_UP) frontStack.key(K::Up);
                        else if (sc2 == SDL_SCANCODE_DOWN) frontStack.key(K::Down);
                        else if (sc2 == SDL_SCANCODE_LEFT) frontStack.key(K::Left);
                        else if (sc2 == SDL_SCANCODE_RIGHT) frontStack.key(K::Right);
                        else if (sc2 == SDL_SCANCODE_RETURN || sc2 == SDL_SCANCODE_KP_ENTER) frontStack.key(K::Enter);
                        else if (sc2 == SDL_SCANCODE_BACKSPACE) frontStack.key(K::Backspace);
                        else if (sc2 == SDL_SCANCODE_DELETE) frontStack.key(K::Delete);
                        else if (sc2 == SDL_SCANCODE_HOME) frontStack.key(K::Home);
                        else if (sc2 == SDL_SCANCODE_END) frontStack.key(K::End);
                        else if (sc2 == SDL_SCANCODE_INSERT) frontStack.key(K::Insert);
                        else if (sc2 == SDL_SCANCODE_PAGEUP) frontStack.key(K::PageUp);
                        else if (sc2 == SDL_SCANCODE_PAGEDOWN) frontStack.key(K::PageDown);
                        else if (sc2 == SDL_SCANCODE_TAB) frontStack.key(K::Tab);
                        continue;
                    }
                    if (ev.type == SDL_EVENT_KEY_DOWN) {
                        using K = ui::MenuStack::Key; // key repeat: only the editing / list keys repeat
                        const auto sc2 = ev.key.scancode;
                        if (sc2 == SDL_SCANCODE_UP) frontStack.key(K::Up);
                        else if (sc2 == SDL_SCANCODE_DOWN) frontStack.key(K::Down);
                        else if (sc2 == SDL_SCANCODE_BACKSPACE) frontStack.key(K::Backspace);
                        else if (sc2 == SDL_SCANCODE_DELETE) frontStack.key(K::Delete);
                        else if (sc2 == SDL_SCANCODE_LEFT) frontStack.key(K::Left);
                        else if (sc2 == SDL_SCANCODE_RIGHT) frontStack.key(K::Right);
                        continue;
                    }
                    if (ev.type == SDL_EVENT_TEXT_INPUT) { frontStack.textInput(ev.text.text); continue; }
                    if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) { frontStack.key(ev.wheel.y > 0 ? ui::MenuStack::Key::WheelUp : ui::MenuStack::Key::WheelDown); continue; }
                }
                if (ev.type == SDL_EVENT_QUIT) running = false;
                else if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat) {
                    input.key(keyName(ev.key.scancode), ev.type == SDL_EVENT_KEY_DOWN);
                    // The bound screenshot commands ask for the capture; the file is written once the frame was drawn.
                    if (ev.type == SDL_EVENT_KEY_DOWN) {
                        std::string c = input.commandFromKey(keyName(ev.key.scancode));
                        for (char& ch : c) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                        if (c == "screenshotjpeg") pendingShot = Shot::Jpeg;
                        else if (c == "screenshot") pendingShot = Shot::Png;
                    }
                }
                else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    // SDL numbers left, middle, right as 1, 2, 3; the engine's MOUSE2 is the right button.
                    input.key(ev.button.button == SDL_BUTTON_RIGHT ? "MOUSE2" : ev.button.button == SDL_BUTTON_MIDDLE ? "MOUSE3"
                                                                                                                     : "MOUSE" + std::to_string(int(ev.button.button)),
                              ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                else if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0)
                    input.key(ev.wheel.y > 0 ? "MWHEELUP" : "MWHEELDOWN", true), input.key(ev.wheel.y > 0 ? "MWHEELUP" : "MWHEELDOWN", false);
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    // The mouse deltas of a frame are summed into the current slot (0x4096f8: two slots, 0x621538 / 0x621540) and
                    // used once per frame by CL_MouseMove (below, after the events).
                    mouseAccX += ev.motion.xrel, mouseAccY += ev.motion.yrel;
                }
                if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    // ui::escRoute: in an MP level the ESCAPE binding (togglemenu, already queued by input.key above) is the only
                    // toggle; toggling here as well opened and closed the menu on the same press.
                    bool escBound = false;
                    for (const std::string& k : input.keysFor("togglemenu")) escBound = escBound || k == "ESCAPE";
                    switch (ui::escRoute(!frontStack.empty(), world != nullptr, world && world->multiplayer(), escBound)) {
                    case ui::EscRoute::ToggleMenu: mpToggleMenu(); break;
                    case ui::EscRoute::PauseMenu: // CL_KeyEvent 0x40c216: ESC in a running level -> UI_SetActiveMenu(2)
                        menuEnv.setCvar("cl_paused", "1");
                        menuMx = 320.0f, menuMy = 240.0f;
                        frontStack.open("main");
                        break;
                    case ui::EscRoute::MainMenu:
                        // 0x40c2c7 -> 0x40e0b0: ESC with no level and no menu never quits; disconnected (state 0) it opens the main
                        // menu again (UI_SetActiveMenu(1)). The game is left only through the menu's Quit.
                        menuMx = 320.0f, menuMy = 240.0f;
                        frontStack.open("main");
                        break;
                    case ui::EscRoute::Menu: case ui::EscRoute::Binding: break;
                    }
                }
                // V (walk/fly) and L (fullbright) are openiw2 developer keys: like the cheat commands, they need sv_cheats.
                else if ((ev.key.scancode == SDL_SCANCODE_L || ev.key.scancode == SDL_SCANCODE_N) && !cheatsOn()) {}
                else if (ev.key.scancode == SDL_SCANCODE_L) {
                    renderer.fullbright = !renderer.fullbright;
                    game::addScreenMessage(screenMessages, renderer.fullbright ? "fullbright ON" : "fullbright OFF", false, SDL_GetTicksNS()); // openiw2 text, no original string
                }
                else if (ev.key.scancode == SDL_SCANCODE_F10) {
                    pendingF10 = true; // cycle off -> god -> demigod -> off, each step a cheat command behind the sv_cheats / alive gate
                }
                else if (ev.key.scancode == SDL_SCANCODE_N) {
                    walk = !walk;
                    game::addScreenMessage(screenMessages, walk ? "walk mode ON" : "fly mode ON", false, SDL_GetTicksNS()); // openiw2 text, no original string
                    if (walk) { ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - ps.viewHeight}; ps.velocity = {0, 0, 0}; }
                }
                else if (ev.key.scancode == SDL_SCANCODE_P) {
                    std::cout << "--pos " << cam.pos.x << "," << cam.pos.y << "," << cam.pos.z << " --yaw " << cam.yaw << " --pitch " << cam.pitch << "\n";
                    if (moveRecorder.write("pmove_replay.bin")) std::cout << "wrote pmove_replay.bin (iw2game <main> <map> --replay pmove_replay.bin)\n";
                }
            }
            const uint64_t now = SDL_GetTicksNS();
            const float dt = float(now - last) * 1e-9f;
            frameDt = std::min(dt, 0.1f);
            if (presentModeDirty) {
                presentModeDirty = false;
                const bool vsync = std::atoi(menuEnv.getCvar("r_swapInterval").c_str()) != 0 || std::atoi(menuEnv.getCvar("r_swapinterval").c_str()) != 0;
                SDL_GPUPresentMode pm = SDL_GPU_PRESENTMODE_VSYNC;
                if (!vsync) {
                    if (SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) pm = SDL_GPU_PRESENTMODE_IMMEDIATE;
                    else if (SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX)) pm = SDL_GPU_PRESENTMODE_MAILBOX;
                }
                if (!SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm))
                    std::cerr << "present mode: " << SDL_GetError() << "\n";
                std::cout << "present mode: " << (pm == SDL_GPU_PRESENTMODE_VSYNC ? "vsync" : pm == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate" : "mailbox") << "\n";
            }
            // CL_CreateCmd 0x409da0: CL_AdjustAngles (0x409400), the key move, then CL_MouseMove (0x409720).
            input.setMouseMove(0, 0);
            {
                const auto cvf = [&](const char* n, float def) {
                    const std::string v = menuEnv.getCvar(n);
                    return v.empty() ? def : float(std::atof(v.c_str()));
                };
                // CL_AdjustAngles: speed = frame msec * 0.001, times cl_anglespeedkey while the +speed button is held (0x409401); without
                // +strafe, +right / +left turn by speed * cl_yawspeed (0x40942c), and +lookup / +lookdown pitch by speed * cl_pitchspeed.
                // It runs for every command, with no frozen-controls test. TODO(spec): [server side not read] whether the server applies
                // keyboard-turned angles while pm_flags 0x8000 is set (the mouse path below is stopped on the client).
                if (world && frontStack.empty()) {
                    const float sp = frameDt * (input.speedHeld() ? cvf("cl_anglespeedkey", 1.5f) : 1.0f);
                    if (!input.strafeHeld()) cam.yaw += sp * cvf("cl_yawspeed", 140.0f) * (input.turnLeft() - input.turnRight());
                    cam.pitch = std::clamp(cam.pitch + sp * cvf("cl_pitchspeed", 140.0f) * (input.lookDown() - input.lookUp()), -89.0f, 89.0f);
                }
                // centerview (0x409240), also -mlook without cl_freelook (0x408b20): the view pitch goes back to level.
                if (input.takeCenterView(cvf("cl_freelook", 1.0f) != 0.0f) && world) cam.pitch = 0.0f;
                // CL_MouseMove 0x409720: m_filter averages this frame's sum with the previous one (* 0.5), else this frame's alone.
                float mx = mouseAccX, my = mouseAccY;
                if (cvf("m_filter", 0.0f) != 0.0f) mx = (mouseAccX + mousePrevX) * 0.5f, my = (mouseAccY + mousePrevY) * 0.5f;
                mousePrevX = mouseAccX, mousePrevY = mouseAccY;
                mouseAccX = mouseAccY = 0.0f;
                if ((mx != 0.0f || my != 0.0f) && world) {
                    // CG_CalcFov 0x4ae8c0: look sensitivity is scaled by zoomSens = fovX / cg_fov (80), linear in degrees (0xf791c8, applied in
                    // CL_MouseMove 0x409720). The base 0.15 deg per SDL count is kept on purpose: the exe turns raw counts into degrees as
                    // count · sensitivity · m_yaw (default 5 · 0.022 = 0.11), but SDL's xrel is in window points, not raw device counts, so the
                    // constant is a feel calibration for the macOS port rather than a spec value.
                    const float zoomSens = std::clamp(cam.fovX43 / 80.0f, 0.01f, 1.0f);
                    // CL_MouseMove: the pitch change is dy * m_pitch (cvar, default 0.022; negative = inverted, set by the invert menus);
                    // the base 0.15 stands for the default.
                    // The base 0.15 deg/count stands for the default sensitivity 5 (cvar sensitivity 1..30 from the options menu).
                    const float lookSens = uiCvars.count("sensitivity") ? std::clamp(float(std::atof(uiCvars["sensitivity"].c_str())), 1.0f, 30.0f) / 5.0f : 1.0f;
                    float pitchScale = 1.0f;
                    if (world)
                        if (const auto it = world->cvars.find("m_pitch"); it != world->cvars.end()) pitchScale = float(std::atof(it->second.c_str())) / 0.022f;
                    // pm_flags 0x8000 (freezecontrols or an absolute playerlinkto): the client mouse function returns before applying the delta
                    // (0x409871); the multiplayer intermission (pm_type 5) takes no look either.
                    if (!(world && world->playerControlsFrozen()) && !netControlsFrozen && !mpIntermissionView) {
                        float dyaw = mx * 0.15f * lookSens * zoomSens, dpitch = my * 0.15f * lookSens * zoomSens * pitchScale;
                        // Shell shock mouse (0x4b22c0): sensitivity scale and per-frame speed limits (degrees per second).
                        if (world)
                            if (const game::ShockRecord* rec = world->shellShockRecord(); rec && rec->mouse) {
                                const auto& ss = world->shellShock();
                                const game::ShockMouse m = game::shockMouse(*rec, game::elapsedMs(cgNowMs(), ss.startMs), ss.durationMs);
                                dyaw *= m.sensitivityScale, dpitch *= m.sensitivityScale;
                                if (m.maxYawSpeed > 0) dyaw = std::clamp(dyaw, -m.maxYawSpeed * frameDt, m.maxYawSpeed * frameDt);
                                if (m.maxPitchSpeed > 0) dpitch = std::clamp(dpitch, -m.maxPitchSpeed * frameDt, m.maxPitchSpeed * frameDt);
                            }
                        // +strafe held: the x motion is the side move (round(mx * m_side), 0x4098ed), else the yaw. The y motion pitches the
                        // view while +mlook or cl_freelook is on and +strafe is not held (0x4099ba..0x4099db), else it is the forward move
                        // (-round(my * m_forward), 0x409a71).
                        int side = 0, fwd = 0;
                        if (input.strafeHeld()) side = int(std::lround(mx * cvf("m_side", 0.25f)));
                        else cam.yaw -= dyaw;
                        if ((input.mlookHeld() || cvf("cl_freelook", 1.0f) != 0.0f) && !input.strafeHeld())
                            cam.pitch = std::clamp(cam.pitch + dpitch, -89.0f, 89.0f);
                        else fwd = -int(std::lround(my * cvf("m_forward", 0.25f)));
                        input.setMouseMove(side, fwd);
                    }
                }
            }
            // The F12 counter shows the frames of the last half second, so that it stays readable.
            ++fpsFrames;
            if (now - fpsSince >= 500000000ull) {
                fpsShown = int(double(fpsFrames) * 1e9 / double(now - fpsSince) + 0.5);
                fpsFrames = 0, fpsSince = now;
            }
            // The frame scale (time_slow) scales every Com_Frame msec (0x42eb70): player movement, weapon and view run at it too.
            const float simDt = game::scaledFrameTime(dt, slowmo);
            last = now;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            // MP session state of the local client (docs/spec/mp-spectate.md): 2 spectator (pm_type 4), 3 intermission (pm_type 5);
            // neither is dead, whatever the health of the entity.
            const int mpSession = world && world->multiplayer() && !netClient ? world->mpSessionState(world->mpLocalClient()) : 0;
            const int mpFollow = mpSession == 2 ? world->mpFollowing(world->mpLocalClient()) : -1;
            netControlsFrozen = false;
            if (netClient)
                if (const net::ClientSnapshot* fs = netClient->latest()) netControlsFrozen = (netgame::readPs(fs->ps).pmFlags & 0x8000) != 0;
            mpIntermissionView = netClient ? ps.pmType == 5 : mpSession == 3; // network client: the server's pm_type (prediction keeps it)
            const bool playerDead = world && world->player() && world->player()->obj->get("health").isNumber() &&
                                    world->player()->obj->get("health").asInt() <= 0 && mpSession != 2 && mpSession != 3;
            // MP: a dead client's command asks for standing (docs/spec/mp.md "Client spawn"): the spawn clears the player
            // state's stance, and a client stance still prone or crouched from the death would bring it back with the first
            // command after the spawn. TODO(spec): where the MP cgame resets its client stance.
            if (playerDead && world->multiplayer()) input.setStance(0);
            // In a vehicle (pm_flags 0x20000) the player does not move by itself; the command drives the vehicle.
            const bool inVehicle = world && (world->playerPmFlags() & 0x20000);
            // On a turret (eFlags & 0x300) PmoveSingle skips the movement and PM_Weapon (0x50c918) and the view stays
            // inside the turret's arcs (PM_UpdateViewAngles 0x50b120).
            const bool onTurret = world && world->playerOnTurret();
            if (onTurret) world->clampPlayerView(cam.pitch, cam.yaw);
            // This frame's usercmd buttons from the bound commands (CL_CmdButtons); weapon states 0x13 / 0x14 force aim.
            // freezecontrols (client +0x8630 → pm_flags 0x8000) and an open menu: no movement or button commands.
            const bool controlsBlocked = !frontStack.empty() || netControlsFrozen || (world && world->controlsFrozen());
            uint32_t buttons = controlsBlocked ? input.stanceButtons() : input.buttons(inVehicle, pweapon.stateNumber() == 0x13 || pweapon.stateNumber() == 0x14);
            if (static const bool wt = std::getenv("IW2_WEAPON_TRACE") != nullptr; wt) { // debug: own weapon state changes (docs/debug-flags.md)
                static std::string lastWt;
                char line[512];
                std::snprintf(line, sizeof line, "weapon=%s view=%s state=%d anim=%s clip=%d stock=%d offhand=%d cook=%d fuse=%d ads=%d buttons=%x blocked=%d",
                              pweapon.name().c_str(), pweapon.viewWeaponName().c_str(), pweapon.stateNumber(), pweapon.anim().c_str(), pweapon.clip(),
                              pweapon.stock(), int(pweapon.offhandActive()), int(pweapon.cooking()), pweapon.fuseMs() > 0 ? 1 : 0, int(pweapon.adsHeld()),
                              unsigned(buttons), int(controlsBlocked));
                if (lastWt != line) {
                    std::printf("weapontrace rt=%llu t=%lld %s\n", (unsigned long long)(SDL_GetTicksNS() / 1000000), (long long)(world ? world->timeMs() : -1), line); // [server time]
                    lastWt = line;
                }
            }
            // IW2_DEBUG_THROW=<seconds>[,<seconds>...]: holds the frag key for 150 ms from each real time (docs/debug-flags.md).
            if (static const char* dt = std::getenv("IW2_DEBUG_THROW"); dt && !controlsBlocked) {
                const double realS = double(SDL_GetTicksNS()) * 1e-9;
                for (const char* c = dt; *c; ) {
                    const double at = std::atof(c);
                    if (realS >= at && realS < at + 0.15) buttons |= game::Button::Frag;
                    c = std::strchr(c, ',');
                    if (!c) break;
                    ++c;
                }
            }
            const bool reloadPressed = (buttons & game::Button::Reload) && !(prevButtons & game::Button::Reload);
            prevButtons = buttons;
            if (world) {
                game::World::PlayerCommand pc;
                pc.forward = controlsBlocked ? 0 : input.forwardMove();
                pc.right = controlsBlocked ? 0 : input.rightMove();
                pc.buttons = buttons;
                // freezecontrols blocks movement and firing (pm_flags 0x8000) but the script still sees the held buttons (briefing 'fire to skip').
                pc.rawButtons = (frontStack.empty() && world->controlsFrozen()) ? input.buttons(inVehicle, false) : 0u;
                pc.viewAngles = {cam.pitch, cam.yaw, 0};
                pc.stance = ps.stance == game::Stance::Prone ? 2 : ps.stance == game::Stance::Crouch ? 1 : 0;
                pc.viewHeight = ps.viewHeight;
                pc.onGround = ps.onGround;
                pc.weaponState = pweapon.stateNumber();
                pc.grenadeFuseMs = pweapon.fuseMs();
                pc.lean = ps.lean;
                pc.adsFrac = pweapon.adsFrac();
                world->setPlayerCommand(pc);
            }
            // Network game (docs/spec/net.md section 10): this frame's user command goes to the server; the view is the
            // newest snapshot's player state with the unacknowledged commands run again (prediction). On a turret the
            // command carries no move: the server aims and fires the turret from its angles and buttons.
            auto sendNetCommand = [&](float forward, float right) {
                if (!netClient || netClient->state() < net::ClientState::Primed) return;
                const int64_t nowNet = netNowMs();
                // The selected weapon as its number in the weapon list configstring (docs/spec/net.md section 13).
                int weaponByte = 0;
                for (int wi = 1; wi <= netgame::kWeaponConfigstringCount && !weaponSelect.empty(); ++wi)
                    if (netgame::weaponNameFromList(netClient->configstrings, wi) == weaponSelect) {
                        weaponByte = wi;
                        break;
                    }
                // The view the server set (delta_angles, docs/spec/net.md section 10 "View angles"): the drawn view is
                // the command's angles plus the delta, so a new delta turns the view by its change and the commands go
                // out without it.
                if (const net::ClientSnapshot* s0 = netClient->latest()) {
                    const netgame::PsFields f0 = netgame::readPs(s0->ps);
                    const int dp = int16_t((f0.deltaAngles[0] - netDeltaApplied[0]) & 0xffff);
                    const int dy = int16_t((f0.deltaAngles[1] - netDeltaApplied[1]) & 0xffff);
                    cam.pitch = std::clamp(cam.pitch + float(dp) * 360.0f / 65536.0f, -89.0f, 89.0f);
                    cam.yaw += float(dy) * 360.0f / 65536.0f;
                    for (int k = 0; k < 3; ++k) netDeltaApplied[k] = f0.deltaAngles[k];
                }
                net::UserCmd uc = netgame::makeUserCmd(netClient->commandTime(nowNet), buttons, forward, right, cam.pitch,
                                                       cam.yaw, weaponByte);
                for (int k = 0; k < 3; ++k) uc.angles[k] = (uc.angles[k] - netDeltaApplied[k]) & 0xffff;
                netClient->sendCommand(uc, nowNet);
                if (const net::ClientSnapshot* snap = netClient->latest()) {
                    const netgame::PsFields f = netgame::readPs(snap->ps);
                    // The prediction runs the commands with this client's own weapon state, as the server does with its copy.
                    const netgame::WeaponMove wm = netgame::weaponMoveOf(pweapon); // the same inputs the server uses
                    // Following another client (killcam): its player state as sent, none of our commands run on it.
                    // A new connection or gamestate (map change, restart): the prediction starts afresh with the new
                    // level's commands, and so does the event sequence handled.
                    if (netPredictorFor != std::make_pair<const net::GameClient*, int>(netClient.get(), int(netClient->gamestates))) {
                        netPredictorFor = {netClient.get(), netClient->gamestates};
                        netPredictor.reset();
                        netPredictedWeapon.clear();
                        netWeaponSnapshot = -1;
                        lastEventSeq = 0;
                    }
                    // The weapon against the acknowledged player state, once per snapshot: rebuilt from the server's
                    // state when the prediction differs (docs/spec/net.md section 10 "Predicted weapon").
                    if (snap->messageNum != netWeaponSnapshot && world->player()) {
                        netWeaponSnapshot = snap->messageNum;
                        if (!netgame::RemoteView::following(*snap)) {
                            game::Inventory& inv = world->inventory(*world->player());
                            netPredictedWeapon.snapshot(f, netgame::weaponNameFromList(netClient->configstrings, f.weapon), pweapon, inv.clip, inv.ammo);
                        }
                    }
                    // Every frame from the newest snapshot again (CG_PredictPlayerState, 0x4deae0): the same commands give
                    // the same states and event numbers, so an event is handled once; a correction is drawn away.
                    netDrawNowMs = nowNet;
                    ps = netPredictor.predict(world->collision(), f,
                                              netgame::RemoteView::following(*snap) ? std::vector<net::UserCmd>{}
                                                                                    : netClient->commandsAfter(f.commandTime),
                                              &wm, nowNet, world->player() ? world->player()->number : -1);
                    if (netgame::viewInputIgnored(f.pmType)) cam.pitch = f.viewangles[0], cam.yaw = f.viewangles[1]; // the server's angles
                }
            };
            aimSpreadBeforeCmd = ps.aimSpread; // ClientThink keeps ps+0x5b0 / 255 at client +0x8670 (0x4e7c7d) before Pmove; the shot's spread reads that (0x531c85)
            if (onTurret) {
                // The eye is the turret's tag_player (0x4e8b70); the turret code places the player behind it.
                game::Vec3 eye;
                const script::Vec3 po = world->player()->obj->get("origin").asVector();
                float tp = 0, ty = 0;
                playerTurretPose.reset();
                if (world->playerTurretAnglesFor({cam.pitch, cam.yaw, 0}, tp, ty)) {
                    playerTurretPose = std::make_pair(tp, ty);
                    if (world->playerTurretEyeAt(tp, ty, eye)) cam.pos = {eye[0], eye[1], eye[2]};
                } else if (world->playerTurretEye(eye)) {
                    cam.pos = {eye[0], eye[1], eye[2]};
                }
                ps.origin = {po[0], po[1], po[2]};
                ps.velocity = {0, 0, 0};
                if (netClient) sendNetCommand(0.0f, 0.0f);
            } else if (inVehicle) {
                // The view turns with the vehicle's tag_body (cgame 0x4aeb60, cg_viewVehicleInfluence 1): the view axes
                // times Bprevᵀ·Bcur; the pitch always follows, the yaw only while pm_flags 0x40000 is set.
                std::array<game::Vec3, 3> body;
                if (world->playerVehicleBodyAxis(body)) {
                    if (haveTickBodyPrev) { // the body axes between the last two server frames, re-orthonormalized
                        for (size_t i = 0; i < 3; ++i)
                            for (size_t j = 0; j < 3; ++j) body[i][j] = tickBodyPrev[i][j] + (body[i][j] - tickBodyPrev[i][j]) * lerpFrac;
                        auto norm = [](game::Vec3& v) {
                            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                            if (l > 0) v = {v[0] / l, v[1] / l, v[2] / l};
                        };
                        norm(body[0]);
                        const float d = body[1][0] * body[0][0] + body[1][1] * body[0][1] + body[1][2] * body[0][2];
                        body[1] = {body[1][0] - d * body[0][0], body[1][1] - d * body[0][1], body[1][2] - d * body[0][2]};
                        norm(body[1]);
                        body[2] = {body[0][1] * body[1][2] - body[0][2] * body[1][1], body[0][2] * body[1][0] - body[0][0] * body[1][2],
                                   body[0][0] * body[1][1] - body[0][1] * body[1][0]};
                    }
                    if (!havePrevBody) prevBody = body;
                    float d[3][3], v[3][3], m[3][3];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) {
                            d[i][j] = 0;
                            for (int k = 0; k < 3; ++k) d[i][j] += prevBody[size_t(k)][size_t(i)] * body[size_t(k)][size_t(j)];
                        }
                    const Vec3 f = cam.forward(), r = cam.right(), u = cross(r, f);
                    const float rows[3][3] = {{f.x, f.y, f.z}, {-r.x, -r.y, -r.z}, {u.x, u.y, u.z}};
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) {
                            v[i][j] = rows[i][j];
                            m[i][j] = 0;
                        }
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            for (int k = 0; k < 3; ++k) m[i][j] += v[i][k] * d[k][j];
                    cam.pitch = std::clamp(-std::atan2(m[0][2], std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1])) * 180.0f / float(M_PI), -89.0f, 89.0f);
                    if (world->playerPmFlags() & 0x40000) cam.yaw = std::atan2(m[0][1], m[0][0]) * 180.0f / float(M_PI);
                    prevBody = body;
                    havePrevBody = true;
                }
                const script::Vec3 po = world->player()->obj->get("origin").asVector();
                const script::Vec3 pl{prevPlayerO[0] + (po[0] - prevPlayerO[0]) * lerpFrac, prevPlayerO[1] + (po[1] - prevPlayerO[1]) * lerpFrac,
                                      prevPlayerO[2] + (po[2] - prevPlayerO[2]) * lerpFrac};
                cam.pos = {pl[0], pl[1], pl[2] + 60.0f}; // stand view height 60 in a vehicle (0x509d10), at the interpolated origin
                camLinkLerp = true;
                camLinkHeight = 60.0f;
                ps.origin = {po[0], po[1], po[2]};
                ps.velocity = {0, 0, 0};
            } else if (world && world->playerHasLinkParent()) {
                // Linked by script (pm_type 1): the link places the player (src/game/playerlink.cpp); no own movement. PmoveSingle still
                // runs its linked branch (0x50c881): aim command and fraction, stance and view height, lean, hold breath (game::playerMove
                // with ps.pmType 1, docs/spec/pmove.md "Linked player").
                const script::Vec3 po = world->player()->obj->get("origin").asVector();
                ps.origin = {po[0], po[1], po[2]};
                ps.velocity = {0, 0, 0};
                havePrevBody = false;
                {
                    game::PlayerInput in;
                    in.stanceHeld = (buttons & game::Button::StanceHeld) != 0;
                    in.stance = (buttons & game::Button::Prone) ? game::Stance::Prone
                                : (buttons & game::Button::Crouch) ? game::Stance::Crouch : game::Stance::Stand;
                    in.leanLeft = (buttons & game::Button::LeanLeft) != 0 && !(world->playerPmFlags() & 0x2000000u);
                    in.leanRight = (buttons & game::Button::LeanRight) != 0 && !(world->playerPmFlags() & 0x4000000u);
                    in.yaw = cam.yaw;
                    in.pitch = cam.pitch;
                    in.dead = playerDead;
                    ps.pmType = world->playerPmType();
                    in.controlsFrozen = world->playerControlsFrozen();
                    in.adsHeld = (buttons & game::Button::Ads) != 0;
                    in.adsAllowed = pweapon.adsAllowed();
                    in.adsFrac = pweapon.adsFrac();
                    in.weaponState = pweapon.stateNumber();
                    in.holdBreathHeld = !controlsBlocked && (buttons & game::Button::HoldBreath) != 0;
                    {
                        const std::string ret = pweapon.field("adsOverlayReticle");
                        breathWeapon = pweapon.equipped() && !ret.empty() && ret != "none" && pweapon.field("weaponClass") != "item";
                        in.breathWeapon = breathWeapon;
                    }
                    game::MoveParams lmp = mp;
                    if (world->player()) lmp.passEntity = world->player()->number;
                    if (pweapon.equipped()) {
                        auto n = [&](const char* k) { return pweapon.number(k); };
                        lmp.hipSpread = {n("hipSpreadStandMin"), n("hipSpreadDuckedMin"), n("hipSpreadProneMin"), n("hipSpreadMax"),
                                         n("hipSpreadDuckedMax"), n("hipSpreadProneMax"), n("hipSpreadDecayRate"), n("hipSpreadFireAdd"),
                                         n("hipSpreadTurnAdd"), n("hipSpreadMoveAdd"), n("hipSpreadDuckedDecay"), n("hipSpreadProneDecay")};
                    }
                    if (scene.collision) game::playerMove(world->collision(), ps, in, std::min(simDt, 0.1f), lmp);
                    ps.origin = {po[0], po[1], po[2]}; // the link's placement, whatever the step did
                    ps.velocity = {0, 0, 0};
                    world->setPlayerPmFlag(0x4000, ps.holdingBreath);
                    world->setPlayerPmFlag(1, ps.stance == game::Stance::Prone);
                    world->setPlayerPmFlag(2, ps.stance == game::Stance::Crouch);
                }
                cam.pos = {po[0], po[1], po[2] + ps.viewHeight};
                camLinkLerp = true;
                camLinkHeight = ps.viewHeight;
            } else if (walk && world) {
                camLinkLerp = false;
                if (havePrevBody) { // just got out: continue from where the vehicle code put the player (tag_detach)
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    ps.origin = {po[0], po[1], po[2]};
                }
                havePrevBody = false;
                game::PlayerInput in;
                in.forward = controlsBlocked ? 0.0f : float(input.forwardMove()) / 127.0f;
                if (const char* df = std::getenv("IW2_DEBUG_FORWARD"); df && !controlsBlocked) { // debug: "forward[,yaw deg/s]" holds forward and turns
                    in.forward = float(std::atof(df));
                    if (const char* c = std::strchr(df, ',')) cam.yaw += float(std::atof(c + 1)) * std::min(simDt, 0.1f);
                }
                in.right = controlsBlocked ? 0.0f : float(input.rightMove()) / 127.0f;
                in.jump = (buttons & game::Button::Jump) != 0;
                in.stanceHeld = (buttons & game::Button::StanceHeld) != 0; // 0x2000 (0x50a1f5)
                in.stance = (buttons & game::Button::Prone) ? game::Stance::Prone
                            : (buttons & game::Button::Crouch) ? game::Stance::Crouch : game::Stance::Stand;
                // allowleanleft/right(false) set pm_flags 0x2000000/0x4000000, which cancel the lean (0x50ae13).
                in.leanLeft = (buttons & game::Button::LeanLeft) != 0 && !(world->playerPmFlags() & 0x2000000u);
                in.leanRight = (buttons & game::Button::LeanRight) != 0 && !(world->playerPmFlags() & 0x4000000u);
                in.yaw = cam.yaw;
                in.dead = playerDead;
                // ps+4 (ClientThink 0x4e7bc0: noclip 2, ufo 3, linked 1, dead 4 + linked), ps+0xc & 0x8000 and ps+0x7c & 0x20300 for the lean gates (0x50ade0).
                ps.pmType = world->playerPmType(); // noclip 2, ufo 3, else by health and link: 0 / 1 / 4 / 5
                in.controlsFrozen = world->playerControlsFrozen();
                in.eFlagsNoLean = inVehicle || world->playerOnTurret();
                in.adsHeld = (buttons & game::Button::Ads) != 0;
                in.adsAllowed = pweapon.adsAllowed();
                in.weaponState = pweapon.stateNumber();
                in.holdBreathHeld = !controlsBlocked && (buttons & game::Button::HoldBreath) != 0;
                {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    breathWeapon = pweapon.equipped() && !ret.empty() && ret != "none" && pweapon.field("weaponClass") != "item";
                    in.breathWeapon = breathWeapon;
                }
                // Fixed 20 Hz-style substeps keep movement stable at any frame rate.
                float remaining = netClient ? 0.0f : std::min(simDt, 0.1f); // network game: predicted below
                // MP: intermission does not move; a following spectator shows the target's view (below); a free spectator flies with
                // the spectator move (pm_type 4), speed 400 with free look allowed, else 0, times player_spectateSpeedScale.
                if (mpSession == 3 || mpFollow >= 0) remaining = 0.0f;
                // With a running level the player clips against the world and its solid entities (brush models,
                // actors) like G_Trace in the original pmove (0x507b00 → 0x4db7f0 → 0x44bf00), skipping itself.
                // A multiplayer map moves with the MP dvar defaults (docs/spec/mp-pmove.md).
                game::MoveParams pmp = world && world->multiplayer() ? game::multiplayerMoveParams() : mp;
                if (world && world->player()) pmp.passEntity = world->player()->number;
                pmp.mantle = &mantleAnims;
                if (mpSession == 2) {
                    pmp.mpSpectator = true;
                    pmp.spectateSpeed = world->mpSpectateAllowed(world->mpLocalClient(), 4) ? 400.0f : 0.0f;
                    const std::string sss = menuEnv.getCvar("player_spectateSpeedScale");
                    pmp.spectateSpeedScale = sss.empty() ? 2.0f : std::clamp(float(std::atof(sss.c_str())), 0.0f, 20.0f);
                    pmp.traceMask = 0x800811u;
                }
                // The weapon's moveSpeedScale (weapon def +0x264) scales the walk (0x508140).
                if (pweapon.equipped()) pmp.weaponSpeedScale = pweapon.number("moveSpeedScale");
                if (world && world->shellShockRecord()) { // usercmd 0x800 (0x409c82): cgame flag 0x621560 = shock running (0x4b24db)
                    const auto& ss = world->shellShock();
                    const int64_t el = world->timeMs() - ss.startMs; // [server time] pmove input, simulation
                    pmp.shellShockSlow = ss.startMs != 0 && el >= 0 && el < ss.durationMs;
                }
                if (pweapon.equipped()) {
                    auto n = [&](const char* k) { return pweapon.number(k); };
                    pmp.hipSpread = {n("hipSpreadStandMin"), n("hipSpreadDuckedMin"), n("hipSpreadProneMin"), n("hipSpreadMax"),
                                     n("hipSpreadDuckedMax"), n("hipSpreadProneMax"), n("hipSpreadDecayRate"), n("hipSpreadFireAdd"),
                                     n("hipSpreadTurnAdd"), n("hipSpreadMoveAdd"), n("hipSpreadDuckedDecay"), n("hipSpreadProneDecay")};
                }
                in.pitch = cam.pitch;
                in.adsFrac = pweapon.adsFrac();
                const game::CollisionWorld& pcw = world ? world->collision() : *scene.collision; // null reference only in the front end, where it is unused
                float pmoveChunkLeft = 0; // Pmove (0x50cc10) runs one PmoveSingle per usercmd of up to 66 ms: the dead move's fixed speed drop (0x508a10) is per such step
                while (remaining > 0 && scene.collision) {
                    const float step = std::min(remaining, 1.0f / 60.0f);
                    pmp.deadSpeedStep = pmoveChunkLeft <= 0;
                    if (pmp.deadSpeedStep) pmoveChunkLeft = 0.066f;
                    pmoveChunkLeft -= step;
                    game::RecordedMove rec{ps, {}, in, step, pmp, pcw.entityClips()};
                    game::playerMove(pcw, ps, in, step, pmp);
                    // Mantle stance events (0x8d crouch, 0x8c stand) set the client stance.
                    if (ps.stanceEvent >= 0) input.setStance(ps.stanceEvent);
                    rec.after = ps;
                    moveRecorder.add(std::move(rec));
                    remaining -= step;
                }
                sendNetCommand(in.forward, in.right);
                if (world) {
                    for (const game::KnockbackHit& hit : world->takePlayerKnockbacks()) game::applyKnockback(ps, hit, world->cvarFloat("g_knockback", 1000.0f));
                    world->setPlayerPmFlag(0x4000, ps.holdingBreath); // hold breath, read by the HUD hint
                    // Stance bits the HUD stance icon reads (pm_flags & 3, 0x4a80f0): 1 prone, 2 ducked.
                    world->setPlayerPmFlag(1, ps.stance == game::Stance::Prone);
                    world->setPlayerPmFlag(2, ps.stance == game::Stance::Crouch);
                }
                // The step's player-state events (footsteps, landings): sounds and fall damage. Only the last 4 kept.
                if (world) {
                    lastEventSeq = game::resyncEventSeq(ps.eventSeq, lastEventSeq);
                    for (; lastEventSeq < ps.eventSeq; ++lastEventSeq) {
                        world->applyPlayerEvent(ps.events[lastEventSeq & 3], ps.eventParms[lastEventSeq & 3]);
                        if (ps.events[lastEventSeq & 3] == 0x8f) viewImp.step(ps.eventParms[lastEventSeq & 3], cgNowMs()); // CG_EntityEvent 0x4bac84
                        // Events 0x8d / 0x8e with parm 1 / 2 (the blocked stance changes of 0x509d10): the hint states 4 / 5 (0x4ba880).
                        const int ev = ps.events[lastEventSeq & 3], parm = ps.eventParms[lastEventSeq & 3];
                        input.stanceEvent(ev); // the wish follows the event, so a blocked rise is not asked for again
                        if ((ev == 0x8d || ev == 0x8e) && (parm == 1 || parm == 2)) setInvalidCmd(3 + parm, cgNowMs());
                        if (ev == 0x95) input.clearAds(); // 0x4dd090 (the prone aim cancel): the cgame clears the toggled aim (0x4bafbc)
                    }
                }
                // Nothing under the player for 4000 units: it fell out of the world. Keep the moves that led there.
                if (!fellOut && (world || scene.collision)) {
                    const game::TraceResult under = pcw.trace(ps.origin, {ps.origin[0], ps.origin[1], ps.origin[2] - 4000}, {0, 0, 0},
                                                              {0, 0, 0}, pmp.traceMask, pmp.passEntity);
                    if (under.fraction == 1 && !under.startSolid) {
                        fellOut = true;
                        if (moveRecorder.write("pmove_replay.bin"))
                            std::cout << "player fell out of the world; wrote pmove_replay.bin (iw2game <main> <map> --replay pmove_replay.bin)\n";
                    }
                }
                mantleHint = (ps.mantleFlags & 8) != 0;
                // PM_Weapon (0x4dfce0): crawling or meleeing keeps the aim spread at its maximum.
                if ((ps.stance == game::Stance::Prone && (in.forward != 0 || in.right != 0)) || pweapon.stateNumber() == 10 ||
                    pweapon.stateNumber() == 11)
                    ps.aimSpread = 255.0f;
                cam.yaw = game::mantleClampYaw(ps, cam.yaw); // the view yaw cap while mantling (0x525b60)
                cam.pos = {ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeight};
                if (netClient && netClient->latest()) { // the remaining part of a prediction correction (cg_errorDecay)
                    const game::Vec o = netPredictor.drawOrigin(netDrawNowMs);
                    cam.pos = {o[0], o[1], o[2] + ps.viewHeight};
                    // IW2_PRED_TRACE=1: the own player's prediction per rendered frame (docs/debug-flags.md).
                    if (static const bool predTrace = std::getenv("IW2_PRED_TRACE") != nullptr; predTrace) {
                        static int64_t lastRealMs = 0;
                        const int64_t realMs = int64_t(SDL_GetTicksNS() / 1000000);
                        const net::ClientSnapshot& sn = *netClient->latest();
                        const netgame::PsFields sf = netgame::readPs(sn.ps);
                        const net::UserCmd* lc = netClient->lastCommand();
                        const net::ClientSnapshot *ia = nullptr, *ib = nullptr;
                        float ifrac = 0;
                        const int rtime = netClient->renderTime(netDrawNowMs);
                        const bool interp = netClient->interpolation(rtime, ia, ib, ifrac);
                        const std::vector<net::UserCmd> replay = netClient->commandsAfter(sf.commandTime);
                        std::printf("predtrace rt=%lld dt=%lld now=%lld cmd=%d snap=%d msg=%d ack=%d srv=%.2f,%.2f,%.2f pred=%.2f,%.2f,%.2f "
                                    "miss=%.2f draw=%.2f,%.2f,%.2f render=%d frac=%.2f replay=%d first=%d fwd=%d snaps=%d dropped=%d yaw=%.1f pitch=%.1f%s\n",
                                    (long long)realMs, (long long)(lastRealMs ? realMs - lastRealMs : 0), (long long)netDrawNowMs,
                                    lc ? lc->serverTime : 0, sn.serverTime, sn.messageNum, sf.commandTime, double(sf.origin[0]),
                                    double(sf.origin[1]), double(sf.origin[2]), double(ps.origin[0]), double(ps.origin[1]),
                                    double(ps.origin[2]), double(netPredictor.lastMiss()), double(o[0]), double(o[1]), double(o[2]),
                                    rtime, double(ifrac), int(replay.size()), replay.empty() ? 0 : replay.front().serverTime,
                                    lc ? int(lc->forwardmove) : 0, netClient->snapshots, netClient->droppedSnapshots,
                                    double(cam.yaw), double(cam.pitch),
                                    interp ? "" : " nointerp");
                        lastRealMs = realMs;
                    }
                }
                if (netFollow) { // network killcam: the followed client's view the server sent (0x80f6916)
                    cam.pos = {netFollowEye[0], netFollowEye[1], netFollowEye[2]};
                    cam.pitch = netFollowAngles[0];
                    cam.yaw = netFollowAngles[1];
                }
                if (mpFollow >= 0) { // following: the target's view (its player state is copied over the spectator's, 0x80f6916)
                    script::Vec3 eye, ang;
                    if (world->mpClientView(mpFollow, eye, ang)) {
                        cam.pos = {eye[0], eye[1], eye[2]};
                        cam.pitch = ang[0];
                        cam.yaw = ang[1];
                    }
                }
                // Dead view (cgame 0x4ae2f5: pm_type ≥ 4): pitch −15, roll 40, yaw the player state's dead yaw
                // (ps+0x10c). TODO(spec): [writer of ps+0x10c not found] that yaw (set at death towards the killer); the current yaw is kept.
                // The command's view angles are not applied while dead (0x50b12b), so the yaw stays where it was.
                // MP instead: a dead movement type (6 / 7) turns the third-person view on (0x4cfcb0 in the MP executable) and the
                // camera orbits behind the body (applied below, after the lean; docs/spec/mp-spectate.md "Dead view").
                const bool mpDeadThirdPerson = playerDead && world && world->multiplayer();
                if (playerDead) {
                    if (!wasDead) deadYaw = world->playerDeadYaw() ? *world->playerDeadYaw() : cam.yaw; // ps+0x10c (0x4ae314)
                    if (!wasDead) deadPitch = cam.pitch; // the view angles stay those of the death (0x50b12b)
                    if (!mpDeadThirdPerson) {
                        cam.yaw = deadYaw;
                        cam.pitch = -15.0f;
                        cam.roll = 40.0f;
                    } else {
                        cam.roll = 0;
                    }
                } else {
                    cam.roll = 0;
                }
                wasDead = playerDead;
                // Camera bob (0x4ae290): up by the bob wave, sideways along the right vector (pitch/yaw/roll of the view).
                // Landing dip (0xf790a0, viewImp.land), step offset and the minimum eye height (origin z + 8, 0x592010; applied below)
                // are implemented; the damage kick is applied to the view angles further down (0x4ae371).
                // TODO(spec): [branch not read] whether the 0x4ae775 minimum also applies while dead (it is skipped here).
                if (!playerDead) {
                    const float xy = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]);
                    const auto vb = game::viewBob(ps.bobCycle, xy, ps.viewHeightTarget, pweapon.adsFrac(), pweapon.number("adsViewBobMult"));
                    bobAngles = {vb[2], vb[3]};
                    const Vec3 rgt = cam.right();
                    cam.pos = cam.pos + Vec3{0, 0, vb[0]} + rgt * vb[1];
                    if (world) {
                        if (ps.landSeq != lastLandSeq) { // a landing (cgame 0x4bab6e / 0x4bab15: the own client's fall event)
                            lastLandSeq = ps.landSeq;
                            const game::MoveParams lp = world->multiplayer() ? game::multiplayerMoveParams() : game::MoveParams{};
                            viewImp.land(ps.landHeight, ps.landDamage, cgNowMs(), lp.fallDamageMinHeight, lp.fallDamageMaxHeight);
                        }
                        cam.pos.z += viewImp.landOffset(cgNowMs()); // 0x4ae6c7
                        cam.pos.z -= viewImp.stepOffset(cgNowMs()); // 0x4addb0 (step event 0x8f)
                    }
                }
                // Lean moves the view origin sideways and down (0x4ae770); the view itself does not roll.
                const game::Vec lo = game::leanOffset(cam.yaw, ps.lean);
                cam.pos = cam.pos + Vec3{lo[0], lo[1], lo[2]};
                if (!playerDead) cam.pos.z = std::max(cam.pos.z, game::ViewImpulses::minEyeZ(ps.origin[2])); // 0x4ae775
                if (mpDeadThirdPerson) { // MP 0x4ce890: cg_thirdPersonRange 120, cg_thirdPersonAngle 0, box +-4, mask 0x811
                    const int pass = pmp.passEntity;
                    const game::ThirdPersonView tv = game::thirdPersonView(
                        {cam.pos.x, cam.pos.y, cam.pos.z}, deadPitch, deadYaw, 0.0f, deadYaw, 120.0f, 0.0f,
                        [&](const game::Vec& a, const game::Vec& b, const game::Vec& mn, const game::Vec& mx) {
                            return pcw.trace(a, b, mn, mx, game::kThirdPersonMask, pass);
                        });
                    cam.pos = {tv.origin[0], tv.origin[1], tv.origin[2]};
                    cam.pitch = tv.pitch, cam.yaw = tv.yaw, cam.roll = tv.roll;
                }
            } else {
                const float speed = (keys[SDL_SCANCODE_LSHIFT] ? 1200.0f : 350.0f) * dt;
                Vec3 move;
                if (keys[SDL_SCANCODE_W]) move = move + cam.forward();
                if (keys[SDL_SCANCODE_S]) move = move + cam.forward() * -1.0f;
                if (keys[SDL_SCANCODE_D]) move = move + cam.right();
                if (keys[SDL_SCANCODE_A]) move = move + cam.right() * -1.0f;
                if (keys[SDL_SCANCODE_SPACE]) move = move + Vec3{0, 0, 1};
                if (keys[SDL_SCANCODE_C]) move = move + Vec3{0, 0, -1};
                cam.pos = cam.pos + normalize(move) * speed;
            }

            hudAimSpread = ps.aimSpread; // on every branch: the crosshair of a linked player too
            hudViewHeight = ps.viewHeight;
            if (world && world->player() && pendingF10) {
                pendingF10 = false;
                if (world->godOn()) { if (world->cheatGod()) world->cheatDemigod(); } // god -> demigod (the gate was checked by the first call)
                else if (world->demigodOn()) world->cheatDemigod();                    // demigod -> off
                else world->cheatGod();                                                // off -> god
            }
            if (world && world->player() && (pendingGod || pendingDemigod || pendingNotarget)) {
                if (pendingGod) world->cheatGod();
                if (pendingDemigod) world->cheatDemigod();
                if (pendingNotarget) world->cheatNotarget();
                pendingGod = pendingDemigod = pendingNotarget = false;
            }
            if (world && world->player() && (pendingNoclip || pendingUfo)) { // cheat commands noclip / ufo (0x4ea9b1 / 0x4ea9d8)
                if (pendingNoclip) world->cheatNoclip();
                if (pendingUfo) world->cheatUfo();
                pendingNoclip = pendingUfo = false;
            }
            if (world) {
                runConsoleCommands();
                syncWeapon();
                pweapon.grenadeCook = world->cvarFloat("iw2_grenadeCook", 1.0f) != 0; // openiw2 option (docs/debug-flags.md)
                game::PlayerWeapon::Input wi;
                // weapnext / weapprev (cgame 0x4b6fd0 / 0x4b7030 -> 0x4b7410): the weapon in the other of the two slots. Rounds do not
                // count (both commands pass 0). The walk over weapons outside the slots (0x4b72f0) finds nothing: only item, turret and
                // non-player classes have no slot, and no shipped SP script gives the player a turret or non-player weapon.
                if (const int cycle = input.takeWeaponCycle(); cycle && world->player() &&
                    game::weaponCycleAllowed(menuEnv.getCvar("cl_paused") == "1", world->playerControlsFrozen() ? 0x8000u : 0u, world->playerPmFlags(), world->timeMs(), lastWeaponCycleMs, // [server time]
                                             int(world->cvarFloat("cg_weaponCycleDelay", 0.0f)))) {
                    lastWeaponCycleMs = world->timeMs(); // [server time]
                    const game::Inventory& inv = world->inventory(*world->player());
                    const int cur = inv.slot[2] == weaponSelect ? 2 : 1;
                    const std::string& other = inv.slot[cur == 1 ? 2 : 1];
                    if (!other.empty()) weaponSelect = other;
                }
                wi.fire = (buttons & game::Button::Attack) != 0;
                // The aim fraction follows pm_flags 0x40 (0x4dd0d0), which pmove sets from the aim command (0x4dd020).
                wi.ads = walk && !inVehicle && !onTurret ? ps.ads : (buttons & game::Button::Ads) != 0;
                wi.reload = reloadPressed;
                wi.melee = (buttons & game::Button::Melee) != 0;
                wi.frag = (buttons & game::Button::Frag) != 0;
                wi.smoke = (buttons & game::Button::Smoke) != 0;
                // A new selected weapon ends the toggled aim unless it is the current weapon's altWeapon (cgame 0x4b8360).
                if (weaponSelect != prevSelect) {
                    if (netClient) netSelectUntilMs = SDL_GetTicks() + 2000;
                    if (!prevSelect.empty() && !(pweapon.name() == prevSelect && pweapon.field("altWeapon") == weaponSelect))
                        input.clearAds();
                    // Selecting a weapon (0x4b8360) stamps the weapon-change time 0xf796e4 and touches the weapon-info fade (0xf79a40, via 0x4a6130 case 4)
                    // at that moment, so the ammo and name displays (which follow the selection) show for their fade window from the key press.
                    if (!weaponSelect.empty()) hudFade.ammo = hudFade.weaponChange = cgNowMs();
                    prevSelect = weaponSelect;
                }
                wi.weapon = weaponSelect;
                wi.holster = game::moveLowersWeapon(ps, inVehicle); // 0x4de880: mantle, ladder (0x4de91b), vehicle
                wi.proneMoving = ps.stance == game::Stance::Prone && (input.forwardMove() != 0 || input.rightMove() != 0);
                wi.binoculars = (buttons & game::Button::Binoculars) != 0;
                wi.use = (buttons & (game::Button::Activate | game::Button::UseReload)) != 0;
                // PM_Weapon 0x4dfc30: pm_type >= 4 (dead, spectator, intermission) clears the weapon and does nothing else.
                // Following another client (the killcam): no prediction (0x4deae0), the view weapon is the followed one.
                {
                    const int pmt = netClient ? ps.pmType : mpSession == 2 ? 4 : mpSession == 3 ? 5 : 0;
                    wi.dead = game::weaponInputBlocked(playerDead, pmt, netClient && !netgame::RemoteView::ownWeaponRuns(netClient->latest()));
                }
                wi.inVehicle = inVehicle;
                wi.offhandBlocked = inVehicle; // eFlags 0x20300 (0x4df473); the turret skips PM_Weapon altogether (0x4dfc59)
                {
                    wi.damaged = world->playerDamageFeedback().event != binocDamageEvent;
                    const auto sx = world->cvars.find("player_scopeExitOnDamage");
                    wi.scopeExitOnDamage = sx != world->cvars.end() && std::atoi(sx->second.c_str()) != 0;
                    binocDamageEvent = world->playerDamageFeedback().event;
                }
                if (inVehicle) // the weapon fire mask excludes attack, melee, reload and the offhands in a vehicle (0x4df9b0: 0x30014)
                    wi.fire = wi.melee = wi.reload = wi.frag = wi.smoke = false;
                if (onTurret) pweapon.blockOffhand();
                // The weapon step of this frame's command (sent above), in the command's own time and pieces, as the server
                // runs it; the shots are the fire events to play (docs/spec/net.md section 10 "Predicted weapon"). The local
                // game has no commands: one step per frame, at most 100 ms.
                int shots = 0;
                const net::UserCmd* weaponCmd = netClient && world->player() ? netClient->lastCommand() : nullptr;
                if (onTurret) {
                    if (weaponCmd) shots = netPredictedWeapon.idle(weaponCmd->serverTime);
                } else if (weaponCmd) {
                    const game::Inventory& inv = world->inventory(*world->player());
                    shots = netPredictedWeapon.command(weaponCmd->serverTime, wi, pweapon, inv.clip, inv.ammo);
                } else {
                    shots = pweapon.update(std::min(simDt, 0.1f), wi);
                }
                // The host's recorded view state (docs/spec/mp-hud.md "The recorded view state"): what its killcam shows.
                if (world->multiplayer() && !netClient) world->mpSetLocalViewState(game::mpViewStateOf(pweapon, ps));
                if (shots > 0) game::breathOnFire(ps, mp, pweapon.adsFrac(), breathWeapon); // 0x4dee18 -> 0x4dd2b0
                if (aimBlendWeapon != pweapon.name()) { aimBlendWeapon = pweapon.name(); aimBlend.reset(); }
                aimBlend.update(pweapon.viewAiming(), std::min(simDt, 0.1f));
                writeWeapon();
                {
                    // HUD fade touches. 0x4a6130(n) touches one HUD menu's fade time (menu flag +0xe8 |= 4): 0 Health (0xf79a3c),
                    // 1 weaponinfo (0xf79a40) + offhand, 2 Compass (0xf79a38), 3 stance (0xf79a44), 4 offhandinfo (0xf79a48),
                    // 5 objectiveinfo (0xf79200); the console command menu_show_notify <n> (0x4a70ff) calls it with n.
                    // Weapon change (0x4b76e8 / 0x4b839a): touch(4) + ammo display + weapon change time. Per frame 0x4be5f0:
                    //  - 0x4be520: attack (0x1), or reload / use-reload (0x30) while not mantling (pm_flags 4, 0xf708f4) and with
                    //    eFlags & 0x300 clear, touches offhand and weaponinfo (0x4be556) and touches the compass;
                    //  - 0x4be440: a change of the buttons 0x2300 touches stance and the compass, held ones touch stance only;
                    //  - the offhand buttons 0x30000 touch offhandinfo and the compass;
                    //  - the compass also for view angle / other button (0xffffd4ff) changes and movement.
                    // TODO(spec): the second reload condition (bit 2 of the dword at (0xf5bd24 -> previous snapshot) + 0x580, 0x4be545).
                    const int64_t nowMs = cgNowMs();
                    if (pweapon.name() != hudFade.weapon) {
                        if (!pweapon.name().empty()) hudFade.ammo = hudFade.weaponChange = hudFade.offhand = nowMs;
                        hudFade.weapon = pweapon.name();
                    }
                    // 0x4a6220 (called from 0x4bfa50 / 0x4bfbb7 while cl_paused && cg_drawpaused) re-touches the HUD fade timestamps every
                    // frame so the HUD stays visible in the pause menu. Wired: compass 0xf79a38, weaponinfo 0xf79a40, stance 0xf79a44,
                    // offhandinfo 0xf79a48. TODO(spec): [owner of the stamp unidentified] 0xf79a3c and the remaining stamps it writes (health bar owner unidentified).
                    if (menuEnv.getCvar("cl_paused") == "1" && menuEnv.getCvar("cg_drawpaused") != "0")
                        hudFade.compass = hudFade.ammo = hudFade.stance = hudFade.offhand = nowMs;
                    const bool turretBits = world && world->playerOnTurret(); // 0xf70964 & 0x300
                    const bool fireTouch = !turretBits && ((buttons & game::Button::Attack) != 0 || ((buttons & 0x30u) != 0 && !ps.mantling));
                    const bool viewMoved = cam.pitch != hudFade.pitch || cam.yaw != hudFade.yaw;
                    hudFade.pitch = cam.pitch, hudFade.yaw = cam.yaw;
                    if (fireTouch) hudFade.ammo = hudFade.offhand = nowMs;
                    const uint32_t changed = buttons ^ hudFade.buttons;
                    bool stanceTouch = false;
                    if ((changed & 0x2300u) != 0) hudFade.stance = nowMs, stanceTouch = true;
                    else if ((buttons & 0x2300u) != 0) hudFade.stance = nowMs;
                    if ((buttons & 0x30000u) != 0) hudFade.offhand = nowMs;
                    if (fireTouch || stanceTouch || viewMoved || input.forwardMove() != 0 || input.rightMove() != 0 ||
                        (changed & 0xffffd4ffu) != 0 || (buttons & 0x30000u))
                        hudFade.compass = nowMs;
                    hudFade.buttons = buttons;
                    if (std::getenv("IW2_STANCE_TRACE")) { // debug: stance and hold breath per frame, every 250 ms (docs/debug-flags.md)
                        static int64_t nextTrace = 0;
                        if (nowMs >= nextTrace) {
                            nextTrace = nowMs + 250;
                            std::cout << "stancetrace t=" << nowMs << " menu=" << !frontStack.empty() << " buttons=0x" << std::hex << buttons << std::dec
                                      << " stance=" << int(ps.stance) << " ads=" << pweapon.adsFrac() << " breath=" << ps.holdingBreath
                                      << " breathScale=" << ps.breathScale << " net=" << (netClient != nullptr) << "\n";
                        }
                    }
                    // +scores / -scores (0x4a4d40 / 0x4a4d20): each change of the objectives key stamps the touch time 0xf79200.
                    if (!std::getenv("IW2_SHOT_OBJECTIVES") && input.objectivesHeld() != hudFade.objectivesHeld) hudFade.objectivesHeld = input.objectivesHeld(), hudFade.objectives = nowMs;
                }
                for (const auto& pe : world->takePickupEvents()) { // events 0x90 / 0x91 for the local client (0x4bad52 -> 0x4ba680)
                    const int64_t nowMs = cgNowMs();
                    // The selected offhand 0xf79700 is only ever written with a weapon (0x4b8ba0), so "none" is the first pickup of a level;
                    // the primary state 0xf796e0 is weaponSelect.
                    switch (game::pickupHudAction(pe.itemClass, pe.offhand, !weaponSelect.empty(), !cgOffhand.empty())) {
                    case game::PickupHudAction::SelectPrimary: // 0x4b8360
                        weaponSelect = pe.weapon;
                        hudFade.ammo = hudFade.weaponChange = hudFade.offhand = nowMs;
                        break;
                    case game::PickupHudAction::TouchOffhand: hudFade.offhand = nowMs; break; // 0x4b8ba0
                    case game::PickupHudAction::None: break;
                    }
                }
                if (world->multiplayer() && !netClient) { // the host's offhand prepare event (entering state 13, 0x4df1b4)
                    if (pweapon.stateNumber() == 13 && hostWeaponStatePrev != 13) world->mpPushLocalClientEvent(game::mpev::kPrepOffhand, pweapon.viewWeaponName());
                    hostWeaponStatePrev = pweapon.stateNumber();
                }
                if (world->player()) cgOffhand = world->inventory(*world->player()).offhand;
                for (const auto& e : pweapon.takeEvents()) {
                    const game::Vec3 eye{cam.pos.x, cam.pos.y, cam.pos.z}, angles{cam.pitch, cam.yaw, 0};
                    if (world->multiplayer() && !netClient) { // the host's entity events (its puppet and its killcam replay)
                        if (e.kind == game::PlayerWeapon::Event::ReloadBegin && !e.weapon.empty())
                            world->mpPushLocalClientEvent(pweapon.clip() == 0 ? game::mpev::kReloadFromEmpty : game::mpev::kReload, e.weapon);
                        else if (e.kind == game::PlayerWeapon::Event::MeleeSwing) world->mpPushLocalClientEvent(game::mpev::kMeleeSwipe, e.weapon);
                        else if (e.kind == game::PlayerWeapon::Event::MeleeHit) world->mpPushLocalClientEvent(game::mpev::kFireMelee, e.weapon);
                        else if (e.kind == game::PlayerWeapon::Event::OffhandThrow) world->mpPushLocalClientEvent(game::mpev::kUseOffhand, e.weapon);
                    }
                    if (e.kind == game::PlayerWeapon::Event::ReloadBegin) { input.clearAds(); if (e.weapon.empty()) ps.ads = false; } // event 0x95 (0x4bafbc); 0x4dd090 (the empty-named ones) also clears pm_flags 0x40
                    else if (e.kind == game::PlayerWeapon::Event::EjectBrass) viewFlash->brassPending = true; // event 0xa2 (0x4bb113)
                    else if (e.kind == game::PlayerWeapon::Event::WeaponRaised) // aim spread on a weapon change (0x4dddd2 / 0x4ddecd / 0x4dde9f)
                        ps.aimSpread = e.alt ? std::max(ps.aimSpread, 128.0f) : 255.0f;
                    else if (e.kind >= game::PlayerWeapon::Event::BinocularEnter && e.kind <= game::PlayerWeapon::Event::BinocularDrop) {
                        static const char* const kBinocNotify[] = {"binocular_enter", "binocular_exit", "binocular_fire", "binocular_release",
                                                                   "binocular_drop"}; // 0xa8..0xac (ClientEvents 0x4e76e0..0x4e7740)
                        world->notifyPlayer(kBinocNotify[e.kind - game::PlayerWeapon::Event::BinocularEnter]);
                    }
                    else if (e.kind == game::PlayerWeapon::Event::OutOfAmmo && world->player()) { // event 0x92 (0x4baec1 -> 0x4b7090)
                        const std::string next = world->outOfAmmoSwitch(world->inventory(*world->player()), e.weapon);
                        if (!next.empty()) weaponSelect = next;
                    }
                    else if (e.kind == game::PlayerWeapon::Event::MeleeHit) world->playerMelee(eye, angles);
                    else if (e.kind == game::PlayerWeapon::Event::OffhandThrow)
                        world->playerThrowGrenade(e.weapon, eye, angles, ps.velocity, pweapon.takeFuseMs());
                    else if (e.kind == game::PlayerWeapon::Event::OffhandExplode && world->player()) // event 200 (0x4e775d)
                        world->playerGrenadeInHand(eye, e.weapon);
                    else if (e.kind == game::PlayerWeapon::Event::NoAmmo) setInvalidCmd(1, cgNowMs()); // event 0x92 (0x4baeb6)
                    else if (e.kind == game::PlayerWeapon::Event::NoOffhand) { // events 0x94 (touch offhand) + 0xc9 / 0xca (0x4baf06)
                        hudFade.offhand = cgNowMs();
                        setInvalidCmd(e.weapon == "Frag Grenade" ? 2 : 3, cgNowMs());
                    }
                }
                float shockRatio = 0;
                if (world)
                    if (const game::ShockRecord* rec = world->shellShockRecord()) {
                        const auto& ss = world->shellShock();
                        shockRatio = game::shellShockRatio(int(ss.startMs + ss.durationMs - cgNowMs()), rec->viewKickFadeMs);
                    }
                // Shell shock sounds (0x4b1f90): the loop (blended to its silent twin over the last loopFade ms) and the end sound.
                if (world) {
                    const game::ShockRecord* rec = world->shellShockRecord();
                    const auto& ss = world->shellShock();
                    if (!rec || int(ss.startMs) != clientSnd.shockSeenStart) {
                        if (clientSnd.shockLoop) mixer.stop(clientSnd.shockLoop), clientSnd.shockLoop = 0;
                        clientSnd.shockSeenStart = rec ? int(ss.startMs) : -1;
                    }
                    switch (shockEnd.update(rec && rec->sound, int(cgNowMs()), int(ss.startMs), rec ? game::shockEndSoundMs(*rec, ss.durationMs) : 0)) {
                    case game::ShockEndSound::Action::PlayEnd: mixer.play("shellshock_end", std::nullopt, nullptr, audio::PlayOptions{0x7ff}); break; // TODO(spec): [mixer has no start offset] the start offset (shockEnd.offsetMs())
                    case game::ShockEndSound::Action::PlayAbort: mixer.play("shellshock_end_abort", std::nullopt, nullptr, audio::PlayOptions{0x7ff}); break; // null.wav, replaces the end sound (same owner + channel)
                    case game::ShockEndSound::Action::None: break;
                    }
                    {
                        // Channel volumes: the sound system keeps 5 volume slots (0x793a34, 0x88 bytes each) and uses the highest
                        // active one (0x793cdc; set 0x4431f0, clear 0x443390, fade 0 = 1 ms). Shell shock writes slot 4 (0x4b2237)
                        // while its record has cg_shock_sound and the fraction is > 0, else clears it (0x4b1fa0 / 0x4b1ffc); the
                        // hold-breath blend writes slot 2 (0x4b682a) and clears it at blend 0 (0x4b671d); none active = 1.
                        const int el = rec ? int(cgNowMs() - ss.startMs) : 0;
                        const float f = rec && rec->sound ? game::shockSoundFraction(*rec, el, ss.durationMs) : 0.0f;
                        const float bb = breathSnd.volumeBlend();
                        const float vb = volSlots.active[1] ? std::clamp(float(SDL_GetTicksNS() - victoryVol.startNs) * 1e-6f / float(victoryVol.ms), 0.0f, 1.0f) : 0.0f;
                        float sc[11];
                        if (volSlots.active[1]) {
                            for (int c = 0; c < 11; ++c) sc[c] = game::shockChannelVolume(victoryVol.rec, c, vb);
                            volSlots.set(1, sc);
                        }
                        if (bb != 0) {
                            for (int c = 0; c < 11; ++c) sc[c] = game::shockChannelVolume(holdBreathRec, c, bb);
                            volSlots.set(2, sc);
                        } else volSlots.clear(2);
                        if (f > 0) {
                            for (int c = 0; c < 11; ++c) sc[c] = game::shockChannelVolume(*rec, c, f);
                            volSlots.set(4, sc);
                        } else volSlots.clear(4);
                        volSlots.resolve(mixer.channelScale);
                    }
                    if (rec && rec->sound) {
                        const int el = int(cgNowMs() - ss.startMs);
                        const float b = game::shockLoopBlend(*rec, el, ss.durationMs);
                        const audio::Vec3 at{cam.pos.x, cam.pos.y, cam.pos.z};
                        if (b >= 0 && !clientSnd.shockLoop) clientSnd.shockLoop = mixer.playBlend("shellshock_loop", "shellshock_loop_silent", b, at);
                        else if (b >= 0) mixer.setBlend(clientSnd.shockLoop, b, at);
                        else if (clientSnd.shockLoop) mixer.stop(clientSnd.shockLoop), clientSnd.shockLoop = 0;
                    }
                } else if (clientSnd.shockLoop) mixer.stop(clientSnd.shockLoop), clientSnd.shockLoop = 0;
                if (!onTurret && (pweapon.equipped() || pweapon.binocularsUp()) && (!inVehicle || (pweapon.stateNumber() >= 0x11 && pweapon.stateNumber() <= 0x16))) {
                    // Hold-breath sounds (0x4b6840 from the first-person weapon pass 0x4b69b0: a weapon, eFlags & 0x300 clear, in a
                    // vehicle only in weapon states 0x11-0x16). The heartbeat loop lives while it is played every frame.
                    const bool hb = breathSnd.update(ps.holdingBreath, int(std::min(simDt, 0.1f) * 1000.0f), game::BreathSoundParams{},
                                                     [&](const char* a) {
                                                         if (std::strcmp(a, "weap_sniper_heartbeat") == 0) {
                                                             if (!clientSnd.heartbeatLoop) clientSnd.heartbeatLoop = mixer.play(a);
                                                             return 0;
                                                         }
                                                         float len = 0;
                                                         mixer.play(a, std::nullopt, &len);
                                                         return int(len * 1000.0f);
                                                     });
                    if (!hb && clientSnd.heartbeatLoop) mixer.stop(clientSnd.heartbeatLoop), clientSnd.heartbeatLoop = 0;
                } else if (clientSnd.heartbeatLoop) mixer.stop(clientSnd.heartbeatLoop), clientSnd.heartbeatLoop = 0;
                vmMotion.update([&](const char* k) { return pweapon.number(k); }, int(std::min(simDt, 0.1f) * 1000.0f),
                                std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]), 190.0f,
                                ps.viewHeight, pweapon.adsFrac(), ps.lean);
                {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    vmMotion.sway([&](const char* k) { return pweapon.number(k); }, int(std::min(simDt, 0.1f) * 1000.0f), cam.pitch, cam.yaw,
                                  pweapon.adsFrac(), ps.stance == game::Stance::Prone ? 2 : ps.stance == game::Stance::Crouch ? 1 : 0,
                                  !ret.empty() && ret != "none", shockRatio);
                }
                {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    gunBobAngles = game::gunBob(ps.bobCycle, std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]),
                                                ps.viewHeightTarget, pweapon.adsFrac(), pweapon.number("adsBobFactor"),
                                                !ret.empty() && ret != "none");
                    rawGunBobPrev = rawGunBob;
                    rawGunBob = game::gunBob(ps.bobCycle, std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]),
                                             ps.viewHeightTarget, pweapon.adsFrac(), pweapon.number("adsBobFactor"), false);
                }
                if (std::getenv("IW2_BOB_TRACE")) // debug: the movement inputs of the gun bob and offsets
                    std::cout << "bob cycle " << ps.bobCycle << " xy " << std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1])
                              << " vht " << ps.viewHeightTarget << " ground " << ps.onGround << " gunBob " << gunBobAngles[0] << "," << gunBobAngles[1] << ","
                              << gunBobAngles[2] << " move " << vmMotion.move()[1] << "," << vmMotion.move()[2] << " anim " << pweapon.anim() << " dt " << simDt << "\n";
                recoil.update([&](const char* k) { return pweapon.number(k); }, int(std::min(simDt, 0.1f) * 1000.0f), true, pweapon.adsFrac());
                // On a turret the view fov is 55 (0x4ae8a8); the aim fraction is kept for after getting off.
                cam.fovX43 = game::playerViewFov(game::adsFov(80.0f, pweapon.number("adsZoomFov") > 0 ? pweapon.number("adsZoomFov") : 80.0f,
                                          pweapon.number("adsZoomInFrac"), pweapon.number("adsZoomOutFrac"), pweapon.adsFrac(),
                                          pweapon.adsZoomingIn()),
                                                 onTurret);
                for (const auto& snd : pweapon.takeSounds()) mixer.play(snd);
                {
                    const Vec3 f = cam.forward(), r = cam.right();
                    mixer.setListener({cam.pos.x, cam.pos.y, cam.pos.z}, {f.x, f.y, f.z}, {r.x, r.y, r.z});
                }
                // The gun pose of the command before this one (0x531b90; a vehicle shot keeps the view angles, 0x531bbc).
                const game::GunAim shotAim = gunAimDelay.step(drawnGunAim);
                Camera shotCam = cam;
                game::Vec3 shotOffset{0, 0, 0};
                if (shotAim.valid && !inVehicle) {
                    shotCam.pitch = shotAim.pitch;
                    shotCam.yaw = shotAim.yaw;
                    shotOffset = {shotAim.offset[0], shotAim.offset[1], shotAim.offset[2]};
                }
                const Vec3 f = shotCam.forward();
                {
                    game::HipSpread hs;
                    hs.fireAdd = pweapon.number("hipSpreadFireAdd");
                    for (int i = 0; i < shots; ++i) game::addFireSpread(ps, hs, pweapon.adsFrac()); // 0x4dece0
                }
                for (int i = 0; i < shots; ++i) recoil.fire([&](const char* k) { return pweapon.number(k); }, pweapon.adsFrac(), pweapon.reducedKick());
                if (shots) viewShot(); // EV_FIRE_WEAPON -> fire flag (0x4b7800)
                for (int i = 0; i < shots; ++i) {
                    float spreadDeg = 0;
                    if (const auto* d = world->weapon(pweapon.name())) {
                        auto fld = [&](const char* k) { auto it = d->find(k); return it == d->end() ? 0.0f : float(std::atof(it->second.c_str())); };
                        game::HipSpread hs;
                        hs.standMin = fld("hipspreadstandmin"); hs.duckedMin = fld("hipspreadduckedmin"); hs.proneMin = fld("hipspreadpronemin");
                        hs.max = fld("hipspreadmax"); hs.duckedMax = fld("hipspreadduckedmax"); hs.proneMax = fld("hipspreadpronemax");
                        spreadDeg = game::fireSpreadDegrees(hs, ps.viewHeight, aimSpreadBeforeCmd, pweapon.adsFrac(), fld("adsspread"));
                    }
                    const auto kick = world->playerFire({cam.pos.x, cam.pos.y, cam.pos.z}, {f.x, f.y, f.z}, pweapon.clip() == 0 && i == shots - 1, spreadDeg, shotOffset);
                    for (int k = 0; k < 3; ++k) ps.velocity[k] -= kick[k]; // 0x531b1b
                }
                if (shots) std::cout << pweapon.name() << " " << pweapon.clip() << "/" << pweapon.stock() << "  hits " << world->stats.hits << " kills " << world->stats.kills << "\n";
                if (!inVehicle && !onTurret && !world->playerLinked()) {
                    world->setPlayerOrigin({ps.origin[0], ps.origin[1], ps.origin[2]}, {0, cam.yaw, 0});
                    world->playerPmEvents(ps.eventSeq, ps.events, ps.stance == game::Stance::Crouch); // G_ClientEvents
                }
                // time_slow: the frame scale moves linearly from `from` to `to` over the real time (0x4bee00).
                if (slowStartNs) {
                    slowmo = game::frameScaleAt(int64_t(slowStartNs / 1000), int64_t(slowEndNs / 1000), int64_t(SDL_GetTicksNS() / 1000), slowFrom, slowTo, slowmo);
                    if (SDL_GetTicksNS() >= slowEndNs) slowStartNs = slowEndNs = 0;
                }
                mixer.fadeStep(dt * 1000.0f);
                world->setRealTimeMs(int64_t(SDL_GetTicksNS() / 1000000ull));
                if (menuEnv.getCvar("cl_paused") != "1") gameClock += game::comFrameSeconds(dt, slowmo); // cl_paused freezes the simulation
                mixer.setPaused(menuEnv.getCvar("cl_paused") == "1"); // the sound update pauses the non-menu channels (0x442dc0)
                while (gameClock >= game::kServerFrameSeconds) {
                    // The previous server frame's entity placement, for the client interpolation below.
                    lerpPrev.clear();
                    const bool remotePuppets = world->mpRemote(); // a network client's other players are placed per rendered frame
                    for (const auto& e : world->entities()) {
                        // Network game: the puppets of the other players already stand at the drawn time (RemoteView::players
                        // interpolates the snapshots per rendered frame); lerping them again against a 50 ms old origin would
                        // move them in a sawtooth. TODO(spec): the models from the newest snapshot only (mpSetRemoteModels).
                        if (remotePuppets && (e->isPlayer || e->obj->get("$nettraj").truthy())) continue;
                        const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                        if (o.isVector())
                            lerpPrev[e->number] = {o.asVector(), a.isVector() ? a.asVector() : script::Vec3{0, 0, 0}};
                    }
                    for (const auto& e : world->entities()) {
                        if (e->turret) prevTurret[e->number] = {e->turret->pitch, e->turret->yaw};
                        if (e->vehicle) prevTurret[e->number] = {e->vehicle->turretPitch, e->vehicle->turretYaw};
                        if (e->vehicle) prevTexScroll[e->number] = e->vehicle->texScroll;
                        if (!e->anim) continue;
                        prevAnim.erase(e->number);
                        prevAnim.emplace(e->number, *e->anim);
                    }
                    if (world->player()) prevPlayerO = world->player()->obj->get("origin").asVector();
                    haveTickBodyPrev = world->playerVehicleBodyAxis(tickBodyPrev);
                    world->frame();
                    if (startTrace && traceFramesAfterLoad >= 0 && traceFramesAfterLoad < 3) trace("server frame run", world->timeMs()); // [server time]
                    gameClock -= game::kServerFrameSeconds;
                }
                lerpFrac = game::serverFrameLerp(gameClock);
                if (world->takePlayerNoInterp() && world->player()) { // dontinterpolate: no interpolation from the previous snapshot (0x4b9940)
                    prevPlayerO = world->player()->obj->get("origin").asVector();
                    lerpPrev.erase(world->player()->number);
                    linkView.clear();
                }
                if (camLinkLerp && world->player()) { // see prevPlayerO
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    cam.pos = {prevPlayerO[0] + (po[0] - prevPlayerO[0]) * lerpFrac, prevPlayerO[1] + (po[1] - prevPlayerO[1]) * lerpFrac,
                               prevPlayerO[2] + (po[2] - prevPlayerO[2]) * lerpFrac + camLinkHeight};
                }
                // Damage feedback (cgame 0x4ba2a0 on a new damageEvent): the aim spread scale grows by the count; unless
                // the damage came from the world (pitch and yaw 255), the oldest of 8 icon slots gets the time,
                // cg_hudDamageIconTime and yaw = yaw byte / 255 · 360 + (rand / 32768 − 0.5) · 20, through a short
                // angle. The view kick (0xf7979c / 0xf797a0) is viewImp.damage. TODO(spec): [flag not read] the "Health" menu flag.
                if (const auto& df = world->playerDamageFeedback(); df.event != lastDamageEvent) {
                    lastDamageEvent = df.event;
                    if (hudFade.health < cgNowMs()) hudFade.health = cgNowMs(); // 0x4ba537: damage touches the health bar
                    ps.aimSpread = std::min(255.0f, ps.aimSpread + float(df.count));
                    if (df.count != 0) { // 0x4ba1c3: only with a damage count
                        const Vec3 f = cam.forward(), l = cam.right() * -1.0f; // viewaxis rows 0 and 1 (left)
                        viewImp.damage(df.yaw, df.pitch, df.count, {f.x, f.y, f.z}, {l.x, l.y, l.z}, cgNowMs());
                    }
                    if (!(df.pitch == 0xff && df.yaw == 0xff)) {
                        size_t slot = 0;
                        for (size_t k = 1; k < damageIcons.size(); ++k)
                            if (damageIcons[k].start < damageIcons[slot].start) slot = k;
                        const float y = float(df.yaw) / 255.0f * 360.0f + (float(std::rand() % 32768) / 32768.0f - 0.5f) * 20.0f;
                        const int sh = int(y * (65536.0f / 360.0f)) & 0xffff;
                        damageIcons[slot] = {cgNowMs(), 2000, float(sh) * (360.0f / 65536.0f)};
                    }
                }
                if (const auto sp = world->takePlayerSpawn()) { // MP client spawn: the whole player state restarts (stance, view height, lean, aim)
                    const int64_t t = ps.timeMs;
                    ps = game::PlayerState{};
                    ps.origin = {(*sp)[0], (*sp)[1], (*sp)[2]};
                    ps.timeMs = t;
                    input.clearAds();
                }
                if (const auto tp = world->takePlayerTeleport()) { // setorigin
                    ps.origin = {(*tp)[0], (*tp)[1], (*tp)[2]};
                    ps.velocity = {0, 0, 0};
                    lerpPrev.erase(world->player()->number);
                }
                if (const auto va = world->takePlayerViewAngles()) { // setplayerangles
                    cam.pitch = std::remainder((*va)[0], 360.0f); // axisAngles gives [0,360): 355 would clamp to 89 (looking down)
                    cam.yaw = (*va)[1];
                }
                if (const auto lv = world->takePlayerLinkView()) linkView.push(lv->angles[0], lv->angles[1], cam.pitch, cam.yaw, lv->absolute);
                if (!world->playerHasLinkParent()) linkView.clear();
                else linkView.step(lerpFrac, cam.pitch, cam.yaw);
                if (const auto rel = world->takeTurretRelease()) {
                    // Off the turret (0x51b3d0): back where the player got on, in the stance it had (events 0x8c–0x8e).
                    ps.origin = {rel->origin[0], rel->origin[1], rel->origin[2]};
                    ps.velocity = {0, 0, 0};
                    if (rel->stance >= 0) input.setStance(rel->stance);
                }
                // MP obituaries (event 0xc6 -> 0x4e03f0, docs/spec/mp-hud.md).
                if (world->multiplayer())
                    for (const auto& ev : world->takeMpObituaries()) pushObituary(ev, SDL_GetTicksNS(), cgNowMs());
                for (const auto& cmd : world->takeClientCommands()) {
                    std::istringstream a(cmd.arg);
                    // iprintln / iprintlnbold: logged localized, as the message window shows it (0x496e80), not as the key.
                    if (cmd.name == "gm" || cmd.name == "gmb")
                        std::cout << "[" << cmd.name << "] " << logText(cmd.arg) << "\n";
                    if (cmd.name == "print") { // client command `print "%s"` (0x584038): console text, here stdout
                        std::cout << "[print] " << logText(game::printMessageText(cmd.arg)) << "\n";
                        // openiw2: shown for con_gamemessagetime in the game-message window (cheat toggles etc.), see screenmessages.h.
                        game::addScreenMessage(screenMessages, game::printMessageText(cmd.arg), false, SDL_GetTicksNS());
                    } else if (cmd.name == "popupopen" || cmd.name == "popupclose") {
                        menuCommand(cmd.name, cmd.arg);
                    } else if (cmd.name == "h" || cmd.name == "i") {
                        chatCommand(cmd.name, cmd.arg);
                    } else if (cmd.name == "openvictoryscreen") {
                        // Client 0x4a6d6a: the channel pause flags become "all but menu and music" (0x4429e0 with the 11-byte block at
                        // 0x4a6d7f), sound volume slot 1 is cleared (0x443390(1)), then 0x4022b0("victoryscreen") -> UI_SetActiveMenu(15)
                        // (0x4c679e: no close-all, cursor to (320, 448), UI key catcher) opens the menu, whose onOpen pauses the game.
                        // 0x4022b0 opens it only while the client is active (state 4) and not playing a timedemo.
                        for (int c = 0; c < 11; ++c) mixer.channelPausable[c] = c != 2 && c != 8;
                        volSlots.clear(1);
                        menuMx = 320.0f, menuMy = 448.0f;
                        frontStack.open("victoryscreen");
                        std::cout << "victory screen (next map " << world->storedNextMap() << ")\n";
                    } else if (cmd.name == "setchannelvol") { // setchannelvol <slot> <shock> <ms>: slot 1 takes the shock file's channel volumes
                        int slot = 0, ms = 0;
                        std::string shockName;
                        a >> slot >> shockName >> ms;
                        if (const auto f = vfs.read("shock/" + shockName + ".shock")) {
                            victoryVol.rec = game::parseShockFile(std::string(f->begin(), f->end()));
                            volSlots.active[1] = true;
                            victoryVol.startNs = SDL_GetTicksNS();
                            victoryVol.ms = std::max(ms, 1);
                        }
                    } else if (cmd.name == "obj_update" || cmd.name == "obj_complete" || cmd.name == "gm")
                        screenMessages.push_back({cmd.name == "gm" ? cmd.arg : "^2\x14" + cmd.arg, /* obj_*: formatted "^2%s" (0x57f6a0) */ SDL_GetTicksNS() + 5000000000ull, false, SDL_GetTicksNS(), SDL_GetTicksNS() + 5000000000ull}); // con_gamemessagetime 5 s (0x571118)
                    else if (cmd.name == "gmb")
                        screenMessages.push_back({cmd.arg, SDL_GetTicksNS() + 8000000000ull, true, SDL_GetTicksNS(), SDL_GetTicksNS() + 8000000000ull}); // con_boldgamemessagetime 8 s (0x571100)
                    else if (cmd.name == "opendeadscreen")
                        deadScreenMs = cgNowMs(); // cg.time (0x4a6d3c)
                    else if (cmd.name == "time_slow") { // time_slow <ms> <from> <to> (0x4a69a0 → 0x4bec20)
                        int ms = 0;
                        a >> ms >> slowFrom >> slowTo;
                        slowStartNs = SDL_GetTicksNS();
                        slowEndNs = slowStartNs + uint64_t(std::max(ms, 0)) * 1000000ull;
                    } else if (cmd.name == "mu_play") { // mu_play <alias> (0x4a6e2b -> 0x443030): the music slot; a new one replaces it
                        if (clientSnd.music) mixer.stop(clientSnd.music);
                        clientSnd.music = mixer.play(cmd.arg, std::nullopt, nullptr, {.dedicated = true, .music = true});
                    } else if (cmd.name == "ambient_play") { // ambient_play <alias> <fade ms> (client side of configstring 3, 0x443080)
                        std::string alias;
                        int ms = 0;
                        a >> alias >> ms;
                        clientSnd.playAmbient(mixer, alias, ms);
                    } else if (cmd.name == "ambient_stop") { // ambient_stop <fade ms>
                        int ms = 0;
                        a >> ms;
                        clientSnd.stopAmbient(mixer, ms);
                    } else if (cmd.name == "mu_stop") { // mu_stop <ms> (0x4a6e60 -> 0x442fb0): stop at once or fade out over ms
                        int ms = 0;
                        a >> ms;
                        if (clientSnd.music) mixer.fadeOut(clientSnd.music, ms);
                        clientSnd.music = 0;
                    } else if (cmd.name == "reverb") { // reverb <prio> "<room>" <dry> <wet> <fade s> (CG_ReverbCmd 0x4a6350)
                        int prio = 0;
                        std::string room;
                        float dry = 0, wet = 0, fadeS = 0;
                        a >> prio >> std::quoted(room) >> dry >> wet >> fadeS;
                        mixer.reverbCommand(prio, room, dry, wet, fadeS);
                    } else if (cmd.name == "deactivatereverb") { // deactivatereverb <prio> <fade s> (0x4a6500)
                        int prio = 0;
                        float fadeS = 0;
                        a >> prio >> fadeS;
                        mixer.deactivateReverbCommand(prio, fadeS);
                    } else if (cmd.name == "snd_fade") { // snd_fade <volume> <ms> (0x4a6ec4)
                        float vol = 1;
                        int ms = 0;
                        a >> vol >> ms;
                        mixer.fade(vol, ms);
                    }
                    else if (cmd.name == "fog") { // fog <near> <far> <1> <r> <g> <b> <ms> (0x4a5770)
                        float nearD = 0, farD = 0, one = 1, r = 0, g = 0, b = 0, ms = 0;
                        a >> nearD >> farD >> one >> r >> g >> b >> ms;
                        if (one >= 1.0f) world->clientFog.command(nearD, farD, r, g, b, int(ms), cgNowMs());
                        else world->clientFog.commandExp(one, r, g, b, int(ms), cgNowMs());
                    }
                    else if (cmd.name == "scr_fade") { // scr_fade <alpha> <start ms> <duration ms> (0x4a6f3c)
                        float alpha = 0;
                        int startMs = 0, durMs = 0;
                        a >> alpha >> startMs >> durMs;
                        screenFade.command(alpha, startMs, durMs, cgNowMs());
                    }
                    else if (cmd.name == "clear_blur")
                        screenBlur.clear();
                    else if (cmd.name == "scr_blur") { // scr_blur <ms> <radius> <realtime> <priority> (0x4beb80)
                        int ms = 0, rt = 0, prio = 0;
                        float radius = 0;
                        a >> ms >> radius >> rt >> prio;
                        const int64_t now = rt ? int64_t(SDL_GetTicks()) : cgNowMs();
                        // The original's real-time base is "ms since first use" (0x1cbaec4); only differences are used, so SDL ticks are equivalent.
                        screenBlur.command(ms, radius, rt != 0, prio, std::max<int64_t>(now, 1), screenBlur.composed());
                    }
                }
                // Saves the level produced this frame (0x44a230): written to save/<name>.svg; a save that is not internal
                // becomes sv_lastSaveGame (0x4e0554..0x4e0573, internal names 0x44a2f0).
                for (auto& f : world->takeSaveFiles()) {
                    const std::filesystem::path path = saveDir / (f.name + ".svg");
                    std::error_code ec;
                    std::filesystem::create_directories(path.parent_path(), ec);
                    std::ofstream of(path, std::ios::binary);
                    of.write(reinterpret_cast<const char*>(f.bytes.data()), std::streamsize(f.bytes.size()));
                    const bool ok = bool(of);
                    world->setSaveFailed(!ok);
                    std::cout << (ok ? "saved " : "save failed: ") << path.string() << "\n";
                    if (ok && !ui::saveNameInternal(f.name)) {
                        uiCvars["sv_lastSaveGame"] = f.name;
                        world->cvars["sv_lastSaveGame"] = f.name;
                        profileDirty = true; // archived dvar (flags 0x1001): goes into the profile's config.cfg
                    }
                    for (const char* r : {"vid_restart", "snd_restart"}) // 0x44a050 (exact names, 0x15 bytes compared)
                        if (ok && f.name == std::string("internal/") + r) {
                            std::cout << "Game saved for " << r << "\n";
                            execText(std::string("disconnect;") + r + ";loadgame internal/" + r);
                        }
                    saveRowsAt = 0;
                }
                // MP end of the map (docs/spec/mp.md "End of the map"): exitLevel queues the console command map_rotate, which
                // runs on the next command-buffer pass: "map <next>" from sv_mapRotation, or a restart of this map without one.
                // A fast restart (map_restart, sd rounds) rebuilds the level inside the same World: every entity is new, so the
                // client-side caches keyed by entity number and the view weapon start over as after a load (docs/spec/mp.md).
                static std::pair<int, int> seenRestarts{0, 0}; // (world generation, restart count): a new World may reuse the old one's address
                if (world->multiplayer() && seenRestarts.first == worldGeneration && seenRestarts.second != world->mpRestarts()) {
                    std::cout << "fast restart " << world->mpRestarts() << " of " << currentMap << "\n";
                    lerpPrev.clear();
                    prevAnim.clear();
                    prevTurret.clear();
                    prevTexScroll.clear();
                    pweapon = game::PlayerWeapon{};
                    weaponSelect.clear();
                    weaponWritten.clear();
                    prevSelect.clear();
                    lastDamageEvent = 0;
                    damageIcons = {};
                    lastLandSeq = 0;
                    lastEventSeq = 0;
                    viewImp.reset();
                }
                seenRestarts = {worldGeneration, world->mpRestarts()};
                static int rotatedGeneration = 0; // map_rotate runs once per exitLevel (per world generation: a new World may reuse the old one's address)
                if (world->multiplayer() && world->mpState().exitRequested && rotatedGeneration != worldGeneration && !pendingLevel) {
                    rotatedGeneration = worldGeneration;
                    const game::MapRotateResult rot = game::mapRotate(menuEnv.getCvar, menuEnv.setCvar);
                    for (const std::string& l : rot.console) std::cout << l;
                    if (rot.kind == game::MapRotateResult::Kind::Restart)
                        pendingLevel = std::make_pair(currentMap, false); // TODO(spec): fast_restart keeps the server running; here the map reloads
                    else if (vfs.exists("maps/mp/" + rot.map + ".d3dbsp"))
                        pendingLevel = std::make_pair("mp/" + rot.map, false);
                    else
                        std::cout << "Can't find map " << rot.map << "\n"; // the "map" command refuses; the server stays as it is
                }
                if (auto change = world->takeDueLevelChange()) {
                    const std::string last = menuEnv.getCvar("sv_lastSaveGame");
                    if (change->loadContinue && !last.empty() && std::filesystem::exists(saveDir / (last + ".svg")))
                        pendingLoad = last, quickReload = true, continueRestart = true;
                    else if (change->victoryScreen) {} // the client command openvictoryscreen opens the menu; `nextlevel` loads the map
                    else pendingLevel = std::make_pair(change->map, change->failed), pendingPersistent = change->persistent && !change->failed;
                }
            }
            if (pendingDisconnect) { // CL_Disconnect: the level ends, UI_SetActiveMenu(MAIN) shows the main menu with sv_running 0
                // CL_Disconnect 0x40cd10 (patch 1.3 MP executable): tells the server, shuts cgame down (0x4b2490) and stops every sound
                // (0x44f940, no keep flags: the music and ambient streams too), clears the client state and the connection. The player's
                // own `disconnect`, a server drop and an error all end here; resetClientState is that clearing, shared with the level load.
                pendingDisconnect = false;
                const bool leftMultiplayer = netClient || listenServer || (world && world->multiplayer());
                if (netClient) { // CL_Disconnect: tell the server (no-op after a drop) and forget the connection
                    netClient->disconnect();
                    netClient.reset();
                }
                if (listenServer) { // the host leaves: its listen server ends (docs/spec/net.md section 14)
                    listenServer->shutdown("EXE_SERVERQUIT");
                    listenServer.reset();
                    listenGame.reset();
                    listenWorldGeneration = -1;
                }
                mpMenuBots.reset(); // the bots belonged to that server; the next StartServer sets its own
                pendingLevel.reset();
                pendingPersistent = false;
                loadScreenPhase = 0;
                if (world && world->multiplayer()) ui::carryWorldCvars(world->cvars, uiCvars); // one dvar store (docs/spec/mp-ui.md)
                endWorldFx(); // before the world goes: a bolted effect must not read it
                world.reset();
                releaseScene(dev, scene);
                scene = emptyScene(dev);
                currentMap.clear();
                resetClientState("disconnect");
                if (netgame::mpMenuAfterDisconnect(mpFrontEnd, leftMultiplayer)) pendingFrontEnd = true;
                frontStack.closeAll();
                frontStack.open("main");
            }
            applyFrontEnd();
            pumpConnect();
            showNetError();
            // The extension menus belong to createserver: they close with it, and the panel gives way to the original settings column.
            if (mpFrontEnd) {
                bool cs = false, ext = false, settings = false;
                for (const ui::Menu* m : frontStack.menus()) {
                    cs = cs || m->name == "createserver";
                    ext = ext || m->name == "iw2_createserver_ext";
                    settings = settings || m->name == "createserver_serversettings" || m->name.rfind("settings_", 0) == 0;
                }
                if (ext && (!cs || settings)) frontStack.close("iw2_createserver_ext");
                if (!cs) frontStack.close("iw2_createserver_extbutton");
            }
            if (!deferredExec.empty()) execText(std::exchange(deferredExec, std::string()));
            std::optional<game::LoadedSave> loadedSave;
            if (pendingLoad) {
                const std::filesystem::path path = saveDir / (*pendingLoad + ".svg");
                game::LoadedSave ls;
                const std::string err = game::readSaveFile(path.string(), *pendingLoad, ls);
                if (err.empty()) {
                    loadedSave = std::move(ls);
                    pendingLevel = std::make_pair(loadedSave->info.mapName, false);
                } else {
                    std::cout << path.string() << ": " << err << "\n";
                    if (std::exchange(continueRestart, false) && !currentMap.empty()) { // loadgame_continue found no usable save: restart
                        std::cout << "loadgame_continue: no usable save, restarting " << currentMap << "\n";
                        pendingLevel = std::make_pair(currentMap, true);
                    }
                    quickReload = false;
                }
                continueRestart = false;
                pendingLoad.reset();
            }
            static std::optional<game::LoadedSave> heldSave; // a savegame load waiting for the load-screen frame
            static script::ObjectPtr heldGame;               // game[] of a persistent change (0x9bef30 survives, 0x4d9d3c)
            static std::optional<game::World::CarriedPlayer> heldPlayer;
            bool loadNow = pendingLevel && loadScreenPhase == 1; // the load-screen frame was drawn last pass
            if (pendingLevel && loadScreenPhase == 0) {
                // Leaving the level (G_ShutdownGame 0x4d9ce0): a cinematic() recorded by the script plays first (0x4d9f90); game[] and the
                // player's weapons are kept only for a persistent change. Then the client starts the map load (0x40ce90: maps/<map>.csv
                // aliases, client state 5) and the cgame load progress opens the load screen (0x4a300d / 0x4b9215 -> 0x4022b0("briefing")
                // -> UI_SetActiveMenu(13) 0x4c6730: close every menu, open "briefing"; the "loadbar" name it also opens is no menu).
                // openiw2 loads synchronously, so the load screen is drawn for one frame before the load.
                if (world) {
                    const std::string movie = world->takeCinematic();
                    if (!movie.empty()) playScripted(movie);
                }
                heldGame = world && pendingPersistent && !loadedSave ? world->vm().game() : script::ObjectPtr();
                heldPlayer.reset();
                if (world && pendingPersistent && !loadedSave) heldPlayer = world->carriedPlayerState();
                heldSave = std::move(loadedSave);
                loadedSave.reset();
                if (world && world->multiplayer()) ui::carryWorldCvars(world->cvars, uiCvars); // one dvar store (docs/spec/mp-ui.md)
                endWorldFx(); // before the world goes: a bolted effect must not read it
                world.reset();
                releaseScene(dev, scene);
                scene = emptyScene(dev);
                // The map load shuts cgame down and stops every sound like a disconnect does (0x4b2490, 0x44f940); the SP load starts
                // with the stop-all (0x449340 -> 0x40c9c0 -> 0x444730 flags 0; the cgame init 0x4a5cc7 repeats it): the previous level's
                // music, ambient track, loops and queued voices end here, no fade. The pause flags go back to the defaults (0x40cc80)
                // and the volume slots 1..4 are cleared (the previous level's victory-screen volume must not carry over). Voices the
                // new level's script starts come later.
                resetClientState("level change");
                loadMapAliases(pendingLevel->first);
                frontStack.closeAll();
                // MP (a map under mp/): the MP UI has no briefing menu; its load screen is the "connect" menu (ui_mp/connect.menu,
                // drawn by the connect screen 0x5390c0 -> 0x4d3390 -> 0x5332c0 once the map name is known) with the gametype and
                // the map's long name over it (docs/spec/mp-ui.md "Load screen"). TODO: an MP map started outside the MP front end
                // (command line) has the SP menu set, whose "connect" is the SP one; it gets no load screen.
                mpLoadText.reset();
                if (!quickReload && pendingLevel->first.rfind("mp/", 0) == 0) {
                    if (mpFrontEnd) frontStack.open("connect");
                    std::string gt = netClient ? net::infoValueForKey(netClient->configstrings[0], "g_gametype")
                                     : std::getenv("IW2_MP_GAMETYPE") ? std::string(std::getenv("IW2_MP_GAMETYPE"))
                                                                      : menuEnv.getCvar("g_gametype");
                    if (gt.empty()) gt = "dm";
                    mpLoadText = ui::mpLoadScreenText(vfs, gt, pendingLevel->first.substr(3));
                } else if (!quickReload) frontStack.open("briefing");
                loadScreenPhase = 1;
                if (quickReload) loadNow = true; // no load screen: load in the same pass
            }
            if (loadNow) {
                    loadScreenPhase = 0;
                    const bool wasQuick = std::exchange(quickReload, false);
                    loadedSave = std::move(heldSave);
                    heldSave.reset();
                    // Level change or restart: rebuild the world and the scene for the next map.
                    currentMap = pendingLevel->first;
                    std::cout << (pendingLevel->second ? "mission failed, restarting " : loadedSave ? "loading save " + loadedSave->name + ": " : "changing level to ") << currentMap
                              << (pendingPersistent ? " (persistent)" : "") << "\n";
                    const script::ObjectPtr carried = std::exchange(heldGame, script::ObjectPtr());
                    releaseScene(dev, scene);
                    // The level's sound aliases are loaded before its script runs (set_ambient checks them in main()).
                    aliases.clear();
                    if (currentMap.rfind("mp/", 0) == 0) aliases.loadLevel(vfs, currentMap.substr(3), "all_mp"); // MP: the bare map name, mode tag all_mp
                    else aliases.loadLevel(vfs, currentMap); // level rows + the pause menu's interface sounds
                    try {
                        world = makeWorld(currentMap, loadedSave ? nullptr : carried, loadedSave ? &*loadedSave : nullptr,
                                          loadedSave ? std::nullopt : std::exchange(heldPlayer, std::nullopt));
                    } catch (const std::exception& e) {
                        if (!loadedSave) throw;
                        std::cout << "loadgame failed: " << e.what() << "; restarting " << currentMap << "\n";
                        world = makeWorld(currentMap, nullptr);
                    }
                    scene = loadScene(dev, vfs, currentMap, false);
                    if (world && world->player()) { // log: the weapons the level gave (or a persistent change carried)
                        std::cout << "player weapons:";
                        for (const auto& wn : world->inventory(*world->player()).owned) std::cout << " " << wn;
                        std::cout << "\n";
                    }
                    frontStack.closeAll(); // leaving the front end / pause menu
                    // Load finished (0x449a2c, client state 4): with ui_autoContinue (bool, default 0, flags 0x1000, 0x4c32c4) or a
                    // timedemo (0x4c5530) the game starts at once (0x4c54c0); otherwise cl_paused 1, every menu closed and "pregame"
                    // ("click to start the mission") opened; its click runs uiScript playerstart.
                    // MP maps never wait (ui::loadWaitsForClick): a map rotation would otherwise stop the listen server.
                    if (ui::loadWaitsForClick(currentMap, std::atoi(menuEnv.getCvar("ui_autoContinue").c_str()) != 0, wasQuick)) {
                        menuEnv.setCvar("cl_paused", "1");
                        frontStack.open("pregame");
                    } else menuEnv.setCvar("cl_paused", "0");
                    endLoad();
                    last = SDL_GetTicksNS(); // 0x449340 ends with the Com_Frame clock set to the end of the load: the load time is no frame time
                    cam = scene.start;
                    ps = game::PlayerState{};
                    ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - ps.viewHeight};
                    if (loadedSave && world->player()) { // a loaded save puts the player where the save has him
                        // TODO(spec): [save format choice, needs a version bump] the front end's pmove state (velocity, stance, view) is not part of the openiw2 save
                        const auto o = world->player()->obj->get("origin"), an = world->player()->obj->get("angles");
                        if (o.isVector()) ps.origin = o.asVector();
                        if (an.isVector()) cam.yaw = an.asVector()[1];
                    }
                    game::markSpawned(ps);
                    // The rest of cgame's new-level reset (0x4ba570; the fades, weapon, springs, caches and clock) was done by resetClientState
                    // when the old level went; the new level starts with a fresh client.
                pendingLevel.reset();
                pendingPersistent = false;
            }
            if (const char* lim = std::getenv("IW2_EXIT_AFTER")) // debug: leave after N seconds (keeps test runs from lingering)
                if (SDL_GetTicksNS() > uint64_t(std::atof(lim) * 1e9)) running = false;
            if (const char* keys = std::getenv("IW2_MENU_KEYS")) { // debug: scripted menu keys (up,down,enter,esc), one per 2 s
                static std::string rest = keys;
                static uint64_t nextKey = SDL_GetTicksNS() + 3000000000ull;
                if (!rest.empty() && SDL_GetTicksNS() >= nextKey) {
                    const size_t comma = rest.find(',');
                    const std::string k = rest.substr(0, comma);
                    rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                    using K = ui::MenuStack::Key;
                    std::cout << "menu key " << k << "\n";
                    if (menuExtraToken(k)) {
                    } else if (k == "pause" && world && frontStack.empty()) {
                        menuEnv.setCvar("cl_paused", "1");
                        frontStack.open("main");
                    } else if (k.rfind("set:", 0) == 0) { // set:name=value
                        const size_t eq = k.find('=');
                        menuEnv.setCvar(k.substr(4, eq - 4), k.substr(eq + 1));
                    } else if (k.rfind("call:", 0) == 0 && world) { // call:file.gsc:function: a thread on level (headless test of script flows)
                        const size_t cl = k.rfind(':');
                        world->vm().start(k.substr(5, cl - 5), k.substr(cl + 1), {}, script::Value(world->vm().level()));
                    } else if (k.rfind("exec:", 0) == 0) { // exec:console text, e.g. exec:map moscow
                        execText(k.substr(5));
                    } else if (k.rfind("open:", 0) == 0) {
                        frontStack.open(k.substr(5));
                    } else if (k.rfind("click:", 0) == 0) { // click:X/Y in virtual pixels (left press and release)
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                        frontStack.click(menuMx, menuMy);
                        frontStack.release();
                    } else if (k.rfind("mpdamage:", 0) == 0 && world && world->multiplayer()) { // mpdamage:victim:attacker:amount (MP damage path)
                        int v = -1, a = -1, n = 0;
                        if (std::sscanf(k.c_str() + 9, "%d:%d:%d", &v, &a, &n) == 3) world->mpDebugDamage(v, a, n, "MOD_RIFLE_BULLET");
                    } else if (k == "togglemenu" && world && world->multiplayer()) { // the ESC binding of default_mp.cfg
                        mpToggleMenu();
                    } else if (k.rfind("shot:", 0) == 0) { // shot:<file.png>: writes the next presented frame
                        pendingShotPng = k.substr(5);
                    } else if (k.rfind("turn:", 0) == 0) { // turn:<deg> turns the view yaw by deg (scripted walks in live play)
                        cam.yaw += float(std::atof(k.c_str() + 5));
                    } else if (k.rfind("yaw:", 0) == 0) { // yaw:<deg> sets the view yaw (absolute, e.g. to face a placed player)
                        cam.yaw = float(std::atof(k.c_str() + 4));
                    } else if (k.rfind("pitch:", 0) == 0) { // pitch:<deg> sets the view pitch (positive looks down)
                        cam.pitch = std::clamp(float(std::atof(k.c_str() + 6)), -89.0f, 89.0f);
                    } else if (k.rfind("cmd:", 0) == 0) { // cmd:+forward / cmd:-forward / cmd:weapnext: a bound command as if from a key
                        const std::string c = k.substr(4);
                        if (!c.empty() && c[0] == '-') input.command("+" + c.substr(1), false, "SCRIPTKEY");
                        else input.command(c, true, "SCRIPTKEY");
                    } else if (k == "quit") {
                        running = false;
                    } else if (k == "wait") {
                    } else if (k.rfind("bindkey:", 0) == 0) {
                        frontStack.captureKey(k.substr(8));
                    } else
                    frontStack.key(k == "up" ? K::Up : k == "down" ? K::Down : k == "esc" ? K::Escape : k == "left" ? K::Left : k == "right" ? K::Right : K::Enter);
                    if (const ui::Item* f = frontStack.focus()) std::cout << "  focus " << f->name << ", menus " << frontStack.menus().size() << "\n";
                    std::cout << "  cl_paused=" << menuEnv.getCvar("cl_paused") << " clock=" << (world ? world->timeMs() : -1) << "\n"; // [server time]
                    static const uint64_t keyGap = std::getenv("IW2_MENU_KEY_MS") ? uint64_t(std::max(1, std::atoi(std::getenv("IW2_MENU_KEY_MS")))) * 1000000ull : 2000000000ull;
                    nextKey = SDL_GetTicksNS() + keyGap;
                }
            }
            if (world) { // mission progress written by scripts (setmissioncvar, maps/_endmission.gsc) is archived: copy it to the profile
                for (const char* n : {"mis_01", "mis_02", "mis_03", "mis_04", "mis_05", "mis_06", "mis_07", "mis_08", "mis_09", "mis_10", "mis_difficulty"}) {
                    const auto it = world->cvars.find(n);
                    if (it == world->cvars.end()) continue;
                    auto& u = uiCvars[n];
                    if (u != it->second) { u = it->second; profileDirty = true; }
                }
            }
            if (profileDirty) saveProfileCfg(); // write the archived cvars (config.cfg) after a menu changed one
            frontStack.tick(int(SDL_GetTicks()));
            if (const auto sv = uiCvars.find("snd_volume"); sv != uiCvars.end()) mixer.masterVolume = std::clamp(float(std::atof(sv->second.c_str())), 0.0f, 1.0f) * 0.75f;
            frontStack.resumeLoop();
            if (world && frontStack.empty() && menuEnv.getCvar("cl_paused") == "1") menuEnv.setCvar("cl_paused", "0"); // menus closed: unpause
            mixer.pausedChannels = menuEnv.getCvar("cl_paused") == "1"; // 0x442dc0: pause / resume the pausable channels when cl_paused changes
            {   // The cursor is shown while menus are open (relative mouse look otherwise).
                const bool wantCursor = !frontStack.empty();
                if (wantCursor == SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, !wantCursor);
            }
            const Camera camBase = cam;
            cam.pitch += recoil.viewKick()[0]; // view kick spring (0x4ae327)
            cam.yaw += recoil.viewKick()[1];
            if (world && onTurret && menuEnv.getCvar("cl_paused") != "1") { // turret view jitter (0x4aee10) while the turret fires
                float vj = 0, hj = 0;
                if (world->playerTurretFireJitter(vj, hj)) {
                    const game::V3 j = game::turretViewJitter(vj, hj, turretJitterSeed);
                    cam.pitch += j[0];
                    cam.yaw += j[1];
                }
            }
            if (world) { // screen shake (0x4bf700, after the view offsets): pitch, yaw, roll
                const game::V3 eye{cam.pos.x, cam.pos.y, cam.pos.z};
                for (const auto& ev : world->takeShakeEvents())
                    viewShake.add({ev.origin[0], ev.origin[1], ev.origin[2]}, ev.scale, ev.durationMs, ev.radius, ev.startMs, eye);
                const game::V3 sh = viewShake.update(cgNowMs(), eye);
                cam.pitch += sh[0];
                cam.yaw += sh[1];
                cam.roll += sh[2];
            }
            if (world)
                if (const game::ShockRecord* rec = world->shellShockRecord()) { // shell shock view kick (0x4b2380), render only
                    const auto& ss = world->shellShock();
                    const game::ShockKick k = game::shockViewKick(*rec, game::elapsedMs(cgNowMs(), ss.startMs), ss.durationMs);
                    cam.yaw += std::atan(k.x) * 57.29578f;
                    cam.pitch -= std::atan(k.y) * 57.29578f;
                }
            if (!onTurret && !playerDead && (pweapon.equipped() || pweapon.binocularsUp()) && pweapon.adsFrac() != 0) { // scope sway of the view (0x4ae060)
                const std::string ret = pweapon.field("adsOverlayReticle");
                const auto ss = vmMotion.scopeSway([&](const char* k) { return pweapon.number(k); }, int(std::min(simDt, 0.1f) * 1000.0f),
                                                   pweapon.adsFrac(), ps.stance == game::Stance::Prone ? 2 : ps.stance == game::Stance::Crouch ? 1 : 0,
                                                   ps.breathScale, !ret.empty() && ret != "none");
                cam.pitch += ss[0];
                cam.yaw += ss[1];
                const game::V3 vb = game::scopedViewBob(rawGunBobPrev, pweapon.adsFrac(), !ret.empty() && ret != "none"); // 0x4ae210
                cam.pitch += vb[0];
                cam.yaw += vb[1];
                cam.roll += vb[2];
            }
            cam.pitch += bobAngles[0]; // ADS view bob (0x4ae4a9)
            cam.yaw += bobAngles[1];
            if (world && !onTurret) { // damage kick (0x4ae371): pitch and roll
                const std::string ret = pweapon.field("adsOverlayReticle");
                const auto dk = viewImp.damageKick(cgNowMs(), pweapon.adsFrac(), !ret.empty() && ret != "none");
                cam.pitch += dk[0];
                cam.roll += dk[1];
            }
            // Killcam: the killer's archived view replaces the camera for this frame (restored with the live view at the end).
            const Vec3 killcamLivePos = cam.pos;
            const float killcamLiveFov = cam.fovX43;
            const auto killcamView = killcamCamera(cam, lerpFrac);
            fxRight = cam.right();
            fxUp = cross(cam.right(), cam.forward());
            // The per-element spawnRange / cullrange gate (0x485300): view origin and tan(fovX / 2) / tan(40 deg) (0x486e00).
            fxSystem.viewOrigin = {cam.pos.x, cam.pos.y, cam.pos.z};
            fxSystem.viewScale = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) / std::tan(0.6981317f);
            fxSystem.viewValid = true;
            setFxFrustum(fxSystem, cam, float(width) / float(height));
            setFxFar(fxSystem, cam, (world ? world->clientFog.current(cgNowMs()) : game::FogParams{}), world ? world->cullDist() : 0.0f);
            setFxFar(fxSystem, cam, (world ? world->clientFog.current(cgNowMs()) : game::FogParams{}), world ? world->cullDist() : 0.0f);
            // The gun pose of this frame (cgame 0x4b6d40, after the view offsets), read by the next command. The pass stops, keeping the last
            // pose, with pm_type 2 or 3, on a turret, and in a vehicle outside the binocular weapon states (0x4b6d54..0x4b6db9).
            if (const int gs = pweapon.stateNumber(); world && ps.pmType != 2 && ps.pmType != 3 && !world->playerOnTurret() &&
                                                      !((world->playerPmFlags() & 0x20000) && !(gs >= 0x11 && gs <= 0x16))) {
                game::GunAimPose gp;
                gp.viewAngles = {cam.pitch, cam.yaw, cam.roll};
                if (pweapon.equipped() || pweapon.binocularsUp()) { // no weapon: the view angles and no offset (0x4b6ede)
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    const bool reticle = !ret.empty() && ret != "none";
                    const auto dk = viewImp.gunDamageKick(cgNowMs(), pweapon.adsFrac(), reticle);
                    gp.adsWeapon = pweapon.number("aimDownSight") != 0;
                    gp.adsFrac = pweapon.adsFrac();
                    gp.overlay = scopeAlpha() > 0;
                    gp.moveBeforeLag = vmMotion.moveBeforeLag();
                    gp.rawBob = rawGunBob;
                    gp.lagSide = vmMotion.lagSide();
                    gp.lagUp = vmMotion.lagUp();
                    gp.gunAngles = gunRelativeAngles();
                    gp.gunAngles[0] += pweapon.number("adsAimPitch") * gp.adsFrac + dk[0]; // 0x4b5b6c, the damage kick
                    gp.gunAngles[1] += dk[1];
                    gp.gunAngles[2] += dk[2];
                    gp.landZ = viewImp.gunLandOffset(cgNowMs());
                }
                drawnGunAim = game::computeGunAim(gp);
            }
            // The view model is posed (and its flash / brass played) before the effects are drawn: the exe adds the view weapon
            // (0x4b026f) before the effect draw, and the bolted particles re-read tag_flash / tag_brass through 0x47fcd0 ->
            // 0x48abb0 once per frame stamp 0x6221a8 (bumped at the frame start, 0x4afca3), i.e. this frame's gun pose. Posing it
            // after gatherItems left the particles on the previous frame's tag (the flash trailed the gun while turning).
            std::vector<DrawItem> vmItems;
            // No view model on a turret (cgame 0x4b6d40 returns with eFlags & 0x300); in a vehicle only in the
            // binocular weapon states 0x11–0x16.
            const int wstate = pweapon.stateNumber();
            if (!(world && world->playerOnTurret()) &&
                !(world && (world->playerPmFlags() & 0x20000) && !(wstate >= 0x11 && wstate <= 0x16)))
                // Network client: no view weapon for the server's pm_type 4 / 5 (spectator, intermission), whatever weapon
                // the player state still carries (docs/spec/mp-spectate.md).
                if (!(world && world->playerDeadWeaponless()) && !(netClient && netgame::RemoteView::viewWeaponHidden(ps.pmType))) { // dead: entity weapon 0 (0x4b6a0a)
                    if (!(world && scopeAlpha() > 0)) addViewModel(vmItems, cam);
                    else if (viewFlash->pending || viewFlash->brassPending) { // scope overlay (0x4b6d7c): posed, not drawn; the flash is consumed unplayed
                        std::vector<DrawItem> scratch;
                        addViewModel(scratch, cam, false);
                    }
                }
            if (killcamView) {
                vmItems.clear();
                addKillcamViewModel(vmItems, cam, *killcamView);
            }
            auto items = killcamView ? gatherKillcamItems(cam, lerpFrac) : gatherItems(cam);
            items.insert(items.end(), vmItems.begin(), vmItems.end());
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUTexture* swap = nullptr;
            uint32_t w = 0, h = 0;
            const uint64_t profA = SDL_GetTicksNS();
            const bool gotSwap = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h);
            profWait += double(SDL_GetTicksNS() - profA) * 1e-6;
            if (gotSwap && swap)
            {
                renderer.fog = (world ? world->clientFog.consts(cgNowMs()) : game::FogConsts{});
                applyTextureFilter(renderer, uiCvars);
                renderer.dlights.clear(); for (const auto& d : selectDlights(fxSystem.lights(), cam.pos, dlightLimitFrom(uiCvars), fxSystem.viewPlanes)) renderer.dlights.push_back({{d[0], d[1], d[2]}, d[3], {d[4], d[5], d[6]}});
                screenBlur.update(cgNowMs(), int64_t(SDL_GetTicks()));
                const float blur = std::max(game::composeSceneBlur(worldMenuBlur(), screenBlur.composed()), forcedBlur);
                { // shell shock screen blend (0x4b16b0): blendMs scaled by the remaining time; elapsed = real time since the last capture
                    static int64_t lastGhostTicks = 0;
                    const int64_t nowTicks = int64_t(SDL_GetTicks());
                    renderer.ghostActive = false;
                    if (world)
                        if (const game::ShockRecord* rec = world->shellShockRecord()) {
                            const auto& ss = world->shellShock();
                            const int remaining = int(ss.startMs + ss.durationMs - cgNowMs());
                            if (ss.startMs != 0 && ss.durationMs != 0 && remaining > 0) {
                                renderer.ghostActive = true;
                                renderer.ghostBlendMs = float(game::shockBlendMs(*rec, remaining));
                                renderer.ghostElapsedMs = float(nowTicks - lastGhostTicks);
                            }
                        }
                    lastGhostTicks = nowTicks;
                }
                const uint64_t profB = SDL_GetTicksNS();
                updateWaterMaps(scene, dev, world ? float(cgNowMs()) / 1000.0f : float(SDL_GetTicks()) / 1000.0f);
                const uint64_t profC = SDL_GetTicksNS();
                renderer.gameTimeSeconds = float(((world ? cgNowMs() : int64_t(SDL_GetTicks())) % 1000) / 1000.0); // gameTime.z = frac(t)
                hudKillcam = killcamView;
                const std::vector<Quad2D> hud = buildHud(w, h, cam);
                hudKillcam.reset();
                renderer.gamma = gammaFrom(uiCvars), renderer.zFeather = zFeatherFrom(uiCvars);
                renderer.draw(cmd, swap, w, h, scene, cam, items, hud, blur);
                // debug token shot:<file.png> (IW2_MENU_KEYS): the same frame rendered once more into a PNG
                if (!pendingShotPng.empty()) screenshot(dev, renderer, scene, cam, w, h, std::exchange(pendingShotPng, std::string()), items, hud, blur);
                // debug: IW2_CAPTURE=<dir>[,every[,div]] renders every Nth presented frame of live play (size / div) into <dir>/frame_NNNNN.png
                if (const char* capEnv = std::getenv("IW2_CAPTURE"); capEnv && world) {
                    static uint64_t capFrame = 0;
                    std::string dir = capEnv;
                    int every = 1, div = 1;
                    if (const size_t cm = dir.find(','); cm != std::string::npos) {
                        std::sscanf(dir.c_str() + cm + 1, "%d,%d", &every, &div);
                        every = std::max(1, every), div = std::clamp(div, 1, 8);
                        dir.resize(cm);
                    }
                    if (capFrame++ % uint64_t(every) == 0) {
                        { std::error_code ec; std::filesystem::create_directories(dir, ec); }
                        char name[32];
                        std::snprintf(name, sizeof name, "/frame_%05llu.png", static_cast<unsigned long long>(capFrame / uint64_t(every)));
                        std::cout << "capture t=" << world->timeMs() << " anim=" << pweapon.anim() << " weapon=" << pweapon.name() << "\n"; // [server time]
                        hudKillcam = killcamView; // the capture's HUD is the killcam HUD too (killer's weapon and ammo)
                        screenshot(dev, renderer, scene, cam, w / uint32_t(div), h / uint32_t(div), dir + name, items, buildHud(w / uint32_t(div), h / uint32_t(div), cam), blur);
                        hudKillcam.reset();
                    }
                }
                profWater += double(profC - profB) * 1e-6;
                profDraw += double(SDL_GetTicksNS() - profC) * 1e-6;
                // screenshotJpeg / screenshot: the same frame drawn again into an offscreen target (the swapchain texture
                // cannot be read back), so the file holds the frame as displayed.
                if (pendingShot != Shot::None)
                    takeShot(std::exchange(pendingShot, Shot::None), captureRgba(dev, renderer, scene, cam, w, h, items, hud, blur), w, h);
            }
            SDL_SubmitGPUCommandBuffer(cmd);
            if (std::exchange(firstFramePending, false)) last = SDL_GetTicksNS(); // see endLoad
            if (traceFramesAfterLoad >= 0 && traceFramesAfterLoad < 3) {
                std::string what = "frame presented #" + std::to_string(++traceFramesAfterLoad);
                if (traceFramesAfterLoad == 1)
                    for (const std::string& v : mixer.describeVoices())
                        if (!v.empty()) what += " [" + v + "]"; // the opening dialogue's remaining length
                trace(what, world ? world->timeMs() : 0); // [server time]
            }
            profFrame += double(SDL_GetTicksNS() - profFrameStart) * 1e-6;
            if (killcamView) {
                cam.pos = killcamLivePos;
                cam.fovX43 = killcamLiveFov;
            }
            cam.pitch = camBase.pitch; // the ADS bob turns only the drawn view
            cam.yaw = camBase.yaw;
            cam.roll = camBase.roll;
        }
        if (netClient) netClient->disconnect();
        if (audioStream) SDL_DestroyAudioStream(audioStream);
        SDL_ReleaseWindowFromGPUDevice(dev, window);
        SDL_DestroyWindow(window);
        Uploader::releasePool(dev);
        SDL_DestroyGPUDevice(dev);
        SDL_Quit();
        if (dedicatedStart) { // the client is down; the server runs on in this process (docs/spec/mp-ui.md "Dedicated start")
            netClient.reset();
            if (listenServer) listenServer->shutdown("EXE_SERVERQUIT");
            listenServer.reset();
            world.reset();
            return runDedicatedConsole(vfs, *dedicatedStart);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
