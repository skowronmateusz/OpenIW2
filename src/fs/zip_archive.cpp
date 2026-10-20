#include "fs/zip_archive.h"

#include <zlib.h>

#include <algorithm>
#include <stdexcept>

namespace iw2::fs {
namespace {

constexpr uint32_t kEndOfCentralDirSig = 0x06054b50;
constexpr uint32_t kCentralDirEntrySig = 0x02014b50;
constexpr uint32_t kLocalHeaderSig = 0x04034b50;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error(path.string() + ": " + what);
}

void readAt(std::ifstream& file, uint64_t offset, void* dst, size_t size, const std::filesystem::path& path) {
    file.clear();
    file.seekg(std::streamoff(offset));
    file.read(static_cast<char*>(dst), std::streamsize(size));
    if (!file || size_t(file.gcount()) != size) fail(path, "unexpected end of file");
}

} // namespace

ZipArchive::ZipArchive(std::filesystem::path path) : path_(std::move(path)), file_(path_, std::ios::binary) {
    if (!file_) fail(path_, "cannot open");
    file_.seekg(0, std::ios::end);
    const uint64_t fileSize = uint64_t(file_.tellg());

    // The end-of-central-directory record is 22 bytes plus an optional comment of up to 64 KiB.
    const uint64_t tailSize = std::min<uint64_t>(fileSize, 22 + 0xFFFF);
    std::vector<uint8_t> tail(tailSize);
    readAt(file_, fileSize - tailSize, tail.data(), tail.size(), path_);

    std::optional<size_t> eocd;
    for (size_t i = tail.size() >= 22 ? tail.size() - 22 + 1 : 0; i-- > 0;) {
        if (le32(&tail[i]) == kEndOfCentralDirSig) { eocd = i; break; }
    }
    if (!eocd) fail(path_, "not a zip archive (no end of central directory)");

    const uint8_t* e = &tail[*eocd];
    const uint16_t entryCount = le16(e + 10);
    const uint32_t dirSize = le32(e + 12);
    const uint32_t dirOffset = le32(e + 16);
    if (uint64_t(dirOffset) + dirSize > fileSize) fail(path_, "central directory out of range");

    std::vector<uint8_t> dir(dirSize);
    readAt(file_, dirOffset, dir.data(), dir.size(), path_);

    entries_.reserve(entryCount);
    size_t pos = 0;
    for (uint16_t i = 0; i < entryCount; ++i) {
        if (pos + 46 > dir.size() || le32(&dir[pos]) != kCentralDirEntrySig) fail(path_, "corrupt central directory");
        const uint8_t* c = &dir[pos];
        const uint16_t nameLen = le16(c + 28), extraLen = le16(c + 30), commentLen = le16(c + 32);
        if (pos + 46 + nameLen > dir.size()) fail(path_, "corrupt central directory name");

        ZipEntry entry;
        entry.method = le16(c + 10);
        entry.crc32 = le32(c + 16);
        entry.compressedSize = le32(c + 20);
        entry.uncompressedSize = le32(c + 24);
        entry.localHeaderOffset = le32(c + 42);
        entry.name.assign(reinterpret_cast<const char*>(c + 46), nameLen);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        if (!entry.name.empty() && entry.name.back() != '/') entries_.push_back(std::move(entry));

        pos += 46 + nameLen + extraLen + commentLen;
    }
}

std::vector<uint8_t> ZipArchive::read(const ZipEntry& entry) const {
    uint8_t local[30];
    readAt(file_, entry.localHeaderOffset, local, sizeof local, path_);
    if (le32(local) != kLocalHeaderSig) fail(path_, "bad local header for " + entry.name);
    const uint64_t dataOffset = uint64_t(entry.localHeaderOffset) + 30 + le16(local + 26) + le16(local + 28);

    std::vector<uint8_t> packed(entry.compressedSize);
    if (!packed.empty()) readAt(file_, dataOffset, packed.data(), packed.size(), path_);

    std::vector<uint8_t> out;
    if (entry.method == 0) {
        out = std::move(packed);
    } else if (entry.method == 8) {
        out.resize(entry.uncompressedSize);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) fail(path_, "inflateInit2 failed");
        zs.next_in = packed.data();
        zs.avail_in = uInt(packed.size());
        zs.next_out = out.data();
        zs.avail_out = uInt(out.size());
        const int rc = inflate(&zs, Z_FINISH);
        const uLong produced = zs.total_out;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END || produced != out.size()) fail(path_, "inflate failed for " + entry.name);
    } else {
        fail(path_, "unsupported compression method " + std::to_string(entry.method) + " for " + entry.name);
    }

    const uint32_t crc = uint32_t(::crc32(0L, out.data(), uInt(out.size())));
    if (crc != entry.crc32) fail(path_, "CRC mismatch for " + entry.name);
    return out;
}

} // namespace iw2::fs
