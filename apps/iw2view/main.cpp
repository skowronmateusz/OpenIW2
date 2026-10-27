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
#include "assets/xanim.h"
#include "assets/xmodel.h"
#include "fs/vfs.h"
#include "audio/sound.h"
#include "video/roq.h"
#include "game/compassspring.h"
#include "game/clientinput.h"
#include "game/collision.h"
#include "game/playerweapon.h"
#include "game/screen_blur.h"
#include "game/viewweapon.h"
#include "game/watersim.h"
#include "game/fx.h"
#include "game/fog.h"
#include "game/marks.h"
#include "game/pmove.h"
#include "game/pmoverecord.h"
#include "game/turrets.h"
#include "game/world.h"
#include "ui/menu.h"
#include "ui/menurun.h"
#include "ui/frontend.h"
#include "ui/profile.h"
#include <filesystem>
#include <fstream>
#include "util/png.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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

    Mat4 viewProj(float aspect) const {
        const Vec3 f = forward(), r0 = right(), u0 = cross(r0, f);
        const float cr = std::cos(roll * float(M_PI) / 180), sr = std::sin(roll * float(M_PI) / 180);
        const Vec3 r = r0 * cr + u0 * -sr, u = cross(r, f);
        const Mat4 view = {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0, r.z, u.z, -f.z, 0,
                           -dot(r, pos), -dot(u, pos), dot(f, pos), 1};
        const float fovY = 2.0f * std::atan(std::tan(fovX43 * float(M_PI) / 360.0f) * 0.75f), n = 4.0f, fa = 40000.0f;
        const float t = 1.0f / std::tan(fovY / 2);
        const Mat4 proj = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, fa / (n - fa), -1, 0, 0, n * fa / (n - fa), 0};
        return mul(proj, view);
    }
};

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
};
static_assert(sizeof(GpuVertex) == 56);

enum class Pass { Opaque, AlphaTest, Blend, Additive, Multiply, Sky, Skip, Water };

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
};

struct Lightmap {
    SDL_GPUTexture* coeffs = nullptr; // 3-layer array (R, G, B coefficients)
    SDL_GPUTexture* sun = nullptr;
};

struct MatInfo {
    Pass pass;
    int texture;
    // mtlPolygonOffset: bits 4–5 of the material's second state word (file offset 0x30; statemap variable table of
    // the 1.3 renderer at 0x1018fe20: mask 0x30 of loadBits[1], values "0" / "1" / "2" = 0 / 0x10 / 0x20).
    int polygonOffset = 0;
    // mtlCullFace: bits 14–15 of state word 0 (file offset 0x2c; mask 0xc000, values None / Back / Front = 0x4000 /
    // 0x8000 / 0xc000, 1.3 renderer table 0x1018fa10), set as D3DRS_CULLMODE through {0, NONE, CCW, CW} (0x10190704,
    // 0x1003cb13). Here 0 none, 1 back (D3DCULL_CCW), 2 front (D3DCULL_CW).
    int cull = 0;
    // For Pass::Water `texture` indexes Scene::waters.
    bool scroll = false; // techset `*_scroll` (the scroll technique subtracts the game-time value from the V coordinate)
    int normalTex = -1, specTex = -1; // techset `phong_*_spec*`: normalMap / specularMap images (Scene::textures indices)
    bool waterBlend = false; // mtlBlendOp != Disable (bits 8-10 of state word 0: 0 Disable, 1 Add; inferred, see nofog.sm)
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
    SDL_GPUTexture* specularity = nullptr;  // "$specularity" lookup (32 x 256 L8): U = specular power, V = N.H
    std::vector<SDL_GPUTexture*> textures; // [0] = white
    std::vector<Lightmap> lightmaps;
    Lightmap dummyLightmap;
    std::vector<Batch> batches;
    SDL_GPUTexture* sky = nullptr;
    std::vector<WaterMat> waters;
    std::unique_ptr<game::CollisionWorld> collision;
    game::MarkSystem marks;
    std::unique_ptr<assets::LightGrid> grid;
    std::unordered_map<std::string, int> imageIndex;
    std::unordered_map<std::string, MatInfo> matCache;
    std::unordered_map<std::string, std::optional<GpuModel>> models;
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
        }
        try {
            const auto m = assets::parseMaterial(*md);
            mi.pass = classify(m.techniqueSet);
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
            const std::string& img = m.colorImage();
            auto imageSlot = [&](const std::string& image) {
                auto it = scene.imageIndex.find(image);
                if (it == scene.imageIndex.end()) {
                    SDL_GPUTexture* t = nullptr;
                    if (image == "$black") { // built-in image (string "$black" in gfx_d3d_x86_s.dll 0x193510); TODO(spec): its exact pixels, here 1x1 opaque black
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
            } else if (!img.empty() && (img == "$black" || (img[0] != '$' && mi.pass != Pass::Skip))) { // the "black" material (technique set "tools") is a UI background
                mi.texture = imageSlot(img);
                // phong_*_spec* techsets (lmap_s / lprobe_s): normalMap (semantic 3) + specularMap (semantic 4).
                if (m.techniqueSet.rfind("phong_", 0) == 0 && m.techniqueSet.find("_spec") != std::string::npos)
                    for (const auto& t : m.textures) {
                        if (t.imageName.empty() || t.imageName[0] == '$') continue;
                        if (t.semantic == assets::TextureSemantic::Normal && t.samplerName == "normalMap") mi.normalTex = imageSlot(t.imageName);
                        if (t.semantic == assets::TextureSemantic::Specular && t.samplerName == "specularMap") mi.specTex = imageSlot(t.imageName);
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
                std::memcpy(g.color, v.color.data(), 4);
                g.uv[0] = v.uv[0];
                g.uv[1] = v.uv[1];
                g.light[0] = g.light[1] = g.light[2] = 1.0f;
                verts.push_back(g);
            }
            Batch b;
            b.pass = mi.pass;
            b.texture = mi.texture;
            b.lightmap = -3;
            b.polygonOffset = mi.polygonOffset;
            b.cull = mi.cull;
        b.scroll = mi.scroll;
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
            std::memcpy(g.color, v.color.data(), 4);
            g.uv[0] = v.uv[0];
            g.uv[1] = v.uv[1];
            verts.push_back(g);
        }
        Batch b;
        b.pass = mi.pass;
        b.texture = mi.texture;
        b.lightmap = -3;
        b.polygonOffset = mi.polygonOffset;
        b.cull = mi.cull;
        b.scroll = mi.scroll;
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
        if (mi.pass == Pass::Skip || mi.pass == Pass::Sky) continue;
        Batch b;
        b.pass = mi.pass == Pass::Opaque || mi.pass == Pass::AlphaTest ? Pass::Blend : mi.pass;
        b.texture = mi.texture;
        b.lightmap = -3;
        b.fullbright = true;
        b.firstIndex = uint32_t(verts.size() / 4 * 6);
        for (const auto* s : list) {
            if (verts.size() / 4 >= kMax) break;
            const Vec3 c{s->position[0], s->position[1], s->position[2]};
            const Vec3 r = right * s->size, u = upv * s->size;
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
            }
            for (int k = 0; k < 4; ++k) {
                GpuVertex g{};
                g.pos[0] = corners[k].x; g.pos[1] = corners[k].y; g.pos[2] = corners[k].z;
                g.normal[2] = 1;
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
// Impact marks (game/marks.h) into a per-frame model: one batch per (material, lightmap), drawn in the blend pass
// with the lightmap of the surface each fragment came from (mark +0x20, the surface's lightmap, FUN_10002a00).
// TODO(spec): the renderer's mark drawing (refexport slot 0x68, 0x10024320) and its depth offset are not read.
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
        b.texture = mi.texture;
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
// diffuseFraction 0.5 (r_lightTweakDiffuseFraction), sunlight 1. TODO(spec): that this RGB is exactly the shader's lightColor0.
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
    Uploader up(dev);

    // White fallback texture.
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        SDL_GPUTexture* t = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1);
        up.texture(t, 0, 0, 1, 1, white, 4);
        scene.textures.push_back(t);
    }

    auto material = [&](const std::string& name) { return sceneMaterial(scene, dev, up, vfs, name); };
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
        std::memcpy(g.color, v.color.data(), 4);
        std::memcpy(g.uv, v.uv.data(), 8);
        std::memcpy(g.lmUv, v.lightmapUv.data(), 8);
    }

    // Batches: group soups by (pass, texture, lightmap) and emit global 32-bit indices.
    std::map<std::tuple<int, int, int, int, int, int, int>, std::vector<uint32_t>> groups;
    for (const auto& s : bsp.soups()) {
        const MatInfo& mi = mats[s.material];
        if (mi.pass == Pass::Skip) continue;
        const int lm = s.lightmap < scene.lightmaps.size() ? int(s.lightmap) : -1;
        // Specular only on lightmapped soups with both maps present (lmap_s); IW2_NO_SPEC disables it.
        static const bool noSpec = std::getenv("IW2_NO_SPEC") != nullptr;
        const bool spec = !noSpec && lm >= 0 && mi.normalTex > 0 && mi.specTex > 0 &&
                          (mi.pass == Pass::Opaque || mi.pass == Pass::AlphaTest || mi.pass == Pass::Blend);
        auto& dst = groups[{int(mi.pass), mi.texture, lm, mi.polygonOffset, mi.cull, spec ? mi.normalTex : -1, spec ? mi.specTex : -1}];
        for (uint32_t k = 0; k < s.indexCount; ++k) dst.push_back(s.firstVertex + bsp.indices()[s.firstIndex + k]);
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
                std::memcpy(g.color, v.color.data(), 4);
                g.uv[0] = v.uv[0]; g.uv[1] = v.uv[1];
                const float ndl = std::max(0.0f, dot(n, scene.sunDirection));
                std::array<float, 3> probe = {0.45f, 0.45f, 0.45f};
                if (haveGrid) probe = assets::LightGrid::evaluate(coeffs, {n.x, n.y, n.z});
                g.light[0] = probe[0] + sunVis * ndl * scene.sunColor.x;
                g.light[1] = probe[1] + sunVis * ndl * scene.sunColor.y;
                g.light[2] = probe[2] + sunVis * ndl * scene.sunColor.z;
                verts.push_back(g);
            }
            auto& dst = groups[{int(mi.pass), mi.texture, -2, mi.polygonOffset, mi.cull, -1, -1}];
            for (uint16_t i : surf.indices) dst.push_back(base + i);
        }
        ++modelCount;
    }
    std::cout << "placed " << modelCount << " static models (" << modelCache.size() << " unique)\n";

    std::vector<uint32_t> indices;
    for (const auto& [key, idx] : groups) {
        Batch b;
        b.pass = Pass(std::get<0>(key));
        b.texture = std::get<1>(key);
        b.lightmap = std::get<2>(key);
        b.polygonOffset = std::get<3>(key);
        b.cull = std::get<4>(key);
        b.normalTex = std::get<5>(key);
        b.specTex = std::get<6>(key);
        b.firstIndex = uint32_t(indices.size());
        b.indexCount = uint32_t(idx.size());
        indices.insert(indices.end(), idx.begin(), idx.end());
        scene.batches.push_back(b);
    }

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

struct FragmentUniforms {
    float sunDirection[4];
    float sunColor[4];
    float lmWeights[4];
    float params[4];
    float ambient[4];
    float scroll[4]; // x: V offset subtracted from the texture coordinate (tread scroll)
    float fogParams[4]; // x: 1 = on, y: scale, z: shift, w: exponent (!= 0: exponential fog)
    float fogColor[4];
};

