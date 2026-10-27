// RoQ decoder: a hand-made 16x16 stream (info, codebook, vector-quantised frame, unchanged frame, DPCM sound).
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
    // Second frame: all blocks unchanged (code 0).
    chunk(f, 0x1011, 0, {0, 0});

    RoqDecoder d(f);
    CHECK(d.fps() == 24);
    CHECK(d.nextFrame());
    CHECK(d.width() == 16 && d.height() == 16);
    const auto& px = d.rgba();
    CHECK(px[0] == 100 && px[1] == 100 && px[2] == 100 && px[3] == 255); // pixel (0,0): luma 100
    CHECK(px[(0 * 16 + 2) * 4] == 100 && px[(0 * 16 + 2) * 4 + 1] == 100); // the 2x2 cell magnified to 4x4 (x2 for the 8x8 vector)
    CHECK(px[((15 * 16) + 15) * 4] == 200); // bottom right pixel of the last cell: luma 200
    CHECK(d.takeAudio().empty());
    CHECK(d.nextFrame());
    CHECK(d.frameIndex() == 2 && d.rgba()[0] == 100);
    const auto audio = d.takeAudio();
    CHECK(audio.size() == 2 && audio[0] == 9 && audio[1] == 0);
    CHECK(!d.nextFrame());
    return failures ? 1 : 0;
}
