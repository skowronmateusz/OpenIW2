// An independent decoder of the server-to-client stream written from docs/spec/net.md (see specdecoder.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace specdec {

class Decoder {
public:
    int challenge = 0;
    // The client's reliable command with this sequence number (the unscrambling string).
    std::function<std::string(int)> clientCommandText;

    // One UDP packet from the server (not connectionless). False on a decode error (see err).
    bool packet(const uint8_t* data, int len);

    struct Frame {
        int messageNum = 0, serverTime = 0, snapFlags = 0;
        std::vector<uint8_t> ps;
        std::map<int, std::vector<uint8_t>> entities, clients;
    };
    std::map<int, std::string> configstrings;
    std::map<int, std::vector<uint8_t>> baselines;
    std::map<int, Frame> frames;  // the last 32 by message number
    std::string commands[128];
    int commandCount = 0;
    int clientNum = -1, checksumFeed = 0, snapshots = 0;
    bool haveGamestate = false;
    std::string err;

private:
    bool gamestate(void* reader);
    bool snapshot(void* reader, int messageNum);
    bool error(const std::string& e);
    int incoming_ = 0, fragSeq_ = -1, commandSequence_ = 0;
    std::vector<uint8_t> frag_;
};

}  // namespace specdec
