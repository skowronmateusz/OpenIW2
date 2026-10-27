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
#include "game/clientinput.h"
#include "game/collision.h"
#include "game/playerweapon.h"
#include "game/viewweapon.h"
#include "game/fx.h"
#include "game/marks.h"
#include "game/pmove.h"
#include "game/pmoverecord.h"
#include "game/turrets.h"
#include "game/world.h"
#include "ui/menu.h"
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

// ---------------------------------------------------------------------------------------------
// GPU upload helper: stages data in one transfer buffer and flushes when it is full.

class Uploader {
public:
    explicit Uploader(SDL_GPUDevice* dev) : dev_(dev) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = kSize;
        tb_ = SDL_CreateGPUTransferBuffer(dev_, &info);
        if (!tb_) sdlFail("SDL_CreateGPUTransferBuffer");
    }
    ~Uploader() {
        flush();
        SDL_ReleaseGPUTransferBuffer(dev_, tb_);
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
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
        SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);
        cmd_ = nullptr;
        copy_ = nullptr;
        mapped_ = nullptr;
        used_ = 0;
    }

private:
    static constexpr uint32_t kSize = 64u << 20;

    uint32_t reserve(uint32_t size) {
        uint32_t aligned = (used_ + 255u) & ~255u;
        if (cmd_ && aligned + size > kSize) {
            flush();
            aligned = 0;
        }
        if (!cmd_) {
            mapped_ = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, tb_, true));
            cmd_ = SDL_AcquireGPUCommandBuffer(dev_);
            copy_ = SDL_BeginGPUCopyPass(cmd_);
        }
        used_ = aligned + size;
        return aligned;
    }
    SDL_GPUCopyPass* pass() { return copy_; }

    SDL_GPUDevice* dev_;
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

enum class Pass { Opaque, AlphaTest, Blend, Additive, Multiply, Sky, Skip };

