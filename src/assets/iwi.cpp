#include "assets/iwi.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace iw2::assets {
namespace {

constexpr size_t kHeaderSize = 28;

int bytesPerPixel(IwiFormat f) {
    switch (f) {
    case IwiFormat::ARGB32: return 4;
    case IwiFormat::RGB24: return 3;
    case IwiFormat::LumAlpha: return 2;
    case IwiFormat::Lum:
    case IwiFormat::Alpha: return 1;
    default: return 0;
    }
}

size_t levelSize(IwiFormat f, int w, int h) {
    switch (f) {
    case IwiFormat::DXT1: return size_t((w + 3) / 4) * size_t((h + 3) / 4) * 8;
    case IwiFormat::DXT3:
    case IwiFormat::DXT5: return size_t((w + 3) / 4) * size_t((h + 3) / 4) * 16;
    default: return size_t(w) * size_t(h) * size_t(bytesPerPixel(f));
    }
}

void rgb565(uint16_t c, uint8_t* out) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t((r << 3) | (r >> 2));
    out[1] = uint8_t((g << 2) | (g >> 4));
    out[2] = uint8_t((b << 3) | (b >> 2));
}

// ---- Wavelet formats 6-10 (docs/spec/iwi.md, "Wavelet formats") ----

// One prefix code: a value is coded as the bits of its magnitude's prefix followed, for a non-zero
// magnitude, by one sign bit (0 = positive, 1 = negative). Bits are listed in stream order.
struct CodeWord {
    int magnitude;
    const char* bits;
};

struct DeltaCode {
    const CodeWord* words;
    size_t count;
    const char* escape; // followed by `escapeBits` raw bits, value = raw - escapeBias
    int escapeBits;
    int escapeBias;
};

// Code A: first channel of a pixel group.
constexpr CodeWord kCodeA[] = {
    {0, "100"}, {1, "1110"}, {2, "1010"}, {3, "0101"}, {4, "0010"}, {5, "11111"}, {6, "11010"}, {7, "11000"},
    {8, "01111"}, {9, "01100"}, {10, "01000"}, {11, "00011"}, {12, "00000"}, {13, "111100"}, {14, "110110"},
    {15, "101111"}, {16, "101101"}, {17, "011101"}, {18, "011010"}, {19, "010010"}, {20, "001101"},
    {21, "000101"}, {22, "000011"}, {23, "1111010"}, {24, "1101110"}, {25, "1100110"}, {26, "1100100"},
    {27, "1011100"}, {28, "1011000"}, {29, "0111000"}, {30, "0100111"}, {31, "0011101"}, {32, "0011001"},
    {33, "0001001"}, {34, "0001000"}, {35, "11110111"}, {36, "11011111"}, {37, "11011110"}, {38, "11001011"},
    {39, "10111011"}, {40, "10110011"}, {41, "01110011"}, {42, "01101111"}, {43, "01101110"}, {44, "01101100"},
    {45, "01001100"}, {46, "00111000"}, {47, "00110000"}, {48, "00001010"}, {49, "00001001"},
    {50, "111101101"}, {51, "110011111"}, {52, "110011101"}, {53, "110010101"}, {54, "101110101"},
    {55, "101110100"}, {56, "101100100"}, {57, "011100100"}, {58, "011011011"}, {59, "010011011"},
    {60, "001110011"}, {61, "001110010"}, {62, "001100010"}, {63, "000010111"}, {64, "000010001"},
    {65, "000010000"}, {66, "1111011000"}, {67, "1100111101"}, {68, "1100111001"}, {69, "1100111000"},
    {70, "1100101000"}, {71, "1011001011"}, {72, "0111001011"}, {73, "0111001010"}, {74, "0110110101"},
    {75, "0110110100"}, {76, "0100110101"}, {77, "0100110100"}, {78, "0011000111"}, {79, "0011000110"},
    {80, "0000101101"}, {81, "0000101100"}, {82, "11110110011"}, {83, "11110110010"}, {84, "11001111001"},
    {85, "11001111000"}, {86, "11001010011"}, {87, "11001010010"}, {88, "10110010101"}, {89, "10110010100"},
};

