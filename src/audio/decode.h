// Compressed sound decoding (docs/spec/sound.md). The platform decoder lives in decode_apple.cpp.
#pragma once

#include "audio/sound.h"

#include <span>
#include <string_view>

namespace iw2::audio {

// Decodes an MPEG audio (.mp3) file to interleaved 16-bit PCM at its own rate and channel count.
// Throws std::runtime_error when the data cannot be decoded.
Wav decodeMp3(std::span<const uint8_t> data);

// Picks the decoder by the file name's extension (.mp3, otherwise RIFF WAVE).
Wav decodeSound(std::span<const uint8_t> data, std::string_view fileName);

} // namespace iw2::audio
