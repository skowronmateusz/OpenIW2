// Tests for .d3dbsp parsing using a synthetic map.

#include "assets/d3dbsp.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
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

std::vector<uint8_t> buildMap() {
    std::vector<std::vector<uint8_t>> lumps(39);
    // One material.
    lumps[0].resize(72, 0);
    std::memcpy(lumps[0].data(), "test/material", 13);
    // Three vertices.
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> v(68, 0);
        const float pos[3] = {float(i), float(i * 2), float(i * 3)};
        std::memcpy(v.data(), pos, 12);
        const float n[3] = {0, 0, 1};
        std::memcpy(v.data() + 12, n, 12);
        lumps[8].insert(lumps[8].end(), v.begin(), v.end());
    }
    for (uint16_t i : {0, 1, 2}) put(lumps[9], i);
    // One soup: material 0, no lightmap, vertices 0..2, indices 0..2.
    put<uint16_t>(lumps[7], 0); put<uint16_t>(lumps[7], 31); put<uint32_t>(lumps[7], 0);
    put<uint16_t>(lumps[7], 3); put<uint16_t>(lumps[7], 3); put<uint32_t>(lumps[7], 0);
    const char* ents = "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"info_player_start\"\n\"origin\" \"1 2 3\"\n}\n";
    lumps[37].assign(ents, ents + std::strlen(ents) + 1);

    std::vector<uint8_t> out = {'I', 'B', 'S', 'P'};
    put<uint32_t>(out, 4);
    uint32_t offset = 8 + 39 * 8;
    for (const auto& l : lumps) {
        put<uint32_t>(out, uint32_t(l.size()));
        put<uint32_t>(out, offset);
        offset += uint32_t((l.size() + 3) & ~size_t(3));
    }
    for (const auto& l : lumps) {
        out.insert(out.end(), l.begin(), l.end());
        out.resize((out.size() + 3) & ~size_t(3), 0);
    }
    return out;
}

} // namespace

int main() {
    using namespace iw2::assets;
    const auto data = buildMap();
    const D3dBsp bsp(data);
    CHECK(bsp.materials().size() == 1 && bsp.materials()[0].name == "test/material");
    CHECK(bsp.vertices().size() == 3 && bsp.vertices()[2].position[2] == 6.0f);
    CHECK(bsp.vertices()[0].normal[2] == 1.0f);
    CHECK(bsp.soups().size() == 1 && bsp.soups()[0].lightmap == D3dBsp::kNoLightmap);
    const auto ents = bsp.entities();
    CHECK(ents.size() == 2);
    if (ents.size() == 2) CHECK(ents[1].get("origin") && *ents[1].get("origin") == "1 2 3");

    // Out-of-range soup index is rejected.
    auto bad = data;
    const uint32_t idxOffset = *reinterpret_cast<const uint32_t*>(bad.data() + 12 + 9 * 8);
    bad[idxOffset] = 7;
    bool threw = false;
    try { D3dBsp b(bad); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    // Wrong magic is rejected.
    auto wrong = data;
    wrong[0] = 'X';
    threw = false;
    try { D3dBsp b(wrong); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_d3dbsp: all checks passed\n";
    return 0;
}
