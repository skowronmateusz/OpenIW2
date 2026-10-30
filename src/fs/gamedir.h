// Game folder checks and app start-up helpers for iw2view started without arguments (from Finder / the .app).
// No SDL here, so the rules are unit-tested (tests/test_gamedir.cpp).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iw2::fs {

struct GameDirCheck {
    std::vector<std::string> missing; // file names (lower case) that must be added to the folder
    std::string language;             // the first complete localized set, in name order; empty when there is none
    bool ok() const { return missing.empty() && !language.empty(); }
};

// Checks a `main` folder: iw_00.iwd .. iw_15.iwd and at least one complete localized_<language>_iw00.iwd .. iw10.iwd set.
// Names are compared case-insensitively (files copied from Windows may differ in case).
// TODO(spec): no cheap, proven marker of patch 1.3 is known, so the patch level is not checked.
GameDirCheck checkGameDir(const std::filesystem::path& dir);

// settings.cfg in the app's preference folder: one `mainPath=<folder>` line (other lines are kept for later keys).
std::optional<std::filesystem::path> readMainPath(const std::filesystem::path& settingsFile);
bool writeMainPath(const std::filesystem::path& settingsFile, const std::filesystem::path& mainPath);

// Moves <log> to <log>.1 (replacing an older .1) and returns false when the folder cannot be created.
bool rotateLog(const std::filesystem::path& log);

} // namespace iw2::fs