// Code B: second and third colour channel, coded relative to the first channel's deltas.
constexpr CodeWord kCodeB[] = {
    {0, "11"}, {1, "01"}, {2, "100"}, {3, "1011"}, {4, "0010"}, {5, "10101"}, {6, "00110"}, {7, "00001"},
    {8, "101001"}, {9, "001110"}, {10, "000110"}, {11, "000001"}, {12, "1010001"}, {13, "0011111"},
    {14, "0001110"}, {15, "0001001"}, {16, "0000001"}, {17, "10100001"}, {18, "00111101"}, {19, "00011111"},
    {20, "00010101"}, {21, "00010001"}, {22, "00000001"}, {23, "101000001"}, {24, "001111001"},
    {25, "001111000"}, {26, "000111100"}, {27, "000101001"}, {28, "000100001"}, {29, "000000001"},
    {30, "000000000"}, {31, "1010000000"}, {32, "0001111011"}, {33, "0001111010"}, {34, "0001010001"},
    {35, "0001010000"}, {36, "0001000001"}, {37, "0001000000"}, {38, "10100000011"}, {39, "10100000010"},
};

// Code C: the last channel (alpha, or the only channel) and the low-resolution refinement pass.
constexpr CodeWord kCodeC[] = {
    {0, "1"}, {1, "010"}, {2, "0011"}, {3, "01101"}, {4, "00011"}, {5, "011101"}, {6, "001011"}, {7, "001000"},
    {8, "0111111"}, {9, "0111100"}, {10, "0110011"}, {11, "0110000"}, {12, "0010100"}, {13, "0001011"},
    {14, "0001000"}, {15, "0001001"}, {16, "01111011"}, {17, "01110001"}, {18, "01100011"}, {19, "01100010"},
    {20, "00101010"}, {21, "00100111"}, {22, "00100101"}, {23, "00010100"}, {24, "00010101"},
    {25, "011111001"}, {26, "011110101"}, {27, "011100111"}, {28, "011100100"}, {29, "011100000"},
    {30, "011001001"}, {31, "011001000"}, {32, "011111010"}, {33, "001010110"}, {34, "001001101"},
    {35, "001001100"}, {36, "001001001"}, {37, "001001000"}, {38, "0111110111"}, {39, "0111110110"},
    {40, "0111110001"}, {41, "0111110000"}, {42, "0111101001"}, {43, "0111101000"}, {44, "0111001101"},
    {45, "0111001011"}, {46, "0111000011"}, {47, "0110010101"}, {48, "0111001100"}, {49, "0111000010"},
    {50, "0110010100"}, {64, "0111001010"}, {127, "011001011"}, {128, "001010111"},
};

constexpr DeltaCode kDeltaA{kCodeA, std::size(kCodeA), "001111", 9, 255};
constexpr DeltaCode kDeltaB{kCodeB, std::size(kCodeB), "0001011", 10, 510};
constexpr DeltaCode kDeltaC{kCodeC, std::size(kCodeC), "0000", 9, 255};

constexpr int kLookupBits = 12;
constexpr int16_t kEscape = INT16_MIN;

// 12-bit lookup built from a code: entry = (value, code length).
struct DeltaLookup {
    int16_t value[1 << kLookupBits];
    uint8_t length[1 << kLookupBits];
    int escapeBits = 0, escapeBias = 0;

