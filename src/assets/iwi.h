// .iwi texture decoding (docs/spec/iwi.md).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

enum class IwiFormat : uint8_t {
    ARGB32 = 1, RGB24 = 2, LumAlpha = 3, Lum = 4, Alpha = 5,
    WaveletARGB32 = 6, WaveletRGB24 = 7, WaveletLumAlpha = 8, WaveletLum = 9, WaveletAlpha = 10,
    DXT1 = 11, DXT3 = 12, DXT5 = 13,
};

namespace IwiFlags {
constexpr uint8_t NoMips = 0x02;
constexpr uint8_t Cube = 0x04;
} // namespace IwiFlags

struct IwiHeader {
    IwiFormat format{};
    uint8_t flags = 0;
    int width = 0, height = 0, depth = 0;
};

struct IwiLevel {
    int width = 0, height = 0;
    // Pixel data per face. For DXT this is still block-compressed. For the wavelet formats 6-10 it is the
    // decompressed pixels in the layout of the matching raw format (RGB is widened to 4 bytes, alpha 0xFF).
    std::vector<std::span<const uint8_t>> faces;
};

struct IwiImage {
    IwiHeader header;
    int faceCount = 1;
    std::vector<IwiLevel> levels; // index 0 = largest
    std::shared_ptr<const std::vector<uint8_t>> decoded; // owns the pixels of wavelet images
};

// Parses the header and splits the data into mip levels and faces; wavelet images are decompressed here.
// `data` must outlive the result.
// Throws std::runtime_error on malformed input.
IwiImage parseIwi(std::span<const uint8_t> data);

bool isWavelet(IwiFormat f);
const char* formatName(IwiFormat f);

// Decodes one face of one level to tightly packed RGBA8.
std::vector<uint8_t> decodeToRgba(const IwiImage& image, int level, int face = 0);

// DXT block decoding, exposed for tests. `out` receives 4x4 RGBA pixels (64 bytes).
void decodeDxt1Block(const uint8_t* block, uint8_t* out, bool dxt1Alpha = true);
void decodeDxt3Block(const uint8_t* block, uint8_t* out);
void decodeDxt5Block(const uint8_t* block, uint8_t* out);

// Decompresses the wavelet stream of formats 6-10 (`stream` starts right after the 28-byte header).
// Returns all levels and faces, smallest level first, each face in turn, in the raw layout described above.
std::vector<uint8_t> decodeWaveletStream(std::span<const uint8_t> stream, IwiFormat format, int width, int height,
                                         int mipCount, int faceCount);

} // namespace iw2::assets
