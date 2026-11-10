// Sequenced packet channel with fragmentation, implemented from the spec (docs/spec/net.md section 4).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "net/msg.h"

namespace iw2::net {

constexpr int kMaxMsgLen = 0x20000;
constexpr int kFragmentSize = 1300;
constexpr int kMaxPacketLen = 1400;
constexpr uint32_t kFragmentBit = 0x80000000u;

enum class NetSide { Client = 0, Server = 1 };

class Netchan {
public:
    using SendFn = std::function<void(const uint8_t* data, int len)>;

    Netchan(NetSide side, int qport, SendFn send);

    // Sends a message; a long one is split into fragments and only the first one is sent here. Returns false on a
    // message larger than kMaxMsgLen.
    bool transmit(const uint8_t* data, int len);
    // Sends the next pending fragment.
    void transmitNextFragment();
    bool hasPendingFragments() const { return unsentFragments_; }

    // Processes a received packet in place. Returns true when msg holds a complete message, positioned after the
    // header (for a reassembled message the read position follows the leading sequence long).
    bool process(Msg& msg);

    int outgoingSequence() const { return outgoingSequence_; }
    int incomingSequence() const { return incomingSequence_; }
    int dropped() const { return dropped_; }
    int qport() const { return qport_; }

private:
    NetSide side_;
    int qport_;
    SendFn send_;
    int outgoingSequence_ = 1;
    int incomingSequence_ = 0;
    int dropped_ = 0;

    int fragmentSequence_ = 0;
    int fragmentLength_ = 0;
    std::vector<uint8_t> fragmentBuffer_;

    bool unsentFragments_ = false;
    int unsentFragmentStart_ = 0;
    int unsentLength_ = 0;
    std::vector<uint8_t> unsentBuffer_;
};

} // namespace iw2::net