    explicit DeltaLookup(const DeltaCode& code) : value{}, length{}, escapeBits(code.escapeBits), escapeBias(code.escapeBias) {
        auto add = [&](const std::string& bits, int v) {
            const int n = int(bits.size());
            if (n == 0 || n > kLookupBits) throw std::logic_error("iwi: bad wavelet code");
            unsigned prefix = 0;
            for (int i = 0; i < n; ++i)
                if (bits[size_t(i)] == '1') prefix |= 1u << i;
            for (unsigned hi = 0; hi < (1u << (kLookupBits - n)); ++hi) {
                const unsigned idx = prefix | (hi << n);
                if (length[idx]) throw std::logic_error("iwi: wavelet code is not prefix-free");
                value[idx] = int16_t(v);
                length[idx] = uint8_t(n);
            }
        };
        for (size_t i = 0; i < code.count; ++i) {
            const CodeWord& w = code.words[i];
            if (w.magnitude == 0) {
                add(w.bits, 0);
            } else {
                add(std::string(w.bits) + "0", w.magnitude);
                add(std::string(w.bits) + "1", -w.magnitude);
            }
        }
        add(code.escape, kEscape);
        for (uint8_t l : length)
            if (!l) throw std::logic_error("iwi: wavelet code is incomplete");
    }
};

const DeltaLookup& lookupA() { static const DeltaLookup t(kDeltaA); return t; }
const DeltaLookup& lookupB() { static const DeltaLookup t(kDeltaB); return t; }
const DeltaLookup& lookupC() { static const DeltaLookup t(kDeltaC); return t; }

// Byte cursor for the raw small levels, then an LSB-first bit reader once the first 2-D level is reached.
class WaveletReader {
public:
    explicit WaveletReader(std::span<const uint8_t> d) : data_(d) {}

    uint8_t rawByte() {
        if (bitMode_) throw std::runtime_error("iwi: raw wavelet level after the bit stream started");
        if (bytePos_ >= data_.size()) throw std::runtime_error("iwi: truncated wavelet data");
        return data_[bytePos_++];
    }

    void startBits() {
        if (!bitMode_) { bitMode_ = true; bitPos_ = bytePos_ * 8; }
    }

    unsigned peek16() const {
        unsigned v = 0;
        for (size_t b = bitPos_ >> 3, shift = 0; shift < 24 && b < data_.size(); ++b, shift += 8) v |= unsigned(data_[b]) << shift;
        return (v >> (bitPos_ & 7)) & 0xFFFF;
    }

    void skip(int n) {
        bitPos_ += size_t(n);
        if (bitPos_ > data_.size() * 8) throw std::runtime_error("iwi: truncated wavelet data");
    }

    int bit() { const int b = int(peek16() & 1); skip(1); return b; }

    int delta(const DeltaLookup& t) {
        const unsigned idx = peek16() & ((1u << kLookupBits) - 1);
        skip(t.length[idx]);
        const int v = t.value[idx];
        if (v != kEscape) return v;
        const int raw = int(peek16() & ((1u << t.escapeBits) - 1));
        skip(t.escapeBits);
        return raw - t.escapeBias;
    }

private:
    std::span<const uint8_t> data_;
    size_t bytePos_ = 0, bitPos_ = 0;
    bool bitMode_ = false;
};

// Writes one channel of a 2x2 output block from the predicted (low-resolution) value and three deltas.
void reconstructBlock(uint8_t* out, size_t right, size_t down, int low, int a, int b, int c, int roundBit) {
    const int p = 2 * low;
    out[0] = uint8_t(((p + a + b + c) >> 1) + roundBit);
    out[right] = uint8_t((p + a - b - c) >> 1);
    out[down] = uint8_t((p + b - a - c) >> 1);
    out[down + right] = uint8_t((p + c - a - b) >> 1);
}

int waveletChannels(IwiFormat f) {
    switch (f) {
    case IwiFormat::WaveletARGB32: return 4;
    case IwiFormat::WaveletRGB24: return 3;
    case IwiFormat::WaveletLumAlpha: return 2;
    default: return 1;
    }
}

int waveletStride(IwiFormat f) { const int c = waveletChannels(f); return c == 3 ? 4 : c; }

