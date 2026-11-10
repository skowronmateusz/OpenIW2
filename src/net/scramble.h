// Per-byte XOR scrambling of game messages, implemented from the spec (docs/spec/net.md section 4).
#pragma once

#include <cstdint>
#include <string>

namespace iw2::net {

// XORs data[0..len) in place with a running 8-bit key. The key starts at `key` and, before each byte i, is XORed
// with the next character of `text` shifted left by (i & 1); the text restarts at its end. The operation is its
// own inverse.
void scramble(uint8_t* data, int len, uint8_t key, const std::string& text);

// Initial key of a server-to-client message: low bytes of the challenge and the netchan's outgoing sequence.
inline uint8_t serverToClientKey(int32_t challenge, int32_t sequence) {
    return static_cast<uint8_t>(challenge ^ sequence);
}
// Initial key of a client-to-server message: low bytes of the challenge, the server id byte and the message
// acknowledge.
inline uint8_t clientToServerKey(int32_t challenge, int serverId, int32_t messageAcknowledge) {
    return static_cast<uint8_t>(challenge ^ serverId ^ messageAcknowledge);
}

// Hash of a reliable command text (at most maxLen characters) that is part of the user-command key.
int32_t commandHashKey(const std::string& text, int maxLen = 32);

} // namespace iw2::net
