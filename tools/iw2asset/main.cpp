// iw2asset: decode game assets into open formats.
//
//   iw2asset <main-dir> iwi-info <images/name.iwi>
//   iw2asset <main-dir> iwi-png  <images/name.iwi> <out.png> [--face N]
//   iw2asset <main-dir> iwi-check                    decode every .iwi and report per-format results
//   iw2asset <main-dir> iwi-export <out-dir>         convert every .iwi (mip 0) to PNG
//   iw2asset <main-dir> bsp-info <maps/name.d3dbsp>   lump table and counts
//   iw2asset <main-dir> bsp-obj  <maps/name.d3dbsp> <out.obj>   export draw geometry as Wavefront OBJ
//   iw2asset <main-dir> bsp-topdown <maps/name.d3dbsp> <out.png> [size]   shaded top-down preview
//   iw2asset <main-dir> mat-info <materials/name>    techset, textures, constants
//   iw2asset <main-dir> mat-check                    parse every material
//   iw2asset <main-dir> model-info <xmodel/name>     LODs, materials, bones, surfaces
//   iw2asset <main-dir> model-png <xmodel/name> <out.png> [size]   shaded side view of LOD 0
//   iw2asset <main-dir> model-check                  parse every xmodel with all its LODs
//   iw2asset <main-dir> anim-info <xanim/name>       frames, bones, notetracks
//   iw2asset <main-dir> anim-check                   parse every xanim
//   iw2asset <main-dir> bsp-check                    parse every map
//   iw2asset <main-dir> sound-info <sound/name>      decode a .wav/.mp3: rate, channels, frames
//   iw2asset <main-dir> sound-check                  decode every .wav and .mp3 under sound/

#include "assets/d3dbsp.h"
#include "assets/efx.h"
#include "audio/decode.h"
#include "assets/iwi.h"
#include "assets/material.h"
#include "assets/xanim.h"
#include "assets/xmodel.h"
#include "fs/vfs.h"
#include "util/png.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <map>
#include <string>
#include <vector>

using namespace iw2;

