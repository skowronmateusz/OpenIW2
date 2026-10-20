// Virtual filesystem over a game directory: loose files plus .iwd packs.
// Search order follows docs/spec/filesystem.md.
#pragma once

#include "fs/zip_archive.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iw2::fs {

struct PackInfo {
    std::filesystem::path path;
    bool localized = false;
    std::string language; // set for localized packs, lowercase
    size_t fileCount = 0;
};

class Vfs {
public:
    // Mounts `dir` (e.g. <game>/main). Packs mounted later take priority over earlier mounts.
    // When `language` is non-empty, localized packs for other languages are skipped.
    void mountDirectory(const std::filesystem::path& dir, std::string_view language = {});

    // Paths are matched case-insensitively with '/' or '\\' separators.
    bool exists(std::string_view path) const;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const;

    // Which pack (or loose directory) would serve `path`; empty when not found.
    std::string sourceOf(std::string_view path) const;

    // All visible files (highest priority source wins), sorted by normalized path.
    std::vector<std::string> list(std::string_view prefix = {}) const;

    // Packs in search order, highest priority first.
    std::vector<PackInfo> packs() const;

    static std::string normalize(std::string_view path);

    // Splits "localized_<language>_<rest>.iwd"; returns nullopt for non-localized names.
    static std::optional<std::string> localizedLanguage(std::string_view fileName);

private:
    struct Source {
        std::filesystem::path looseDir;          // non-empty for a loose-file directory
        std::unique_ptr<ZipArchive> archive;     // non-null for a pack
        PackInfo info;
        std::unordered_map<std::string, size_t> index; // normalized path -> entry index
    };

    const Source* find(std::string_view path, size_t* entryIndex) const;

    std::vector<Source> sources_; // highest priority first
};

} // namespace iw2::fs