// Decodes one level of one face. `low` is the previous (half-size) level of the same face, or empty.
void decodeWaveletLevel(WaveletReader& rd, int channels, int stride, int w, int h, std::span<const uint8_t> low,
                        uint8_t* out) {
    if (w < 2 || h < 2) { // a 1-pixel-wide level is stored as raw bytes
        for (int i = 0, n = w * h; i < n; ++i) {
            for (int c = 0; c < channels; ++c) out[i * stride + c] = rd.rawByte();
            if (stride != channels) out[i * stride + stride - 1] = 0xFF;
        }
        return;
    }
    if (w % 2 || h % 2) throw std::runtime_error("iwi: odd wavelet level size");
    const int lw = w / 2, lh = h / 2;
    std::vector<uint8_t> pred(size_t(lw) * size_t(lh) * size_t(stride), 0);
    if (!low.empty()) {
        if (low.size() != pred.size()) throw std::runtime_error("iwi: wavelet level size mismatch");
        std::copy(low.begin(), low.end(), pred.begin());
    }

    rd.startBits();
    if (rd.bit()) { // refinement of the low-resolution level before it is used as the prediction
        for (int i = 0; i < lw * lh; ++i) {
            for (int c = 0; c < channels; ++c) pred[size_t(i * stride + c)] = uint8_t(pred[size_t(i * stride + c)] + rd.delta(lookupC()));
            if (stride != channels) pred[size_t(i * stride + stride - 1)] = 0xFF;
        }
    }

    const size_t right = size_t(stride), down = size_t(w) * size_t(stride);
    for (int by = 0; by < lh; ++by) {
        for (int bx = 0; bx < lw; ++bx) {
            const uint8_t* lp = &pred[(size_t(by) * size_t(lw) + size_t(bx)) * size_t(stride)];
            uint8_t* o = out + (size_t(2 * by) * size_t(w) + size_t(2 * bx)) * size_t(stride);
            if (channels != 1) {
                int r = rd.bit();
                const int a = rd.delta(lookupA()), b = rd.delta(lookupA()), c = rd.delta(lookupA());
                reconstructBlock(o, right, down, lp[0], a, b, c, r);
                if (channels > 2) {
                    for (int ch = 1; ch <= 2; ++ch) {
                        r = rd.bit();
                        const int a2 = a + rd.delta(lookupB()), b2 = b + rd.delta(lookupB()), c2 = c + rd.delta(lookupB());
                        reconstructBlock(o + ch, right, down, lp[ch], a2, b2, c2, r);
                    }
                }
            }
            if (channels == 3) {
                o[stride - 1] = o[right + stride - 1] = o[down + stride - 1] = o[down + right + stride - 1] = 0xFF;
            } else {
                const int r = rd.bit();
                const int a = rd.delta(lookupC()), b = rd.delta(lookupC()), c = rd.delta(lookupC());
                reconstructBlock(o + stride - 1, right, down, lp[stride - 1], a, b, c, r);
            }
        }
    }
}

} // namespace

std::vector<uint8_t> decodeWaveletStream(std::span<const uint8_t> stream, IwiFormat format, int width, int height,
                                         int mipCount, int faceCount) {
    if (!isWavelet(format)) throw std::invalid_argument("iwi: not a wavelet format");
    const int channels = waveletChannels(format), stride = waveletStride(format);
    auto levelBytes = [&](int l) {
        return size_t(std::max(1, width >> l)) * size_t(std::max(1, height >> l)) * size_t(stride);
    };
    size_t total = 0;
    for (int l = 0; l < mipCount; ++l) total += levelBytes(l) * size_t(faceCount);
    std::vector<uint8_t> out(total);

    WaveletReader rd(stream);
    std::vector<size_t> prevOffset(size_t(faceCount), SIZE_MAX);
    size_t pos = 0;
    for (int l = mipCount - 1; l >= 0; --l) {
        const size_t n = levelBytes(l);
        const size_t prevSize = l + 1 < mipCount ? levelBytes(l + 1) : 0;
        for (int f = 0; f < faceCount; ++f) {
            std::span<const uint8_t> low;
            if (prevOffset[size_t(f)] != SIZE_MAX) low = std::span<const uint8_t>(out).subspan(prevOffset[size_t(f)], prevSize);
            decodeWaveletLevel(rd, channels, stride, std::max(1, width >> l), std::max(1, height >> l), low, out.data() + pos);
            prevOffset[size_t(f)] = pos;
            pos += n;
        }
    }
    return out;
}

