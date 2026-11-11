// RoQ decoder: a hand-made 16x16 stream (info, codebook, vector-quantised frame, unchanged frames, motion block, DPCM sound).
// Two picture buffers alternate, so an unchanged block shows the picture from two frames ago; after the first frame both
// buffers hold that picture (regressions: stale blocks in the logo movies, black blocks in the second frame of the intro).
#include "video/roq.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

void le16(std::vector<uint8_t>& v, unsigned x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
void le32(std::vector<uint8_t>& v, unsigned x) { le16(v, x & 0xffff), le16(v, x >> 16); }
void chunk(std::vector<uint8_t>& v, unsigned id, unsigned arg, const std::vector<uint8_t>& body) {
    le16(v, id), le32(v, unsigned(body.size())), le16(v, arg);
    v.insert(v.end(), body.begin(), body.end());
}
} // namespace

int main() {
    using iw2::video::RoqDecoder;
    std::vector<uint8_t> f;
    le16(f, 0x1084), le32(f, 0xffffffffu), le16(f, 24);
    chunk(f, 0x1001, 0, {16, 0, 16, 0, 8, 0, 4, 0});
    // One 2x2 cell (luma 100/100/100/200, U = V = 128) and one 4x4 vector made of that cell four times.
    chunk(f, 0x1002, 0x0101, {100, 100, 100, 200, 128, 128, 0, 0, 0, 0});
    // Four 8x8 blocks, each "vector" (code 2) with entry 0: the code word 0b10101010 in the high byte.
    chunk(f, 0x1011, 0, {0x00, 0xaa, 0, 0, 0, 0});
    // Mono DPCM: predictor 0, +3*3, then -(3*3).
    chunk(f, 0x1020, 0, {3, 0x83});
    // Second frame: all blocks unchanged (code 0): after the first picture both buffers hold it.
    chunk(f, 0x1011, 0, {0, 0});
    // Third frame: a new codebook (luma 50), block 0 a vector, the other blocks unchanged: the buffer being decoded holds the
    // first picture.
    chunk(f, 0x1002, 0x0101, {50, 50, 50, 50, 128, 128, 0, 0, 0, 0});
    chunk(f, 0x1011, 0, {0x00, 0x80, 0});
    // Fourth frame: all unchanged: the second frame's buffer (the first picture), not the third frame's block 0.
    chunk(f, 0x1011, 0, {0, 0});
    // Fifth frame: block 0 a motion block (code 1, byte 0x88 = no displacement) from the previous picture (the first picture);
    // the other blocks unchanged: the third frame's buffer.
    chunk(f, 0x1011, 0, {0x00, 0x40, 0x88});

    RoqDecoder d(f);
    CHECK(d.fps() == 24);
    CHECK(d.nextFrame());
    CHECK(d.width() == 16 && d.height() == 16);
    const auto& px = d.rgba();
    CHECK(px[0] == 100 && px[1] == 100 && px[2] == 100 && px[3] == 255); // pixel (0,0): luma 100
    CHECK(px[(0 * 16 + 2) * 4] == 100 && px[(0 * 16 + 2) * 4 + 1] == 100); // the 2x2 cell magnified to 4x4 (x2 for the 8x8 vector)
    CHECK(px[((15 * 16) + 15) * 4] == 200); // bottom right pixel of the last cell: luma 200
    CHECK(d.takeAudio().empty());
    const size_t last = ((15 * 16) + 15) * 4;
    CHECK(d.nextFrame());
    CHECK(d.frameIndex() == 2 && d.rgba()[0] == 100 && d.rgba()[last] == 200);
    const auto audio = d.takeAudio();
    CHECK(audio.size() == 2 && audio[0] == 9 && audio[1] == 0);
    CHECK(d.nextFrame());
    CHECK(d.rgba()[0] == 50 && d.rgba()[last] == 200); // block 0 repainted, the rest from the first picture
    CHECK(d.nextFrame());
    CHECK(d.rgba()[0] == 100 && d.rgba()[last] == 200); // two frames ago: block 0 is the first picture again
    CHECK(d.nextFrame());
    CHECK(d.rgba()[0] == 100 && d.rgba()[last] == 200);
    CHECK(!d.nextFrame());
    return failures ? 1 : 0;
}
