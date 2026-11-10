// Implemented from the spec (docs/spec/net.md section 2).
#include "net/msg.h"

#include <cmath>
#include <cstring>

namespace iw2::net {

namespace {
// Starts a new bit byte at cursize when the bit cursor is on a byte boundary.
inline void startBitByte(uint8_t* data, int& cursize, int& bit) {
    if ((bit & 7) == 0) {
        bit = cursize * 8;
        data[cursize] = 0;
        ++cursize;
    }
}
} // namespace

void Msg::writeBits(int32_t value, int bits) {
    if (maxsize_ - cursize_ < 4) {
        overflowed_ = true;
        return;
    }
    uint32_t v = static_cast<uint32_t>(value);
    for (int i = 0; i < bits; ++i) {
        startBitByte(data_, cursize_, bit_);
        if (v & 1u) data_[bit_ >> 3] |= static_cast<uint8_t>(1u << (bit_ & 7));
        ++bit_;
        v >>= 1;
    }
}

void Msg::writeBit0() {
    if (cursize_ >= maxsize_) {
        overflowed_ = true;
        return;
    }
    startBitByte(data_, cursize_, bit_);
    ++bit_;
}

void Msg::writeBit1() {
    if (cursize_ >= maxsize_) {
        overflowed_ = true;
        return;
    }
    startBitByte(data_, cursize_, bit_);
    data_[bit_ >> 3] |= static_cast<uint8_t>(1u << (bit_ & 7));
    ++bit_;
}

void Msg::writeByte(int value) {
    if (cursize_ >= maxsize_) {
        overflowed_ = true;
        return;
    }
    data_[cursize_++] = static_cast<uint8_t>(value);
}

void Msg::writeShort(int value) {
    if (cursize_ + 2 > maxsize_) {
        overflowed_ = true;
        return;
    }
    data_[cursize_] = static_cast<uint8_t>(value);
    data_[cursize_ + 1] = static_cast<uint8_t>(value >> 8);
    cursize_ += 2;
}

void Msg::writeLong(int32_t value) {
    if (cursize_ + 4 > maxsize_) {
        overflowed_ = true;
        return;
    }
    uint32_t v = static_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) data_[cursize_ + i] = static_cast<uint8_t>(v >> (8 * i));
    cursize_ += 4;
}

void Msg::writeInt64(int64_t value) {
    if (cursize_ + 8 > maxsize_) {
        overflowed_ = true;
        return;
    }
    uint64_t v = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; ++i) data_[cursize_ + i] = static_cast<uint8_t>(v >> (8 * i));
    cursize_ += 8;
}

void Msg::writeData(const void* src, int len) {
    if (cursize_ + len > maxsize_) {
        overflowed_ = true;
        return;
    }
    std::memcpy(data_ + cursize_, src, static_cast<size_t>(len));
    cursize_ += len;
}

void Msg::writeStringImpl(const std::string& s, size_t limit) {
    if (s.size() >= limit) {
        uint8_t zero = 0;
        writeData(&zero, 1);
        return;
    }
    std::string clean(s);
    for (char& c : clean) c = cleanStringChar(c);
    writeData(clean.c_str(), static_cast<int>(clean.size() + 1));
}

void Msg::writeString(const std::string& s) { writeStringImpl(s, 1024); }
void Msg::writeBigString(const std::string& s) { writeStringImpl(s, 8192); }

void Msg::writeAngle8(float degrees) {
    writeByte(static_cast<int>(std::lround(degrees * 256.0f / 360.0f)) & 0xff);
}

void Msg::writeAngle16(float degrees) {
    writeShort(static_cast<int>(std::lround(degrees * 182.04445f)) & 0xffff);
}

int32_t Msg::readBits(int bits) {
    uint32_t value = 0;
    for (int i = 0; i < bits; ++i) {
        if ((bit_ & 7) == 0) {
            if (readcount_ >= cursize_) {
                overflowed_ = true;
                return -1;
            }
            bit_ = readcount_ * 8;
            ++readcount_;
        }
        value |= static_cast<uint32_t>((data_[bit_ >> 3] >> (bit_ & 7)) & 1) << i;
        ++bit_;
    }
    return static_cast<int32_t>(value);
}

int Msg::readBit() {
    if ((bit_ & 7) == 0) {
        if (readcount_ >= cursize_) {
            overflowed_ = true;
            return -1;
        }
        bit_ = readcount_ * 8;
        ++readcount_;
    }
    int b = (data_[bit_ >> 3] >> (bit_ & 7)) & 1;
    ++bit_;
    return b;
}

int Msg::readByte() {
    if (readcount_ >= cursize_) {
        overflowed_ = true;
        return -1;
    }
    return data_[readcount_++];
}

int Msg::readShort() {
    if (readcount_ + 2 > cursize_) {
        overflowed_ = true;
        return -1;
    }
    int16_t v = static_cast<int16_t>(data_[readcount_] | (data_[readcount_ + 1] << 8));
    readcount_ += 2;
    return v;
}

int32_t Msg::readLong() {
    if (readcount_ + 4 > cursize_) {
        overflowed_ = true;
        return -1;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[readcount_ + i]) << (8 * i);
    readcount_ += 4;
    return static_cast<int32_t>(v);
}

int64_t Msg::readInt64() {
    if (readcount_ + 8 > cursize_) {
        overflowed_ = true;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[readcount_ + i]) << (8 * i);
    readcount_ += 8;
    return static_cast<int64_t>(v);
}

void Msg::readData(void* dst, int len) {
    if (readcount_ + len > cursize_) {
        overflowed_ = true;
        std::memset(dst, 0xff, static_cast<size_t>(len));
        return;
    }
    std::memcpy(dst, data_ + readcount_, static_cast<size_t>(len));
    readcount_ += len;
}

std::string Msg::readStringImpl(size_t cap, bool percentToDot, bool stopAtNewline) {
    std::string out;
    for (;;) {
        int c = readByte();
        if (c == -1) c = 0;
        if (percentToDot && c == '%') c = '.';
        if (stopAtNewline && c == '\n') c = 0;
        if (c == 0) break;
        if (out.size() < cap - 1) out.push_back(cleanStringChar(static_cast<char>(c)));
    }
    return out;
}

std::string Msg::readString() { return readStringImpl(1024, false, false); }
std::string Msg::readBigString() { return readStringImpl(8192, true, false); }
std::string Msg::readStringLine() { return readStringImpl(1024, true, true); }

float Msg::readAngle16() { return static_cast<float>(readShort()) * (360.0f / 65536.0f); }

} // namespace iw2::net