bool isWavelet(IwiFormat f) { return uint8_t(f) >= 6 && uint8_t(f) <= 10; }

const char* formatName(IwiFormat f) {
    switch (f) {
    case IwiFormat::ARGB32: return "ARGB32";
    case IwiFormat::RGB24: return "RGB24";
    case IwiFormat::LumAlpha: return "LA8";
    case IwiFormat::Lum: return "L8";
    case IwiFormat::Alpha: return "A8";
    case IwiFormat::WaveletARGB32: return "wavelet-ARGB32";
    case IwiFormat::WaveletRGB24: return "wavelet-RGB24";
    case IwiFormat::WaveletLumAlpha: return "wavelet-LA8";
    case IwiFormat::WaveletLum: return "wavelet-L8";
    case IwiFormat::WaveletAlpha: return "wavelet-A8";
    case IwiFormat::DXT1: return "DXT1";
    case IwiFormat::DXT3: return "DXT3";
    case IwiFormat::DXT5: return "DXT5";
    }
    return "unknown";
}

IwiImage parseIwi(std::span<const uint8_t> data) {
    if (data.size() < kHeaderSize) throw std::runtime_error("iwi: file too small");
    if (data[0] != 'I' || data[1] != 'W' || data[2] != 'i') throw std::runtime_error("iwi: not an IW image");
    if (data[3] != 5) throw std::runtime_error("iwi: unsupported version " + std::to_string(data[3]));

    IwiImage img;
    IwiHeader& h = img.header;
    if (data[4] < 1 || data[4] > 13) throw std::runtime_error("iwi: unknown format " + std::to_string(data[4]));
    h.format = IwiFormat(data[4]);
    h.flags = data[5];
    auto s16 = [&](size_t o) { return int(int16_t(uint16_t(data[o] | (data[o + 1] << 8)))); };
    h.width = s16(6);
    h.height = s16(8);
    h.depth = s16(10);
    if (h.width <= 0 || h.height <= 0) throw std::runtime_error("iwi: bad dimensions");
    if (h.depth > 1) throw std::runtime_error("iwi: volume textures are not supported");

    img.faceCount = (h.flags & IwiFlags::Cube) ? 6 : 1;

    int mipCount = 1;
    if (!(h.flags & IwiFlags::NoMips)) {
        for (int s = 1; s < h.width || s < h.height || s < h.depth; s <<= 1) ++mipCount;
    }
    img.levels.resize(size_t(mipCount));
    for (int l = 0; l < mipCount; ++l) {
        img.levels[size_t(l)].width = std::max(1, h.width >> l);
        img.levels[size_t(l)].height = std::max(1, h.height >> l);
    }
    if (isWavelet(h.format)) {
        // Decompress the whole chain now; the levels then look like raw pixel data.
        auto pixels = std::make_shared<std::vector<uint8_t>>(
            decodeWaveletStream(data.subspan(kHeaderSize), h.format, h.width, h.height, mipCount, img.faceCount));
        const std::span<const uint8_t> all(*pixels);
        const size_t stride = size_t(waveletStride(h.format));
        size_t pos = 0;
        for (int l = mipCount - 1; l >= 0; --l) {
            IwiLevel& lvl = img.levels[size_t(l)];
            const size_t size = size_t(lvl.width) * size_t(lvl.height) * stride;
            for (int f = 0; f < img.faceCount; ++f, pos += size) lvl.faces.push_back(all.subspan(pos, size));
        }
        img.decoded = std::move(pixels);
        return img;
    }

    // Smallest mip first; each mip holds all faces.
    size_t pos = kHeaderSize;
    for (int l = mipCount - 1; l >= 0; --l) {
        IwiLevel& lvl = img.levels[size_t(l)];
        const size_t size = levelSize(h.format, lvl.width, lvl.height);
        for (int f = 0; f < img.faceCount; ++f) {
            if (pos + size > data.size()) throw std::runtime_error("iwi: truncated data");
            lvl.faces.push_back(data.subspan(pos, size));
            pos += size;
        }
    }
    return img;
}

