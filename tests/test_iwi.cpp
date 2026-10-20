// Tests for .iwi parsing and DXT decoding, using synthetic images.

#include "assets/iwi.h"
#include "util/png.h"

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

std::vector<uint8_t> header(uint8_t format, uint8_t flags, int w, int h) {
    std::vector<uint8_t> d = {'I', 'W', 'i', 5, format, flags,
                              uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 1, 0};
    d.resize(28, 0);
    return d;
}

} // namespace

int main() {
    using namespace iw2::assets;

    // 2x2 ARGB32 with mips: levels 1x1 (stored first) then 2x2.
    {
        auto d = header(1, 0, 2, 2);
        d.insert(d.end(), {10, 20, 30, 40});                                         // 1x1: B G R A
        d.insert(d.end(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}); // 2x2
        const auto img = parseIwi(d);
        CHECK(img.levels.size() == 2);
        CHECK(img.levels[1].width == 1);
        const auto top = decodeToRgba(img, 0);
        CHECK(top.size() == 16);
        CHECK(top[0] == 3 && top[1] == 2 && top[2] == 1 && top[3] == 4); // B,G,R,A -> R,G,B,A
        const auto small = decodeToRgba(img, 1);
        CHECK(small[0] == 30 && small[3] == 40);
    }

    // No-mip flag keeps one level; cube flag gives 6 faces.
    {
        auto d = header(4, 0x02 | 0x04, 2, 2);
        for (int f = 0; f < 6; ++f) d.insert(d.end(), 4, uint8_t(f * 10));
        const auto img = parseIwi(d);
        CHECK(img.levels.size() == 1);
        CHECK(img.faceCount == 6);
        CHECK(decodeToRgba(img, 0, 5)[0] == 50);
    }

    // DXT1: c0 = pure red, c1 = pure blue, indices 0,1,2,3 in the first row.
    {
        const uint8_t blk[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
        uint8_t px[64];
        decodeDxt1Block(blk, px);
        CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255); // red
        CHECK(px[4] == 0 && px[6] == 255);                               // blue
        CHECK(px[8] == 170 && px[10] == 85);                             // 2/3 red + 1/3 blue
        CHECK(px[12] == 85 && px[14] == 170);
    }

    // DXT5 alpha: a0=255, a1=0 with index 1 everywhere -> alpha 0.
    {
        uint8_t blk[16] = {255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
        uint8_t px[64];
        decodeDxt5Block(blk, px);
        CHECK(px[3] == 0 && px[63] == 0);
    }

    // Truncated and wrong-version files throw.
    {
        auto d = header(13, 0, 8, 8);
        bool threw = false;
        try { parseIwi(d); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        d[3] = 6;
        threw = false;
        try { parseIwi(d); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }

    // PNG writer emits a valid signature and IEND.
    {
        const uint8_t px[4] = {1, 2, 3, 4};
        const auto png = iw2::util::encodePngRgba(px, 1, 1);
        CHECK(png.size() > 40 && png[1] == 'P' && std::memcmp(png.data() + png.size() - 8, "IEND", 4) == 0);
    }

    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_iwi: all checks passed\n";
    return 0;
}
