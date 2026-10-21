// Decoder selection by file extension.

#include "audio/decode.h"

#include <cctype>
#include <string>

namespace iw2::audio {

Wav decodeSound(std::span<const uint8_t> data, std::string_view name) {
    if (name.size() >= 4) {
        std::string ext(name.substr(name.size() - 4));
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".mp3") return decodeMp3(data);
    }
    return parseWav(data);
}

} // namespace iw2::audio