void decodeDxt1Block(const uint8_t* b, uint8_t* out, bool dxt1Alpha) {
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !dxt1Alpha) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int p = 0; p < 16; ++p) std::memcpy(out + p * 4, pal[(bits >> (2 * p)) & 3], 4);
}

void decodeDxt3Block(const uint8_t* b, uint8_t* out) {
    decodeDxt1Block(b + 8, out, false);
    for (int p = 0; p < 16; ++p) {
        const int a = (b[p / 2] >> ((p & 1) * 4)) & 15;
        out[p * 4 + 3] = uint8_t(a * 17);
    }
}

void decodeDxt5Block(const uint8_t* b, uint8_t* out) {
    decodeDxt1Block(b + 8, out, false);
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i <= 6; ++i) a[1 + i] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i <= 4; ++i) a[1 + i] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p) out[p * 4 + 3] = a[(bits >> (3 * p)) & 7];
}

std::vector<uint8_t> decodeToRgba(const IwiImage& image, int level, int face) {
    IwiFormat f = image.header.format;
    // Decompressed wavelet levels use the raw layouts (RGB widened to 4 bytes with alpha 0xFF).
    switch (f) {
    case IwiFormat::WaveletARGB32:
    case IwiFormat::WaveletRGB24: f = IwiFormat::ARGB32; break;
    case IwiFormat::WaveletLumAlpha: f = IwiFormat::LumAlpha; break;
    case IwiFormat::WaveletLum: f = IwiFormat::Lum; break;
    case IwiFormat::WaveletAlpha: f = IwiFormat::Alpha; break;
    default: break;
    }
    const IwiLevel& lvl = image.levels.at(size_t(level));
    const std::span<const uint8_t> src = lvl.faces.at(size_t(face));
    const int w = lvl.width, h = lvl.height;
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);

    if (f == IwiFormat::DXT1 || f == IwiFormat::DXT3 || f == IwiFormat::DXT5) {
        const size_t blockSize = f == IwiFormat::DXT1 ? 8 : 16;
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        uint8_t px[64];
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = src.data() + (size_t(by) * size_t(bw) + size_t(bx)) * blockSize;
                if (f == IwiFormat::DXT1) decodeDxt1Block(blk, px);
                else if (f == IwiFormat::DXT3) decodeDxt3Block(blk, px);
                else decodeDxt5Block(blk, px);
                for (int y = 0; y < 4 && by * 4 + y < h; ++y)
                    for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                        std::memcpy(&out[(size_t(by * 4 + y) * size_t(w) + size_t(bx * 4 + x)) * 4], px + (y * 4 + x) * 4, 4);
            }
        }
        return out;
    }

    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = &out[i * 4];
        switch (f) {
        case IwiFormat::ARGB32: // memory order B, G, R, A
            o[0] = src[i * 4 + 2]; o[1] = src[i * 4 + 1]; o[2] = src[i * 4 + 0]; o[3] = src[i * 4 + 3];
            break;
        case IwiFormat::RGB24: // memory order B, G, R
            o[0] = src[i * 3 + 2]; o[1] = src[i * 3 + 1]; o[2] = src[i * 3 + 0]; o[3] = 255;
            break;
        case IwiFormat::LumAlpha: // memory order L, A
            o[0] = o[1] = o[2] = src[i * 2]; o[3] = src[i * 2 + 1];
            break;
        case IwiFormat::Lum:
            o[0] = o[1] = o[2] = src[i]; o[3] = 255;
            break;
        case IwiFormat::Alpha:
            o[0] = o[1] = o[2] = 255; o[3] = src[i];
            break;
        default: break;
        }
    }
    return out;
}

} // namespace iw2::assets
