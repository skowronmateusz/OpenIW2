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

// LSB-first bit writer for building wavelet streams; put() takes bits in stream order.
struct BitWriter {
    std::vector<uint8_t> bytes;
    size_t n = 0;
    void putBit(int b) {
        if (n % 8 == 0) bytes.push_back(0);
        if (b) bytes.back() |= uint8_t(1u << (n % 8));
        ++n;
    }
    void put(const char* bits) { for (; *bits; ++bits) putBit(*bits == '1'); }
    void putBits(unsigned v, int count) { for (int i = 0; i < count; ++i) putBit(int((v >> i) & 1)); }
    void append(const BitWriter& o) { for (size_t i = 0; i < o.n; ++i) putBit((o.bytes[i / 8] >> (i % 8)) & 1); }
};

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

    // Wavelet L8 (format 9), 2x2 with mips: raw 1x1 byte, then the bit stream for the 2x2 level.
    {
        BitWriter bw;
        bw.put("0");    // no low-resolution refinement
        bw.put("1");    // rounding bit
        bw.put("0100"); // +1 (code C: magnitude 1 "010", sign 0)
        bw.put("1");    // 0
        bw.put("0101"); // -1
        auto d = header(9, 0, 2, 2);
        d.push_back(100);
        d.insert(d.end(), bw.bytes.begin(), bw.bytes.end());
        const auto img = parseIwi(d);
        const auto px = decodeToRgba(img, 0);
        CHECK(px[0] == 101 && px[4] == 101 && px[8] == 100 && px[12] == 99);
        CHECK(px[3] == 255 && decodeToRgba(img, 1)[0] == 100);
    }

    // Wavelet A8 (format 10), 4x4: refinement of the 2x2 level and an escaped delta.
    {
        BitWriter bw;
        bw.put("1");                    // refine the 2x2 level
        for (int i = 0; i < 4; ++i) bw.put(i == 0 ? "000110" : "1"); // +4 on the first low pixel, 0 elsewhere
        // 1x1 -> 2x2 happened before; here the 4x4 level: four blocks, all deltas 0 except one escape.
        for (int blk = 0; blk < 4; ++blk) {
            bw.put("0");
            if (blk == 3) { bw.put("0000"); bw.putBits(255 + 40, 9); } else bw.put("1"); // escape: +40
            bw.put("1");
            bw.put("1");
        }
        BitWriter first; // stream for the 2x2 level: no refinement, rounding 0, deltas 0
        first.put("0");
        first.put("0111");
        // Concatenate the two levels' bits into one stream.
        BitWriter all;
        all.append(first);
        all.append(bw);
        auto d = header(10, 0, 4, 4);
        d.push_back(60); // 1x1
        d.insert(d.end(), all.bytes.begin(), all.bytes.end());
        const auto img = parseIwi(d);
        const auto l1 = decodeToRgba(img, 1);
        CHECK(l1[3] == 60 && l1[15] == 60);
        const auto l0 = decodeToRgba(img, 0);
        CHECK(l0[3] == 64 && l0[(1 * 4 + 1) * 4 + 3] == 64); // refined low pixel feeds block 0
        CHECK(l0[(0 * 4 + 2) * 4 + 3] == 60);                // block 1 uses an unrefined pixel
        // Block 3 (bottom right), a = +40: top-left (120+40)>>1 = 80, right 80, down (120-40)>>1 = 40.
        CHECK(l0[(2 * 4 + 2) * 4 + 3] == 80 && l0[(2 * 4 + 3) * 4 + 3] == 80 && l0[(3 * 4 + 2) * 4 + 3] == 40);
        CHECK(l0[0] == 255);
    }

    // Wavelet RGB (format 7): channel order B, G, R in the stream, alpha forced to 0xFF.
    {
        BitWriter bw;
        bw.put("0");
        bw.put("0"); bw.put("11100"); bw.put("100"); bw.put("100"); // B: rounding 0, a = +1 (code A), b = c = 0
        bw.put("0"); bw.put("11"); bw.put("11"); bw.put("11");      // G: same deltas as B (code B, all 0)
        bw.put("0"); bw.put("010"); bw.put("11"); bw.put("11");     // R: a = B's a + 1 = 2 (code B "01" + sign)
        auto d = header(7, 0, 2, 2);
        d.insert(d.end(), {10, 20, 30}); // 1x1: B G R
        d.insert(d.end(), bw.bytes.begin(), bw.bytes.end());
        const auto px = decodeToRgba(parseIwi(d), 0);
        // top-left: R (60+2)>>1 = 31, G (40+1)>>1 = 20, B (20+1)>>1 = 10; top-right: R 31, G 20, B 10
        CHECK(px[0] == 31 && px[1] == 20 && px[2] == 10 && px[3] == 255);
        // bottom-left: R (60-2)>>1 = 29, G (40-1)>>1 = 19, B 9
        CHECK(px[8] == 29 && px[9] == 19 && px[10] == 9 && px[11] == 255);
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