// An entity model to draw this frame.
struct DrawItem {
    const GpuModel* model = nullptr;
    Mat4 transform{};
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float sunVisibility = 1.0f;
    float scrollV = 0.0f; // vehicle tread scroll
};

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
    SDL_GPUGraphicsPipeline* waterPipelines[2][3] = {}; // [blend][cull none / back / front]
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUSampler* waterMapSampler = nullptr;
    SDL_GPUTexture* depth = nullptr;
    uint32_t depthW = 0, depthH = 0;
    // Blur post effect: full-size scene copy, w/2 step and the w/4 x h/4 "$blurred_screen".
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* sceneTex = nullptr;
    SDL_GPUTexture* halfTex = nullptr;
    SDL_GPUTexture* quarterTex = nullptr;
    SDL_GPUBuffer* blurVerts = nullptr;
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

    void init(SDL_GPUDevice* device, SDL_GPUTextureFormat colorFormat_) {
        dev = device;
        colorFormat = colorFormat_;
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
        fs.num_samplers = 3;
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
        sfsi.num_samplers = 6;
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
                    statePipelines[p][lvl][c] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
                    if (!statePipelines[p][lvl][c]) sdlFail("SDL_CreateGPUGraphicsPipeline");
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
            skyPipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (!skyPipeline) sdlFail("sky pipeline");
            SDL_ReleaseGPUShader(dev, sv);
            SDL_ReleaseGPUShader(dev, sf);
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
        repeatSampler = SDL_CreateGPUSampler(dev, &si);
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.enable_anisotropy = false;
        clampSampler = SDL_CreateGPUSampler(dev, &si);
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        waterMapSampler = SDL_CreateGPUSampler(dev, &si);
    }

    void ensureBlurTargets(uint32_t w, uint32_t h) {
        if (sceneTex && blurW == w && blurH == h) return;
        for (SDL_GPUTexture** t : {&sceneTex, &halfTex, &quarterTex})
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
        halfTex = make(w / 2, h / 2);
        quarterTex = make(w / 4, h / 4); // $blurred_screen: (screenW>>2, screenH>>2) clamped >= 1 (gfx 0x100235b5)
        if (!blurVerts) {
            SDL_GPUBufferCreateInfo vb{};
            vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            vb.size = uint32_t(8 * sizeof(GpuVertex));
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
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth = SDL_CreateGPUTexture(dev, &info);
        if (!depth) sdlFail("depth texture");
        depthW = w;
        depthH = h;
    }

    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam,
              const std::vector<DrawItem>& items = {}, const std::vector<Quad2D>& overlay = {}, float blur = 0.0f) {
        ensureDepth(w, h);
        // Blur post effect: only when the composed radius is > 0 (gate in gfx 0x10030ed0); the scene goes to an offscreen
        // texture, then composite pass: scene, blurred quarter copy with alpha min(R / F, 1), then the HUD (not blurred).
        const bool post = game::blurAlpha(blur, float(w)) > 0.0f;
        if (post) {
            ensureBlurTargets(w, h);
            GpuVertex bv[8]{};
            for (int q = 0; q < 2; ++q)
                for (int k = 0; k < 4; ++k) {
                    GpuVertex& g = bv[q * 4 + k];
                    const float xs[4] = {-1, 1, 1, -1}, ys[4] = {1, 1, -1, -1}, us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
                    g.pos[0] = xs[k];
                    g.pos[1] = ys[k];
                    g.normal[2] = 1;
                    std::memset(g.color, 255, 3);
                    g.color[3] = q == 0 ? 255 : uint8_t(std::lround(game::blurAlpha(blur, float(w)) * 255.0f));
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
        color.clear_color = {0.55f, 0.65f, 0.78f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo ds{};
        ds.texture = depth;
        ds.clear_depth = 1.0f;
        ds.load_op = SDL_GPU_LOADOP_CLEAR;
        ds.store_op = SDL_GPU_STOREOP_DONT_CARE;
        ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &ds);

        struct VU { Mat4 vp; float eye[4]; Mat4 model; } vu{cam.viewProj(float(w) / float(h)), {cam.pos.x, cam.pos.y, cam.pos.z, 0}, kIdentity};
        // Binds the pass pipeline for a batch's polygon offset level; w of eye carries the constant depth bias.
        auto bindOffset = [&](Pass p, int level, int cull, VU u) {
            SDL_BindGPUGraphicsPipeline(pass, statePipelines[int(p)][level][cull]);
            u.eye[3] = float(level) * kPolygonOffsetBias / 65536.0f;
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
        for (Pass p : {Pass::Opaque, Pass::AlphaTest, Pass::Blend, Pass::Additive, Pass::Multiply}) {
            if (p == Pass::Blend) {
            // Water (water_l_sun.tech): after the opaque passes, before the blended ones. Polygon offset not applied (TODO(spec)).
            for (const Batch& b : scene.batches) {
                if (b.pass != Pass::Water) continue;
                const WaterMat& wm = scene.waters[size_t(b.texture)];
                if (!wm.cube || !wm.normalMap) continue;
                SDL_BindGPUGraphicsPipeline(pass, waterPipelines[wm.blend ? 1 : 0][b.cull]);
                SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
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
            }
            }
            SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
            for (const Batch& b : scene.batches) {
                if (b.pass != p || b.specTex >= 0) continue;
                if (b.polygonOffset || b.cull) bindOffset(p, b.polygonOffset, b.cull, vu);
                const Lightmap& lm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
                SDL_GPUTextureSamplerBinding tex[3] = {
                    {scene.textures[size_t(b.texture)], repeatSampler}, {lm.coeffs, clampSampler}, {lm.sun, clampSampler}};
                SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                FragmentUniforms fu = {
                    {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0},
                    {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                    {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                    {p == Pass::AlphaTest ? 1.0f : 0.0f, p == Pass::Multiply ? 4.0f : b.lightmap >= 0 ? 1.0f : (b.lightmap == -2 ? 2.0f : 0.0f), lmScale, fullbright ? 1.0f : 0.0f},
                    {0, 0, 0, 0}};
                if (p != Pass::Multiply && p != Pass::Additive) applyFog(fu); // TODO(spec): multiply_fog / additive fog variants
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                if (b.polygonOffset || b.cull) bindOffset(p, 0, 0, vu);
            }
            // lmap_s world batches (lib/phong.hlsl specular): tangent frame from vertex buffer 1.
            if (p == Pass::Opaque || p == Pass::AlphaTest || p == Pass::Blend) {
                bool any = false;
                for (const Batch& b : scene.batches) {
                    if (b.pass != p || b.specTex < 0 || !scene.tbn || !scene.specularity) continue;
                    if (!any) {
                        SDL_GPUBufferBinding tvb[2] = {{scene.vertices, 0}, {scene.tbn, 0}};
                        SDL_BindGPUVertexBuffers(pass, 0, tvb, 2);
                        any = true;
                    }
                    SDL_BindGPUGraphicsPipeline(pass, specPipelines[int(p)][b.polygonOffset][b.cull]);
                    const float specColorScale = specColorScaleDvar();
                    struct SVU { VU base; float sun[4]; } svu{vu, {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0}};
                    svu.base.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f;
                    SDL_PushGPUVertexUniformData(cmd, 0, &svu, sizeof(svu));
                    const Lightmap& lm = scene.lightmaps[size_t(b.lightmap)];
                    SDL_GPUTextureSamplerBinding tex[6] = {{scene.textures[size_t(b.texture)], repeatSampler}, {lm.coeffs, clampSampler},
                                                           {lm.sun, clampSampler}, {scene.textures[size_t(b.normalTex)], repeatSampler},
                                                           {scene.textures[size_t(b.specTex)], repeatSampler}, {scene.specularity, clampSampler}};
                    SDL_BindGPUFragmentSamplers(pass, 0, tex, 6);
                    FragmentUniforms fu = {
                        {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, specColorScale},
                        {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                        {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                        {p == Pass::AlphaTest ? 1.0f : 0.0f, 1.0f, lmScale, fullbright ? 1.0f : 0.0f},
                        {0, 0, 0, 0}};
                    applyFog(fu);
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                }
                if (any) { // restore the single-stream state for the model draws that follow
                    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                    SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
                    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
                }
            }
            // Entity models of this pass.
            for (const DrawItem& it : items) {
                bool bound = false;
                for (const Batch& b : it.model->batches) {
                    if (b.pass != p) continue;
                    if (!bound) {
                        SDL_GPUBufferBinding mvb{it.model->vertices, 0};
                        SDL_BindGPUVertexBuffers(pass, 0, &mvb, 1);
                        SDL_GPUBufferBinding mib{it.model->indices, 0};
                        SDL_BindGPUIndexBuffer(pass, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                        VU mu = vu;
                        mu.model = it.transform;
                        SDL_PushGPUVertexUniformData(cmd, 0, &mu, sizeof(mu));
                        bound = true;
                    }
                    const Lightmap& elm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
                    SDL_GPUTextureSamplerBinding tex[3] = {
                        {scene.textures[size_t(b.texture)], repeatSampler}, {elm.coeffs, clampSampler}, {elm.sun, clampSampler}};
                    SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                    FragmentUniforms fu = {
                        {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0},
                        {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                        {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                        {p == Pass::AlphaTest ? 1.0f : 0.0f, b.lightmap >= 0 ? 1.0f : 3.0f, lmScale, fullbright ? 1.0f : 0.0f},
                        {it.ambient[0], it.ambient[1], it.ambient[2], it.sunVisibility},
                        {b.scroll ? it.scrollV : 0.0f, 0, 0, 0}};
                    if (p != Pass::Multiply && p != Pass::Additive) applyFog(fu); // TODO(spec): multiply_fog / additive fog variants
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    if (b.polygonOffset || b.cull) {
                        VU mu = vu;
                        mu.model = it.transform;
                        bindOffset(p, b.polygonOffset, b.cull, mu);
                    }
                    SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                    if (b.polygonOffset || b.cull) {
                        VU mu = vu;
                        mu.model = it.transform;
                        bindOffset(p, 0, 0, mu);
                    }
                }
                if (bound) {
                    // Restore world buffers for the next pass's world batches.
                    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
                }
            }
        }
        if (nq && !post) drawOverlay(pass, cmd, scene, overlay, nq);
        SDL_EndGPURenderPass(pass);
        if (post) {
            // Bilinear downsample chain w -> w/2 -> w/4. TODO(spec): the filter kernel of $blurred_screen is unknown.
            auto blit = [&](SDL_GPUTexture* src, uint32_t sw, uint32_t sh, SDL_GPUTexture* dst, uint32_t dw, uint32_t dh) {
                SDL_GPUBlitInfo bi{};
                bi.source.texture = src;
                bi.source.w = sw;
                bi.source.h = sh;
                bi.destination.texture = dst;
                bi.destination.w = std::max(dw, 1u);
                bi.destination.h = std::max(dh, 1u);
                bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
                bi.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &bi);
            };
            blit(sceneTex, w, h, halfTex, w / 2, h / 2);
            blit(halfTex, std::max(w / 2, 1u), std::max(h / 2, 1u), quarterTex, w / 4, h / 4);
            SDL_GPUColorTargetInfo c2{};
            c2.texture = target;
            c2.load_op = SDL_GPU_LOADOP_DONT_CARE;
            c2.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo d2 = ds;
            SDL_GPURenderPass* p2 = SDL_BeginGPURenderPass(cmd, &c2, 1, &d2);
            SDL_BindGPUGraphicsPipeline(p2, pipelines[int(Pass::Blend)]);
            struct VU2 { Mat4 vp; float eye[4]; Mat4 model; } ou{kIdentity, {0, 0, 0, 0}, kIdentity};
            SDL_PushGPUVertexUniformData(cmd, 0, &ou, sizeof(ou));
            SDL_GPUBufferBinding bvb{blurVerts, 0};
            SDL_BindGPUVertexBuffers(p2, 0, &bvb, 1);
            SDL_GPUBufferBinding bib{overlayIndices ? overlayIndices : nullptr, 0};
            if (!bib.buffer) bib.buffer = ensureOverlayIndices();
            SDL_BindGPUIndexBuffer(p2, &bib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            FragmentUniforms fu = {{0, 0, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 0, 0}};
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
            SDL_GPUTexture* srcs[2] = {sceneTex, quarterTex};
            for (int q = 0; q < 2; ++q) {
                SDL_GPUTextureSamplerBinding tex[3] = {{srcs[q], clampSampler},
                                                       {scene.dummyLightmap.coeffs, clampSampler},
                                                       {scene.dummyLightmap.sun, clampSampler}};
                SDL_BindGPUFragmentSamplers(p2, 0, tex, 3);
                SDL_DrawGPUIndexedPrimitives(p2, 6, 1, 0, q * 4, 0);
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
            SDL_BindGPUGraphicsPipeline(pass, pipelines[int(Pass::Blend)]);
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
                SDL_GPUTextureSamplerBinding tex[3] = {{scene.textures[size_t(overlay[i].texture)], clampSampler},
                                                       {scene.dummyLightmap.coeffs, clampSampler},
                                                       {scene.dummyLightmap.sun, clampSampler}};
                SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                SDL_DrawGPUIndexedPrimitives(pass, uint32_t((j - i) * 6), 1, uint32_t(i * 6), 0, 0);
                i = j;
            }
        }
    }
};

void screenshot(SDL_GPUDevice* dev, Renderer& r, const Scene& scene, const Camera& cam, uint32_t w, uint32_t h, const std::string& out,
                const std::vector<DrawItem>& items, const std::vector<Quad2D>& overlay = {}, float blur = 0.0f) {
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
    util::writePngRgba(out, px, int(w), int(h));
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTexture(dev, target);
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
    bool menuMode = false; // --menu: start in the front end (main menu) without a level
    float shotViewHeight = 60;
    float shotLean = 0;
    bool shotAds = false;
    bool godMode = false; // --god / F10: the "god" cheat (0x4e9f90 toggles FL_GODMODE on the player)
    bool shotUse = false; // --shot: press F (+activate) after the simulated seconds
    std::string language = "polish";
    bool scripts = true;
    bool spawnAll = false;
    std::vector<std::pair<std::string, std::string>> extraCvars; // --cvar name=value (e.g. start=ride)
    std::optional<std::array<float, 2>> rideDrive; // --ride F,R: with --shot, drive the player's vehicle (forward, right in -1..1) after --ride-after
    float rideAfter = 0;
    uint32_t rideButtons = 0; // --ride-fire sets the attack button
    int followVehicle = -2; // -2 off, -1 list only, N the Nth entity with vehicle state
    float followDist = 400.0f, followYaw = 180.0f, followPitch = 12.0f;
    std::optional<std::array<float, 2>> forceTurret, forceJolt;
    Renderer renderer;
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
            else if (a == "--god") godMode = true;
            else if (a == "--stance") { const std::string st = next(); shotViewHeight = st == "prone" ? 11.0f : st == "crouch" ? 40.0f : 60.0f; }
            else if (a == "--ads") shotAds = true;
            else if (a == "--use") shotUse = true;
            else if (a == "--lean") shotLean = std::stof(next());
            else if (a == "--language") language = next();
            else if (a == "--no-scripts") scripts = false;
            else if (a == "--menu") menuMode = true;
            else if (a == "--spawn-all") spawnAll = true;
            else if (a == "--lm-weights") { auto v = parseFloats(next()); for (int k = 0; k < 4; ++k) renderer.lmWeights[k] = v.at(size_t(k)); }
            else args.push_back(a);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
    }
    if (args.size() < (menuMode ? 1u : 2u)) {
        std::cerr << "usage: iw2view <main-dir> <map-name> [--shot out.png] [--pos x,y,z] [--yaw d] [--pitch d] [--size WxH]\n";
        return 2;
    }

    try {
        fs::Vfs vfs;
        vfs.mountDirectory(args[0]);
        if (const char* overlay = std::getenv("IW2_OVERLAY")) vfs.mountDirectory(overlay); // debug: loose test scripts override the game files

        if (!SDL_Init(SDL_INIT_VIDEO | (shot.empty() && audioOn ? SDL_INIT_AUDIO : 0))) sdlFail("SDL_Init");

        // Sound: aliases for this level, a software mixer and an SDL audio stream that pulls from it.
        audio::SoundAliases aliases;
        if (menuMode) aliases.load(vfs, "menu"); // front-end sounds (loadspec "menu")
        else aliases.load(vfs, args[1]);
        constexpr int kAudioRate = 48000;
        audio::Mixer mixer(vfs, aliases, kAudioRate);
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
        // A level's game world. game[] survives level changes (maps/_endmission.gsc and _loadout.gsc rely on it,
        // docs/spec/campaign.md), so its fields are copied into the new world before its script starts.
        struct Subtitle { std::string key; uint64_t untilNs; };
        std::vector<Subtitle> subtitles; // cg subtitle lines (cgame 0x4a3540 → console channel → 0x4bf2e0)
        // HUD fade "touch" times (world ms; 0 = never touched = hidden when the fade dvar is non-zero): ammo display 0xf79a40 and
        // compass 0xf79a38 (touched by 0x4a6130 / 0x4be5f0 / weapon changes 0x4b76f2, 0x4b8391), and the weapon change time 0xf796e4.
        game::CompassSpring compassSpring; // angle spring of the compass face (0x4b09a0)
        struct HudFade { int64_t compass = 0, ammo = 0, weaponChange = 0, flashStart = 0, stance = 0, offhand = 0; std::string weapon; float pitch = 0, yaw = 0; uint32_t buttons = 0; } hudFade;
        std::map<std::string, std::string> uiCvars; // client-side (menu) cvars, kept across levels and handed to each new world
        // Player profile: <game dir>/players/<name>/config.cfg (exe strings players/%s/, config.cfg; TODO(spec): the real base path
        // and the profile name when none is chosen, here "default").
        const std::filesystem::path playersDir = std::filesystem::path(args[0]).parent_path() / "players";
        std::string profileName = "default";
        if (std::ifstream af(playersDir / "active.txt"); af) { std::string n; std::getline(af, n); if (!n.empty()) profileName = n; }
        const std::filesystem::path profileCfg = playersDir / profileName / "config.cfg";
        bool profileDirty = false;
        if (std::ifstream cf(profileCfg); cf) {
            const std::string cfgText(std::istreambuf_iterator<char>(cf), {});
            uiCvars = ui::parseConfig(cfgText);
            if (cfgText.find("unbindall") != std::string::npos) input.loadConfig(cfgText); // saved key bindings replace the defaults
        }
        mixer.masterVolume = uiCvars.count("snd_volume") ? std::clamp(float(std::atof(uiCvars["snd_volume"].c_str())), 0.0f, 1.0f) : mixer.masterVolume;
        auto makeWorld = [&](const std::string& map, const script::ObjectPtr& carriedGame) {
            auto w = std::make_unique<game::World>(vfs, map);
            for (const auto& [k, v] : uiCvars) w->cvars[k] = v;
            for (const auto& [k, v] : extraCvars) w->cvars[k] = v;
            w->print = [](const std::string& m) { std::cout << "[script] " << m << "\n"; };
            w->playSound = [&mixer, &subtitles](const std::string& alias, const game::Vec3* origin, float* duration) {
                float dur = 0;
                const int handle = mixer.play(alias, origin ? std::optional<audio::Vec3>(*origin) : std::nullopt, &dur);
                if (duration) *duration = dur;
                // A started alias with a subtitle column shows its caption for max(sound length, cg_subtitleMinTime 3 s) (0x4a3540).
                if (handle && !mixer.lastSubtitle().empty())
                    subtitles.push_back({mixer.lastSubtitle(), SDL_GetTicksNS() + uint64_t(std::max(dur, 3.0f) * 1e9f)});
                return handle;
            };
            w->stopSound = [&mixer](int h) { mixer.stop(h); };
            w->soundExists = [&aliases](const std::string& n) { return aliases.exists(n); };
            w->playBlendSound = [&mixer](const std::string& lo, const std::string& hi, float f, const game::Vec3& o) {
                return mixer.playBlend(lo, hi, f, audio::Vec3{o[0], o[1], o[2]});
            };
            w->setBlendSound = [&mixer](int h, float f, const game::Vec3& o) { mixer.setBlend(h, f, audio::Vec3{o[0], o[1], o[2]}); };
            if (carriedGame)
                for (const auto& [k, v] : carriedGame->fields) w->vm().game()->set(k, v);
            // The bindings from default.cfg, reported to scripts through getkeybinding.
            for (const auto& [key, command] : input.bindings()) w->keyBindings[command].push_back(key);
            w->start();
            return w;
        };
        std::string currentMap = menuMode ? std::string() : args[1];
        std::unique_ptr<game::World> world;
        if (scripts && !menuMode) {
            world = makeWorld(currentMap, nullptr);
            if (spawnAll) std::cout << "spawned " << world->spawnAllActors() << " actors\n";
        }
        Scene scene = menuMode ? emptyScene(dev) : loadScene(dev, vfs, args[1], !world);
        // Objective and other on-screen messages from the level script (client commands obj_update/obj_complete).
        struct ScreenMessage { std::string text; uint64_t untilNs; bool bold = false; uint64_t startNs = 0, endNs = 0; };
        std::vector<ScreenMessage> screenMessages;
        // Death screen client state (docs/spec/campaign.md "Death screen"): opendeadscreen start (world time,
        // 0xf7966c), time_slow (real ms, 0xf7965c..0xf79668 → frame scale 0x747edc).
        int64_t deadScreenMs = 0;
        uint64_t slowStartNs = 0, slowEndNs = 0;
        float slowFrom = 1, slowTo = 1, slowmo = 1;
        game::FogState fogState; // "fog" server command (client 0x4a5770)
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
                    if (const auto d = vfs.read(path))
                        it->second = std::make_shared<assets::EfxEffect>(
                            assets::parseEfx(std::string_view(reinterpret_cast<const char*>(d->data()), d->size())));
                } catch (const std::exception& e) {
                    std::cerr << "warning: effect " << path << ": " << e.what() << "\n";
                }
            }
            return it->second;
        };
        fxSystem.loader = efx;
        Vec3 fxRight{0, -1, 0}, fxUp{0, 0, 1};
        // The view weapon's muzzle flash (0x4b6c37): a shot sets the fire flag (+0x142, 0x4b7800); the next view model update
        // plays viewFlashEffect (0xf48e44) bolted to the view model's tag_flash (0x4b6660, edx = 1).
        struct ViewFlash { bool pending = false, valid = false; game::FVec pos{}; bool brassPending = false, brassValid = false; game::FVec brassPos{}; };
        auto viewFlash = std::make_shared<ViewFlash>();

        // Entity models currently visible in the game world.
        // Client interpolation state: entity origin/angles before the latest server frame and the fraction of the
        // next frame already elapsed (cg.frameInterpolation).
        std::map<int, std::pair<script::Vec3, script::Vec3>> lerpPrev;
        // Anim state before the last server frame: the drawn pose is that state advanced by the interpolation fraction, so poses move
        // smoothly with the lerped origin instead of stepping at 20 Hz (the original advances anim time per client frame, 0x4afb70).
        std::map<int, game::AnimState> prevAnim;
        std::map<int, std::pair<float, float>> prevTurret; // turret pitch/yaw before the last server frame (drawn interpolated)
        float lerpFrac = 1.0f;
        // The player's turret pose of this rendered frame (pitch, yaw), from the pre-kick view; the gun is drawn with it and
        // the eye (tag_player) is taken at the same pair.
        std::optional<std::pair<float, float>> playerTurretPose;
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
            Uploader up(dev, false);
            for (const auto& ev : world->takeFxEvents()) {
                if (std::getenv("IW2_MARK_DEBUG")) std::cerr << "fx event " << ev.path << "\n";
                if (ev.entity >= 0 && !ev.tag.empty()) {
                    // Bolted (0x4853f0 attachment): the tag position is re-read while relative particles live (cached per world time).
                    game::World* wp = world.get();
                    auto bolt = std::make_shared<game::FxBolt>([wp, n = ev.entity, tag = ev.tag, lastT = int64_t(-1), last = game::FVec{}, ok = false](game::FVec& out) mutable {
                        if (wp->timeMs() != lastT) {
                            lastT = wp->timeMs();
                            game::Vec3 p{};
                            ok = wp->entityTagPosition(n, tag, p);
                            if (ok) last = {p[0], p[1], p[2]};
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
                    fxSystem.playBolted(efx(ev.path), bolt, game::FxAxes{f, r, up}, world->timeMs());
                    continue;
                }
                fxSystem.play(efx(ev.path), ev.origin, ev.forward, world->timeMs());
            }
            for (const auto& te : world->takeTracerEvents())
                fxSystem.addTracer({te.from[0], te.from[1], te.from[2]}, {te.to[0], te.to[1], te.to[2]}, world->timeMs(), 0);
            fxSystem.viewRight = {fxRight.x, fxRight.y, fxRight.z};
            fxSystem.viewUp = {fxUp.x, fxUp.y, fxUp.z};
            for (const auto& d : fxSystem.takeDecals(world->timeMs()))
                scene.marks.impactMark(d.material, d.origin, d.dir, d.rotation, d.rgba, d.size, world->timeMs());
            if (const GpuModel* mm = marksModel(scene, dev, up, vfs)) {
                DrawItem it;
                it.model = mm;
                it.transform = kIdentity;
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
                scene.marks.markDrawn(world->timeMs()); // TODO(spec): the per-mark view cull (0x486e30)
            }
            // Effects are evaluated at the drawn time (cg.time in the original, per client frame): the server time minus the one
            // frame the drawn state lags plus the interpolation fraction, so particles move smoothly instead of at 20 Hz.
            const int64_t fxNow = world->timeMs() - 50 + int64_t(lerpFrac * 50.0f);
            const auto fxSprites = fxSystem.sprites(fxNow);
            if (std::getenv("IW2_FX_DEBUG"))
                for (size_t i = 0; i < fxSprites.size(); i += std::max<size_t>(1, fxSprites.size() / 12))
                    std::cerr << "fx sprite " << i << "/" << fxSprites.size() << " " << fxSprites[i].shader << " pos " << fxSprites[i].position[0] << ","
                              << fxSprites[i].position[1] << "," << fxSprites[i].position[2] << " size " << fxSprites[i].size << " rgba "
                              << fxSprites[i].rgba[0] << "," << fxSprites[i].rgba[1] << "," << fxSprites[i].rgba[2] << "," << fxSprites[i].rgba[3] << "\n";
            world->setFxSightBlockers(fxSystem.sightBlockers());
            if (const GpuModel* fxm = spriteModel(scene, dev, up, vfs, fxSprites, fxRight, fxUp)) {
                DrawItem it;
                it.model = fxm;
                it.transform = kIdentity;
                it.ambient[0] = it.ambient[1] = it.ambient[2] = 1.0f;
                it.sunVisibility = 0;
                items.push_back(it);
            }
            for (const auto& e : world->entities()) {
                if (e->obj->removed || e->isSpawner || e->isPlayer || e->obj->get("$hidden").truthy()) continue;
                const script::Value m = e->obj->get("model");
                if (!m.isString() || m.asString().rfind("xmodel/", 0) != 0) continue;
                const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                if (!o.isVector()) continue;
                auto ov = o.asVector();
                auto av = a.isVector() ? a.asVector() : script::Vec3{0, 0, 0};
                // CG_CalcEntityLerpPositions (0x4acca0): entities the server moves every frame (TR_INTERPOLATE) are
                // drawn between the previous and the latest server frame at cg.frameInterpolation (0xf708d0):
                // origin lerped (0x4acb90), angles by LerpAngle (0x4acc10 → 0x423ee0). TODO(spec): entities with
                // other trajectory types (evaluated at cg.time) and the anim poses, which still step at 20 Hz.
                if (auto pv = lerpPrev.find(e->number); pv != lerpPrev.end()) {
                    for (int k = 0; k < 3; ++k) {
                        ov[size_t(k)] = pv->second.first[size_t(k)] + (ov[size_t(k)] - pv->second.first[size_t(k)]) * lerpFrac;
                        const float d = std::remainder(av[size_t(k)] - pv->second.second[size_t(k)], 360.0f);
                        av[size_t(k)] = pv->second.second[size_t(k)] + d * lerpFrac;
                    }
                }
                DrawItem base;
                base.transform = entityTransform({ov[0], ov[1], ov[2]}, {av[0], av[1], av[2]});
                if (e->vehicle) base.scrollV = game::vehicleTexScrollOffset(e->vehicle->texScroll); // TODO(spec): accumulator interpolation
                assets::LightGridCoeffs c;
                float sun = 1.0f;
                if (scene.grid && scene.grid->sample({ov[0], ov[1], ov[2] + 4.0f}, c, sun)) {
                    const auto l = assets::LightGrid::evaluate(c, {0, 0, 1});
                    base.ambient[0] = l[0]; base.ambient[1] = l[1]; base.ambient[2] = l[2];
                    base.sunVisibility = sun;
                }
                if (e->vehicle && e->vehicle->info) {
                    // Vehicle turret (cgame 0x4ac2b0 bone controls on tag_turret / tag_barrel; es+0x70 pitch, es+0x74 yaw, lerped).
                    // TODO(spec): which control gets the pitch and which the yaw is a hypothesis (yaw on tag_turret, pitch on tag_barrel).
                    const Scene::CpuModel* vm = sceneCpuModel(scene, dev, vfs, m.asString());
                    if (!vm) continue;
                    float tp = e->vehicle->turretPitch, ty = e->vehicle->turretYaw;
                    if (auto pt = prevTurret.find(e->number); pt != prevTurret.end()) {
                        tp = pt->second.first + angleDelta(tp, pt->second.first) * lerpFrac;
                        ty = pt->second.second + angleDelta(ty, pt->second.second) * lerpFrac;
                    }
                    std::vector<std::pair<int, assets::Quat>> ctl;
                    for (size_t bi = 0; bi < vm->parts.bones.size(); ++bi) {
                        std::string n = vm->parts.bones[bi].name;
                        for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                        if (n == "tag_body") ctl.push_back({int(bi), assets::anglesQuat(e->vehicle->joltPitch, 0, e->vehicle->joltRoll)});
                        else if (n == "tag_turret") ctl.push_back({int(bi), assets::anglesQuat(0, ty, 0)});
                        else if (n == "tag_barrel") ctl.push_back({int(bi), assets::anglesQuat(tp, 0, 0)});
                    }
                    assets::XPose vpose = e->anim ? e->anim->pose(vm->parts) : assets::bindPose(vm->parts);
                    DrawItem it = base;
                    it.model = skinModel(scene, dev, up, e->number, m.asString(), *vm, assets::applyControls(vm->parts, vpose, ctl));
                    if (it.model) items.push_back(it);
                    continue;
                }
                // Actors and animated script_models (drones: spawn("script_model") + useanimtree + attach head/gun, maps/_drones.gsc) are
                // skinned with the blended pose and carry their attachments.
                if (e->isActor || ((e->anim || !e->attachments.empty()) && !e->turret)) {
                    // Animated: skin the body with the blended pose and attach head/helmet/etc.
                    if (!noCull && !sphereVisible(ov[0], ov[1], ov[2] + 36.0f, 256.0f)) continue; // anim state already stepped
                    const Scene::CpuModel* body = sceneCpuModel(scene, dev, vfs, m.asString());
                    if (!body) continue;
                    assets::XPose pose;
                    const game::AnimState* animNow = e->anim.get();
                    std::optional<game::AnimState> tmpAnim;
                    if (auto pa = prevAnim.find(e->number); e->anim && pa != prevAnim.end() && lerpFrac < 1.0f) {
                        tmpAnim.emplace(pa->second); // no notifies: they already fired in the server frame
                        tmpAnim->update(lerpFrac * 0.05f, [](const std::string&, const std::string&) {});
                        animNow = &*tmpAnim;
                        pose = tmpAnim->pose(body->parts);
                    } else {
                        pose = e->anim ? e->anim->pose(body->parts) : assets::bindPose(body->parts);
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
                    DrawItem it = base;
                    it.model = skinModel(scene, dev, up, e->number, m.asString(), *tm,
                                         assets::controlledPose(tm->parts, game::World::turretBoneControls(tm->parts, tp, ty)));
                    if (it.model) items.push_back(it);
                    continue;
                }
                const GpuModel* gm = sceneModel(scene, dev, vfs, m.asString());
                if (!gm) continue;
                DrawItem it = base;
                it.model = gm;
                items.push_back(it);
            }
            up.flush();
            return items;
        };
        // First-person weapon: hands and gun posed by the weapon's current view-model animation and drawn in
        // camera space. The gun's root follows the hands' tag_weapon bone (inferred from the skeletons).
        game::PlayerWeapon pweapon;
        game::ViewWeaponMotion vmMotion;
        game::RecoilKick recoil;
        game::AimBlend aimBlend; // the view model aim layers' cross-fade (0x4b25e0)
        std::string aimBlendWeapon;
        game::ViewImpulses viewImp; // damage kick, landing dip
        int lastLandSeq = 0;
        game::V3 gunBobAngles{0, 0, 0}; // cgame 0x4b4a90
        // The command's weapon (cg.weaponSelect): follows the scripts' weapon changes (switchtoweapon, giveweapon set
        // $weapon) and weapnext; PM_Weapon in PlayerWeapon drops the old one and raises it, then $weapon follows.
        std::string weaponSelect, weaponWritten, prevSelect;
        auto syncWeapon = [&]() {
            if (!world || !world->player()) return;
            pweapon.lookup = [&](const std::string& n) { return world->weapon(n); };
            pweapon.animLength = [&](const std::string& n) {
                const assets::XAnim* an = n.empty() ? nullptr : viewAnim(vfs, n);
                return an && !an->looping ? an->length() : 0.0f;
            };
            pweapon.findOffhand = [&](const std::string& cls) { return world->playerOffhandFor(cls); };
            if (world->player()->obj->get("$weapontaken").truthy()) {
                world->player()->obj->set("$weapontaken", script::Value());
                pweapon.weaponTaken();
            }
            const script::Value w = world->player()->obj->get("$weapon");
            // No weapon ("none", ps weapon 0) means nothing in hand, as at the start of moscow (only the potato).
            std::string name = w.isString() && !w.asString().empty() && w.asString() != "none" ? w.asString() : std::string();
            if (const char* forced = std::getenv("IW2_WEAPON")) name = forced; // debug override
            if (name != weaponWritten) weaponSelect = name;
            if (!pweapon.equipped() && !pweapon.holstered()) // the first weapon is in hand at once
                if (const auto* def = world->weapon(weaponSelect)) pweapon.equip(weaponSelect, *def);
        };
        auto writeWeapon = [&]() {
            if (!world || !world->player() || !pweapon.equipped()) return;
            world->player()->obj->set("$weapon", script::Value(pweapon.name()));
            weaponWritten = pweapon.name();
        };
        auto addViewModel = [&](std::vector<DrawItem>& items, const Camera& c) {
            if (!pweapon.equipped()) return;
            const Scene::CpuModel* hands = sceneCpuModel(scene, dev, vfs, pweapon.handModel());
            const Scene::CpuModel* gun = sceneCpuModel(scene, dev, vfs, pweapon.gunModel());
            const assets::XAnim* an = viewAnim(vfs, pweapon.anim());
            float frame = 0;
            if (an) {
                const float last = float(std::max(0, an->numFrames - 1));
                const float f = pweapon.animTime() * float(an->frameRate);
                frame = pweapon.animLoops() && last > 0 ? std::fmod(f, last) : std::min(f, last);
            }
            if (const char* forced = std::getenv("IW2_VM_ANIM")) { // debug: "anim,frame"
                std::string spec = forced;
                const auto comma = spec.find(',');
                if (const assets::XAnim* fa = viewAnim(vfs, spec.substr(0, comma))) {
                    an = fa;
                    frame = comma == std::string::npos ? 0.0f : std::stof(spec.substr(comma + 1));
                }
            }
            // The view model's anim tree (0x4b2c90, 0x4b25e0): the current action animation (idle, fire, reload...) with
            // weight 1 and both aim animations: adsUpAnim at time = aim fraction, adsDownAnim at time = 1 - aim fraction,
            // their weights cross-fading over 0.5 s (game::AimBlend); all are children of the root, so the engine's calc
            // adds them and divides by the weight sum (docs/spec/xanim.md, "Blending").
            std::vector<assets::PoseLayer> layers;
            if (an) layers.push_back({an, frame, 1.0f});
            if (!std::getenv("IW2_VM_NOADS")) {
                for (const bool up : {true, false}) {
                    const float wgt = up ? aimBlend.up() : aimBlend.down();
                    if (wgt <= 0) continue;
                    const float t = up ? pweapon.adsFrac() : 1.0f - pweapon.adsFrac();
                    if (const assets::XAnim* ads = viewAnim(vfs, pweapon.field(up ? "adsUpAnim" : "adsDownAnim")))
                        layers.push_back({ads, t * float(std::max(0, ads->numFrames - 1)), wgt});
                }
            }
            DrawItem base;
            // Stance, movement and aiming offsets (forward, right, up; pitch, yaw, roll) from ViewWeaponMotion.
            const auto mv = vmMotion.move();
            auto rt = vmMotion.rot();
            for (int k = 0; k < 3; ++k) rt[size_t(k)] += gunBobAngles[size_t(k)]; // 0x4b4d10
            {
                const auto gk = recoil.gunKick(); // 0x4b5d10
                for (int k = 0; k < 3; ++k) rt[size_t(k)] += gk[size_t(k)];
            }
            base.transform = entityTransform(c.pos + c.forward() * mv[0] + c.right() * mv[1] + cross(c.right(), c.forward()) * mv[2],
                                             {c.pitch + rt[0], c.yaw + rt[1], rt[2]});
            if (const char* dbg = std::getenv("IW2_VIEWMODEL_DEBUG")) {
                // Debug: show the view model from outside, `dist` units ahead, turned by `yaw` degrees.
                float dist = 40, turn = 180;
                std::sscanf(dbg, "%f,%f", &dist, &turn);
                base.transform = entityTransform(c.pos + c.forward() * dist, {0, c.yaw + turn, 0});
            }
            assets::LightGridCoeffs lc;
            float sun = 1.0f;
            if (scene.grid && scene.grid->sample({c.pos.x, c.pos.y, c.pos.z}, lc, sun)) {
                const auto l = assets::LightGrid::evaluate(lc, {0, 0, 1});
                base.ambient[0] = l[0]; base.ambient[1] = l[1]; base.ambient[2] = l[2];
                base.sunVisibility = sun;
            }
            Uploader up(dev, false);
            BoneMap bones;
            if (hands) {
                const assets::XPose hp = an ? assets::blendPose(hands->parts, layers) : assets::bindPose(hands->parts);
                addBones(bones, hands->parts, hp);
                DrawItem it = base;
                it.model = skinModel(scene, dev, up, -1, pweapon.handModel(), *hands, hp);
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
                it.model = skinModel(scene, dev, up, -1, pweapon.gunModel(), *gun, gp);
                if (it.model) items.push_back(it);
                {
                    BoneMap gunBones;
                    addBones(gunBones, gun->parts, gp);
                    const auto tf = gunBones.find("tag_flash");
                    viewFlash->valid = tf != gunBones.end();
                    if (viewFlash->valid) {
                        const auto& m = base.transform; // columns: forward, left, up, origin (entityTransform)
                        const auto& q = tf->second.first;
                        for (int k = 0; k < 3; ++k) viewFlash->pos[size_t(k)] = q[0] * m[size_t(k)] + q[1] * m[size_t(4 + k)] + q[2] * m[size_t(8 + k)] + m[size_t(12 + k)];
                        if (std::getenv("IW2_FX_DEBUG")) std::cerr << "view flash tag " << viewFlash->pos[0] << "," << viewFlash->pos[1] << "," << viewFlash->pos[2] << " pending " << viewFlash->pending << " fx \"" << pweapon.field("viewFlashEffect") << "\"\n";
                        // CG_EjectWeaponBrass (0x4b7730) for the local client: shellEjectEffect bolted on the view weapon's tag_brass.
                        const auto tb = gunBones.find("tag_brass");
                        viewFlash->brassValid = tb != gunBones.end();
                        if (viewFlash->brassValid) {
                            const auto& m = base.transform;
                            const auto& q = tb->second.first;
                            for (int k = 0; k < 3; ++k) viewFlash->brassPos[size_t(k)] = q[0] * m[size_t(k)] + q[1] * m[size_t(4 + k)] + q[2] * m[size_t(8 + k)] + m[size_t(12 + k)];
                        }
                        if (viewFlash->brassPending && world) {
                            viewFlash->brassPending = false;
                            const std::string path = pweapon.field("shellEjectEffect");
                            if (viewFlash->brassValid && !path.empty() && path != "none") {
                                auto bolt = std::make_shared<game::FxBolt>([vf = viewFlash](game::FVec& o) {
                                    o = vf->brassPos;
                                    return vf->brassValid;
                                });
                                const auto& m = base.transform;
                                const game::FVec f{m[0], m[1], m[2]}, l{m[4], m[5], m[6]}, u{m[8], m[9], m[10]};
                                fxSystem.playBolted(efx(path), bolt, game::FxAxes{f, l, u}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 0));
                            }
                        }
                        if (viewFlash->pending && world) {
                            viewFlash->pending = false;
                            const std::string path = pweapon.field("viewFlashEffect");
                            if (!path.empty()) {
                                auto bolt = std::make_shared<game::FxBolt>([vf = viewFlash](game::FVec& o) {
                                    o = vf->pos;
                                    return vf->valid;
                                });
                                const game::FVec f{m[0], m[1], m[2]}, l{m[4], m[5], m[6]}, u{m[8], m[9], m[10]};
                                fxSystem.playBolted(efx(path), bolt, game::FxAxes{f, l, u}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 0)); // IW2_FX_MS: debug age
                            }
                        }
                    }
                }
            }
            up.flush();
        };

        // HUD: script elements, crosshair and ammo counter, laid out on a 640x480 virtual screen
        // (alignment rules are inferred; see docs/spec/ui.md).
        assets::Localization loc;
        loc.load(vfs, language);
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
        auto materialTexture = [&](const std::string& name) {
            Uploader up(dev);
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
        // Script menus (openmenu, docs/spec/ui.md): loaded from "ui/scriptmenus/<name>.menu" on the first "popupopen" (0x4c3ad0).
        ui::MenuLibrary scriptMenus(vfs);
        float menuMx = 320.0f, menuMy = 240.0f; // cursor in the 640x480 virtual screen
        SDL_Window* menuWindow = nullptr; // set once the window exists
        // Client-side cvars of the menus (persist across levels; the running world's cvars take precedence while it exists).
        std::optional<std::pair<std::string, bool>> pendingLevel; // map to load (from "map X" or a level change), failed flag
        // Script menu environment (src/ui/menurun.h): the menu's commands act on the game cvars and send responses back.
        ui::MenuEnv menuEnv;
        menuEnv.getCvar = [&](const std::string& n) {
            if (n == "sv_running") return std::string(world ? "1" : "0"); // TODO(spec): server state cvar
            if (world) {
                const auto it = world->cvars.find(n);
                if (it != world->cvars.end()) return it->second;
            }
            const auto u = uiCvars.find(n);
            return u == uiCvars.end() ? ui::registeredDefault(n) : u->second;
        };
        menuEnv.setCvar = [&](const std::string& n, const std::string& v) {
            if (ui::isArchivedCvar(n) && uiCvars[n] != v) profileDirty = true;
            uiCvars[n] = v;
            if (world) world->cvars[n] = v;
        };
        menuEnv.scriptMenuResponse = [&](const std::string& menu, const std::string& response) {
            // "cmd mr <menu index> <response>" (0x4c8e20); the index is the menu's precache slot.
            if (!world) return;
            const auto& names = world->menuNames();
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == menu) world->menuResponse(int(i), response);
        };
        menuEnv.play = [&](const std::string& alias) {
            float duration = 0;
            mixer.play(alias, std::nullopt, &duration); // interface sounds are 2D
        };
        // Console text run by menu "exec" (src/ui/frontend_exec.h): set, vstr, map, difficulty*, ';' separated.
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
                if (c == "vstr") execText(menuEnv.getCvar(a1));
                else if (c == "set" || c == "seta" || c == "sets") menuEnv.setCvar(a1, a2);
                else if (c == "map") pendingLevel = std::make_pair(a1, false);
                // Exe commands 0x446860..0x4468c0 (difficultyEasy/Medium/Hard/Fu) call 0x432570 with 0/1/2/3 and then re-apply the skill.
                else if (c == "difficultyeasy") menuEnv.setCvar("g_gameskill", "0");
                else if (c == "difficultymedium") menuEnv.setCvar("g_gameskill", "1");
                else if (c == "difficultyhard") menuEnv.setCvar("g_gameskill", "2");
                else if (c == "difficultyfu") menuEnv.setCvar("g_gameskill", "3");
                else if (c == "disconnect" || c == "quit") { /* handled below */ if (c == "quit") std::exit(0); }
                // "closemenu name" (console command registered at 0x4c3a80, string 0x58135c) closes that menu; used by difficulty.menu's ui_selected_map.
                else if (c == "closemenu") menuEnv.close(a1);
                // TODO(spec): fast_restart (reloads the current map here), loadgame_continue (0x447190: needs the savegame format).
                else if (c == "fast_restart" && !currentMap.empty()) pendingLevel = std::make_pair(currentMap, false);
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
        // TODO(spec): the other uiScripts (loadControls, openMenuOnDvar, loadMissionDifficultyOffset, Loadgame, profiles ...).
        menuEnv.uiScript = [&](const std::string& name, const ui::Command&) {
            if (name == "quit") execText("quit");
        };
        menuEnv.restartAvailable = [&] { return bool(world); }; // TODO(spec): the restart point (0x4c4xxx); front end has none
        ui::initUiCvars(menuEnv); // ui_mousePitch from m_pitch (0x4c30db)
        ui::MenuStack frontStack(scriptMenus, menuEnv); // installs open/close/focus/show hooks into menuEnv
        { // the UI loads ui/menus.txt at start regardless of a level (the in-level ESC opens "main" as the pause menu)
            scriptMenus.loadList("ui/menus.txt");
            for (const auto& e : scriptMenus.errors()) std::cerr << "menu: " << e << "\n";
        }
        // Server commands popupopen <index> [1] / popupclose (0x4a7094): the menu file is "ui/scriptmenus/<name>.menu" (0x4c3ad0).
        // TODO(spec): the "1" (openmenunomouse) variant keeps the cursor hidden; here the cursor is always shown.
        auto menuCommand = [&](const std::string& name, const std::string& arg) {
            if (name == "popupclose") {
                if (const ui::Menu* t = frontStack.top()) frontStack.close(t->name);
                return;
            }
            const int idx = std::atoi(arg.c_str());
            if (!world || idx < 0 || size_t(idx) >= world->menuNames().size()) return;
            const std::string& menuName = world->menuNames()[size_t(idx)];
            if (!scriptMenus.find(menuName)) scriptMenus.loadFile("ui/scriptmenus/" + menuName + ".menu");
            menuMx = 320.0f, menuMy = 240.0f;
            if (!frontStack.open(menuName)) std::cerr << "warning: script menu " << menuName << " not found\n";
        };
        bool mantleHint = false;
        float hudAimSpread = 255.0f, hudViewHeight = 60.0f; // ps+0x5b0 and the view height for the crosshair
        // Damage direction icons (cgame 0x4ba2a0 → 8 slots at 0xf79704: start, duration, yaw).
        struct DamageIcon { int64_t start = -1000000; int duration = 0; float yaw = 0; };
        std::array<DamageIcon, 8> damageIcons{};
        int lastDamageEvent = 0;
        // Menu "weaponinfo" (hud.menu:184): the ammo counter backdrop `ammocounterback` (ownerdraw CG_PLAYER_AMMO_BACKDROP 6), rect
        // -100 -50 100 50 from the bottom right of the 4:3 area. TODO(spec): the ownerdraw 6 handler's fade/hidden conditions.
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
            if (!pweapon.equipped()) return 0;
            const std::string shader = pweapon.field("adsOverlayShader"), reticle = pweapon.field("adsOverlayReticle");
            if (shader.empty() && (reticle.empty() || reticle == "none")) return 0;
            const float f = pweapon.adsFrac();
            if (f == 0) return 0;
            const float z = pweapon.number(pweapon.adsZoomingIn() ? "adsZoomInFrac" : "adsZoomOutFrac");
            float a = f - (1.0f - z);
            if (a > 0) a /= z;
            if (!(a > 0.01f)) return 0;
            return std::min(a, 1.0f);
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
            for (const auto& line : lines) {
                float width = 0;
                for (unsigned char c : line)
                    if (const auto* g = f.find(c)) width += float(g->dx) * scale;
                float pen = x - alignX * width;
                for (unsigned char c : line) {
                    const auto* g = f.find(c);
                    if (!g) continue;
                    if (g->width && g->height) {
                        Quad2D q{pen + float(g->x0) * scale, ty + lineH + float(g->y0) * scale, 0, 0, g->s0, g->t0, g->s1, g->t1};
                        q.x1 = q.x0 + float(g->width) * scale;
                        q.y1 = q.y0 + float(g->height) * scale;
                        std::memcpy(q.rgba, rgba, 4);
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
        // Menu item text: localized string and font/scale (TODO(spec): the original's font choice by textscale, here the HUD's rule).
        auto menuLocalized = [&](const std::string& raw) {
            std::string key = raw;
            if (!key.empty() && key[0] == '@') key.erase(0, 1);
            const std::string* t = loc.find(key);
            return t ? *t : key;
        };
        auto menuFont = [&](float ts) { return font(ts <= 0.25f ? "smallFont" : ts >= 0.55f ? "extraBigFont" : ts < 0.4f ? "normalFont" : "bigFont"); };
        menuEnv.bindKey = [&](const std::string& command, const std::string& key) {
            input.rebind(command, key);
            profileDirty = true;
            if (world) { // getkeybinding follows the new bindings
                world->keyBindings.clear();
                for (const auto& [k, c] : input.bindings()) world->keyBindings[c].push_back(k);
            }
        };
        // The renderer's enum dvars (r_mode, r_displayRefresh: the D3D adapter's mode lists) have no equivalent here: empty domain.
        // TODO(spec): the macOS display modes as r_mode entries.
        menuEnv.enumValues = [](const std::string&) { return std::vector<std::string>{}; };
        menuEnv.textWidth = [&](const ui::Item& it) {
            const assets::Font* f = menuFont(it.textScale);
            if (!f) return 0.0f;
            const float gs = it.textScale * 48.0f / float(f->pixelHeight);
            float width = 0;
            for (unsigned char ch : menuLocalized(it.text))
                if (const auto* g = f->find(ch)) width += float(g->dx) * gs;
            return width;
        };
        auto buildHud = [&](uint32_t w, uint32_t h, const Camera& c) {
            std::vector<Quad2D> out, front; // front: foreground script elements, drawn last
            const float sc = float(h) / 480.0f;               // virtual pixel to screen pixel
            const float left43 = (float(w) - 640.0f * sc) * 0.5f; // left edge of the 4:3 area
            auto field = [](const script::ObjectPtr& o, const char* k, float def) {
                const script::Value v = o->get(k);
                return v.isNumber() ? v.asFloat() : def;
            };
            auto lerpOver = [&](const script::ObjectPtr& o, const char* startKey, const char* timeKey) {
                const script::Value st = o->get(startKey), tm = o->get(timeKey);
                if (!st.isNumber() || !tm.isNumber() || tm.asFloat() <= 0) return 1.0f;
                return std::clamp((float(world->timeMs()) - st.asFloat()) / tm.asFloat(), 0.0f, 1.0f);
            };
            if (world) {
                std::vector<script::ObjectPtr> elems = world->hudElems();
                std::stable_sort(elems.begin(), elems.end(), [&](const auto& a, const auto& b) { return field(a, "sort", 0) < field(b, "sort", 0); });
                for (const auto& o : elems) {
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
                    const std::string ha = o->get("horzalign").toString(), va = o->get("vertalign").toString();
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
                        const int secs = std::max(0, int(std::ceil((o->get("$timerend").asFloat() - float(world->timeMs())) / 1000.0f)));
                        text = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
                    } else if (o->get("$value").isDefined()) {
                        const script::Value v = o->get("$value");
                        text = v.isNumber() && v.asFloat() == float(int(v.asFloat())) ? std::to_string(int(v.asFloat())) : v.toString();
                    }
                    const std::string label = str(o->get("label"));
                    if (!label.empty()) {
                        const auto at = label.find("&&1");
                        text = at == std::string::npos ? label + text : label.substr(0, at) + text + label.substr(at + 3);
                    }
                    // Key-name placeholders "[{command}]" (0x4a36b0, found by "[{" / "}]"): replaced by the key(s) bound
                    // to the command (0x4cdb30: up to two, joined by " <KEY_OR> "), shown by their key.str name (0x498a20);
                    // an unbound command shows KEY_UNBOUND. The surrounding "[" "]" of "[[{+x}]]" stay.
                    // TODO(spec): the "KEY_" prefix handling of 0x4c6a20 (key.str has UNBOUND / OR / TAB without it).
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
                    const std::string fontName = o->get("font").toString();
                    const assets::Font* f = font(fontName == "bigfixed" || fontName == "big" ? "bigfont" : "normalfont");
                    if (f) hudText(dst, text, px, py, field(o, "fontscale", 1) * sc * 0.75f, *f, rgba, fx, fy);
                }
            }
            {
                // Subtitles (cgame 0x4bf2e0): x 120, y 400, char height 12 on the 640x480 screen, wrapped at 80 characters.
                // Anchor: 0x406c60 calls 0x4068e0 with horizontal and vertical code 1, i.e. x·scale + 0x793974 = the left edge of the 4:3 area (left43) in widescreen, as here. TODO(spec): colour, cg_subtitles off.
                const uint64_t now = SDL_GetTicksNS();
                std::erase_if(subtitles, [&](const Subtitle& st) { return st.untilNs < now; });
                const uint8_t white[4] = {255, 255, 255, 255};
                // The console text drawer 0x4062d0 picks the font from the scale = charHeight / 48 (0x591f90) * the screen scale
                // (0x793944 = h / 480): <= ui_smallFont (0.25, 0x58156c) $smallfont, >= ui_extraBigFont (0.55) $extrabigfont,
                // < ui_bigFont (0.4) $font (normalFont), else $bigfont (0x4063a0..0x4063e2; the same rule as menu items), so
                // the glyphs stay close to the font's native size instead of one font being stretched.
                // TODO(spec): the second selection path at 0x40631a (no extra-big font) for a non-zero flag argument.
                const float subScale = 12.0f / 48.0f * float(h) / 480.0f;
                if (const assets::Font* f = font(subScale <= 0.25f ? "smallFont" : subScale >= 0.55f ? "extraBigFont" : subScale < 0.4f ? "normalFont" : "bigFont")) {
                    float sy = 400.0f;
                    for (const auto& st : subtitles) {
                        const std::string* t = loc.find(st.key);
                        std::string text = t ? *t : st.key; // an untranslated key is shown as it is
                        std::vector<std::string> lines;
                        std::string cur;
                        std::istringstream words(text);
                        for (std::string wd; words >> wd;) {
                            if (!cur.empty() && cur.size() + 1 + wd.size() > 80) { lines.push_back(cur); cur.clear(); }
                            cur += (cur.empty() ? "" : " ") + wd;
                        }
                        if (!cur.empty()) lines.push_back(cur);
                        for (const auto& ln : lines) {
                            hudText(out, ln, left43 + 120.0f * sc, sy * sc, 12.0f / float(f->pixelHeight) * sc, *f, white, 0, 0, 3);
                            sy += 12.0f;
                        }
                    }
                }
            }
            {
                const uint64_t now = SDL_GetTicksNS();
                std::erase_if(screenMessages, [&](const ScreenMessage& m) { return m.untilNs < now; });
                // Game messages (client commands obj_update / obj_complete, 0x4a6c7f): the string "^2%s" (0x57f6a0, ^2 = green
                // (0,1,0,1) in the colour table at 0x56cd98) goes to the game-message window (type 1; 0x404830 → 0x403aa0
                // → window base of 0x5cc2b0), shown for con_gamemessagetime 5 s (0x571118; 0x403890 case 1), wrapped at
                // cg_gameMessageWidth 70 characters (0x4a2026). Drawn each frame by 0x4bf1c0 → 0x406b60 → 0x4068e0 → 0x4065e0
                // with x 6, y 100, character height 12, horizontal code 1 (LEFT edge of the window, 0x440ff4[1]) and vertical
                // code 1 (TOP of the window, 0x441014[1]), style 3: the rows of the window go bottom-up, the newest row's top
                // at y − 12 and each older row 12 higher (0x4066e3 loop). Same font rule as subtitles (0x4062d0, flag 0).
                // TODO(spec): the exact message text for
                // obj_update when the objective has no string, the flag (4th) argument of 0x404830.
                // Rows (oldest first) of the window of one message type; "\x1f" separates the "&&N" arguments of iprintln.
                struct WindowRow { std::string text; uint64_t startNs, endNs; };
                auto windowRows = [&](bool bold, size_t width, size_t lines) {
                    std::vector<WindowRow> rows;
                    for (const auto& m : screenMessages) {
                        if (m.bold != bold) continue;
                        std::string raw = m.text.substr(0, m.text.find('\x1f'));
                        const std::string* t = loc.find(raw);
                        std::string text = t ? *t : raw;
                        for (size_t at = m.text.find('\x1f'), n = 1; at != std::string::npos; ++n) {
                            const size_t next = m.text.find('\x1f', at + 1);
                            const std::string arg = m.text.substr(at + 1, next == std::string::npos ? next : next - at - 1);
                            const std::string mark = "&&" + std::to_string(n);
                            if (const size_t k = text.find(mark); k != std::string::npos) text.replace(k, mark.size(), arg);
                            at = next;
                        }
                        std::string cur;
                        std::istringstream words(text);
                        for (std::string wd; words >> wd;) {
                            if (!cur.empty() && cur.size() + 1 + wd.size() > width) { rows.push_back({cur, m.startNs, m.endNs}); cur.clear(); }
                            cur += (cur.empty() ? "" : " ") + wd;
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
                                      const uint8_t base[4], float alignX) {
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
                        hudText(out, it->text, x, ry * sc, 12.0f / float(f.pixelHeight) * sc, f, col, alignX, 0, 3);
                    }
                };
                const uint8_t green[4] = {0, 255, 0, 255};
                const float msgScale = 12.0f / 48.0f * float(h) / 480.0f;
                if (const assets::Font* f = font(msgScale <= 0.25f ? "smallFont" : msgScale >= 0.55f ? "extraBigFont" : msgScale < 0.4f ? "normalFont" : "bigFont")) {
                    drawWindow(windowRows(false, 70, 9), left43 + 6.0f * sc, 100.0f, 12.0f, *f, green, 0);
                }
                // Bold messages (client command gmb = iprintlnbold, 0x4a6c3f → 0x4a2750 → window type 2 at 0x5cc2b0+0x6a0,
                // 10 lines (0x5ccff4+0x10), cg_gameBoldMessageWidth 60, con_boldgamemessagetime 8 s): drawn by 0x4bf210 →
                // 0x4068e0 mode 3 at x 0, y 180, character height 12, horizontal code 7 (the centre of the window box),
                // vertical code 1, white; the bold flag adds 4 to the row pitch (0x4065e0) and selects the flag path of
                // the font choice (0x4062d0: scale <= 0.25 small, < ui_bigFont 0.4 normal, else bold font).
                // Re-checked: 0x4bf210 passes only the horizontal code 7 and x 0; 0x440e60 code 7 gives (0x79397c + 0x793974) / 2
                // + x·scale and 0x4062d0 draws the text from that point (0x617bf0), so each row STARTS at the centre of the box
                // (no centring in 0x4062d0 / 0x4065e0 / 0x403b90).
                {
                    const float scale = 12.0f / 48.0f * float(h) / 480.0f;
                    if (const assets::Font* f = font(scale <= 0.25f ? "smallFont" : scale < 0.4f ? "normalFont" : "boldFont")) {
                        const uint8_t white[4] = {255, 255, 255, 255};
                        drawWindow(windowRows(true, 60, 10), left43 + 320.0f * sc, 180.0f, 16.0f, *f, white, 0);
                    }
                }
            }
            // Scope overlay (0x4bcdd0 → 0x4bca90): the weapon's adsOverlayShader, white with the overlay alpha. Wider than
            // 320 or taller than 240 (adsOverlayWidth / Height, virtual units): one quad of that size centred on the
            // view; else four quads of that size around the centre, mirrored (u and/or v flipped) towards the right and
            // bottom. Outside it (0x4bc980) the rest of the screen is filled with the same material: left and right
            // strips with u = 0 (t 0..1), top and bottom with v = 0 (s 0..1). TODO(spec): the centre follows the gun
            // sway (0x4bc6e0 projects the weapon angles); adsOverlayReticle drawing.
            if (const float sa = world ? scopeAlpha() : 0.0f; sa > 0) {
                const std::string shader = pweapon.field("adsOverlayShader");
                const int tex = shader.empty() ? -1 : materialTexture(shader);
                if (tex >= 0) {
                    const float ow = pweapon.number("adsOverlayWidth"), oh = pweapon.number("adsOverlayHeight");
                    const float cx = left43 + 320.0f * sc, cy = 240.0f * sc;
                    const uint8_t col[4] = {255, 255, 255, uint8_t(sa * 255.0f)};
                    auto quad = [&](float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
                        Quad2D q{x0, y0, x1, y1, s0, t0, s1, t1};
                        std::memcpy(q.rgba, col, 4);
                        q.texture = tex;
                        out.push_back(q);
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
            // 3 (no pulse; registered 0x4a1123 with 3, 0, 4). TODO(spec): the fade (cg_hintFadeTime, 0x4b0900), the
            // weapon pickup text (0x4a99b0), HINT_HEALTH's key text.
            if (world && cursorHintItem && !world->playerOnTurret()) {
                const auto& uh = world->playerUseHint();
                std::string icon;
                bool wide = false;
                std::string text = uh.text;
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
                    const uint8_t white[4] = {255, 255, 255, 255};
                    float ix = rx - iw * 0.5f, iy = ry;
                    if (!text.empty()) {
                        const std::string* t = loc.find(text);
                        text = t ? *t : text;
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
            // (cg_hudDamageIconInScope 0). TODO(spec): the corner-to-UV order of the gfx quad draw (0x617b68) is
            // taken as (0,0) (1,0) (1,1) (0,1).
            if (world && !(scopeAlpha() > 0)) {
                const int64_t nowMs = world->timeMs();
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
            // TODO(spec): the dvars it also tests.
            if (world && mantleHint && mantleHintItem) {
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
            // Dead quote (ownerdraw CG_DEADQUOTE, 0x4aa2a0): after opendeadscreen the ui_deadquote dvar (a leading '@'
            // skipped, localized) fades in over hud_deathQuoteFadeTime ms (default 1000) of client time. 0x4cc6a0 wraps
            // it at spaces to the item width, centres each line on rect x + textalignx, line step text height + 5;
            // the glyph scale is textscale × 48 / font pixel height (gfx refexport +0x128, 0x1000bfa0). The item's
            // textstyle reaches the text draw (menu 0x4cec99 → 0x4c4070 → 0x4aa690 → 0x4aa2a0 → 0x4cc6a0 → 0x4c3790),
            // so ITEM_TEXTSTYLE_SHADOWED gets the renderer's shadow (see hudText). TODO(spec): which ownerdraw
            // arguments carry textalignx/textaligny (from the menu's "point to center around" comments).
            if (world && deadQuoteItem && deadScreenMs && deadScreenMs < world->timeMs()) {
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
                    const int64_t since = world->timeMs() - deadScreenMs;
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
            // TODO(spec): the friend / enemy colours (ps flags 0x100000 / 0x200000), the reticleCenter +
            // four reticleSide drawing with the spread (0x4bd2c0, 0x4bd520) and the sway offset (0x4bc6e0).
            const bool onTurretHud = world && world->playerOnTurret();
            const bool inVehicleHud = world && (world->playerPmFlags() & 0x20000) && !(world->playerPmFlags() & 0x80000);
            if (onTurretHud || inVehicleHud) {
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
            const int wst = pweapon.stateNumber();
            const float crossAlpha = 1.0f - (world ? scopeAlpha() : 0.0f);
            const bool drawCross = !onTurretHud && !inVehicleHud && crossAlpha >= 0.01f && pweapon.adsFrac() != 1.0f &&
                                   wst != 1 && wst != 2 && wst != 5 && wst != 10 && wst != 11;
            // Look (0x4bd760 → 0x4bd2c0 centre, 0x4bd520 sides). Aiming (0x4bd230, fraction past 1 − adsCrosshairInFrac
            // while zooming in, adsCrosshairOutFrac out): scale 1 − f/2 and the centre moved down by 480 / fov_y ·
            // adsAimPitch · f. cg_crosshairDynamic (0) keeps it otherwise at the screen centre. Alpha (0x4bd450):
            // (1 − aim spread / 255) · cg_crosshairAlpha (1) · (1 − scope alpha), at least cg_crosshairAlphaMin (0).
            // Centre: reticleCenter, reticleCenterSize · scale (+ fuse % 1000 · 0.01 while cooking a cookOffHold
            // grenade). Sides: reticleSide, reticleSideSize · scale square, at d = max(reticleMinOfs,
            // tan(spread · scale) / tan(fov_y / 2) · 240) − hipReticleSidePos · size from the centre, spread = hip min
            // + (max − min) · aim spread / 255 (0x4bd3a0, 0x4dceb0); top 0°, right 90°, bottom 180°, left 270°, with
            // the 0.125 · size placement terms of 0x4bd4a0. TODO(spec): the friend / enemy colours.
            if (pweapon.equipped() && drawCross) {
                auto num = [&](const char* k) { return pweapon.number(k); };
                float scale = 1.0f, offY = 0.0f;
                const float fovY = 2.0f * std::atan(std::tan(c.fovX43 * 0.5f * float(M_PI) / 180.0f) * 0.75f) * 180.0f / float(M_PI);
                if (const float af = pweapon.adsFrac(); af != 0) {
                    const float cf = num(pweapon.adsZoomingIn() ? "adsCrosshairInFrac" : "adsCrosshairOutFrac");
                    float t = af - (1.0f - cf);
                    if (t > 0 && cf != 0) {
                        t /= cf;
                        scale = 1.0f - 0.5f * t;
                        offY = 480.0f / fovY * num("adsAimPitch") * t;
                    }
                }
                const float alpha = std::max((1.0f - hudAimSpread / 255.0f) * 1.0f * crossAlpha, 0.0f);
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
                const std::string center = pweapon.field("reticleCenter"), side = pweapon.field("reticleSide");
                if (!center.empty()) {
                    float cs = num("reticleCenterSize");
                    if (num("cookOffHold") != 0 && pweapon.fuseMs() != 0 && pweapon.field("weaponType") == "grenade")
                        cs += float(pweapon.fuseMs() % 1000) * 0.01f;
                    cs *= scale;
                    pic(cx - cs * 0.5f, cy - cs * 0.5f, cs, center, 0);
                }
                if (!side.empty()) {
                    const float size = num("reticleSideSize") * scale;
                    if (const auto* d = world ? world->weapon(pweapon.name()) : nullptr) {
                        auto fld = [&](const char* k) { auto it = d->find(k); return it == d->end() ? 0.0f : float(std::atof(it->second.c_str())); };
                        game::HipSpread hs;
                        hs.standMin = fld("hipspreadstandmin"); hs.duckedMin = fld("hipspreadduckedmin"); hs.proneMin = fld("hipspreadpronemin");
                        hs.max = fld("hipspreadmax"); hs.duckedMax = fld("hipspreadduckedmax"); hs.proneMax = fld("hipspreadpronemax");
                        const auto range = game::hipSpreadRange(hs, hudViewHeight);
                        const float spread = range[0] + (range[1] - range[0]) * hudAimSpread / 255.0f;
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
            // Crosshair target names (cgame 0x4bf8b0, drawn when cl_paused is 0): the server's configstrings 0xb / 0xc (0x534aa0:
            // the target actor's name and weapon display name) as localized text at CENTER/CENTER alignment (0x440e60 mode 2):
            // line 1 at (+25, -2) in green (0 1 0 1), line 2 at (+25, +18) in white alpha 0.7, textscale 1/3, style 3
            // (shadowed), baseline at y, left aligned. The font follows the scale as for menu items. TODO(spec): the font
            // selection thresholds (dvars 0x1107418 / 0x10fec20 / 0x10fec18, here the menu rule), \x15 localization fallback.
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
            // baseline at rect.y (4c3790 gets the item rect y unchanged). TODO(spec): low-ammo red (0.89 0.18 0.01,
            // 0x4a76b0: stock < clip size * 0x592020), the hud fade alpha (0x4a7600), the firemode icon (ownerdraw 83).
            // Fade (0x4a7600 / 0x4b0900): dvar seconds 0 -> 1; else touch == 0 or elapsed >= ticks -> 0, else min(1, (ticks - elapsed) / 700).
            // Ammo backdrop / text use hud_fade_ammodisplay (default 2 s, 0x10438d8) from the ammo touch time; the firemode icon the same dvar
            // but a fixed 0x708 = 1800 ms window from the weapon change time (0xf796e4); the compass hud_fade_compass (default 8 s).
            auto fadeAlpha = [&](int64_t touch, float dvarSeconds, int ticksMs) {
                if (dvarSeconds == 0.0f) return 1.0f;
                const int64_t now = world ? world->timeMs() : 0, el = now - touch;
                if (touch == 0 || el >= ticksMs) return 0.0f;
                const int64_t rem = ticksMs - el;
                return rem >= 700 ? 1.0f : float(rem) / 700.0f;
            };
            const float ammoAlpha = fadeAlpha(hudFade.ammo, 2.0f, 2000);
            const bool inVeh = world && (world->playerPmFlags() & 0x20000); // the ammo ownerdraws draw nothing in a vehicle (0x4a7818)
            // Low stock (0x4a76b0): stock <= 0.2 * maxAmmo (def +0x1d4, clamped to 999; 0 when negative); low clip (0x4a7760): clip <= 0.33 * clipSize
            // (def +0x1d8; false when clipSize <= 0). Both tint red (0.89 0.18 0.01).
            // TODO(spec): that def +0x1d4 / +0x1d8 are maxAmmo / clipSize (matches the usage: stock vs max, clip vs clip size).
            const float maxAmmo = std::min(pweapon.number("maxAmmo"), 999.0f), clipSize = std::min(pweapon.number("clipSize"), 999.0f);
            const bool stockLow = maxAmmo >= 0 && float(std::min(pweapon.stock(), 999)) <= maxAmmo * 0.2f;
            const bool clipLow = clipSize > 0 && float(std::min(pweapon.clip(), 999)) <= clipSize * 0.33f;
            if (pweapon.equipped() && !inVeh && ammoAlpha > 0.0f) {
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
                        const std::string clip = fmt("%2i", pweapon.clip()), stock = fmt("%3i", pweapon.stock());
                        const int st = ammoTextItem->textStyle;
                        // Colours (0x4a7930): alpha = the fade; the clip keeps the item colour, "|" and the stock turn red when the
                        // stock is low; a low clip is overdrawn in red with a sawtooth alpha (800 ms, 1 -> 0, restarting),
                        // limited by the fade alpha (0x4a7aab..0x4a7b09, state 0xf79690).
                        col[3] = uint8_t(std::clamp(ammoAlpha, 0.0f, 1.0f) * 255.0f);
                        uint8_t col2[4] = {col[0], col[1], col[2], col[3]};
                        if (stockLow) col2[0] = uint8_t(0.89f * 255.0f), col2[1] = uint8_t(0.18f * 255.0f), col2[2] = uint8_t(0.01f * 255.0f);
                        hudText(out, clip, x0, y0, gs, *f, col, 0.0f, 1.0f, st);
                        if (clipLow) {
                            const int64_t now = world ? world->timeMs() : 0;
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
            // Weapon name (ownerdraw 81, 0x4a7da0): not in a vehicle (pm_flags 0x20000); same fade as the firemode icon (hud_fade_ammodisplay,
            // 1800 ms from the weapon change time 0xf796e4, 0x4a7600). Text = localized displayName (client weapon info +0xb4), plus
            // " / " + localized modeName (+0xb8, def +0x70) when the weapon has a modeName ("%s / %s" 0x57fb54, else "%s" 0x56ca4c).
            // Drawn at x = rect.x + rect.w - textWidth - 28 (0x592084), y = rect.y, item colour with the fade alpha, item text style,
            // font by the HUD rule. TODO(spec): the weapon preselected while switching (0xf796e0) is not tracked; the current weapon is used.
            if (weapNameItem && pweapon.equipped() && !inVeh) {
                const float a = fadeAlpha(hudFade.weaponChange, 2.0f, 1800);
                const float ts = weapNameItem->textScale, fs = ts * float(h) / 480.0f;
                if (a > 0.0f)
                    if (const assets::Font* f = font(fs <= 0.25f ? "smallFont" : fs >= 0.55f ? "extraBigFont" : fs < 0.4f ? "normalFont" : "bigFont")) {
                        auto name = [&](const std::string& k) { const std::string* t = loc.find(k); return t ? *t : k; };
                        std::string text = name(pweapon.field("displayName"));
                        const std::string mode = pweapon.field("modeName");
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
            // Firemode icon (ownerdraw 83, 0x4a8040): not in a vehicle; alpha = fade over a fixed 1800 ms (0x708) from the weapon
            // change time (0xf796e4); the material is the weapon's modeIcon (client weapon info +0x140; only weapons with a non-empty
            // modeIcon key draw one), item colour, rect from the item (RIGHT / BOTTOM aligned like the other weaponinfo items).
            if (fireModeItem && pweapon.equipped() && !inVeh) {
                const std::string icon = pweapon.field("modeIcon");
                const float a = fadeAlpha(hudFade.weaponChange, 2.0f, 1800);
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
            // Compass (ownerdraw 85/84/86, exe 4a91f0 / 4a90b0 / 4a9310). LEFT/BOTTOM alignment is relative to the window
            // edges (0x440e60), like the ammo counter. TODO(spec): the fade alpha (hud_fade_compass 8 s after a touch event,
            // 4a6130 case 2; alpha 1 for now), the default pointer material when an objective has no icon, ownerdraw 87/88.
            // The compass fade (0x4a90f3..: hud_fade_compass default 8 s, touch 0xf79a38) is written into the item colour alpha.
            // Objective ring pulse touch (0x4a9775..0x4a97b8): while a pointer's ring is active (cg_hudObjectiveNumRings 10 > 0,
            // cg_hudObjectiveRingTime 10000 > 0, start >= 0, start + time > now) the compass is touched every frame.
            constexpr int ringNum = 10, ringTime = 10000;
            auto ringElapsed = [&](const game::Objective& ob) -> int64_t {
                if (!world || ob.state != 4 || ob.ringStartMs < 0 || ob.ringStartMs + ringTime <= world->timeMs()) return -1;
                return world->timeMs() - ob.ringStartMs;
            };
            if (world)
                for (const auto& ob : world->objectives())
                    if (ringElapsed(ob) >= 0 && ob.positions[0] != std::array<float, 3>{0, 0, 0}) hudFade.compass = world->timeMs();
            const float compassAlpha = fadeAlpha(hudFade.compass, 8.0f, 8000);
            if (compassMenu && world && compassAlpha > 0.0f) {
                const float csize = 1.0f; // cg_hudCompassSize
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
                        // Friendly actors (4c0f20): R = size * r * 43.75, icon size*10 (0x591e8c), r from the distance between
                        // cg_hudCompassMinRange 0 and MaxRange 1500 (0xf48b54) clamped to the rim (no culling), minRadius 0;
                        // placed at the compass centre - icon/2 - (sin, cos)(th) * R with th = yaw to the actor - view yaw;
                        // the arrow turns by (view yaw - actor yaw). TODO(spec): the 800 ms entry age, springy yaw
                        // (cg_hudCompassSpringyPointers 0), the sound ping rings (cg_hudCompassSoundPingFadeTime).
                        if (it.background.empty()) continue;
                                                // 88 tanks (4c1700): the same formulas over the vehicles with addvehicletocompass, icon 16 (0x591e84).
                        const bool tanks = it.ownerDraw == 88;
                        const float ccx = bx + qw * 0.5f, ccy = by + qh * 0.5f, icon = (tanks ? 16.0f : 10.0f) * csize * sc;
                        const int tex = materialTexture(it.background);
                        for (const auto& m : tanks ? world->compassVehicles() : world->compassFriendlies()) {
                            const float dx = m.pos[0] - c.pos.x, dy = m.pos[1] - c.pos.y;
                            const float th = wrap360(std::atan2(dy, dx) * 180.0f / float(M_PI) - cyaw) * 0.0174532925f;
                            const float t = std::clamp(std::sqrt(dx * dx + dy * dy) / 1500.0f, 0.0f, 1.0f);
                            const float rad = csize * t * 43.75f * sc;
                            const float ix = ccx - icon * 0.5f - std::sin(th) * rad, iy = ccy - icon * 0.5f - std::cos(th) * rad;
                            Quad2D q{ix, iy, ix + icon, iy + icon};
                            std::memcpy(q.rgba, col, 4);
                            q.texture = tex;
                            q.rotDeg = wrap360(cyaw - m.yaw);
                            out.push_back(q);
                        }
                        continue;
                    }
                    if (it.ownerDraw != 86) {
                        if (it.background.empty()) continue;
                        Quad2D q{bx, by, bx + qw, by + qh};
                        std::memcpy(q.rgba, col, 4);
                        q.texture = materialTexture(it.background);
                        if (it.ownerDraw == 84) q.rotDeg = compassSpring.update(int(world->timeMs()), wrap360(cyaw - north)); // 0x4b09a0
                        out.push_back(q);
                        continue;
                    }
                    // Pointers: centre (x + w*size/2, y + h*size/2 - (size-1)*h); K = 43.75 (double @0x591e90); icon 16*size.
                    const float ccx = bx + qw * 0.5f, ccy = by + qh * 0.5f;
                    const float maxR = 1500.0f, minR = 0.0f, minRad = 0.0f, objMax = 2048.0f, minAlpha = 1.0f;
                    const float icon = 16.0f * csize * sc;
                    for (const auto& ob : world->objectives()) {
                        // An objective without an icon uses the default material "objective" (0xf5b6cc, registered at 0x4a2d62
                        // from the string 0x57e3f4; used at 0x4a96da when the icon lookup 0x4a8fc0 fails): the star on the compass.
                        if (ob.state != 4) continue;
                        for (const auto& p : ob.positions) {
                            if (p[0] == 0 && p[1] == 0 && p[2] == 0) continue;
                            const float dx = p[0] - c.pos.x, dy = p[1] - c.pos.y, d = std::sqrt(dx * dx + dy * dy);
                            const float a = wrap360(std::atan2(dy, dx) * 180.0f / float(M_PI) - cyaw) * 0.0174532925f;
                            float alpha = 1.0f;
                            if (objMax > maxR) alpha = (minAlpha - 1.0f) * (std::clamp(d, maxR, objMax) - maxR) / (objMax - maxR) + 1.0f;
                            const float t = (std::clamp(d, minR, maxR) - minR) / (maxR - minR);
                            const float rad = csize * ((1.0f - minRad) * t + minRad) * 43.75f * sc;
                            const float ix = ccx - icon * 0.5f - std::sin(a) * rad, iy = ccy - icon * 0.5f - std::cos(a) * rad;
                            Quad2D q{ix, iy, ix + icon, iy + icon};
                            std::memcpy(q.rgba, col, 4);
                            q.rgba[3] = uint8_t(std::clamp(alpha * compassAlpha, 0.0f, 1.0f) * 255.0f); // TODO(spec): fade x pointer alpha
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
                                rq.rgba[3] = uint8_t(std::clamp(std::min(tri, alpha * compassAlpha), 0.0f, 1.0f) * 255.0f);
                                rq.texture = materialTexture(ob.icon.empty() ? std::string("objective_ring") : ob.icon);
                                out.push_back(rq);
                            }
                        }
                    }
                }
            }
            if (!frontStack.empty()) {
                // Menus on the stack (front end and script menus), items laid out relative to the menu rect in the 640x480 virtual
                // screen. Window style 1 (FILLED) draws backColor, 3 (SHADER) the background material, 6 (DVAR_SHADER) the material
                // named by the item's dvar; text items draw their (localized) text, the focused item in the menu's focusColor.
                // TODO(spec): the original's font choice by textscale (here the HUD's rule), item/menu alignment, draw order.
                auto col8 = [](const ui::Color& c) { return std::array<uint8_t, 4>{uint8_t(std::clamp(c[0], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[1], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[2], 0.0f, 1.0f) * 255), uint8_t(std::clamp(c[3], 0.0f, 1.0f) * 255)}; };
                std::vector<const ui::Menu*> order;
                if (const ui::Menu* bg = scriptMenus.find("bg")) order.push_back(bg); // TODO(spec): how the background menu is shown
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
                    if (menu.style == 1) quad(menu.rect.x, menu.rect.y, menu.rect.w, menu.rect.h, col8(menu.backColor), "");
                    else if (menu.style == 3 && !menu.background.empty()) quad(menu.rect.x, menu.rect.y, menu.rect.w, menu.rect.h, col8(menu.foreColor), menu.background);
                    for (const auto& itp : menu.items) {
                        const ui::Item& it = *itp;
                        if (!frontStack.itemVisible(it)) continue;
                        const float ix = menu.rect.x + it.rect.x, iy = menu.rect.y + it.rect.y;
                        if (it.style == 1) quad(ix, iy, it.rect.w, it.rect.h, col8(it.backColor), "");
                        else if (it.style == 3 || it.style == 6) {
                            std::string mat = it.background;
                            if (it.style == 6 || (mat.empty() && !it.dvar.empty())) mat = menuEnv.getCvar(it.dvar);
                            if (!mat.empty()) quad(ix, iy, it.rect.w, it.rect.h, col8(it.foreColor), mat);
                        }
                        if (it.text.empty() && it.type != 11 && it.type != 12 && it.type != 13 && it.type != 14 && it.type != 17) continue;
                        const std::string text = menuLocalized(it.text);
                        const float ts = it.textScale;
                        const assets::Font* f = menuFont(ts);
                        if (!f) continue;
                        const float gs = ts * 48.0f / float(f->pixelHeight);
                        auto c = col8(&it == frontStack.focus() ? menu.focusColor : it.foreColor);
                        if (it.type == 14 && &it == frontStack.focus()) {
                            // BIND (exe 0x4cdee0): capturing = (0.8, 0, 0, 0.8); focused = focus colour pulsing with
                            // (sin(ms / 75) + 1) * 0.5 against focus * 0.8 (0x4c6e50). TODO(spec): argument order of the blend.
                            if (&it == frontStack.captureItem()) c = col8(std::array<float, 4>{0.8f, 0.0f, 0.0f, 0.8f});
                            else {
                                const float t = (std::sin(float(SDL_GetTicks() / 75)) + 1.0f) * 0.5f;
                                for (int i = 0; i < 3; ++i) c[size_t(i)] = uint8_t(float(c[size_t(i)]) * (1.0f - 0.2f * t));
                            }
                        }
                        const float ax = it.textAlign == 1 || it.textAlign == 3 ? 0.5f : it.textAlign == 2 ? 1.0f : 0.0f;
                        hudText(out, text, x0 + (ix + it.textAlignX) * xs, (iy + it.textAlignY) * sc, gs * sc, *f, c.data(), ax, 1.0f, it.textStyle);
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
                    (void)0;
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
            out.insert(out.end(), front.begin(), front.end());
            (void)c;
            return out;
        };
        Camera cam = scene.start;
        if (pos) cam.pos = *pos;
        if (yaw) cam.yaw = *yaw;
        if (pitch) cam.pitch = *pitch;

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
                frontStack.open(rest.substr(0, comma));
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
                    if (k.rfind("open:", 0) == 0) frontStack.open(k.substr(5));
                    else if (k.rfind("click:", 0) == 0) {
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                        frontStack.click(menuMx, menuMy);
                        frontStack.release();
                    } else if (k.rfind("hover:", 0) == 0) {
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                    } else if (k.rfind("bindkey:", 0) == 0) {
                        frontStack.captureKey(k.substr(8));
                    } else frontStack.key(k == "up" ? K::Up : k == "down" ? K::Down : k == "esc" ? K::Escape : k == "left" ? K::Left : k == "right" ? K::Right : K::Enter);
                }
            }
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
                for (int f = 0; f < int(simSeconds * 20); ++f) {
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
                    world->frame();
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
                    for (const auto& cmd : world->takeClientCommands()) // --shot shows the death screen and script menus too
                        if (cmd.name == "opendeadscreen") deadScreenMs = world->timeMs();
                        else if (cmd.name == "popupopen" || cmd.name == "popupclose") menuCommand(cmd.name, cmd.arg);
                        else if (cmd.name == "fog") {
                            std::istringstream a(cmd.arg);
                            float nearD = 0, farD = 0, one = 1, r = 0, g = 0, b = 0, ms = 0;
                            a >> nearD >> farD >> one >> r >> g >> b >> ms;
                            if (one >= 1.0f) fogState.command(nearD, farD, r, g, b, int(ms), world->timeMs());
                            else fogState.commandExp(one, r, g, b, int(ms), world->timeMs());
                        }
                        else if (cmd.name == "obj_update" || cmd.name == "obj_complete" || cmd.name == "gm") shotMsgs.push_back({cmd.arg, world->timeMs(), false});
                        else if (cmd.name == "gmb") shotMsgs.push_back({cmd.arg, world->timeMs(), true});
                }
                // Messages still on screen (con_gamemessagetime 5 s, con_boldgamemessagetime 8 s) at the end of the simulation.
                for (const auto& m : shotMsgs)
                    if (world->timeMs() - m.at < (m.bold ? 8000 : 5000)) { const uint64_t nowNs = SDL_GetTicksNS(), st = nowNs - uint64_t(world->timeMs() - m.at) * 1000000ull;
                        screenMessages.push_back({m.text, nowNs + 600000000000ull, m.bold, st, st + (m.bold ? 8000000000ull : 5000000000ull)}); }
                // Debug: IW2_BOLD_MESSAGE="<text or localized key>" shows an iprintlnbold message in the shot.
                if (const char* bm = std::getenv("IW2_BOLD_MESSAGE")) screenMessages.push_back({bm, SDL_GetTicksNS() + 600000000000ull, true, SDL_GetTicksNS() - 1000000000ull, SDL_GetTicksNS() + 600000000000ull});
                // A still shot has no input history: show the faded HUD blocks as just touched (IW2_HUD_FADED=1 keeps them untouched).
                if (!std::getenv("IW2_HUD_FADED")) hudFade.compass = hudFade.ammo = hudFade.weaponChange = world->timeMs();
                if (world->playerLinked()) { // a script link carries the player: shoot from there (--yaw / --pitch kept)
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    cam.pos = {po[0], po[1], po[2] + 60.0f};
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
                                  << " yaw " << an[1] << " turret p/y " << e->vehicle->turretPitch << "/" << e->vehicle->turretYaw
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
            renderer.init(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
            syncWeapon();
            pweapon.update(1.0f, {});
            game::PlayerWeapon::Input shotIn;
            shotIn.fire = fireSeconds > 0;
            shotIn.ads = shotAds;
            if (pweapon.update(fireSeconds, shotIn) > 0) viewFlash->pending = viewFlash->brassPending = true;
            shotIn.fire = false;
            shotIn.frag = std::getenv("IW2_SHOT_FRAG") != nullptr; // debug: hold +frag for the shot (offhand view model)
            for (int i = 0; i < 100; ++i) {
                pweapon.update(0.02f, shotIn);
                if (aimBlendWeapon != pweapon.name()) { aimBlendWeapon = pweapon.name(); aimBlend.reset(); }
                aimBlend.update(pweapon.viewAiming(), 0.02f);
                vmMotion.update([&](const char* k) { return pweapon.number(k); }, 20, 0.0f, 190.0f, shotViewHeight, pweapon.adsFrac(), shotLean);
            }
            if (shotIn.frag) std::cout << "shot weapon state " << pweapon.stateNumber() << " anim " << pweapon.anim() << " t " << pweapon.animTime() << "\n";
            cam.pos.z += shotViewHeight - 60.0f;
            {
                const game::Vec lo = game::leanOffset(cam.yaw, shotLean);
                cam.pos = cam.pos + Vec3{lo[0], lo[1], lo[2]};
            }
            cam.fovX43 = game::adsFov(80.0f, pweapon.number("adsZoomFov") > 0 ? pweapon.number("adsZoomFov") : 80.0f,
                                      pweapon.number("adsZoomInFrac"), pweapon.number("adsZoomOutFrac"), pweapon.adsFrac(), pweapon.adsZoomingIn());
            fxRight = cam.right();
            fxUp = cross(cam.right(), cam.forward());
            // The per-element spawnRange / cullrange gate (0x485300): view origin and tan(fovX / 2) / tan(40 deg) (0x486e00).
            fxSystem.viewOrigin = {cam.pos.x, cam.pos.y, cam.pos.z};
            fxSystem.viewScale = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) / std::tan(0.6981317f);
            fxSystem.viewValid = true;
            setFxFrustum(fxSystem, cam, float(width) / float(height));
            if (const char* test = std::getenv("IW2_FX_TEST"); test && world) {
                // Debug: play an effect 150 units ahead and show it after IW2_FX_MS milliseconds.
                const Vec3 at = cam.pos + cam.forward() * 150.0f;
                fxSystem.play(efx(test), {at.x, at.y, at.z}, {0, 0, 1}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 200));
            }
            if (const char* tt = std::getenv("IW2_TRACER_TEST"); tt && world) {
                // Debug: N tracer attempts (cg_tracerchance applies) from beside the view towards 1500 units ahead, aged IW2_FX_MS.
                const Vec3 from = cam.pos + cam.right() * 30.0f + Vec3{0, 0, -15.0f};
                const Vec3 to = cam.pos + cam.forward() * 1500.0f;
                for (int i = 0; i < std::atoi(tt); ++i)
                    fxSystem.addTracer({from.x, from.y, from.z}, {to.x, to.y + float(i) * 10.0f, to.z}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 100));
            }
            if (const char* as = std::getenv("IW2_AIM_SPREAD")) hudAimSpread = float(std::atof(as)); // debug: aim spread for the shot
            if (viewFlash->pending && !(world && world->playerOnTurret())) {
                std::vector<DrawItem> scratch;
                addViewModel(scratch, cam); // plays the view flash (the live loop shows it one frame later)
            }
            auto shotItems = gatherItems(cam);
            std::cout << "effect particles: " << fxSystem.count() << "\n";
            if (atExitMarks) std::cout << "marks: " << scene.marks.marks().size() << "\n";
            if (!(world && world->playerOnTurret()) && !(world && scopeAlpha() > 0)) addViewModel(shotItems, cam);
            updateWaterMaps(scene, dev, world ? float(world->timeMs()) / 1000.0f : 0.0f);
            renderer.fog = fogState.consts(world ? world->timeMs() : 0);
            screenshot(dev, renderer, scene, cam, width, height, shot, shotItems, std::getenv("IW2_NO_HUD") ? std::vector<Quad2D>{} : buildHud(width, height, cam), forcedBlur); // IW2_NO_HUD: debug, no script HUD / fade
            Uploader::releasePool(dev);
            SDL_DestroyGPUDevice(dev);
            SDL_Quit();
            return 0;
        }

        SDL_Window* window = SDL_CreateWindow(("iw2view - " + args[1]).c_str(), int(width), int(height),
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) sdlFail("SDL_CreateWindow");
        if (!SDL_ClaimWindowForGPUDevice(dev, window)) sdlFail("SDL_ClaimWindowForGPUDevice");
        renderer.init(dev, SDL_GetGPUSwapchainTextureFormat(dev, window));
        SDL_SetWindowRelativeMouseMode(window, true);
        menuWindow = window;

        if (menuMode) frontStack.open("main"); // UI_SetActiveMenu(MAIN) opens "main" (0x4c6520)
        uint64_t last = SDL_GetTicksNS();
        bool running = true;
        bool walk = true;
        uint32_t prevButtons = 0;
        float gameClock = 0;
        // The camera of a player placed by a link or a vehicle is drawn interpolated like the entities (they lag one server frame): the
        // player origin before the last server frame and whether the camera follows the link this frame. TODO(spec): the original's rule.
        script::Vec3 prevPlayerO{0, 0, 0};
        bool camLinkLerp = false;
        float camLinkHeight = 60.0f;
        game::PlayerState ps;
        ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - 60.0f + 8.0f};
        bool wasDead = false;
        std::array<float, 2> bobAngles{0, 0};
        float deadYaw = 0;
        game::MoveRecorder moveRecorder;
        bool fellOut = false;
        std::array<game::Vec3, 3> prevBody{};
        bool havePrevBody = false;
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
        // Cinematic playback (docs/spec/video.md): modal, the way the original's `cinematic <name> 0` (0x4d9f90) is: the movie
        // fills the 4:3 screen area, any key or mouse button skips it (skippable byte 0x59a0d0), and the level load goes on after it.
        auto playCinematic = [&](const std::string& name) {
            const auto data = vfs.read("video/" + name + ".roq");
            if (!data) { std::cerr << "cinematic: no video/" << name << ".roq\n"; return; }
            std::unique_ptr<video::RoqDecoder> dec;
            try { dec = std::make_unique<video::RoqDecoder>(*data); } catch (const std::exception& e) { std::cerr << "cinematic " << name << ": " << e.what() << "\n"; return; }
            std::cout << "cinematic " << name << "\n";
            Scene movieScene = emptyScene(dev);
            SDL_GPUTexture* tex = nullptr;
            int tw = 0, th = 0;
            SDL_AudioStream* snd = nullptr;
            if (audioOn) {
                const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, video::RoqDecoder::kAudioRate};
                snd = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (snd) SDL_ResumeAudioStreamDevice(snd);
            }
            const uint64_t t0 = SDL_GetTicksNS();
            bool more = true, skipped = false;
            while (more && !skipped && running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT) running = false;
                    else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) skipped = true;
                }
                const int64_t due = int64_t(double(SDL_GetTicksNS() - t0) * 1e-9 * dec->fps()) + 1; // frames that should have been shown
                bool fresh = false;
                while (more && dec->frameIndex() < due) {
                    more = dec->nextFrame();
                    if (!more) break;
                    fresh = true;
                    auto pcm = dec->takeAudio();
                    if (snd && !pcm.empty()) {
                        if (dec->audioChannels() == 1) {
                            std::vector<int16_t> st(pcm.size() * 2);
                            for (size_t i = 0; i < pcm.size(); ++i) st[i * 2] = st[i * 2 + 1] = pcm[i];
                            pcm = std::move(st);
                        }
                        SDL_PutAudioStreamData(snd, pcm.data(), int(pcm.size() * sizeof(int16_t)));
                    }
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
                }
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
                SDL_GPUTexture* swap = nullptr;
                uint32_t w = 0, h = 0;
                if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h) && swap) {
                    const float sc = float(h) / 480.0f, left = (float(w) - 640.0f * sc) * 0.5f;
                    Quad2D q{left, 0, left + 640.0f * sc, 480.0f * sc};
                    q.texture = 1;
                    renderer.fog = {};
                    renderer.draw(cmd, swap, w, h, movieScene, cam, {}, std::vector<Quad2D>{q}, 0.0f);
                }
                SDL_SubmitGPUCommandBuffer(cmd);
            }
            if (snd) SDL_DestroyAudioStream(snd);
            SDL_WaitForGPUIdle(dev);
            releaseScene(dev, movieScene);
            // Held keys and buttons that skipped the movie must not reach the game (their release does).
            SDL_PumpEvents();
        };
        if (const char* mv = std::getenv("IW2_PLAY_CINEMATIC")) playCinematic(mv); // debug: play video/<name>.roq before the level
        // IW2_PROF=N: print per-frame CPU timings (ms) every N frames and quit after the second report.
        const int profEvery = std::getenv("IW2_PROF") ? std::atoi(std::getenv("IW2_PROF")) : 0;
        double profWater = 0, profDraw = 0, profFrame = 0, profWait = 0;
        int profFrames = 0, profReports = 0;
        uint64_t profT0 = SDL_GetTicksNS();
        while (running) {
            if (profEvery > 0 && ++profFrames > profEvery) {
                const double n = double(profFrames - 1);
                std::cout << "prof: frame " << profFrame / n << " ms, waterUpdate " << profWater / n << ", draw " << profDraw / n
                          << ", swapchainWait " << profWait / n << ", wall " << double(SDL_GetTicksNS() - profT0) * 1e-6 / n << " ms/frame\n";
                profFrames = 1; profWater = profDraw = profFrame = profWait = 0; profT0 = SDL_GetTicksNS();
                if (++profReports >= 2) running = false;
            }
            const uint64_t profFrameStart = SDL_GetTicksNS();
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (!frontStack.empty()) {
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
                        continue;
                    }
                    if (ev.type == SDL_EVENT_KEY_DOWN) continue;
                }
                if (ev.type == SDL_EVENT_QUIT) running = false;
                else if ((ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && !ev.key.repeat)
                    input.key(keyName(ev.key.scancode), ev.type == SDL_EVENT_KEY_DOWN);
                else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    // SDL numbers left, middle, right as 1, 2, 3; the engine's MOUSE2 is the right button.
                    input.key(ev.button.button == SDL_BUTTON_RIGHT ? "MOUSE2" : ev.button.button == SDL_BUTTON_MIDDLE ? "MOUSE3"
                                                                                                                     : "MOUSE" + std::to_string(int(ev.button.button)),
                              ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                else if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0)
                    input.key(ev.wheel.y > 0 ? "MWHEELUP" : "MWHEELDOWN", true), input.key(ev.wheel.y > 0 ? "MWHEELUP" : "MWHEELDOWN", false);
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    // CG_CalcFov 0x4ae8c0: look sensitivity is scaled by zoomSens = fovX / cg_fov (80), linear in degrees (0xf791c8, applied in
                    // CL_MouseMove 0x409720). The base 0.15 deg per count is kept (exe default sensitivity 5 · m_yaw 0.022 = 0.11, TODO(spec):
                    // SDL xrel units).
                    const float zoomSens = std::clamp(cam.fovX43 / 80.0f, 0.01f, 1.0f);
                    // CL_MouseMove: the pitch change is dy * m_pitch (cvar, default 0.022; negative = inverted, set by the invert menus);
                    // the base 0.15 stands for the default.
                    // The base 0.15 deg/count stands for the default sensitivity 5 (cvar sensitivity 1..30 from the options menu).
                    const float lookSens = uiCvars.count("sensitivity") ? std::clamp(float(std::atof(uiCvars["sensitivity"].c_str())), 1.0f, 30.0f) / 5.0f : 1.0f;
                    float pitchScale = 1.0f;
                    if (world)
                        if (const auto it = world->cvars.find("m_pitch"); it != world->cvars.end()) pitchScale = float(std::atof(it->second.c_str())) / 0.022f;
                    if (!(world && world->controlsFrozen())) { // freezecontrols: TODO(spec) pm_flags 0x8000 effect on the view
                        cam.yaw -= ev.motion.xrel * 0.15f * lookSens * zoomSens;
                        cam.pitch = std::clamp(cam.pitch + ev.motion.yrel * 0.15f * lookSens * zoomSens * pitchScale, -89.0f, 89.0f);
                    }
                }
                if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (world && frontStack.empty()) { // CL_KeyEvent 0x40c216: ESC in a running level -> UI_SetActiveMenu(2)
                        menuEnv.setCvar("cl_paused", "1");
                        menuMx = 320.0f, menuMy = 240.0f;
                        frontStack.open("main");
                    } else running = false;
                }
                else if (ev.key.scancode == SDL_SCANCODE_L) renderer.fullbright = !renderer.fullbright;
                else if (ev.key.scancode == SDL_SCANCODE_F10) {
                    godMode = !godMode;
                    std::cout << (godMode ? "godmode ON" : "godmode OFF") << "\n"; // GAME_GODMODE_ON / _OFF
                }
                else if (ev.key.scancode == SDL_SCANCODE_V) {
                    walk = !walk;
                    if (walk) { ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - ps.viewHeight}; ps.velocity = {0, 0, 0}; }
                }
                else if (ev.key.scancode == SDL_SCANCODE_P) {
                    std::cout << "--pos " << cam.pos.x << "," << cam.pos.y << "," << cam.pos.z << " --yaw " << cam.yaw << " --pitch " << cam.pitch << "\n";
                    if (moveRecorder.write("pmove_replay.bin")) std::cout << "wrote pmove_replay.bin (iw2game <main> <map> --replay pmove_replay.bin)\n";
                }
            }
            const uint64_t now = SDL_GetTicksNS();
            const float dt = float(now - last) * 1e-9f;
            last = now;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            const bool playerDead = world && world->player() && world->player()->obj->get("health").isNumber() &&
                                    world->player()->obj->get("health").asInt() <= 0;
            // In a vehicle (pm_flags 0x20000) the player does not move by itself; the command drives the vehicle.
            const bool inVehicle = world && (world->playerPmFlags() & 0x20000);
            // On a turret (eFlags & 0x300) PmoveSingle skips the movement and PM_Weapon (0x50c918) and the view stays
            // inside the turret's arcs (PM_UpdateViewAngles 0x50b120).
            const bool onTurret = world && world->playerOnTurret();
            if (onTurret) world->clampPlayerView(cam.pitch, cam.yaw);
            // This frame's usercmd buttons from the bound commands (CL_CmdButtons); weapon states 0x13 / 0x14 force aim.
            // freezecontrols (client +0x8630 → pm_flags 0x8000) and an open menu: no movement or button commands.
            const bool controlsBlocked = !frontStack.empty() || (world && world->controlsFrozen());
            const uint32_t buttons = controlsBlocked ? 0u : input.buttons(inVehicle, pweapon.stateNumber() == 0x13 || pweapon.stateNumber() == 0x14);
            const bool reloadPressed = (buttons & game::Button::Reload) && !(prevButtons & game::Button::Reload);
            prevButtons = buttons;
            if (world) {
                game::World::PlayerCommand pc;
                pc.forward = controlsBlocked ? 0 : input.forwardMove();
                pc.right = controlsBlocked ? 0 : input.rightMove();
                pc.buttons = buttons;
                pc.viewAngles = {cam.pitch, cam.yaw, 0};
                pc.stance = ps.stance == game::Stance::Prone ? 2 : ps.stance == game::Stance::Crouch ? 1 : 0;
                pc.viewHeight = ps.viewHeight;
                pc.onGround = ps.onGround;
                pc.weaponState = pweapon.stateNumber();
                pc.grenadeFuseMs = pweapon.fuseMs();
                pc.lean = ps.lean;
                world->setPlayerCommand(pc);
            }
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
            } else if (inVehicle) {
                // The view turns with the vehicle's tag_body (cgame 0x4aeb60, cg_viewVehicleInfluence 1): the view axes
                // times Bprevᵀ·Bcur; the pitch always follows, the yaw only while pm_flags 0x40000 is set.
                std::array<game::Vec3, 3> body;
                if (world->playerVehicleBodyAxis(body)) {
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
                cam.pos = {po[0], po[1], po[2] + 60.0f}; // stand view height 60 in a vehicle (0x509d10)
                camLinkLerp = true;
                camLinkHeight = 60.0f;
                ps.origin = {po[0], po[1], po[2]};
                ps.velocity = {0, 0, 0};
            } else if (world && world->playerLinked()) {
                // Linked by script (pm_type 1): the link places the player (src/game/playerlink.cpp); no own movement.
                const script::Vec3 po = world->player()->obj->get("origin").asVector();
                cam.pos = {po[0], po[1], po[2] + ps.viewHeight};
                camLinkLerp = true;
                camLinkHeight = ps.viewHeight;
                ps.origin = {po[0], po[1], po[2]};
                ps.velocity = {0, 0, 0};
            } else if (walk) {
                camLinkLerp = false;
                if (havePrevBody) { // just got out: continue from where the vehicle code put the player (tag_detach)
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    ps.origin = {po[0], po[1], po[2]};
                }
                havePrevBody = false;
                game::PlayerInput in;
                in.forward = controlsBlocked ? 0.0f : float(input.forwardMove()) / 127.0f;
                in.right = controlsBlocked ? 0.0f : float(input.rightMove()) / 127.0f;
                in.jump = (buttons & game::Button::Jump) != 0;
                in.stance = (buttons & game::Button::Prone) ? game::Stance::Prone
                            : (buttons & game::Button::Crouch) ? game::Stance::Crouch : game::Stance::Stand;
                // allowleanleft/right(false) set pm_flags 0x2000000/0x4000000, which cancel the lean (0x50ae13).
                in.leanLeft = (buttons & game::Button::LeanLeft) != 0 && !(world->playerPmFlags() & 0x2000000u);
                in.leanRight = (buttons & game::Button::LeanRight) != 0 && !(world->playerPmFlags() & 0x4000000u);
                in.yaw = cam.yaw;
                in.dead = playerDead;
                in.adsHeld = (buttons & game::Button::Ads) != 0;
                in.adsAllowed = pweapon.adsAllowed();
                in.weaponState = pweapon.stateNumber();
                // Fixed 20 Hz-style substeps keep movement stable at any frame rate.
                float remaining = std::min(dt, 0.1f);
                // With a running level the player clips against the world and its solid entities (brush models,
                // actors) like G_Trace in the original pmove (0x507b00 → 0x4db7f0 → 0x44bf00), skipping itself.
                game::MoveParams pmp = mp;
                if (world && world->player()) pmp.passEntity = world->player()->number;
                pmp.mantle = &mantleAnims;
                // The weapon's moveSpeedScale (weapon def +0x264) scales the walk (0x508140).
                if (pweapon.equipped()) pmp.weaponSpeedScale = pweapon.number("moveSpeedScale");
                if (pweapon.equipped()) {
                    auto n = [&](const char* k) { return pweapon.number(k); };
                    pmp.hipSpread = {n("hipSpreadStandMin"), n("hipSpreadDuckedMin"), n("hipSpreadProneMin"), n("hipSpreadMax"),
                                     n("hipSpreadDuckedMax"), n("hipSpreadProneMax"), n("hipSpreadDecayRate"), n("hipSpreadFireAdd"),
                                     n("hipSpreadTurnAdd"), n("hipSpreadMoveAdd"), n("hipSpreadDuckedDecay"), n("hipSpreadProneDecay")};
                }
                in.pitch = cam.pitch;
                in.adsFrac = pweapon.adsFrac();
                const game::CollisionWorld& pcw = world ? world->collision() : *scene.collision; // null reference only in the front end, where it is unused
                while (remaining > 0 && scene.collision) {
                    const float step = std::min(remaining, 1.0f / 60.0f);
                    game::RecordedMove rec{ps, {}, in, step, pmp, pcw.entityClips()};
                    game::playerMove(pcw, ps, in, step, pmp);
                    // Mantle stance events (0x8d crouch, 0x8c stand) set the client stance.
                    if (ps.stanceEvent >= 0) input.setStance(ps.stanceEvent);
                    rec.after = ps;
                    moveRecorder.add(std::move(rec));
                    remaining -= step;
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
                hudAimSpread = ps.aimSpread;
                hudViewHeight = ps.viewHeight;
                cam.yaw = game::mantleClampYaw(ps, cam.yaw); // the view yaw cap while mantling (0x525b60)
                cam.pos = {ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeight};
                // Dead view (cgame 0x4ae2f5: pm_type ≥ 4): pitch −15, roll 40, yaw the player state's dead yaw
                // (ps+0x10c). TODO(spec): that yaw (set at death towards the killer); the current yaw is kept.
                // The command's view angles are not applied while dead (0x50b12b), so the yaw stays where it was.
                if (playerDead) {
                    if (!wasDead) deadYaw = world->playerDeadYaw() ? *world->playerDeadYaw() : cam.yaw; // ps+0x10c (0x4ae314)
                    cam.yaw = deadYaw;
                    cam.pitch = -15.0f;
                    cam.roll = 40.0f;
                } else {
                    cam.roll = 0;
                }
                wasDead = playerDead;
                // Camera bob (0x4ae290): up by the bob wave, sideways along the right vector (pitch/yaw/roll of the view).
                // TODO(spec): the landing dip (0xf790a0), the damage kick (0xf79764), the minimum eye height (0x592010).
                if (!playerDead) {
                    const float xy = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]);
                    const auto vb = game::viewBob(ps.bobCycle, xy, ps.viewHeightTarget, pweapon.adsFrac(), pweapon.number("adsViewBobMult"));
                    bobAngles = {vb[2], vb[3]};
                    const Vec3 rgt = cam.right();
                    cam.pos = cam.pos + Vec3{0, 0, vb[0]} + rgt * vb[1];
                    if (world) {
                        if (ps.landSeq != lastLandSeq) { // a landing (cgame 0x4bab6e / 0x4bab15: the own client's fall event)
                            lastLandSeq = ps.landSeq;
                            viewImp.land(ps.landHeight, ps.landDamage, world->timeMs());
                        }
                        cam.pos.z += viewImp.landOffset(world->timeMs()); // 0x4ae6c7
                    }
                }
                // Lean moves the view origin sideways and down (0x4ae770); the view itself does not roll.
                const game::Vec lo = game::leanOffset(cam.yaw, ps.lean);
                cam.pos = cam.pos + Vec3{lo[0], lo[1], lo[2]};
                if (!playerDead) cam.pos.z = std::max(cam.pos.z, game::ViewImpulses::minEyeZ(ps.origin[2])); // 0x4ae775
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

            if (world && world->player()) world->player()->obj->set("$godmode", script::Value(godMode));
            if (world) {
                syncWeapon();
                game::PlayerWeapon::Input wi;
                // weapnext / weapprev (cgame 0x4b7410): the weapon in the other of the two slots (primary, primaryb).
                if (const int cycle = input.takeWeaponCycle(); cycle && world->player()) {
                    const game::Inventory& inv = world->inventory(*world->player());
                    const int cur = inv.slot[2] == weaponSelect ? 2 : 1;
                    const std::string& other = inv.slot[cur == 1 ? 2 : 1];
                    if (!other.empty()) weaponSelect = other; // TODO(spec): weapons outside the slots (0x4b72f0)
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
                    if (!prevSelect.empty() && !(pweapon.name() == prevSelect && pweapon.field("altWeapon") == weaponSelect))
                        input.clearAds();
                    prevSelect = weaponSelect;
                }
                wi.weapon = weaponSelect;
                wi.holster = game::mantleLowersWeapon(ps) || inVehicle; // 0x4de880
                if (inVehicle) // the weapon fire mask excludes attack, melee, reload and the offhands in a vehicle (0x4df9b0: 0x30014)
                    wi.fire = wi.melee = wi.reload = wi.frag = wi.smoke = false;
                const int shots = onTurret ? 0 : pweapon.update(std::min(dt, 0.1f), wi);
                if (aimBlendWeapon != pweapon.name()) { aimBlendWeapon = pweapon.name(); aimBlend.reset(); }
                aimBlend.update(pweapon.viewAiming(), std::min(dt, 0.1f));
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
                    const int64_t nowMs = world ? world->timeMs() : 0;
                    if (pweapon.name() != hudFade.weapon) {
                        if (!pweapon.name().empty()) hudFade.ammo = hudFade.weaponChange = hudFade.offhand = nowMs;
                        hudFade.weapon = pweapon.name();
                    }
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
                }
                for (const auto& e : pweapon.takeEvents()) {
                    const game::Vec3 eye{cam.pos.x, cam.pos.y, cam.pos.z}, angles{cam.pitch, cam.yaw, 0};
                    if (e.kind == game::PlayerWeapon::Event::MeleeHit) world->playerMelee(eye, angles);
                    else if (e.kind == game::PlayerWeapon::Event::OffhandThrow)
                        world->playerThrowGrenade(e.weapon, eye, angles, ps.velocity);
                    else if (e.kind == game::PlayerWeapon::Event::OffhandExplode && world->player()) // event 200 (0x4e775d)
                        world->playerGrenadeInHand(eye);
                }
                vmMotion.update([&](const char* k) { return pweapon.number(k); }, int(std::min(dt, 0.1f) * 1000.0f),
                                std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]), 190.0f,
                                ps.viewHeight, pweapon.adsFrac(), ps.lean);
                {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    vmMotion.sway([&](const char* k) { return pweapon.number(k); }, int(std::min(dt, 0.1f) * 1000.0f), cam.pitch, cam.yaw,
                                  pweapon.adsFrac(), ps.stance == game::Stance::Prone ? 2 : ps.stance == game::Stance::Crouch ? 1 : 0,
                                  !ret.empty() && ret != "none");
                }
                {
                    const std::string ret = pweapon.field("adsOverlayReticle");
                    gunBobAngles = game::gunBob(ps.bobCycle, std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]),
                                                ps.viewHeightTarget, pweapon.adsFrac(), pweapon.number("adsBobFactor"),
                                                !ret.empty() && ret != "none");
                }
                recoil.update([&](const char* k) { return pweapon.number(k); }, int(std::min(dt, 0.1f) * 1000.0f), true, pweapon.adsFrac());
                cam.fovX43 = game::adsFov(80.0f, pweapon.number("adsZoomFov") > 0 ? pweapon.number("adsZoomFov") : 80.0f,
                                          pweapon.number("adsZoomInFrac"), pweapon.number("adsZoomOutFrac"), pweapon.adsFrac(),
                                          pweapon.adsZoomingIn());
                for (const auto& snd : pweapon.takeSounds()) mixer.play(snd);
                {
                    const Vec3 f = cam.forward(), r = cam.right();
                    mixer.setListener({cam.pos.x, cam.pos.y, cam.pos.z}, {f.x, f.y, f.z}, {r.x, r.y, r.z});
                }
                const Vec3 f = cam.forward();
                {
                    game::HipSpread hs;
                    hs.fireAdd = pweapon.number("hipSpreadFireAdd");
                    for (int i = 0; i < shots; ++i) game::addFireSpread(ps, hs, pweapon.adsFrac()); // 0x4dece0
                }
                for (int i = 0; i < shots; ++i) recoil.fire([&](const char* k) { return pweapon.number(k); }, pweapon.adsFrac(), false);
                if (shots) viewFlash->pending = viewFlash->brassPending = true; // EV_FIRE_WEAPON -> fire flag (0x4b7800)
                for (int i = 0; i < shots; ++i) world->playerFire({cam.pos.x, cam.pos.y, cam.pos.z}, {f.x, f.y, f.z});
                if (shots) std::cout << pweapon.name() << " " << pweapon.clip() << "/" << pweapon.stock() << "  hits " << world->stats.hits << " kills " << world->stats.kills << "\n";
                if (!inVehicle && !onTurret && !world->playerLinked()) {
                    world->setPlayerOrigin({ps.origin[0], ps.origin[1], ps.origin[2]}, {0, cam.yaw, 0});
                    world->playerPmEvents(ps.eventSeq, ps.events, ps.stance == game::Stance::Crouch); // G_ClientEvents
                }
                // time_slow: the frame scale moves linearly from `from` to `to` over the real time (0x4bee00).
                if (slowStartNs) {
                    const uint64_t nowNs = SDL_GetTicksNS();
                    if (nowNs >= slowEndNs) slowmo = slowTo, slowStartNs = slowEndNs = 0;
                    else if (nowNs > slowStartNs) {
                        const float t = float(nowNs - slowStartNs) / float(slowEndNs - slowStartNs);
                        slowmo = (1.0f - t) * slowFrom + slowTo * t;
                    }
                }
                mixer.fadeStep(dt * 1000.0f);
                world->setRealTimeMs(int64_t(SDL_GetTicksNS() / 1000000ull));
                if (menuEnv.getCvar("cl_paused") != "1") gameClock += std::min(dt, 0.25f) * slowmo; // cl_paused freezes the simulation
                while (gameClock >= 0.05f) {
                    // The previous server frame's entity placement, for the client interpolation below.
                    lerpPrev.clear();
                    lastDamageEvent = 0;
                    lastLandSeq = 0;
                    viewImp.reset(); // 0x4ba570
                    for (const auto& e : world->entities()) {
                        const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                        if (o.isVector())
                            lerpPrev[e->number] = {o.asVector(), a.isVector() ? a.asVector() : script::Vec3{0, 0, 0}};
                    }
                    for (const auto& e : world->entities()) {
                        if (e->turret) prevTurret[e->number] = {e->turret->pitch, e->turret->yaw};
                        if (e->vehicle) prevTurret[e->number] = {e->vehicle->turretPitch, e->vehicle->turretYaw};
                        if (!e->anim) continue;
                        prevAnim.erase(e->number);
                        prevAnim.emplace(e->number, *e->anim);
                    }
                    if (world->player()) prevPlayerO = world->player()->obj->get("origin").asVector();
                    world->frame();
                    gameClock -= 0.05f;
                }
                lerpFrac = std::clamp(gameClock / 0.05f, 0.0f, 1.0f);
                if (camLinkLerp && world->player()) { // see prevPlayerO
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    cam.pos = {prevPlayerO[0] + (po[0] - prevPlayerO[0]) * lerpFrac, prevPlayerO[1] + (po[1] - prevPlayerO[1]) * lerpFrac,
                               prevPlayerO[2] + (po[2] - prevPlayerO[2]) * lerpFrac + camLinkHeight};
                }
                // Damage feedback (cgame 0x4ba2a0 on a new damageEvent): the aim spread scale grows by the count; unless
                // the damage came from the world (pitch and yaw 255), the oldest of 8 icon slots gets the time,
                // cg_hudDamageIconTime and yaw = yaw byte / 255 · 360 + (rand / 32768 − 0.5) · 20, through a short
                // angle. TODO(spec): the view kick (0xf7979c / 0xf797a0) and the "Health" menu flag.
                if (const auto& df = world->playerDamageFeedback(); df.event != lastDamageEvent) {
                    lastDamageEvent = df.event;
                    ps.aimSpread = std::min(255.0f, ps.aimSpread + float(df.count));
                    if (df.count != 0) { // 0x4ba1c3: only with a damage count
                        const Vec3 f = cam.forward(), l = cam.right() * -1.0f; // viewaxis rows 0 and 1 (left)
                        viewImp.damage(df.yaw, df.pitch, df.count, {f.x, f.y, f.z}, {l.x, l.y, l.z}, world->timeMs());
                    }
                    if (!(df.pitch == 0xff && df.yaw == 0xff)) {
                        size_t slot = 0;
                        for (size_t k = 1; k < damageIcons.size(); ++k)
                            if (damageIcons[k].start < damageIcons[slot].start) slot = k;
                        const float y = float(df.yaw) / 255.0f * 360.0f + (float(std::rand() % 32768) / 32768.0f - 0.5f) * 20.0f;
                        const int sh = int(y * (65536.0f / 360.0f)) & 0xffff;
                        damageIcons[slot] = {world->timeMs(), 2000, float(sh) * (360.0f / 65536.0f)};
                    }
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
                if (const auto rel = world->takeTurretRelease()) {
                    // Off the turret (0x51b3d0): back where the player got on, in the stance it had (events 0x8c–0x8e).
                    ps.origin = {rel->origin[0], rel->origin[1], rel->origin[2]};
                    ps.velocity = {0, 0, 0};
                    if (rel->stance >= 0) input.setStance(rel->stance);
                }
                for (const auto& cmd : world->takeClientCommands()) {
                    std::istringstream a(cmd.arg);
                    if (cmd.name == "popupopen" || cmd.name == "popupclose") {
                        menuCommand(cmd.name, cmd.arg);
                    } else if (cmd.name == "obj_update" || cmd.name == "obj_complete" || cmd.name == "gm")
                        screenMessages.push_back({cmd.arg, SDL_GetTicksNS() + 5000000000ull, false, SDL_GetTicksNS(), SDL_GetTicksNS() + 5000000000ull}); // con_gamemessagetime 5 s (0x571118)
                    else if (cmd.name == "gmb")
                        screenMessages.push_back({cmd.arg, SDL_GetTicksNS() + 8000000000ull, true, SDL_GetTicksNS(), SDL_GetTicksNS() + 8000000000ull}); // con_boldgamemessagetime 8 s (0x571100)
                    else if (cmd.name == "opendeadscreen")
                        deadScreenMs = world->timeMs(); // cg.time (0x4a6d3c)
                    else if (cmd.name == "time_slow") { // time_slow <ms> <from> <to> (0x4a69a0 → 0x4bec20)
                        int ms = 0;
                        a >> ms >> slowFrom >> slowTo;
                        slowStartNs = SDL_GetTicksNS();
                        slowEndNs = slowStartNs + uint64_t(std::max(ms, 0)) * 1000000ull;
                    } else if (cmd.name == "snd_fade") { // snd_fade <volume> <ms> (0x4a6ec4)
                        float vol = 1;
                        int ms = 0;
                        a >> vol >> ms;
                        mixer.fade(vol, ms);
                    }
                    else if (cmd.name == "fog") { // fog <near> <far> <1> <r> <g> <b> <ms> (0x4a5770)
                        float nearD = 0, farD = 0, one = 1, r = 0, g = 0, b = 0, ms = 0;
                        a >> nearD >> farD >> one >> r >> g >> b >> ms;
                        if (one >= 1.0f) fogState.command(nearD, farD, r, g, b, int(ms), world->timeMs());
                        else fogState.commandExp(one, r, g, b, int(ms), world->timeMs());
                    }
                    else if (cmd.name == "clear_blur")
                        screenBlur.clear();
                    else if (cmd.name == "scr_blur") { // scr_blur <ms> <radius> <realtime> <priority> (0x4beb80)
                        int ms = 0, rt = 0, prio = 0;
                        float radius = 0;
                        a >> ms >> radius >> rt >> prio;
                        const int64_t now = rt ? int64_t(SDL_GetTicks()) : world->timeMs();
                        // TODO(spec): the original's real-time base is "ms since first use" (0x1cbaec4); SDL ticks differ only by an offset.
                        screenBlur.command(ms, radius, rt != 0, prio, std::max<int64_t>(now, 1), screenBlur.composed());
                    }
                }
                if (auto change = world->dueLevelChange()) pendingLevel = std::make_pair(change->map, change->failed);
            }
            if (pendingLevel) {
                    // Level change or restart: rebuild the world and the scene for the next map.
                    currentMap = pendingLevel->first;
                    std::cout << (pendingLevel->second ? "mission failed, restarting " : "changing level to ") << currentMap << "\n";
                    fogState.clear();
                    screenBlur.clear(); // a new level starts unblurred (the death blur stays until the level reloads)
                    if (world) { // a cinematic() recorded by the script plays before the next level loads (0x4d9f90)
                        const std::string movie = world->takeCinematic();
                        if (!movie.empty()) playCinematic(movie);
                    }
                    const script::ObjectPtr carried = world ? world->vm().game() : script::ObjectPtr();
                    world.reset();
                    releaseScene(dev, scene);
                    world = makeWorld(currentMap, carried);
                    scene = loadScene(dev, vfs, currentMap, false);
                    frontStack.closeAll(); // leaving the front end / pause menu
                    aliases.clear();
                    aliases.load(vfs, currentMap);
                    aliases.load(vfs, "menu", true); // the pause menu keeps its interface sounds
                    cam = scene.start;
                    ps = game::PlayerState{};
                    ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - ps.viewHeight};
                    pweapon = game::PlayerWeapon{};
                    weaponSelect.clear();
                    weaponWritten.clear();
                    prevSelect.clear();
                    input.clearAds(); // the toggled aim ends with the level (0x40e821)
                    lerpPrev.clear();
                    prevAnim.clear();
                    prevTurret.clear();
                    gameClock = 0;
                    // The new level starts with a fresh client: no dead screen, normal speed and volume (inferred: the
                    // original's reset on load is not read).
                    deadScreenMs = 0;
                    slowStartNs = slowEndNs = 0;
                    slowmo = 1;
                    mixer.fade(1.0f, 0);
                pendingLevel.reset();
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
                    if (k == "pause" && world && frontStack.empty()) {
                        menuEnv.setCvar("cl_paused", "1");
                        frontStack.open("main");
                    } else if (k.rfind("set:", 0) == 0) { // set:name=value
                        const size_t eq = k.find('=');
                        menuEnv.setCvar(k.substr(4, eq - 4), k.substr(eq + 1));
                    } else if (k.rfind("open:", 0) == 0) {
                        frontStack.open(k.substr(5));
                    } else if (k.rfind("click:", 0) == 0) { // click:X/Y in virtual pixels (left press and release)
                        const size_t sl = k.find('/');
                        menuMx = float(std::atof(k.substr(6, sl - 6).c_str())), menuMy = float(std::atof(k.substr(sl + 1).c_str()));
                        frontStack.mouseMove(menuMx, menuMy);
                        frontStack.click(menuMx, menuMy);
                        frontStack.release();
                    } else if (k == "quit") {
                        running = false;
                    } else if (k == "wait") {
                    } else if (k.rfind("bindkey:", 0) == 0) {
                        frontStack.captureKey(k.substr(8));
                    } else
                    frontStack.key(k == "up" ? K::Up : k == "down" ? K::Down : k == "esc" ? K::Escape : k == "left" ? K::Left : k == "right" ? K::Right : K::Enter);
                    if (const ui::Item* f = frontStack.focus()) std::cout << "  focus " << f->name << ", menus " << frontStack.menus().size() << "\n";
                    std::cout << "  cl_paused=" << menuEnv.getCvar("cl_paused") << " clock=" << (world ? world->timeMs() : -1) << "\n";
                    nextKey = SDL_GetTicksNS() + 2000000000ull;
                }
            }
            if (profileDirty) { // write the archived cvars (config.cfg) after a menu changed one
                profileDirty = false;
                std::error_code ec;
                std::filesystem::create_directories(profileCfg.parent_path(), ec);
                if (std::ofstream of(profileCfg); of) of << ui::formatConfig(uiCvars) << input.formatBindings();
            }
            if (const auto sv = uiCvars.find("snd_volume"); sv != uiCvars.end()) mixer.masterVolume = std::clamp(float(std::atof(sv->second.c_str())), 0.0f, 1.0f);
            if (world && frontStack.empty() && menuEnv.getCvar("cl_paused") == "1") menuEnv.setCvar("cl_paused", "0"); // menus closed: unpause
            {   // The cursor is shown while menus are open (relative mouse look otherwise).
                const bool wantCursor = !frontStack.empty();
                if (wantCursor == SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, !wantCursor);
            }
            const Camera camBase = cam;
            cam.pitch += recoil.viewKick()[0]; // view kick spring (0x4ae327)
            cam.yaw += recoil.viewKick()[1];
            cam.pitch += bobAngles[0]; // ADS view bob (0x4ae4a9)
            cam.yaw += bobAngles[1];
            if (world && !onTurret) { // damage kick (0x4ae371): pitch and roll
                const std::string ret = pweapon.field("adsOverlayReticle");
                const auto dk = viewImp.damageKick(world->timeMs(), pweapon.adsFrac(), !ret.empty() && ret != "none");
                cam.pitch += dk[0];
                cam.roll += dk[1];
            }
            fxRight = cam.right();
            fxUp = cross(cam.right(), cam.forward());
            // The per-element spawnRange / cullrange gate (0x485300): view origin and tan(fovX / 2) / tan(40 deg) (0x486e00).
            fxSystem.viewOrigin = {cam.pos.x, cam.pos.y, cam.pos.z};
            fxSystem.viewScale = std::tan(cam.fovX43 * 0.5f * float(M_PI) / 180.0f) / std::tan(0.6981317f);
            fxSystem.viewValid = true;
            setFxFrustum(fxSystem, cam, float(width) / float(height));
            auto items = gatherItems(cam);
            // No view model on a turret (cgame 0x4b6d40 returns with eFlags & 0x300); in a vehicle only in the
            // binocular weapon states 0x11–0x16.
            const int wstate = pweapon.stateNumber();
            if (!(world && world->playerOnTurret()) &&
                !(world && (world->playerPmFlags() & 0x20000) && !(wstate >= 0x11 && wstate <= 0x16)))
                if (!(world && scopeAlpha() > 0) && !(world && world->playerDeadWeaponless())) // scope overlay (0x4b6d7c); dead: entity weapon 0 (0x4b6a0a)
                    addViewModel(items, cam);
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUTexture* swap = nullptr;
            uint32_t w = 0, h = 0;
            const uint64_t profA = SDL_GetTicksNS();
            const bool gotSwap = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h);
            profWait += double(SDL_GetTicksNS() - profA) * 1e-6;
            if (gotSwap && swap)
            {
                renderer.fog = fogState.consts(world ? world->timeMs() : 0);
                screenBlur.update(world ? world->timeMs() : 0, int64_t(SDL_GetTicks()));
                const float blur = std::max(screenBlur.composed(), forcedBlur);
                const uint64_t profB = SDL_GetTicksNS();
                updateWaterMaps(scene, dev, world ? float(world->timeMs()) / 1000.0f : float(SDL_GetTicks()) / 1000.0f);
                const uint64_t profC = SDL_GetTicksNS();
                renderer.draw(cmd, swap, w, h, scene, cam, items, buildHud(w, h, cam), blur);
                profWater += double(profC - profB) * 1e-6;
                profDraw += double(SDL_GetTicksNS() - profC) * 1e-6;
            }
            SDL_SubmitGPUCommandBuffer(cmd);
            profFrame += double(SDL_GetTicksNS() - profFrameStart) * 1e-6;
            cam.pitch = camBase.pitch; // the ADS bob turns only the drawn view
            cam.yaw = camBase.yaw;
            cam.roll = camBase.roll;
        }
        if (audioStream) SDL_DestroyAudioStream(audioStream);
        SDL_ReleaseWindowFromGPUDevice(dev, window);
        SDL_DestroyWindow(window);
        Uploader::releasePool(dev);
        SDL_DestroyGPUDevice(dev);
        SDL_Quit();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
