// Platforms without an MP3 decoder (Linux builds of the headless tools such as iw2server, which never play sound).

#include "audio/decode.h"

#include <stdexcept>

namespace iw2::audio {

Wav decodeMp3(std::span<const uint8_t>) { throw std::runtime_error("mp3 decoding is not available on this platform"); }

} // namespace iw2::audio
