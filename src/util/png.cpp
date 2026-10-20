#include "util/png.h"

#include <zlib.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace iw2::util {
namespace {

void put32be(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32be(out, uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}

} // namespace

std::vector<uint8_t> encodePngRgba(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const uint8_t* row = rgba + size_t(y) * size_t(width) * 4;
        raw.insert(raw.end(), row, row + size_t(width) * 4);
    }
    uLongf packedSize = compressBound(uLong(raw.size()));
    std::vector<uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), uLong(raw.size()), 6) != Z_OK)
        throw std::runtime_error("png: compression failed");
    packed.resize(packedSize);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, uint32_t(width));
    put32be(ihdr, uint32_t(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", packed);
    chunk(out, "IEND", {});
    return out;
}

void writePngRgba(const std::filesystem::path& path, const uint8_t* rgba, int width, int height) {
    const auto png = encodePngRgba(rgba, width, height);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
}

} // namespace iw2::util
