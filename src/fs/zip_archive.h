// Read-only ZIP archive reader used for .iwd asset packs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace iw2::fs {

struct ZipEntry {
    std::string name;            // path as stored in the archive, '/' separated
    uint16_t method = 0;         // 0 = stored, 8 = deflate
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localHeaderOffset = 0;
};

class ZipArchive {
public:
    // Opens the archive and reads its central directory. Throws std::runtime_error on failure.
    explicit ZipArchive(std::filesystem::path path);

    const std::filesystem::path& path() const { return path_; }
    const std::vector<ZipEntry>& entries() const { return entries_; }

    // Reads and decompresses one entry, verifying its CRC-32. Throws on failure.
    std::vector<uint8_t> read(const ZipEntry& entry) const;

private:
    std::filesystem::path path_;
    std::vector<ZipEntry> entries_;
    mutable std::ifstream file_;
};

} // namespace iw2::fs
