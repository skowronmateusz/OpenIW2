// Message buffer with mixed byte / bit packing, implemented from the spec (docs/spec/net.md section 2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace iw2::net {

class Msg {
public:
    Msg(uint8_t* data, int maxsize) : data_(data), maxsize_(maxsize) {}

    // Writing.
    void writeBits(int32_t value, int bits);
    void writeBit0();
    void writeBit1();
    void writeByte(int value);
    void writeShort(int value);
    void writeLong(int32_t value);
    void writeInt64(int64_t value);
    void writeData(const void* src, int len);
    void writeString(const std::string& s);      // limit 1024
    void writeBigString(const std::string& s);   // limit 8192
    void writeAngle8(float degrees);
    void writeAngle16(float degrees);

    // Reading.
    void beginReading() { overflowed_ = false; readcount_ = 0; bit_ = 0; }
    int32_t readBits(int bits);
    int readBit();
    int readByte();
    int readShort();
    int32_t readLong();
    int64_t readInt64();
    void readData(void* dst, int len);
    std::string readString();      // stops at 0 / end, 1024 cap
    std::string readBigString();   // '%' -> '.', 8192 cap
    std::string readStringLine();  // '%' -> '.', stops at '\n', 1024 cap
    float readAngle16();

    bool overflowed() const { return overflowed_; }
    int cursize() const { return cursize_; }
    int readcount() const { return readcount_; }
    int maxsize() const { return maxsize_; }
    int bitPos() const { return bit_; }
    int bitLength() const { return cursize_ * 8 - ((8 - bit_) & 7); }
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    void setCursize(int n) { cursize_ = n; }
    void setReadcount(int n) { readcount_ = n; }
    void clear() { cursize_ = 0; bit_ = 0; overflowed_ = false; readcount_ = 0; }

private:
    std::string readStringImpl(size_t cap, bool percentToDot, bool stopAtNewline);
    void writeStringImpl(const std::string& s, size_t limit);

    bool overflowed_ = false;
    uint8_t* data_;
    int maxsize_;
    int cursize_ = 0;
    int readcount_ = 0;
    int bit_ = 0;
};

// The byte 0x92 is carried as an apostrophe in message strings.
inline char cleanStringChar(char c) { return static_cast<uint8_t>(c) == 0x92 ? '\'' : c; }

} // namespace iw2::net
