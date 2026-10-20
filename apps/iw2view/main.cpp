// iw2view: fly through a single-player map with its textures and lightmaps (SDL3 GPU / Metal).
//
//   iw2view <main-dir> <map-name> [--shot out.png] [--pos x,y,z] [--yaw deg] [--pitch deg]
//           [--size WxH] [--lm-scale s] [--lm-weights a,b,c,d]
//
// Controls: mouse to look, WASD to move, Space jump (walk) / up (fly), C down (fly), Shift faster,
// V toggles walk/fly, F fullbright, P prints the camera position, Esc quits.

#include "assets/d3dbsp.h"
#include "assets/iwi.h"
#include "assets/lightgrid.h"
#include "assets/material.h"
#include "assets/xmodel.h"
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/pmove.h"
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

    Vec3 forward() const {
        const float cy = std::cos(yaw * float(M_PI) / 180), sy = std::sin(yaw * float(M_PI) / 180);
        const float cp = std::cos(pitch * float(M_PI) / 180), sp = std::sin(pitch * float(M_PI) / 180);
        return {cy * cp, sy * cp, -sp};
    }
    Vec3 right() const { return normalize(cross(forward(), {0, 0, 1})); }

    Mat4 viewProj(float aspect) const {
        const Vec3 f = forward(), r = right(), u = cross(r, f);
        const Mat4 view = {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0, r.z, u.z, -f.z, 0,
                           -dot(r, pos), -dot(u, pos), dot(f, pos), 1};
        const float fovY = 65.0f * float(M_PI) / 180, n = 4.0f, fa = 40000.0f;
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

struct GpuVertex {
    float pos[3];
    float normal[3];
    uint8_t color[4];
    float uv[2];
    float lmUv[2];
    float light[3];
};
static_assert(sizeof(GpuVertex) == 56);

enum class Pass { Opaque, AlphaTest, Blend, Additive, Sky, Skip };

struct Batch {
    Pass pass = Pass::Opaque;
    int texture = 0;   // index into Scene::textures
    int lightmap = -1; // -1 = none, -2 = per-vertex light from the light grid
    bool fullbright = false;
    uint32_t firstIndex = 0, indexCount = 0;
};

struct Lightmap {
    SDL_GPUTexture* coeffs = nullptr; // 3-layer array (R, G, B coefficients)
    SDL_GPUTexture* sun = nullptr;
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
    Vec3 sunDirection{0, 0, 1};
    Vec3 sunColor{1, 1, 1};
    Camera start;
};

Pass classify(const std::string& techset) {
    if (techset == "sky") return Pass::Sky;
    if (techset == "tools" || techset.rfind("tools", 0) == 0) return Pass::Skip;
    if (techset.find("_add") != std::string::npos) return Pass::Additive;
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
        if (img.faceCount != 1 || assets::isWavelet(img.header.format)) return nullptr;
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

Scene loadScene(SDL_GPUDevice* dev, const fs::Vfs& vfs, const std::string& mapName) {
    Scene scene;
    const std::string path = "maps/" + mapName + ".d3dbsp";
    const auto data = vfs.read(path);
    if (!data) throw std::runtime_error("map not found: " + path);
    const assets::D3dBsp bsp(*data);
    scene.collision = std::make_unique<game::CollisionWorld>(bsp);
    Uploader up(dev);

    // White fallback texture.
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        SDL_GPUTexture* t = createTexture(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, 1);
        up.texture(t, 0, 0, 1, 1, white, 4);
        scene.textures.push_back(t);
    }

    // Materials -> pass + color texture (cached by material name).
    struct MatInfo { Pass pass; int texture; };
    std::unordered_map<std::string, int> imageIndex;
    std::unordered_map<std::string, MatInfo> matCache;
    auto material = [&](const std::string& name) -> MatInfo {
        if (auto it = matCache.find(name); it != matCache.end()) return it->second;
        MatInfo mi{Pass::Opaque, 0};
        if (const auto md = vfs.read("materials/" + name)) {
            try {
                const auto m = assets::parseMaterial(*md);
                mi.pass = classify(m.techniqueSet);
                const std::string& img = m.colorImage();
                if (mi.pass == Pass::Sky) {
                    if (!scene.sky) scene.sky = loadCubeImage(dev, up, vfs, img);
                } else if (!img.empty() && img[0] != '$' && mi.pass != Pass::Skip) {
                    auto it = imageIndex.find(img);
                    if (it == imageIndex.end()) {
                        SDL_GPUTexture* t = loadImage(dev, up, vfs, img);
                        int idx = 0;
                        if (t) {
                            idx = int(scene.textures.size());
                            scene.textures.push_back(t);
                        }
                        it = imageIndex.emplace(img, idx).first;
                    }
                    mi.texture = it->second;
                }
            } catch (const std::exception& e) {
                std::cerr << "warning: material " << name << ": " << e.what() << "\n";
            }
        }
        matCache.emplace(name, mi);
        return mi;
    };
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
    std::map<std::tuple<int, int, int>, std::vector<uint32_t>> groups;
    for (const auto& s : bsp.soups()) {
        const MatInfo& mi = mats[s.material];
        if (mi.pass == Pass::Skip) continue;
        const int lm = s.lightmap < scene.lightmaps.size() ? int(s.lightmap) : -1;
        auto& dst = groups[{int(mi.pass), mi.texture, lm}];
        for (uint32_t k = 0; k < s.indexCount; ++k) dst.push_back(s.firstVertex + bsp.indices()[s.firstIndex + k]);
    }
    // Sun from worldspawn (needed for baking model light).
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        if (!cls || *cls != "worldspawn") continue;
        float light = 1.0f;
        if (auto sl = e.get("sunlight")) light = std::stof(*sl);
        if (auto c = e.get("suncolor")) scene.sunColor = parseVec3(*c) * light;
        if (auto d = e.get("sundirection")) {
            const Vec3 a = parseVec3(*d);
            Camera c;
            c.pitch = a.x;
            c.yaw = a.y;
            scene.sunDirection = c.forward();
        }
    }
    const assets::LightGrid grid(bsp.lump(2), bsp.lump(3));

    // Static models placed by entities (world-space copies, lit per vertex from the light grid).
    struct ModelData { std::vector<assets::XSurface> surfaces; std::vector<MatInfo> materials; };
    std::unordered_map<std::string, std::optional<ModelData>> modelCache;
    size_t modelCount = 0;
    for (const auto& e : bsp.entities()) {
        const std::string* cls = e.get("classname");
        const std::string* model = e.get("model");
        if (!cls || !model || (*cls != "misc_model" && *cls != "script_model")) continue;
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
            auto& dst = groups[{int(mi.pass), mi.texture, -2}];
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
            float light = 1.0f;
            if (auto s = e.get("sunlight")) light = std::stof(*s);
            if (auto c = e.get("suncolor")) scene.sunColor = parseVec3(*c) * light;
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
};

struct Renderer {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUGraphicsPipeline* pipelines[4] = {}; // Opaque, AlphaTest, Blend, Additive
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

        for (int p = 0; p < 4; ++p) {
            SDL_GPUColorTargetDescription ct{};
            ct.format = colorFormat;
            if (p >= 2) {
                ct.blend_state.enable_blend = true;
                ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.src_color_blendfactor = p == 2 ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.dst_color_blendfactor = p == 2 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
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

    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam) {
        ensureDepth(w, h);
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

        struct { Mat4 vp; float eye[4]; } vu{cam.viewProj(float(w) / float(h)), {cam.pos.x, cam.pos.y, cam.pos.z, 1}};
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
        for (Pass p : {Pass::Opaque, Pass::AlphaTest, Pass::Blend, Pass::Additive}) {
            SDL_BindGPUGraphicsPipeline(pass, pipelines[int(p)]);
            for (const Batch& b : scene.batches) {
                if (b.pass != p) continue;
                const Lightmap& lm = b.lightmap >= 0 ? scene.lightmaps[size_t(b.lightmap)] : scene.dummyLightmap;
                SDL_GPUTextureSamplerBinding tex[3] = {
                    {scene.textures[size_t(b.texture)], repeatSampler}, {lm.coeffs, clampSampler}, {lm.sun, clampSampler}};
                SDL_BindGPUFragmentSamplers(pass, 0, tex, 3);
                FragmentUniforms fu = {
                    {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z, 0},
                    {scene.sunColor.x, scene.sunColor.y, scene.sunColor.z, 0},
                    {lmWeights[0], lmWeights[1], lmWeights[2], lmWeights[3]},
                    {p == Pass::AlphaTest ? 1.0f : 0.0f, b.lightmap >= 0 ? 1.0f : (b.lightmap == -2 ? 2.0f : 0.0f), lmScale, fullbright ? 1.0f : 0.0f}};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
            }
        }
        SDL_EndGPURenderPass(pass);
    }
};

void screenshot(SDL_GPUDevice* dev, Renderer& r, const Scene& scene, const Camera& cam, uint32_t w, uint32_t h, const std::string& out) {
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
    r.draw(cmd, target, w, h, scene, cam);
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

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::string shot;
    std::optional<Vec3> pos;
    std::optional<float> yaw, pitch;
    uint32_t width = 1600, height = 900;
    float walkSeconds = 0;
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

        if (!SDL_Init(SDL_INIT_VIDEO)) sdlFail("SDL_Init");
        SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
        if (!dev) sdlFail("SDL_CreateGPUDevice");

        Scene scene = loadScene(dev, vfs, args[1]);
        Camera cam = scene.start;
        if (pos) cam.pos = *pos;
        if (yaw) cam.yaw = *yaw;
        if (pitch) cam.pitch = *pitch;

        if (!shot.empty()) {
            if (walkSeconds > 0) {
                // Drop the player at the camera position and walk forward, then take the shot from the eyes.
                const game::MoveParams mp;
                game::PlayerState ps;
                ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - mp.eyeHeight + 8.0f};
                game::PlayerInput in;
                in.yaw = cam.yaw;
                for (int i = 0; i < 20; ++i) game::playerMove(*scene.collision, ps, in, 0.05f, mp);
                in.forward = 1;
                for (int i = 0; i < int(walkSeconds * 60); ++i) game::playerMove(*scene.collision, ps, in, 1.0f / 60.0f, mp);
                cam.pos = {ps.origin[0], ps.origin[1], ps.origin[2] + mp.eyeHeight};
                std::cout << "walked to " << cam.pos.x << ", " << cam.pos.y << ", " << cam.pos.z << "\n";
            }
            renderer.init(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
            screenshot(dev, renderer, scene, cam, width, height, shot);
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
        game::PlayerState ps;
        ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - 60.0f + 8.0f};
        const game::MoveParams mp;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) running = false;
                else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_F) renderer.fullbright = !renderer.fullbright;
                else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_V) {
                    walk = !walk;
                    if (walk) { ps.origin = {cam.pos.x, cam.pos.y, cam.pos.z - mp.eyeHeight}; ps.velocity = {0, 0, 0}; }
                }
                else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_P)
                    std::cout << "--pos " << cam.pos.x << "," << cam.pos.y << "," << cam.pos.z << " --yaw " << cam.yaw << " --pitch " << cam.pitch << "\n";
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    cam.yaw -= ev.motion.xrel * 0.15f;
                    cam.pitch = std::clamp(cam.pitch + ev.motion.yrel * 0.15f, -89.0f, 89.0f);
                }
            }
            const uint64_t now = SDL_GetTicksNS();
            const float dt = float(now - last) * 1e-9f;
            last = now;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            if (walk) {
                game::PlayerInput in;
                in.forward = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
                in.right = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
                in.jump = keys[SDL_SCANCODE_SPACE];
                in.sprint = keys[SDL_SCANCODE_LSHIFT];
                in.yaw = cam.yaw;
                // Fixed 20 Hz-style substeps keep movement stable at any frame rate.
                float remaining = std::min(dt, 0.1f);
                while (remaining > 0) {
                    const float step = std::min(remaining, 1.0f / 60.0f);
                    game::playerMove(*scene.collision, ps, in, step, mp);
                    remaining -= step;
                }
                cam.pos = {ps.origin[0], ps.origin[1], ps.origin[2] + mp.eyeHeight};
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

            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUTexture* swap = nullptr;
            uint32_t w = 0, h = 0;
            if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h) && swap)
                renderer.draw(cmd, swap, w, h, scene, cam);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
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
