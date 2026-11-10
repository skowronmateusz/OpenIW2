// Implemented from the spec (docs/spec/net.md section 4).
#include "net/scramble.h"

namespace iw2::net {

void scramble(uint8_t* data, int len, uint8_t key, const std::string& text) {
    size_t index = 0;
    for (int i = 0; i < len; ++i) {
        if (index >= text.size()) index = 0;
        const unsigned ch = index < text.size() ? static_cast<uint8_t>(text[index]) : 0u;
        key = static_cast<uint8_t>(key ^ (ch << (i & 1)));
        ++index;
        data[i] ^= key;
    }
}

int32_t commandHashKey(const std::string& text, int maxLen) {
    int32_t hash = 0;
    for (int i = 0; i < maxLen && i < static_cast<int>(text.size()) && text[i]; ++i)
        hash += static_cast<int32_t>(static_cast<signed char>(text[i])) * (i + 119);
    return hash ^ (hash >> 10) ^ (hash >> 20);
}

} // namespace iw2::net
