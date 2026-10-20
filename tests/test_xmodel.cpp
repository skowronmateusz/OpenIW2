// Tests for xmodel math and a minimal synthetic model.

#include "assets/xmodel.h"

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
        for (int k = 0; k < 6; ++k) put<float>(surfs, 0);
        put<float>(surfs, v == 0 ? 1.0f : 0.0f); put<float>(surfs, 0); put<float>(surfs, float(v));
    }
    for (uint16_t i : {0, 1, 2}) put<uint16_t>(surfs, i);
    const auto out = parseXModelSurfs(surfs, p);
    CHECK(out.size() == 1 && out[0].vertices.size() == 3 && out[0].indices.size() == 3);
    if (!out.empty()) {
        CHECK(near(out[0].vertices[0].position[0], 10) && near(out[0].vertices[0].position[1], 1));
        CHECK(near(out[0].vertices[2].position[2], 2));
        CHECK(near(out[0].vertices[0].uv[1], 0.25f));
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
