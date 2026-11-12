// Tests for xmodel math and a minimal synthetic model.

#include "assets/material.h"
#include "assets/xmodel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

template <typename T> void put(std::vector<uint8_t>& b, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}
void putStr(std::vector<uint8_t>& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }
bool near(float a, float b) { return std::abs(a - b) < 1e-3f; }

} // namespace

int main() {
    {
        // Flag technique: the vertex alpha (wave weight, 0 on the pole and the base) does not hide the vertex.
        using iw2::assets::drawnVertexColor;
        CHECK(iw2::assets::isFlagTechniqueSet("phong_flag_alphatest_spec"));
        CHECK(!iw2::assets::isFlagTechniqueSet("phong_alphatest_spec"));
        const std::array<uint8_t, 4> pole{10, 20, 30, 0};
        CHECK((drawnVertexColor(true, pole) == std::array<uint8_t, 4>{10, 30, 20, 255}));
        CHECK((drawnVertexColor(false, pole) == pole));
    }
    {
        // Flag cloth wave (docs/spec/material.md "Flag techniques").
        using iw2::assets::flagWave;
        auto close = [](float a, float b, float eps) { return std::abs(a - b) < eps; };
        // Weight 0 (pole, base) never moves or tilts.
        const auto rigid = flagWave(0.3f, 0.7f, 0.0f, 3.4f);
        CHECK(rigid.offset == 0.0f && rigid.tiltU == 0.0f && rigid.tiltV == 0.0f);
        // Hand-computed: at u = v = 1 the phases are 2 pi speed t; t = 0.25 s, weight 1.
        CHECK(close(flagWave(1, 1, 1, 0.25f).offset, 11.1458f, 1e-3f));
        // At t = 0 the slopes are the summed amplitude * cos(angle) / length (and sin), through atan.
        const auto t0 = flagWave(1, 1, 1, 0);
        CHECK(close(t0.offset, 0, 1e-5f) && close(t0.tiltU, 1.21756f, 1e-4f) && close(t0.tiltV, 0.202611f, 1e-4f));
        // The weight scales the offset and the tilt linearly.
        CHECK(close(flagWave(0.2f, 0.4f, 0.5f, 1.3f).offset, 0.5f * flagWave(0.2f, 0.4f, 1, 1.3f).offset, 1e-4f));
        // Bounded by 5 * (1.5 + 0.6 + 0.34) and repeating every 10 s.
        float maxOff = 0;
        for (int i = 0; i < 2000; ++i) {
            const float u = float(i % 37) / 36.0f, v = float(i % 11) / 10.0f, t = float(i) * 0.005f;
            const auto a = flagWave(u, v, 1, t), b = flagWave(u, v, 1, t + 10.0f);
            maxOff = std::max(maxOff, std::abs(a.offset));
            CHECK(close(a.offset, b.offset, 2e-3f) && close(a.tiltU, b.tiltU, 1e-3f));
        }
        CHECK(maxOff <= iw2::assets::kFlagWaveMaxOffset && maxOff > 5.0f);
        CHECK(close(iw2::assets::kFlagWaveMaxOffset, 12.2f, 1e-5f));
        // The cloth moves between two drawn frames 16 ms apart (render time, not 20 Hz world steps).
        CHECK(std::abs(flagWave(0.5f, 0.5f, 1, 1.00f).offset - flagWave(0.5f, 0.5f, 1, 1.016f).offset) > 1e-3f);

        // gameTime code constant lanes: x sin, y cos of 2 pi frac(t), z frac(t), w t (wrapped every 10 s).
        using iw2::assets::gameTimeConst;
        const auto g = gameTimeConst(12250);
        CHECK(close(g.w, 2.25f, 1e-6f) && close(g.z, 0.25f, 1e-6f) && close(g.x, 1, 1e-5f) && close(g.y, 0, 1e-5f));
        const auto n = gameTimeConst(-750); // before the epoch: still in [0, 10)
        CHECK(close(n.w, 9.25f, 1e-6f) && close(n.z, 0.25f, 1e-6f));
        const auto z = gameTimeConst(0);
        CHECK(z.w == 0 && z.z == 0 && close(z.y, 1, 1e-6f));
    }
    using namespace iw2::assets;

    // 90 degree rotation about Z maps +X to +Y.
    const float s = std::sqrt(0.5f);
    const auto r = rotate(Quat{0, 0, s, s}, {1, 0, 0});
    CHECK(near(r[0], 0) && near(r[1], 1) && near(r[2], 0));

    // Skeleton: one root, one child at (10,0,0) rotated 90 degrees about Z.
    std::vector<uint8_t> parts;
    put<int16_t>(parts, 20); put<int16_t>(parts, 1); put<int16_t>(parts, 1);
    put<uint8_t>(parts, 0); put<float>(parts, 10); put<float>(parts, 0); put<float>(parts, 0);
    put<int16_t>(parts, 0); put<int16_t>(parts, 0); put<int16_t>(parts, int16_t(std::lround(s * 32767)));
    putStr(parts, "tag_origin"); putStr(parts, "child");
    put<uint8_t>(parts, 0); put<uint8_t>(parts, 0);
    const auto p = parseXModelParts(parts);
    CHECK(p.bones.size() == 2 && p.bones[1].parent == 0 && p.bones[1].name == "child");
    CHECK(near(p.bones[1].position[0], 10) && near(p.bones[1].rotation.w, s));

    // One rigid triangle on bone 1: local (1,0,0) ends at (10,1,0).
    std::vector<uint8_t> surfs;
    put<int16_t>(surfs, 20); put<int16_t>(surfs, 1);
    put<uint8_t>(surfs, 0); put<int16_t>(surfs, 3); put<int16_t>(surfs, 1); put<int16_t>(surfs, 1);
    for (int v = 0; v < 3; ++v) {
        put<float>(surfs, 0); put<float>(surfs, 0); put<float>(surfs, 1);
        for (int c = 0; c < 4; ++c) put<uint8_t>(surfs, 255);
        put<float>(surfs, 0.5f); put<float>(surfs, 0.25f);
        put<float>(surfs, 1); put<float>(surfs, 0); put<float>(surfs, 0); // binormal
        put<float>(surfs, 0); put<float>(surfs, 1); put<float>(surfs, 0); // tangent
        put<float>(surfs, v == 0 ? 1.0f : 0.0f); put<float>(surfs, 0); put<float>(surfs, float(v));
    }
    for (uint16_t i : {0, 1, 2}) put<uint16_t>(surfs, i);
    const auto out = parseXModelSurfs(surfs, p);
    CHECK(out.size() == 1 && out[0].vertices.size() == 3 && out[0].indices.size() == 3);
    if (!out.empty()) {
        CHECK(near(out[0].vertices[0].position[0], 10) && near(out[0].vertices[0].position[1], 1));
        CHECK(near(out[0].vertices[2].position[2], 2));
        CHECK(near(out[0].vertices[0].uv[1], 0.25f));
        CHECK(near(out[0].vertices[0].boneBinormal[0], 1) && near(out[0].vertices[0].boneTangent[1], 1));
    }

    // Trailing bytes are rejected.
    surfs.push_back(0);
    bool threw = false;
    try { parseXModelSurfs(surfs, p); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_xmodel: all checks passed\n";
    return 0;
}
