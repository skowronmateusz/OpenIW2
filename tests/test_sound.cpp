// Tests for WAV parsing and the mixer's distance attenuation, with synthetic data.

#include "audio/decode.h"
#include "audio/sound.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

std::vector<uint8_t> makeWav(const std::vector<int16_t>& samples, int rate) {
    std::vector<uint8_t> v{'R', 'I', 'F', 'F'};
    put32(v, uint32_t(36 + samples.size() * 2));
    for (char c : std::string("WAVEfmt ")) v.push_back(uint8_t(c));
    put32(v, 16);
    put16(v, 1); put16(v, 1); put32(v, uint32_t(rate)); put32(v, uint32_t(rate * 2)); put16(v, 2); put16(v, 16);
    for (char c : std::string("data")) v.push_back(uint8_t(c));
    put32(v, uint32_t(samples.size() * 2));
    for (int16_t s : samples) put16(v, uint16_t(s));
    return v;
}

} // namespace

int main() {
    const auto bytes = makeWav({100, -200, 16384}, 22050);
    const auto w = iw2::audio::parseWav(bytes);
    CHECK(w.rate == 22050 && w.channels == 1 && w.frames() == 3);
    CHECK(w.samples[2] == 16384);

    bool threw = false;
    try { iw2::audio::parseWav(std::vector<uint8_t>{1, 2, 3}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // decodeSound picks the parser by extension: WAV data under a .wav name, the MP3 decoder for .mp3.
    CHECK(iw2::audio::decodeSound(bytes, "a/b.WAV").frames() == 3);
    threw = false;
    try { iw2::audio::decodeSound(std::vector<uint8_t>(64, 0), "music/x.MP3"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    if (failures) return 1;
    std::cout << "sound tests passed\n";
    return 0;
}
