// Implemented from the spec (docs/spec/net.md section 4).
#include "net/netchan.h"

#include <cstring>

namespace iw2::net {

Netchan::Netchan(NetSide side, int qport, SendFn send)
    : side_(side), qport_(qport), send_(std::move(send)), fragmentBuffer_(kMaxMsgLen), unsentBuffer_(kMaxMsgLen) {}

void Netchan::transmitNextFragment() {
    uint8_t buf[kMaxPacketLen];
    Msg m(buf, sizeof buf);
    m.writeLong(static_cast<int32_t>(static_cast<uint32_t>(outgoingSequence_) | kFragmentBit));
    if (side_ == NetSide::Client) m.writeShort(qport_);
    int len = kFragmentSize;
    if (unsentFragmentStart_ + len > unsentLength_) len = unsentLength_ - unsentFragmentStart_;
    m.writeLong(unsentFragmentStart_);
    m.writeShort(len);
    m.writeData(unsentBuffer_.data() + unsentFragmentStart_, len);
    send_(buf, m.cursize());
    unsentFragmentStart_ += len;
    // A final piece of exactly kFragmentSize bytes is followed by an empty one.
    if (unsentFragmentStart_ == unsentLength_ && len != kFragmentSize) {
        ++outgoingSequence_;
        unsentFragments_ = false;
    }
}

bool Netchan::transmit(const uint8_t* data, int len) {
    if (len > kMaxMsgLen) return false;
    unsentFragmentStart_ = 0;
    if (len >= kFragmentSize) {
        unsentFragments_ = true;
        unsentLength_ = len;
        std::memcpy(unsentBuffer_.data(), data, static_cast<size_t>(len));
        transmitNextFragment();
        return true;
    }
    uint8_t buf[kMaxPacketLen];
    Msg m(buf, sizeof buf);
    m.writeLong(outgoingSequence_);
    ++outgoingSequence_;
    if (side_ == NetSide::Client) m.writeShort(qport_);
    m.writeData(data, len);
    send_(buf, m.cursize());
    return true;
}

bool Netchan::process(Msg& msg) {
    msg.beginReading();
    uint32_t sequence = static_cast<uint32_t>(msg.readLong());
    bool fragmented = (sequence & kFragmentBit) != 0;
    sequence &= ~kFragmentBit;
    if (side_ == NetSide::Server) msg.readShort();  // qport, already matched by the caller
    int fragmentStart = 0, fragmentLength = 0;
    if (fragmented) {
        fragmentStart = msg.readLong();
        fragmentLength = msg.readShort();
    }
    const int seq = static_cast<int>(sequence);
    if (seq <= incomingSequence_) return false;  // out of order or duplicate
    dropped_ = seq - (incomingSequence_ + 1);

    if (fragmented) {
        if (seq != fragmentSequence_) {
            fragmentSequence_ = seq;
            fragmentLength_ = 0;
        }
        if (fragmentStart != fragmentLength_) return false;  // a piece was lost
        if (fragmentLength < 0 || msg.readcount() + fragmentLength > msg.cursize() ||
            fragmentLength_ + fragmentLength > kMaxMsgLen)
            return false;
        std::memcpy(fragmentBuffer_.data() + fragmentLength_, msg.data() + msg.readcount(),
                    static_cast<size_t>(fragmentLength));
        fragmentLength_ += fragmentLength;
        if (fragmentLength == kFragmentSize) return false;  // more to come
        if (fragmentLength_ + 4 > msg.maxsize()) return false;
        Msg rebuilt(msg.data(), msg.maxsize());
        rebuilt.writeLong(seq);
        rebuilt.writeData(fragmentBuffer_.data(), fragmentLength_);
        msg.setCursize(rebuilt.cursize());
        fragmentLength_ = 0;
        msg.beginReading();
        msg.readLong();
    }
    incomingSequence_ = seq;
    return true;
}

} // namespace iw2::net
