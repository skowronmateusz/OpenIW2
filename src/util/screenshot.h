// Screenshot file naming of the original renderer (docs/spec/controls.md, section Screenshots).
#pragma once

#include <filesystem>
#include <string>

namespace iw2::util {

// The name of the next screenshot in `dir`: shotNNNN.<ext> with the first free number 0000..9999, else shot9999
// (which then overwrites), as the renderer DLL names its files. `ext` is given without the dot ("jpg", "png").
std::filesystem::path screenshotPath(const std::filesystem::path& dir, const std::string& ext);

} // namespace iw2::util
