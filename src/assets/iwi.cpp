#include "assets/iwi.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

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

} // namespace

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
    if (isWavelet(h.format)) return img; // compressed stream; level spans are not fixed-size

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
    const IwiFormat f = image.header.format;
    if (isWavelet(f)) throw std::runtime_error(std::string("iwi: ") + formatName(f) + " is not supported yet");
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
