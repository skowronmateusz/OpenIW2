// Tests for the zip reader and the pack search order, using synthetic packs.

#include <unistd.h>
#include <string>
#include "fs/vfs.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

struct File { std::string name; std::string data; bool deflate; };

// Writes a minimal zip archive (stored or raw-deflate entries).
void writeZip(const std::filesystem::path& path, const std::vector<File>& files) {
    std::vector<uint8_t> out, dir;
    for (const File& f : files) {
        std::vector<uint8_t> payload(f.data.begin(), f.data.end());
        uint16_t method = 0;
        if (f.deflate) {
            z_stream zs{};
            deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            std::vector<uint8_t> packed(deflateBound(&zs, uLong(payload.size())));
            zs.next_in = payload.data(); zs.avail_in = uInt(payload.size());
            zs.next_out = packed.data(); zs.avail_out = uInt(packed.size());
            deflate(&zs, Z_FINISH);
            packed.resize(zs.total_out);
            deflateEnd(&zs);
            payload = std::move(packed);
            method = 8;
        }
        const uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(f.data.data()), uInt(f.data.size())));
        const uint32_t offset = uint32_t(out.size());

        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, method); put32(out, 0);
        put32(out, crc); put32(out, uint32_t(payload.size())); put32(out, uint32_t(f.data.size()));
        put16(out, uint16_t(f.name.size())); put16(out, 0);
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.insert(out.end(), payload.begin(), payload.end());

        put32(dir, 0x02014b50); put16(dir, 20); put16(dir, 20); put16(dir, 0); put16(dir, method); put32(dir, 0);
        put32(dir, crc); put32(dir, uint32_t(payload.size())); put32(dir, uint32_t(f.data.size()));
        put16(dir, uint16_t(f.name.size())); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0);
        put32(dir, 0); put32(dir, offset);
        dir.insert(dir.end(), f.name.begin(), f.name.end());
    }
    const uint32_t dirOffset = uint32_t(out.size());
    out.insert(out.end(), dir.begin(), dir.end());
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, uint16_t(files.size()));
    put16(out, uint16_t(files.size())); put32(out, uint32_t(dir.size())); put32(out, dirOffset); put16(out, 0);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

std::string str(const std::optional<std::vector<uint8_t>>& v) { return v ? std::string(v->begin(), v->end()) : "<missing>"; }

} // namespace

int main() {
    namespace stdfs = std::filesystem;
    const stdfs::path root = stdfs::temp_directory_path() / ("iw2_test_vfs_" + std::to_string(::getpid()));
    stdfs::remove_all(root);
    stdfs::create_directories(root / "main" / "maps");

    const std::string big(5000, 'x');
    writeZip(root / "main" / "iw_00.iwd", {{"maps/a.gsc", "from iw_00", true},
                                           {"Sound/Shared.wav", "iw_00 sound", false},
                                           {"images/big.iwi", big, true}});
    writeZip(root / "main" / "iw_01.iwd", {{"maps/a.gsc", "from iw_01", false}});
    writeZip(root / "main" / "localized_polish_iw00.iwd", {{"sound/shared.wav", "polish sound", false},
                                                           {"sound/pl_only.wav", "pl", false}});
    writeZip(root / "main" / "localized_german_iw00.iwd", {{"sound/de_only.wav", "de", false}});
    std::ofstream(root / "main" / "maps" / "loose.gsc") << "loose file";
    std::ofstream(root / "main" / "maps" / "A.GSC") << "loose override";

    iw2::fs::Vfs vfs;
    vfs.mountDirectory(root / "main", "polish");

    // Loose files beat packs; matching is case-insensitive.
    CHECK(str(vfs.read("maps/a.gsc")) == "loose override");
    CHECK(str(vfs.read("MAPS\\loose.gsc")) == "loose file");
    // Deflate + CRC path.
    CHECK(str(vfs.read("images/big.iwi")) == big);
    // Localized packs sort before iw_*, so iw_00 wins over localized_polish for the same name.
    CHECK(str(vfs.read("sound/shared.wav")) == "iw_00 sound");
    CHECK(vfs.sourceOf("sound/shared.wav") == "iw_00.iwd");
    CHECK(str(vfs.read("sound/pl_only.wav")) == "pl");
    // Other languages are filtered out.
    CHECK(!vfs.exists("sound/de_only.wav"));
    CHECK(!vfs.read("missing/file").has_value());

    const auto packs = vfs.packs();
    CHECK(packs.size() == 3);
    if (packs.size() == 3) {
        CHECK(packs[0].path.filename() == "iw_01.iwd");
        CHECK(packs[1].path.filename() == "iw_00.iwd");
        CHECK(packs[2].path.filename() == "localized_polish_iw00.iwd");
        CHECK(packs[2].localized && packs[2].language == "polish");
    }
    CHECK(vfs.list("sound/").size() == 2);

    // Without a language filter every localized pack is mounted.
    iw2::fs::Vfs all;
    all.mountDirectory(root / "main");
    CHECK(all.exists("sound/de_only.wav"));

    stdfs::remove_all(root);
    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_vfs: all checks passed\n";
    return 0;
}
