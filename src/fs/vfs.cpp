#include "fs/vfs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

namespace iw2::fs {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Sort key for packs inside one directory (see docs/spec/filesystem.md):
// the "localized_" prefix is replaced with ten spaces so localized packs sort first.
std::string packSortKey(const std::string& fileName) {
    std::string key = lower(fileName);
    if (key.rfind("localized_", 0) == 0) key.replace(0, 10, std::string(10, ' '));
    return key;
}

} // namespace

std::string Vfs::normalize(std::string_view path) {
    std::string out = lower(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

std::optional<std::string> Vfs::localizedLanguage(std::string_view fileName) {
    const std::string name = lower(fileName);
    constexpr std::string_view prefix = "localized_";
    if (name.rfind(prefix, 0) != 0) return std::nullopt;
    const size_t end = name.find('_', prefix.size());
    if (end == std::string::npos) return std::string{};
    return name.substr(prefix.size(), end - prefix.size());
}

void Vfs::mountDirectory(const std::filesystem::path& dir, std::string_view language) {
    namespace stdfs = std::filesystem;
    const std::string wantedLanguage = lower(language);

    std::vector<std::pair<std::string, stdfs::path>> packFiles;
    for (const auto& item : stdfs::directory_iterator(dir)) {
        if (!item.is_regular_file()) continue;
        if (lower(item.path().extension().string()) != ".iwd") continue;
        packFiles.emplace_back(packSortKey(item.path().filename().string()), item.path());
    }
    std::sort(packFiles.begin(), packFiles.end());

    // Every pack is pushed in front of the previous ones, so the last pack in sort order wins.
    // Loose files in the directory sit above all of its packs.
    std::vector<Source> mounted;
    for (const auto& [key, path] : packFiles) {
        Source src;
        src.info.path = path;
        if (auto lang = localizedLanguage(path.filename().string())) {
            src.info.localized = true;
            src.info.language = *lang;
            if (!wantedLanguage.empty() && *lang != wantedLanguage) continue;
        }
        src.archive = std::make_unique<ZipArchive>(path);
        const auto& entries = src.archive->entries();
        for (size_t i = 0; i < entries.size(); ++i) src.index[normalize(entries[i].name)] = i;
        src.info.fileCount = src.index.size();
        mounted.push_back(std::move(src));
    }
    std::reverse(mounted.begin(), mounted.end());

    Source loose;
    loose.looseDir = dir;
    loose.info.path = dir;
    mounted.insert(mounted.begin(), std::move(loose));

    for (auto& src : sources_) mounted.push_back(std::move(src));
    sources_ = std::move(mounted);
}

const Vfs::Source* Vfs::find(std::string_view path, size_t* entryIndex) const {
    const std::string key = normalize(path);
    for (const Source& src : sources_) {
        if (src.archive) {
            auto it = src.index.find(key);
            if (it != src.index.end()) {
                if (entryIndex) *entryIndex = it->second;
                return &src;
            }
        } else {
            // Loose files: try the exact path first, then a case-insensitive walk.
            std::filesystem::path candidate = src.looseDir;
            bool ok = true;
            size_t start = 0;
            while (ok && start < key.size()) {
                const size_t slash = key.find('/', start);
                const std::string part = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                start = slash == std::string::npos ? key.size() : slash + 1;
                ok = false;
                std::error_code ec;
                for (const auto& item : std::filesystem::directory_iterator(candidate, ec)) {
                    if (lower(item.path().filename().string()) == part) { candidate = item.path(); ok = true; break; }
                }
            }
            if (ok && std::filesystem::is_regular_file(candidate)) {
                if (entryIndex) *entryIndex = 0;
                return &src;
            }
        }
    }
    return nullptr;
}

bool Vfs::exists(std::string_view path) const { return find(path, nullptr) != nullptr; }

std::optional<std::vector<uint8_t>> Vfs::read(std::string_view path) const {
    size_t index = 0;
    const Source* src = find(path, &index);
    if (!src) return std::nullopt;
    if (src->archive) return src->archive->read(src->archive->entries()[index]);

    // Resolve the loose path the same way find() did.
    std::filesystem::path candidate = src->looseDir;
    const std::string key = normalize(path);
    size_t start = 0;
    while (start < key.size()) {
        const size_t slash = key.find('/', start);
        const std::string part = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? key.size() : slash + 1;
        for (const auto& item : std::filesystem::directory_iterator(candidate)) {
            if (lower(item.path().filename().string()) == part) { candidate = item.path(); break; }
        }
    }
    std::ifstream in(candidate, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Vfs::sourceOf(std::string_view path) const {
    const Source* src = find(path, nullptr);
    return src ? src->info.path.filename().string() : std::string{};
}

std::vector<std::string> Vfs::list(std::string_view prefix) const {
    const std::string p = normalize(prefix);
    std::set<std::string> names;
    for (const Source& src : sources_) {
        if (src.archive) {
            for (const auto& [name, idx] : src.index)
                if (name.rfind(p, 0) == 0) names.insert(name);
        } else {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(src.looseDir, ec);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (!it->is_regular_file() || lower(it->path().extension().string()) == ".iwd") continue;
                const std::string name = normalize(std::filesystem::relative(it->path(), src.looseDir).generic_string());
                if (name.rfind(p, 0) == 0) names.insert(name);
            }
        }
    }
    return {names.begin(), names.end()};
}

std::vector<PackInfo> Vfs::packs() const {
    std::vector<PackInfo> out;
    for (const Source& src : sources_)
        if (src.archive) out.push_back(src.info);
    return out;
}

} // namespace iw2::fs
