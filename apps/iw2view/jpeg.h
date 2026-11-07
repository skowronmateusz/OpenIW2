// JPEG encoding with macOS ImageIO, for the in-game screenshot command (docs/spec/controls.md).
#pragma once

#include <cstdint>
#include <filesystem>

namespace iw2view {

// Writes `rgba` (top row first, 4 bytes per pixel, alpha ignored) as a JPEG of `qualityPercent`.
// Returns false when the file or the encoder could not be created.
bool writeJpegRgba(const std::filesystem::path& path, const uint8_t* rgba, int width, int height, int qualityPercent);

} // namespace iw2view