namespace {

struct PreviewTri {
    std::array<float, 3> p[3];
    std::array<float, 3> n[3];
};

// Software-rasterized shaded preview. Top view (x right, y up) or side view (y right, z up).
void renderPreview(const std::vector<PreviewTri>& tris, const std::string& out, int size, bool side) {
    const int ax = side ? 1 : 0, ay = side ? 2 : 1, az = side ? 0 : 2;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& t : tris)
        for (const auto& p : t.p)
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[size_t(a)]); hi[a] = std::max(hi[a], p[size_t(a)]); }
    const float scale = (size - 1) / std::max(1e-3f, std::max(hi[ax] - lo[ax], hi[ay] - lo[ay]));
    const int w = int((hi[ax] - lo[ax]) * scale) + 1, h = int((hi[ay] - lo[ay]) * scale) + 1;
    std::vector<float> depth(size_t(w) * size_t(h), -1e30f);
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    for (const auto& t : tris) {
        float x[3], y[3];
        for (int c = 0; c < 3; ++c) {
            x[c] = (t.p[c][size_t(ax)] - lo[ax]) * scale;
            y[c] = (hi[ay] - t.p[c][size_t(ay)]) * scale;
        }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::abs(area) < 1e-6f) continue;
        const int x0 = std::max(0, int(std::floor(std::min({x[0], x[1], x[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({x[0], x[1], x[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({y[0], y[1], y[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({y[0], y[1], y[2]}))));
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px) {
                const float fx = px + 0.5f, fy = py + 0.5f;
                const float b0 = ((x[1] - fx) * (y[2] - fy) - (x[2] - fx) * (y[1] - fy)) / area;
                const float b1 = ((x[2] - fx) * (y[0] - fy) - (x[0] - fx) * (y[2] - fy)) / area;
                const float b2 = 1 - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                // Side view looks along -x, so nearer means larger x.
                const float z = b0 * t.p[0][size_t(az)] + b1 * t.p[1][size_t(az)] + b2 * t.p[2][size_t(az)];
                const size_t o = size_t(py) * size_t(w) + size_t(px);
                if (z <= depth[o]) continue;
                depth[o] = z;
                float n[3];
                for (int a = 0; a < 3; ++a) n[a] = b0 * t.n[0][size_t(a)] + b1 * t.n[1][size_t(a)] + b2 * t.n[2][size_t(a)];
                const float light = std::clamp(0.35f + 0.65f * (side ? (n[0] * 0.8f + n[1] * 0.2f + n[2] * 0.55f)
                                                                     : (n[0] * -0.4f + n[1] * 0.3f + n[2] * 0.87f)), 0.0f, 1.0f);
                const float hz = (z - lo[az]) / std::max(1.0f, hi[az] - lo[az]);
                rgba[o * 4 + 0] = uint8_t(255 * light * (0.75f + 0.25f * hz));
                rgba[o * 4 + 1] = uint8_t(255 * light * (0.70f + 0.20f * hz));
                rgba[o * 4 + 2] = uint8_t(255 * light * (0.55f + 0.30f * hz));
            }
    }
    util::writePngRgba(out, rgba.data(), w, h);
}

std::vector<uint8_t> mustRead(const fs::Vfs& vfs, const std::string& path) {
    auto data = vfs.read(path);
    if (!data) throw std::runtime_error("not found: " + path);
    return std::move(*data);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    int face = 0;
    std::string animName;
    float animFrame = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--face" && i + 1 < argc) face = std::stoi(argv[++i]);
        else if (a == "--anim" && i + 1 < argc) animName = argv[++i];
        else if (a == "--frame" && i + 1 < argc) animFrame = std::stof(argv[++i]);
        else args.push_back(a);
    }
    if (args.size() < 2) {
        std::cerr << "usage: iw2asset <main-dir> iwi-info <path> | iwi-png <path> <out.png> [--face N] | iwi-check | iwi-export <out-dir>\n";
        return 2;
    }

    try {
        fs::Vfs vfs;
        vfs.mountDirectory(args[0]);
        const std::string& cmd = args[1];

        if (cmd == "iwi-info" && args.size() > 2) {
            const auto data = mustRead(vfs, args[2]);
            const auto img = assets::parseIwi(data);
            const auto& h = img.header;
            std::cout << args[2] << ": " << assets::formatName(h.format) << " " << h.width << "x" << h.height
                      << " flags=0x" << std::hex << int(h.flags) << std::dec << " faces=" << img.faceCount
                      << " mips=" << img.levels.size() << "\n";
        } else if (cmd == "iwi-png" && args.size() > 3) {
            const auto data = mustRead(vfs, args[2]);
            const auto img = assets::parseIwi(data);
            const auto rgba = assets::decodeToRgba(img, 0, face);
            util::writePngRgba(args[3], rgba.data(), img.levels[0].width, img.levels[0].height);
        } else if (cmd == "iwi-check" || (cmd == "iwi-export" && args.size() > 2)) {
            const bool exportPng = cmd == "iwi-export";
            if (exportPng) std::filesystem::create_directories(args[2]);
            std::map<std::string, std::pair<int, int>> stats; // format -> (ok, failed)
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list("images/")) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".iwi") continue;
                std::string fmt = "?";
                try {
                    const auto data = mustRead(vfs, name);
                    const auto img = assets::parseIwi(data);
                    fmt = assets::formatName(img.header.format);
                    for (size_t l = 0; l < img.levels.size(); ++l)
                        for (int f = 0; f < img.faceCount; ++f) {
                            const auto rgba = assets::decodeToRgba(img, int(l), f);
                            if (exportPng && l == 0) {
                                std::string out = name.substr(7, name.size() - 11);
                                if (img.faceCount > 1) out += "_face" + std::to_string(f);
                                util::writePngRgba(std::filesystem::path(args[2]) / (out + ".png"), rgba.data(),
                                                   img.levels[0].width, img.levels[0].height);
                            }
                        }
                    ++stats[fmt].first;
                    ++ok;
                } catch (const std::exception& e) {
                    ++stats[fmt].second;
                    ++failed;
                    std::cerr << name << ": " << e.what() << "\n";
                }
            }
            for (const auto& [fmt, s] : stats) std::cout << fmt << ": " << s.first << " ok, " << s.second << " failed\n";
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "bsp-info" && args.size() > 2) {
            const auto data = mustRead(vfs, args[2]);
            const assets::D3dBsp bsp(data);
            for (int i = 0; i < assets::BspLump::Count; ++i)
                std::cout << "lump " << i << ": " << bsp.lumps()[size_t(i)].length << " bytes\n";
            std::cout << "materials " << bsp.materials().size() << ", soups " << bsp.soups().size() << ", vertices "
                      << bsp.vertices().size() << ", indices " << bsp.indices().size() << ", lightmaps "
                      << bsp.lightmapCount() << ", entities " << bsp.entities().size() << "\n";
        } else if (cmd == "bsp-obj" && args.size() > 3) {
            const auto data = mustRead(vfs, args[2]);
            const assets::D3dBsp bsp(data);
            std::ofstream obj(args[3]);
            obj << "# exported by iw2asset from " << args[2] << "\n";
            for (const auto& v : bsp.vertices())
                obj << "v " << v.position[0] << " " << v.position[2] << " " << -v.position[1] << "\n";
            for (const auto& v : bsp.vertices()) obj << "vt " << v.uv[0] << " " << 1.0f - v.uv[1] << "\n";
            for (const auto& v : bsp.vertices())
                obj << "vn " << v.normal[0] << " " << v.normal[2] << " " << -v.normal[1] << "\n";
            for (const auto& s : bsp.soups()) {
                obj << "usemtl " << bsp.materials()[s.material].name << "\n";
                for (uint32_t k = 0; k + 2 < s.indexCount; k += 3) {
                    obj << "f";
                    // OBJ is 1-based; reverse winding to keep faces outward after the axis swap.
                    for (int c : {2, 1, 0}) {
                        const uint32_t vi = s.firstVertex + bsp.indices()[s.firstIndex + k + uint32_t(c)] + 1;
                        obj << " " << vi << "/" << vi << "/" << vi;
                    }
                    obj << "\n";
                }
            }
        } else if (cmd == "bsp-topdown" && args.size() > 3) {
            const auto data = mustRead(vfs, args[2]);
            const assets::D3dBsp bsp(data);
            std::vector<PreviewTri> tris;
            for (const auto& s : bsp.soups()) {
                if (bsp.materials()[s.material].name.find("sky") != std::string::npos) continue;
                for (uint32_t k = 0; k + 2 < s.indexCount; k += 3) {
                    PreviewTri t;
                    for (int c = 0; c < 3; ++c) {
                        const auto& v = bsp.vertices()[s.firstVertex + bsp.indices()[s.firstIndex + k + uint32_t(c)]];
                        t.p[c] = v.position;
                        t.n[c] = v.normal;
                    }
                    tris.push_back(t);
                }
            }
            renderPreview(tris, args[3], args.size() > 4 ? std::stoi(args[4]) : 1024, false);
        } else if (cmd == "model-png" && args.size() > 3) {
            const auto m = assets::parseXModel(mustRead(vfs, args[2]));
            const auto parts = assets::parseXModelParts(mustRead(vfs, "xmodelparts/" + m.lods.at(0).name));
            const auto surfs = assets::parseXModelSurfs(mustRead(vfs, "xmodelsurfs/" + m.lods.at(0).name), parts);
            assets::XPose pose = assets::bindPose(parts);
            if (!animName.empty()) {
                const auto anim = assets::parseXAnim(mustRead(vfs, "xanim/" + animName));
                pose = assets::animatePose(parts, anim, animFrame);
            }
            std::vector<PreviewTri> tris;
            for (const auto& s : surfs)
                for (size_t k = 0; k + 2 < s.indices.size(); k += 3) {
                    PreviewTri t;
                    for (int c = 0; c < 3; ++c) assets::skinVertex(s.vertices[s.indices[k + size_t(c)]], pose, t.p[c], t.n[c]);
                    tris.push_back(t);
                }
            renderPreview(tris, args[3], args.size() > 4 ? std::stoi(args[4]) : 768, true);
        } else if (cmd == "mat-info" && args.size() > 2) {
            const auto data = mustRead(vfs, args[2]);
            const auto m = assets::parseMaterial(data);
            std::cout << m.name << "\n  techset " << m.techniqueSet << "\n";
            for (const auto& t : m.textures)
                std::cout << "  texture " << t.samplerName << " (semantic " << int(t.semantic) << ") = " << t.imageName << "\n";
            for (const auto& c : m.constants)
                std::cout << "  constant " << c.name << " = " << c.value[0] << " " << c.value[1] << " " << c.value[2] << " " << c.value[3] << "\n";
        } else if (cmd == "mat-check") {
            int ok = 0, failed = 0, missingImages = 0;
            std::map<std::string, int> techsets;
            for (const auto& name : vfs.list("materials/")) {
                if (name.find('.') != std::string::npos || name.find('/', 10) != std::string::npos) continue;
                try {
                    const auto data = mustRead(vfs, name);
                    const auto m = assets::parseMaterial(data);
                    ++techsets[m.techniqueSet];
                    for (const auto& t : m.textures)
                        if (!t.imageName.empty() && t.imageName[0] != '$' && !vfs.exists("images/" + t.imageName + ".iwi")) ++missingImages;
                    ++ok;
                } catch (const std::exception& e) {
                    std::cerr << name << ": " << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "techsets used: " << techsets.size() << ", missing image references: " << missingImages << "\n";
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "model-info" && args.size() > 2) {
            const auto data = mustRead(vfs, args[2]);
            const auto m = assets::parseXModel(data);
            std::cout << args[2] << " flags=" << int(m.flags) << " bounds (" << m.mins[0] << " " << m.mins[1] << " " << m.mins[2]
                      << ") - (" << m.maxs[0] << " " << m.maxs[1] << " " << m.maxs[2] << ")\n";
            const auto parts = assets::parseXModelParts(mustRead(vfs, "xmodelparts/" + m.lods.at(0).name));
            for (const auto& lod : m.lods) {
                const auto surfs = assets::parseXModelSurfs(mustRead(vfs, "xmodelsurfs/" + lod.name), parts);
                size_t verts = 0, tris = 0;
                for (const auto& s : surfs) { verts += s.vertices.size(); tris += s.indices.size() / 3; }
                std::cout << "  lod " << lod.name << " dist " << lod.distance << ": " << parts.bones.size() << " bones, "
                          << surfs.size() << " surfaces, " << verts << " verts, " << tris << " tris\n";
                for (size_t i = 0; i < lod.materials.size(); ++i) std::cout << "    material " << lod.materials[i] << "\n";
            }
        } else if (cmd == "model-check") {
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list("xmodel/")) {
                try {
                    const auto m = assets::parseXModel(mustRead(vfs, name));
                    const auto parts = assets::parseXModelParts(mustRead(vfs, "xmodelparts/" + m.lods.at(0).name));
                    for (const auto& lod : m.lods) {
                        const auto surfs = assets::parseXModelSurfs(mustRead(vfs, "xmodelsurfs/" + lod.name), parts);
                        if (surfs.size() != lod.materials.size()) throw std::runtime_error("surface/material count mismatch in " + lod.name);
                    }
                    ++ok;
                } catch (const std::exception& e) {
                    if (failed < 15) std::cerr << name << ": " << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "anim-info" && args.size() > 2) {
            const auto a = assets::parseXAnim(mustRead(vfs, args[2]));
            std::cout << args[2] << ": " << a.numFrames << " frames @ " << a.frameRate << " fps (" << a.length() << " s), "
                      << a.bones.size() << " bones" << (a.looping ? ", looping" : "") << (a.delta ? ", delta" : "") << "\n";
            for (const auto& b : a.bones)
                std::cout << "  " << b.name << ": " << b.rot.keys.size() << " rot keys, " << b.trans.keys.size() << " trans keys\n";
            for (const auto& n : a.notes) std::cout << "  note " << n.name << " @ " << n.time << "\n";
        } else if (cmd == "anim-check") {
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list("xanim/")) {
                try {
                    assets::parseXAnim(mustRead(vfs, name));
                    ++ok;
                } catch (const std::exception& e) {
                    if (failed < 15) std::cerr << name << ": " << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "bsp-check") {
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list("maps/")) {
                if (name.size() < 7 || name.substr(name.size() - 7) != ".d3dbsp") continue;
                try {
                    const auto data = mustRead(vfs, name);
                    const assets::D3dBsp bsp(data);
                    std::cout << name << ": " << bsp.soups().size() << " soups, " << bsp.vertices().size()
                              << " vertices, " << bsp.lightmapCount() << " lightmaps, " << bsp.entities().size()
                              << " entities\n";
                    ++ok;
                } catch (const std::exception& e) {
                    std::cerr << name << ": " << e.what() << "\n";
                    ++failed;
                }
            }
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else if (cmd == "sound-info" && args.size() > 2) {
            const auto w = audio::decodeSound(mustRead(vfs, args[2]), args[2]);
            std::cout << args[2] << ": " << w.rate << " Hz, " << w.channels << " channel(s), " << w.frames()
                      << " frames (" << double(w.frames()) / w.rate << " s)\n";
        } else if (cmd == "efx-check") {
            int ok = 0, bad = 0, elements = 0;
            for (const auto& path : vfs.list("fx/")) {
                if (path.size() < 4 || path.substr(path.size() - 4) != ".efx") continue;
                const auto d = vfs.read(path);
                try {
                    elements += int(assets::parseEfx(std::string_view(reinterpret_cast<const char*>(d->data()), d->size())).elements.size());
                    ++ok;
                } catch (const std::exception& e) {
                    ++bad;
                    std::cout << path << ": " << e.what() << "\n";
                }
            }
            std::cout << "efx: " << ok << " ok, " << bad << " failed, " << elements << " elements\n";
        } else if (cmd == "sound-check") {
            std::map<std::string, int> ok;
            int failed = 0;
            for (const auto& name : vfs.list("sound/")) {
                const auto dot = name.rfind('.');
                if (dot == std::string::npos) continue;
                std::string ext = name.substr(dot);
                for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ext != ".wav" && ext != ".mp3") continue;
                try {
                    const auto w = audio::decodeSound(mustRead(vfs, name), name);
                    if (w.frames() == 0) throw std::runtime_error("no samples");
                    ++ok[ext];
                } catch (const std::exception& e) {
                    if (failed < 15) std::cerr << name << ": " << e.what() << "\n";
                    ++failed;
                }
            }
            for (const auto& [ext, n] : ok) std::cout << ext << ": " << n << " ok\n";
            std::cout << "failed: " << failed << "\n";
            return failed ? 1 : 0;
        } else {
            std::cerr << "unknown command: " << cmd << "\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
