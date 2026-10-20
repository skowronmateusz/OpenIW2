// Minimal PNG writer (RGBA8, no filtering) for debug and conversion tools.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace iw2::util {

std::vector<uint8_t> encodePngRgba(const uint8_t* rgba, int width, int height);
void writePngRgba(const std::filesystem::path& path, const uint8_t* rgba, int width, int height);

} // namespace iw2::util