struct Batch {
    Pass pass = Pass::Opaque;
    int texture = 0;   // index into Scene::textures
    int lightmap = -1; // -1 = none, -2 = per-vertex light from the light grid
    bool fullbright = false;
    int polygonOffset = 0; // mtlPolygonOffset level 0..2 (material state bits)
    int cull = 0;          // mtlCullFace: 0 none, 1 back, 2 front
    uint32_t firstIndex = 0, indexCount = 0;
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
    std::vector<SDL_GPUTexture*> textures; // [0] = white
    std::vector<Lightmap> lightmaps;
    Lightmap dummyLightmap;
    std::vector<Batch> batches;
    SDL_GPUTexture* sky = nullptr;
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

Pass classify(const std::string& techset) {
    if (techset == "sky") return Pass::Sky;
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
        }
        try {
            const auto m = assets::parseMaterial(*md);
            mi.pass = classify(m.techniqueSet);
            const std::string& img = m.colorImage();
            if (mi.pass == Pass::Sky) {
                if (!scene.sky) scene.sky = loadCubeImage(dev, up, vfs, img);
            } else if (!img.empty() && img[0] != '$' && mi.pass != Pass::Skip) {
                auto it = scene.imageIndex.find(img);
                if (it == scene.imageIndex.end()) {
                    SDL_GPUTexture* t = loadImage(dev, up, vfs, img);
                    int idx = 0;
                    if (t) {
                        idx = int(scene.textures.size());
                        scene.textures.push_back(t);
                    }
                    it = scene.imageIndex.emplace(img, idx).first;
                }
                mi.texture = it->second;
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
            const Vec3 corners[4] = {c + nr + nu, c + r + nu, c + r + u, c + nr + u};
            const float uv[4][2] = {{s->uv[0], s->uv[3]}, {s->uv[2], s->uv[3]}, {s->uv[2], s->uv[1]}, {s->uv[0], s->uv[1]}};
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
    for (size_t i = 1; i < scene.textures.size(); ++i) tex(scene.textures[i]); // [0] is the shared white texture
    for (auto& lm : scene.lightmaps) { tex(lm.coeffs); tex(lm.sun); }
    tex(scene.sky);
    for (auto& [name, m] : scene.models)
        if (m) { buf(m->vertices); buf(m->indices); }
    for (auto& [key, m] : scene.skinned) { buf(m.vertices); buf(m.indices); }
    scene = Scene{};
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
    std::map<std::tuple<int, int, int, int, int>, std::vector<uint32_t>> groups;
    for (const auto& s : bsp.soups()) {
        const MatInfo& mi = mats[s.material];
        if (mi.pass == Pass::Skip) continue;
        const int lm = s.lightmap < scene.lightmaps.size() ? int(s.lightmap) : -1;
        auto& dst = groups[{int(mi.pass), mi.texture, lm, mi.polygonOffset, mi.cull}];
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
            auto& dst = groups[{int(mi.pass), mi.texture, -2, mi.polygonOffset, mi.cull}];
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

// ---------------------------------------------------------------------------------------------
// Renderer

struct FragmentUniforms {
    float sunDirection[4];
    float sunColor[4];
    float lmWeights[4];
    float params[4];
    float ambient[4];
};

// An entity model to draw this frame.
struct DrawItem {
    const GpuModel* model = nullptr;
    Mat4 transform{};
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float sunVisibility = 1.0f;
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
    SDL_GPUGraphicsPipeline* skyPipeline = nullptr;
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUTexture* depth = nullptr;
    uint32_t depthW = 0, depthH = 0;
    float lmScale = 1.0f;
    // Weights for a flat normal map from the $lightmapweights rule (docs/spec/material.md): all weight on
    // stored byte 0, which a B8G8R8A8 texture returns in .b.
    float lmWeights[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    bool fullbright = false;

    void init(SDL_GPUDevice* device, SDL_GPUTextureFormat colorFormat) {
        dev = device;
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
            pi.rasterizer_state.enable_depth_bias = false;
            pi.rasterizer_state.depth_bias_slope_factor = 0;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        }
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
              const std::vector<DrawItem>& items = {}, const std::vector<Quad2D>& overlay = {}) {
        ensureDepth(w, h);
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
            Uploader up(dev);
            up.buffer(overlayVerts, verts.data(), uint32_t(verts.size() * sizeof(GpuVertex)));
        }
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
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
            SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
            for (const Batch& b : scene.batches) {
                if (b.pass != p) continue;
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
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
                if (b.polygonOffset || b.cull) bindOffset(p, 0, 0, vu);
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
                        {it.ambient[0], it.ambient[1], it.ambient[2], it.sunVisibility}};
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
        if (nq) {
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
        SDL_EndGPURenderPass(pass);
    }
};

void screenshot(SDL_GPUDevice* dev, Renderer& r, const Scene& scene, const Camera& cam, uint32_t w, uint32_t h, const std::string& out,
                const std::vector<DrawItem>& items, const std::vector<Quad2D>& overlay = {}) {
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
    r.draw(cmd, target, w, h, scene, cam, items, overlay);
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
    int shootMarks = 0; // --shoot N: with --shot, the player fires N bullets along the view (fanned 1° apart) before the shot
    bool audioOn = true;
    float shotViewHeight = 60;
    float shotLean = 0;
    bool shotAds = false;
    bool godMode = false; // --god / F10: the "god" cheat (0x4e9f90 toggles FL_GODMODE on the player)
    bool shotUse = false; // --shot: press F (+activate) after the simulated seconds
    std::string language = "polish";
    bool scripts = true;
    bool spawnAll = false;
    Renderer renderer;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--shot") shot = next();
            else if (a == "--pos") { auto v = parseFloats(next()); pos = Vec3{v.at(0), v.at(1), v.at(2)}; }
            else if (a == "--yaw") yaw = std::stof(next());
            else if (a == "--pitch") pitch = std::stof(next());
            else if (a == "--size") { const std::string s = next(); width = uint32_t(std::stoul(s)); height = uint32_t(std::stoul(s.substr(s.find('x') + 1))); }
            else if (a == "--lm-scale") renderer.lmScale = std::stof(next());
            else if (a == "--walk-seconds") walkSeconds = std::stof(next());
            else if (a == "--sim-seconds") simSeconds = std::stof(next());
            else if (a == "--fire-seconds") fireSeconds = std::stof(next());
            else if (a == "--shoot") shootMarks = std::stoi(next());
            else if (a == "--no-audio") audioOn = false;
            else if (a == "--god") godMode = true;
            else if (a == "--stance") { const std::string st = next(); shotViewHeight = st == "prone" ? 11.0f : st == "crouch" ? 40.0f : 60.0f; }
            else if (a == "--ads") shotAds = true;
            else if (a == "--use") shotUse = true;
            else if (a == "--lean") shotLean = std::stof(next());
            else if (a == "--language") language = next();
            else if (a == "--no-scripts") scripts = false;
            else if (a == "--spawn-all") spawnAll = true;
            else if (a == "--lm-weights") { auto v = parseFloats(next()); for (int k = 0; k < 4; ++k) renderer.lmWeights[k] = v.at(size_t(k)); }
            else args.push_back(a);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
    }
    if (args.size() < 2) {
        std::cerr << "usage: iw2view <main-dir> <map-name> [--shot out.png] [--pos x,y,z] [--yaw d] [--pitch d] [--size WxH]\n";
        return 2;
    }

    try {
        fs::Vfs vfs;
        vfs.mountDirectory(args[0]);

        if (!SDL_Init(SDL_INIT_VIDEO | (shot.empty() && audioOn ? SDL_INIT_AUDIO : 0))) sdlFail("SDL_Init");

        // Sound: aliases for this level, a software mixer and an SDL audio stream that pulls from it.
        audio::SoundAliases aliases;
        aliases.load(vfs, args[1]);
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
        auto makeWorld = [&](const std::string& map, const script::ObjectPtr& carriedGame) {
            auto w = std::make_unique<game::World>(vfs, map);
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
            if (carriedGame)
                for (const auto& [k, v] : carriedGame->fields) w->vm().game()->set(k, v);
            // The bindings from default.cfg, reported to scripts through getkeybinding.
            for (const auto& [key, command] : input.bindings()) w->keyBindings[command].push_back(key);
            w->start();
            return w;
        };
        std::string currentMap = args[1];
        std::unique_ptr<game::World> world;
        if (scripts) {
            world = makeWorld(currentMap, nullptr);
            if (spawnAll) std::cout << "spawned " << world->spawnAllActors() << " actors\n";
        }
        Scene scene = loadScene(dev, vfs, args[1], !world);
        // Objective and other on-screen messages from the level script (client commands obj_update/obj_complete).
        struct ScreenMessage { std::string text; uint64_t untilNs; };
        std::vector<ScreenMessage> screenMessages;
        // Death screen client state (docs/spec/campaign.md "Death screen"): opendeadscreen start (world time,
        // 0xf7966c), time_slow (real ms, 0xf7965c..0xf79668 → frame scale 0x747edc).
        int64_t deadScreenMs = 0;
        uint64_t slowStartNs = 0, slowEndNs = 0;
        float slowFrom = 1, slowTo = 1, slowmo = 1;

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

        // Entity models currently visible in the game world.
        // Client interpolation state: entity origin/angles before the latest server frame and the fraction of the
        // next frame already elapsed (cg.frameInterpolation).
        std::map<int, std::pair<script::Vec3, script::Vec3>> lerpPrev;
        // Anim state before the last server frame: the drawn pose is that state advanced by the interpolation fraction, so poses move
        // smoothly with the lerped origin instead of stepping at 20 Hz (the original advances anim time per client frame, 0x4afb70).
        std::map<int, game::AnimState> prevAnim;
        std::map<int, std::pair<float, float>> prevTurret; // turret pitch/yaw before the last server frame (drawn interpolated)
        float lerpFrac = 1.0f;
        auto gatherItems = [&](const Camera& view) {
            std::vector<DrawItem> items;
            if (!world) return items;
            Uploader up(dev);
            for (const auto& ev : world->takeFxEvents()) {
                if (std::getenv("IW2_MARK_DEBUG")) std::cerr << "fx event " << ev.path << "\n";
                fxSystem.play(efx(ev.path), ev.origin, ev.forward, world->timeMs());
            }
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
                assets::LightGridCoeffs c;
                float sun = 1.0f;
                if (scene.grid && scene.grid->sample({ov[0], ov[1], ov[2] + 4.0f}, c, sun)) {
                    const auto l = assets::LightGrid::evaluate(c, {0, 0, 1});
                    base.ambient[0] = l[0]; base.ambient[1] = l[1]; base.ambient[2] = l[2];
                    base.sunVisibility = sun;
                }
                // Actors and animated script_models (drones: spawn("script_model") + useanimtree + attach head/gun, maps/_drones.gsc) are
                // skinned with the blended pose and carry their attachments.
                if (e->isActor || ((e->anim || !e->attachments.empty()) && !e->turret)) {
                    // Animated: skin the body with the blended pose and attach head/helmet/etc.
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
                    if (world->playerOnTurret() && e->turret->owner == world->player()->obj) {
                        tp = angleDelta(view.pitch, av[0]);
                        ty = angleDelta(view.yaw, av[1]);
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
            // The view model's anim tree (0x4b2c90, 0x4b25e0): the current action animation (idle, fire, reload…) and
            // one aiming animation, both with weight 1. adsUpAnim plays at time = aim fraction while zooming in,
            // adsDownAnim at time = 1 − aim fraction otherwise; both are children of the root, so the engine's calc
            // adds them and divides by the weight sum (docs/spec/xanim.md, "Blending").
            std::vector<assets::PoseLayer> layers;
            if (an) layers.push_back({an, frame, 1.0f});
            {
                const bool up = pweapon.adsZoomingIn() && pweapon.adsFrac() > 0;
                const float t = up ? pweapon.adsFrac() : 1.0f - pweapon.adsFrac();
                if (const assets::XAnim* ads = viewAnim(vfs, pweapon.field(up ? "adsUpAnim" : "adsDownAnim")))
                    layers.push_back({ads, t * float(std::max(0, ads->numFrames - 1)), 1.0f});
            }
            if (std::getenv("IW2_VM_NOADS") && layers.size() > 1) layers.pop_back();
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
            Uploader up(dev);
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
        bool mantleHint = false;
        float hudAimSpread = 255.0f, hudViewHeight = 60.0f; // ps+0x5b0 and the view height for the crosshair
        // Damage direction icons (cgame 0x4ba2a0 → 8 slots at 0xf79704: start, duration, yaw).
        struct DamageIcon { int64_t start = -1000000; int duration = 0; float yaw = 0; };
        std::array<DamageIcon, 8> damageIcons{};
        int lastDamageEvent = 0;
        // Menu "weaponinfo" (hud.menu:184): the ammo counter backdrop `ammocounterback` (ownerdraw CG_PLAYER_AMMO_BACKDROP 6), rect
        // -100 -50 100 50 from the bottom right of the 4:3 area. TODO(spec): the ownerdraw 6 handler's fade/hidden conditions.
        const ui::Item* ammoBackItem = nullptr;
        const ui::Item* ammoTextItem = nullptr; // item "ammotex" (ownerdraw CG_PLAYER_AMMO_VALUE 5), rect -85 -15 70 40
        if (const ui::Menu* m = hudMenus.find("weaponinfo"))
            for (const auto& it : m->items) {
                if (it->name == "ammocounterback") ammoBackItem = it.get();
                else if (it->name == "ammotex") ammoTextItem = it.get();
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
                    if (text.empty()) continue;
                    const std::string fontName = o->get("font").toString();
                    const assets::Font* f = font(fontName == "bigfixed" || fontName == "big" ? "bigfont" : "normalfont");
                    if (f) hudText(dst, text, px, py, field(o, "fontscale", 1) * sc * 0.75f, *f, rgba, fx, fy);
                }
            }
            {
                // Subtitles (cgame 0x4bf2e0): x 120, y 400, char height 12 on the 640x480 screen, wrapped at 80 characters.
                // TODO(spec): font, colour and shadow (0x4068e0), the wrap algorithm (0x403fb0), cg_subtitles off.
                const uint64_t now = SDL_GetTicksNS();
                std::erase_if(subtitles, [&](const Subtitle& st) { return st.untilNs < now; });
                const uint8_t white[4] = {255, 255, 255, 255};
                if (const assets::Font* f = font("normalfont")) {
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
                float my = 100 * sc;
                const uint8_t white[4] = {255, 255, 255, 255};
                if (const assets::Font* f = font("normalfont"))
                    for (const auto& m : screenMessages) {
                        const std::string* t = loc.find(m.text);
                        hudText(out, t ? *t : m.text, float(w) * 0.5f, my, 0.75f * sc, *f, white, 0.5f, 0);
                        my += 18 * sc;
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
            // centred (0x4bcef0). In a vehicle the vehicle's (0x4bd000, not drawn yet). Otherwise the hand weapon's
            // with alpha (1 − scope overlay alpha) · cg_crosshairAlpha (0x4bce70) when ≥ 0.01, not at full aim
            // (cg_drawGun 1), and not in weapon states 1, 2, 5, 10, 11 (0x4bd110, cg_drawCrosshair 1).
            // TODO(spec): the friend / enemy colours (ps flags 0x100000 / 0x200000), the reticleCenter +
            // four reticleSide drawing with the spread (0x4bd2c0, 0x4bd520) and the sway offset (0x4bc6e0).
            const bool onTurretHud = world && world->playerOnTurret();
            const bool inVehicleHud = world && (world->playerPmFlags() & 0x20000) && !(world->playerPmFlags() & 0x80000);
            if (onTurretHud) {
                const std::string tw = world->playerTurretWeapon();
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
            // Ammo counter. Menu "weaponinfo" has rect 0 0 0 0 HORIZONTAL_ALIGN_RIGHT VERTICAL_ALIGN_BOTTOM and its items
            // give no alignment of their own, so they inherit RIGHT/BOTTOM: the negative rects are offsets from the
            // viewable right/bottom edge (exe 0x440e60: RIGHT = x*sx + 0x79397c, the right edge of the window, set up
            // by 0x440a40 from the real resolution; BOTTOM likewise), i.e. NOT from the 4:3 box on a wide screen.
            // The ammo ownerdraws (0x4a7818 …) special-case only vehicles (pm_flags 0x20000), not turrets, so the hand
            // weapon's ammo stays on a turret. Text rule (handler 0x4a7930, both clip and stock valid): clip "%2i"
            // left at rect.x, stock "%3i" right-aligned at rect.x + rect.w, "|" centred in the rect minus 5 (0x591ef0);
            // baseline at rect.y (4c3790 gets the item rect y unchanged). TODO(spec): low-ammo red (0.89 0.18 0.01,
            // 0x4a76b0: stock < clip size * 0x592020), the hud fade alpha (0x4a7600), the firemode icon (ownerdraw 83).
            if (pweapon.equipped()) {
                const float rightEdge = float(w), bottomEdge = float(h); // RIGHT / BOTTOM alignment, safe area 0
                if (ammoBackItem && !ammoBackItem->background.empty()) {
                    const auto& r = ammoBackItem->rect;
                    Quad2D q{rightEdge + r.x * sc, bottomEdge + r.y * sc, 0, 0};
                    q.x1 = q.x0 + r.w * sc;
                    q.y1 = q.y0 + r.h * sc;
                    for (int k = 0; k < 4; ++k) q.rgba[k] = uint8_t(std::clamp(ammoBackItem->foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
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
                        hudText(out, clip, x0, y0, gs, *f, col, 0.0f, 1.0f, st);
                        hudText(out, "|", x0 + (w0 - width("|")) * 0.5f - 5.0f * sc, y0, gs, *f, col, 0.0f, 1.0f, st);
                        hudText(out, stock, x0 + w0, y0, gs, *f, col, 1.0f, 1.0f, st);
                    }
                }
            }
            // Compass (ownerdraw 85/84/86, exe 4a91f0 / 4a90b0 / 4a9310). LEFT/BOTTOM alignment is relative to the window
            // edges (0x440e60), like the ammo counter. TODO(spec): the fade alpha (hud_fade_compass 8 s after a touch event,
            // 4a6130 case 2; alpha 1 for now), the damped angle spring (4b09a0), objective ring pulses (no ring start time is
            // tracked), the default pointer material when an objective has no icon, ownerdraw 87/88.
            if (compassMenu && world) {
                const float csize = 1.0f; // cg_hudCompassSize
                const float cyaw = c.yaw, north = world->northYaw();
                auto wrap360 = [](float a) { a = std::fmod(a, 360.0f); return a < 0 ? a + 360.0f : a; };
                const auto& mr = compassMenu->rect;
                for (const auto& itp : compassMenu->items) {
                    const ui::Item& it = *itp;
                    if (it.ownerDraw != 84 && it.ownerDraw != 85 && it.ownerDraw != 86) continue;
                    const float rx = mr.x + it.rect.x, ry = mr.y + it.rect.y, rw = it.rect.w, rh = it.rect.h;
                    const float bx = rx * sc;                                  // LEFT: window left edge
                    const float by = float(h) + (ry - (csize - 1.0f) * rh) * sc; // BOTTOM: window bottom edge
                    const float qw = rw * csize * sc, qh = rh * csize * sc;
                    uint8_t col[4];
                    for (int k = 0; k < 4; ++k) col[k] = uint8_t(std::clamp(it.foreColor[size_t(k)], 0.0f, 1.0f) * 255.0f);
                    if (it.ownerDraw != 86) {
                        if (it.background.empty()) continue;
                        Quad2D q{bx, by, bx + qw, by + qh};
                        std::memcpy(q.rgba, col, 4);
                        q.texture = materialTexture(it.background);
                        if (it.ownerDraw == 84) q.rotDeg = wrap360(cyaw - north);
                        out.push_back(q);
                        continue;
                    }
                    // Pointers: centre (x + w*size/2, y + h*size/2 - (size-1)*h); K = 43.75 (double @0x591e90); icon 16*size.
                    const float ccx = bx + qw * 0.5f, ccy = by + qh * 0.5f;
                    const float maxR = 1500.0f, minR = 0.0f, minRad = 0.0f, objMax = 2048.0f, minAlpha = 1.0f;
                    const float icon = 16.0f * csize * sc;
                    for (const auto& ob : world->objectives()) {
                        if (ob.state != 4 || ob.icon.empty()) continue;
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
                            q.rgba[3] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
                            q.texture = materialTexture(ob.icon);
                            out.push_back(q);
                        }
                    }
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
                for (int f = 0; f < int(simSeconds * 20); ++f) {
                    if (!world->playerLinked()) world->setPlayerOrigin({cam.pos.x, cam.pos.y, cam.pos.z - 60.0f}, {0, cam.yaw, 0});
                    world->frame();
                    for (const auto& cmd : world->takeClientCommands()) // --shot shows the death screen too
                        if (cmd.name == "opendeadscreen") deadScreenMs = world->timeMs();
                }
                if (world->playerLinked()) { // a script link carries the player: shoot from there (--yaw / --pitch kept)
                    const script::Vec3 po = world->player()->obj->get("origin").asVector();
                    cam.pos = {po[0], po[1], po[2] + 60.0f};
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
                        if (world->playerTurretEye(eye)) cam.pos = {eye[0], eye[1], eye[2] + 60.0f - shotViewHeight};
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
            pweapon.update(fireSeconds, shotIn);
            shotIn.fire = false;
            shotIn.frag = std::getenv("IW2_SHOT_FRAG") != nullptr; // debug: hold +frag for the shot (offhand view model)
            for (int i = 0; i < 100; ++i) {
                pweapon.update(0.02f, shotIn);
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
            if (const char* test = std::getenv("IW2_FX_TEST"); test && world) {
                // Debug: play an effect 150 units ahead and show it after IW2_FX_MS milliseconds.
                const Vec3 at = cam.pos + cam.forward() * 150.0f;
                fxSystem.play(efx(test), {at.x, at.y, at.z}, {0, 0, 1}, world->timeMs() - (std::getenv("IW2_FX_MS") ? std::atoi(std::getenv("IW2_FX_MS")) : 200));
            }
            if (const char* as = std::getenv("IW2_AIM_SPREAD")) hudAimSpread = float(std::atof(as)); // debug: aim spread for the shot
            auto shotItems = gatherItems(cam);
            std::cout << "effect particles: " << fxSystem.count() << "\n";
            if (atExitMarks) std::cout << "marks: " << scene.marks.marks().size() << "\n";
            if (!(world && world->playerOnTurret()) && !(world && scopeAlpha() > 0)) addViewModel(shotItems, cam);
            screenshot(dev, renderer, scene, cam, width, height, shot, shotItems, buildHud(width, height, cam));
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
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
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
                    cam.yaw -= ev.motion.xrel * 0.15f * zoomSens;
                    cam.pitch = std::clamp(cam.pitch + ev.motion.yrel * 0.15f * zoomSens, -89.0f, 89.0f);
                }
                if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
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
            const uint32_t buttons = input.buttons(inVehicle, pweapon.stateNumber() == 0x13 || pweapon.stateNumber() == 0x14);
            const bool reloadPressed = (buttons & game::Button::Reload) && !(prevButtons & game::Button::Reload);
            prevButtons = buttons;
            if (world) {
                game::World::PlayerCommand pc;
                pc.forward = input.forwardMove();
                pc.right = input.rightMove();
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
                if (world->playerTurretEye(eye)) cam.pos = {eye[0], eye[1], eye[2]};
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
                in.forward = float(input.forwardMove()) / 127.0f;
                in.right = float(input.rightMove()) / 127.0f;
                in.jump = (buttons & game::Button::Jump) != 0;
                in.stance = (buttons & game::Button::Prone) ? game::Stance::Prone
                            : (buttons & game::Button::Crouch) ? game::Stance::Crouch : game::Stance::Stand;
                in.leanLeft = (buttons & game::Button::LeanLeft) != 0;
                in.leanRight = (buttons & game::Button::LeanRight) != 0;
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
                const game::CollisionWorld& pcw = world ? world->collision() : *scene.collision;
                while (remaining > 0) {
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
                if (!fellOut) {
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
                }
                // Lean moves the view origin sideways and down (0x4ae770); the view itself does not roll.
                const game::Vec lo = game::leanOffset(cam.yaw, ps.lean);
                cam.pos = cam.pos + Vec3{lo[0], lo[1], lo[2]};
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
                writeWeapon();
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
                for (int i = 0; i < shots; ++i) world->playerFire({cam.pos.x, cam.pos.y, cam.pos.z}, {f.x, f.y, f.z});
                if (shots) std::cout << pweapon.name() << " " << pweapon.clip() << "/" << pweapon.stock() << "  hits " << world->stats.hits << " kills " << world->stats.kills << "\n";
                if (!inVehicle && !onTurret && !world->playerLinked()) world->setPlayerOrigin({ps.origin[0], ps.origin[1], ps.origin[2]}, {0, cam.yaw, 0});
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
                gameClock += std::min(dt, 0.25f) * slowmo;
                while (gameClock >= 0.05f) {
                    // The previous server frame's entity placement, for the client interpolation below.
                    lerpPrev.clear();
                    lastDamageEvent = 0;
                    for (const auto& e : world->entities()) {
                        const script::Value o = e->obj->get("origin"), a = e->obj->get("angles");
                        if (o.isVector())
                            lerpPrev[e->number] = {o.asVector(), a.isVector() ? a.asVector() : script::Vec3{0, 0, 0}};
                    }
                    for (const auto& e : world->entities()) {
                        if (e->turret) prevTurret[e->number] = {e->turret->pitch, e->turret->yaw};
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
                    if (cmd.name == "obj_update" || cmd.name == "obj_complete")
                        screenMessages.push_back({cmd.arg, SDL_GetTicksNS() + 4000000000ull}); // display time inferred
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
                    // TODO(spec): scr_blur <ms> <radius> <realtime> <priority> / clear_blur (0x4beb80, 0x4bebf0) set the
                    // full-screen blur radius the renderer applies (0xf79658 → refdef 0xf79188); OpenIW2 has no blur pass.
                }
                if (auto change = world->dueLevelChange()) {
                    // Level change or restart: rebuild the world and the scene for the next map.
                    currentMap = change->map;
                    std::cout << (change->failed ? "mission failed, restarting " : "changing level to ") << currentMap << "\n";
                    const script::ObjectPtr carried = world->vm().game();
                    world.reset();
                    releaseScene(dev, scene);
                    world = makeWorld(currentMap, carried);
                    scene = loadScene(dev, vfs, currentMap, false);
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
                }
            }
            const Camera camBase = cam;
            cam.pitch += recoil.viewKick()[0]; // view kick spring (0x4ae327)
            cam.yaw += recoil.viewKick()[1];
            cam.pitch += bobAngles[0]; // ADS view bob (0x4ae4a9)
            cam.yaw += bobAngles[1];
            fxRight = cam.right();
            fxUp = cross(cam.right(), cam.forward());
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
            if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h) && swap)
                renderer.draw(cmd, swap, w, h, scene, cam, items, buildHud(w, h, cam));
            SDL_SubmitGPUCommandBuffer(cmd);
            cam.pitch = camBase.pitch; // the ADS bob turns only the drawn view
            cam.yaw = camBase.yaw;
        }
        if (audioStream) SDL_DestroyAudioStream(audioStream);
        SDL_ReleaseWindowFromGPUDevice(dev, window);
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(dev);
        SDL_Quit();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
