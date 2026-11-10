// Implemented from the spec (docs/spec/net.md sections 6-7).
#include "net/delta.h"

#include <cmath>
#include <cstring>

namespace iw2::net {

namespace {

uint32_t mask(int bits) { return bits >= 32 ? 0xffffffffu : (1u << bits) - 1u; }

// Keyed value: written as value ^ key in the given number of bits.
void writeKeyed(Msg& msg, uint32_t key, uint32_t value, int bits) {
    msg.writeBits(static_cast<int32_t>(key ^ value), bits);
}
uint32_t readKeyed(Msg& msg, uint32_t key, int bits) {
    return static_cast<uint32_t>(msg.readBits(bits)) ^ (mask(bits) & key);
}

void writeKeyedDelta(Msg& msg, uint32_t key, uint32_t from, uint32_t to, int bits) {
    if (from == to) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    writeKeyed(msg, key, to, bits);
}
uint32_t readKeyedDelta(Msg& msg, uint32_t key, uint32_t from, int bits) {
    if (msg.readBit() == 0) return from;
    return readKeyed(msg, key, bits);
}

void writeKeyedDelta16(Msg& msg, uint32_t key, int32_t from, int32_t to) {
    if (static_cast<int16_t>(from) == static_cast<int16_t>(to)) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    msg.writeShort(static_cast<int>(key ^ static_cast<uint32_t>(to)));
}
int32_t readKeyedDelta16(Msg& msg, uint32_t key, int32_t from) {
    if (msg.readBit() == 0) return from;
    uint16_t v = static_cast<uint16_t>(msg.readShort());
    return static_cast<int16_t>(static_cast<uint16_t>(key) ^ v);
}

} // namespace

int moveClass(int forward, int right) {
    int cls = 0;
    if (forward >= 11) cls = 1;
    else if (forward < -10) cls = 2;
    if (right >= 11) cls |= 4;
    else if (right < -10) cls |= 8;
    return cls;
}

void moveFromClass(int cls, int8_t& forward, int8_t& right) {
    forward = (cls & 1) ? 127 : (cls & 2) ? -127 : 0;
    right = (cls & 4) ? 127 : (cls & 8) ? -127 : 0;
}

void writeDeltaUsercmd(Msg& msg, uint32_t key, const UserCmd& from, const UserCmd& to) {
    uint32_t dt = static_cast<uint32_t>(to.serverTime) - static_cast<uint32_t>(from.serverTime);
    if (dt < 256) {
        msg.writeBit1();
        msg.writeByte(static_cast<int>(dt));
    } else {
        msg.writeBit0();
        msg.writeLong(to.serverTime);
    }
    const int toMove = moveClass(to.forwardmove, to.rightmove);
    const int fromMove = moveClass(from.forwardmove, from.rightmove);
    const bool majorSame = (from.buttons >> 1) == (to.buttons >> 1) && from.weapon == to.weapon &&
                           from.offhand == to.offhand && from.angles[2] == to.angles[2];
    if (majorSame) {
        if (from.angles[0] == to.angles[0] && from.angles[1] == to.angles[1] &&
            (from.buttons & 1) == (to.buttons & 1) && fromMove == toMove) {
            writeKeyed(msg, key, 0, 1);  // unchanged
            return;
        }
        writeKeyed(msg, key, 1, 1);  // changed
        writeKeyed(msg, key, 0, 1);  // minor
        key ^= static_cast<uint32_t>(to.serverTime);
        writeKeyed(msg, key, static_cast<uint32_t>(to.buttons), 1);
        writeKeyedDelta16(msg, key, from.angles[0], to.angles[0]);
        writeKeyedDelta16(msg, key, from.angles[1], to.angles[1]);
        writeKeyedDelta(msg, key, static_cast<uint32_t>(fromMove), static_cast<uint32_t>(toMove), 4);
        return;
    }
    writeKeyed(msg, key, 1, 1);  // changed
    writeKeyed(msg, key, 1, 1);  // major
    writeKeyed(msg, key, static_cast<uint32_t>(to.buttons), 1);
    writeKeyedDelta16(msg, key, from.angles[0], to.angles[0]);
    writeKeyedDelta16(msg, key, from.angles[1], to.angles[1]);
    writeKeyedDelta(msg, key, static_cast<uint32_t>(fromMove), static_cast<uint32_t>(toMove), 4);
    key ^= static_cast<uint32_t>(to.serverTime);
    writeKeyedDelta16(msg, key, from.angles[2], to.angles[2]);
    writeKeyedDelta(msg, key, static_cast<uint32_t>(from.buttons >> 1), static_cast<uint32_t>(to.buttons >> 1), 18);
    writeKeyedDelta(msg, key, from.weapon, to.weapon, 7);
    writeKeyedDelta(msg, key, from.offhand, to.offhand, 7);
}

void readDeltaUsercmd(Msg& msg, uint32_t key, const UserCmd& from, UserCmd& to) {
    to = from;
    if (msg.readBit() == 0) to.serverTime = msg.readLong();
    else to.serverTime = from.serverTime + msg.readByte();
    if (readKeyed(msg, key, 1) == 0) return;
    to.buttons &= ~1;
    const bool major = readKeyed(msg, key, 1) != 0;
    if (!major) key ^= static_cast<uint32_t>(to.serverTime);
    to.buttons |= static_cast<int32_t>(readKeyed(msg, key, 1));
    to.angles[0] = readKeyedDelta16(msg, key, from.angles[0]) & 0xffff;
    to.angles[1] = readKeyedDelta16(msg, key, from.angles[1]) & 0xffff;
    int cls = static_cast<int>(readKeyedDelta(msg, key, static_cast<uint32_t>(moveClass(from.forwardmove, from.rightmove)), 4));
    moveFromClass(cls, to.forwardmove, to.rightmove);
    if (!major) return;
    key ^= static_cast<uint32_t>(to.serverTime);
    to.angles[2] = readKeyedDelta16(msg, key, from.angles[2]) & 0xffff;
    to.buttons = (to.buttons & 1) |
                 static_cast<int32_t>(readKeyedDelta(msg, key, static_cast<uint32_t>(from.buttons >> 1), 18) << 1);
    to.weapon = static_cast<uint8_t>(readKeyedDelta(msg, key, from.weapon, 7));
    to.offhand = static_cast<uint8_t>(readKeyedDelta(msg, key, from.offhand, 7));
}

namespace {

int32_t loadInt(const uint8_t* base, int offset) {
    int32_t v;
    std::memcpy(&v, base + offset, 4);
    return v;
}
float loadFloat(const uint8_t* base, int offset) {
    float v;
    std::memcpy(&v, base + offset, 4);
    return v;
}
void storeInt(uint8_t* base, int offset, int32_t v) { std::memcpy(base + offset, &v, 4); }
void storeFloat(uint8_t* base, int offset, float v) { std::memcpy(base + offset, &v, 4); }

// Float with an integer shortcut: [-bias, bias) sent as (v + bias) in lowBits bits plus one byte.
void writeFloatShortcut(Msg& msg, float f, int bias, int lowBits) {
    if (f == 0.0f) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    int i = static_cast<int>(std::lround(f));
    if (static_cast<float>(i) == f && i + bias >= 0 && i + bias < 2 * bias) {
        msg.writeBit0();
        msg.writeBits(i + bias, lowBits);
        msg.writeByte((i + bias) >> lowBits);
    } else {
        msg.writeBit1();
        int32_t raw;
        std::memcpy(&raw, &f, 4);
        msg.writeLong(raw);
    }
}
float readFloatShortcut(Msg& msg, int bias, int lowBits) {
    if (msg.readBit() == 0) return 0.0f;
    if (msg.readBit() == 0) {
        int low = msg.readBits(lowBits);
        int high = msg.readByte();
        return static_cast<float>((low | (high << lowBits)) - bias);
    }
    int32_t raw = msg.readLong();
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

} // namespace

void writeDeltaField(Msg& msg, const uint8_t* from, const uint8_t* to, const NetField& field) {
    if (loadInt(from, field.offset) == loadInt(to, field.offset)) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    switch (field.bits) {
    case kFieldFloat: writeFloatShortcut(msg, loadFloat(to, field.offset), 4096, 5); return;
    case kFieldSmallFloat: writeFloatShortcut(msg, loadFloat(to, field.offset), 512, 2); return;
    case kFieldAngle: {
        float f = loadFloat(to, field.offset);
        if (f == 0.0f) {
            msg.writeBit0();
        } else {
            msg.writeBit1();
            msg.writeAngle16(f);
        }
        return;
    }
    default: break;
    }
    int32_t v = loadInt(to, field.offset);
    if (v == 0) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    int bits = field.bits < 0 ? -field.bits : field.bits;
    int low = bits & 7;
    if (low) {
        msg.writeBits(v, low);
        v >>= low;
        bits -= low;
    }
    for (; bits > 0; bits -= 8) {
        msg.writeByte(v & 0xff);
        v >>= 8;
    }
}

void readDeltaField(Msg& msg, const uint8_t* from, uint8_t* to, const NetField& field) {
    if (msg.readBit() == 0) {
        std::memcpy(to + field.offset, from + field.offset, 4);
        return;
    }
    switch (field.bits) {
    case kFieldFloat: storeFloat(to, field.offset, readFloatShortcut(msg, 4096, 5)); return;
    case kFieldSmallFloat: storeFloat(to, field.offset, readFloatShortcut(msg, 512, 2)); return;
    case kFieldAngle:
        storeFloat(to, field.offset, msg.readBit() == 0 ? 0.0f : msg.readAngle16());
        return;
    default: break;
    }
    if (msg.readBit() == 0) {
        storeInt(to, field.offset, 0);
        return;
    }
    int bits = field.bits < 0 ? -field.bits : field.bits;
    int low = bits & 7;
    uint32_t v = 0;
    int shift = 0;
    if (low) {
        v = static_cast<uint32_t>(msg.readBits(low)) & mask(low);
        shift = low;
        bits -= low;
    }
    for (; bits > 0; bits -= 8, shift += 8) v |= static_cast<uint32_t>(msg.readByte() & 0xff) << shift;
    // TODO(spec): sign extension of negative-coded (signed) integer fields on read (E2, player-state reader).
    storeInt(to, field.offset, static_cast<int32_t>(v));
}

void writeDeltaStruct(Msg& msg, const uint8_t* from, const uint8_t* to, bool force, const NetField* fields,
                      int fieldCount, int number, int numberBits, bool leadingBit) {
    if (!to) {
        if (leadingBit) msg.writeBit1();
        msg.writeBits(number, numberBits);
        msg.writeBit1();
        return;
    }
    int lc = 0;
    for (int i = 0; i < fieldCount; ++i)
        if (loadInt(from, fields[i].offset) != loadInt(to, fields[i].offset)) lc = i + 1;
    if (lc == 0) {
        if (!force) return;
        if (leadingBit) msg.writeBit1();
        msg.writeBits(number, numberBits);
        msg.writeBit0();
        msg.writeBit0();
        return;
    }
    if (leadingBit) msg.writeBit1();
    msg.writeBits(number, numberBits);
    msg.writeBit0();
    msg.writeBit1();
    msg.writeByte(lc);
    for (int i = 0; i < lc; ++i) writeDeltaField(msg, from, to, fields[i]);
}

void writeDeltaFieldList(Msg& msg, const uint8_t* from, const uint8_t* to, const NetField* fields, int fieldCount,
                         bool force) {
    bool changed = force;
    for (int i = 0; i < fieldCount && !changed; ++i)
        changed = loadInt(from, fields[i].offset) != loadInt(to, fields[i].offset);
    if (!changed) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    for (int i = 0; i < fieldCount; ++i) writeDeltaField(msg, from, to, fields[i]);
}

void readDeltaFieldList(Msg& msg, const uint8_t* from, uint8_t* to, const NetField* fields, int fieldCount) {
    if (msg.readBit() == 0) {
        for (int i = 0; i < fieldCount; ++i) std::memcpy(to + fields[i].offset, from + fields[i].offset, 4);
        return;
    }
    for (int i = 0; i < fieldCount; ++i) readDeltaField(msg, from, to, fields[i]);
}

StructDelta readDeltaStructBody(Msg& msg, const uint8_t* from, uint8_t* to, int structSize, const NetField* fields,
                                int fieldCount) {
    std::memcpy(to, from, static_cast<size_t>(structSize));
    if (msg.readBit() == 1) return StructDelta::Removed;
    if (msg.readBit() == 0) return StructDelta::Unchanged;
    int lc = msg.readByte();
    if (lc > fieldCount) lc = fieldCount;  // TODO(spec): the reader's handling of a bad count
    for (int i = 0; i < lc; ++i) readDeltaField(msg, from, to, fields[i]);
    return StructDelta::Changed;
}

} // namespace iw2::net
