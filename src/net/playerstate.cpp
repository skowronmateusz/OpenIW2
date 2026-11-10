// Implemented from the spec (docs/spec/net.md section 7.3).
#include "net/playerstate.h"

#include <cstring>
#include <vector>

#include "net/delta.h"
#include "net/netfields.h"

namespace iw2::net {

namespace {

int32_t ld(const uint8_t* p, int off) {
    int32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}
void st(uint8_t* p, int off, int32_t v) { std::memcpy(p + off, &v, 4); }

// Player-state fields have no "zero" shortcut for integers and floats.
void writePsField(Msg& msg, const uint8_t* from, const uint8_t* to, const NetField& f) {
    int32_t v = ld(to, f.offset);
    if (v == ld(from, f.offset)) {
        msg.writeBit0();
        return;
    }
    msg.writeBit1();
    if (f.bits == kFieldFloat) {
        float fl;
        std::memcpy(&fl, &v, 4);
        int i = static_cast<int>(fl);
        if (static_cast<float>(i) == fl && i + 4096 >= 0 && i + 4096 <= 0x1fff) {
            msg.writeBit0();
            msg.writeBits(i + 4096, 5);
            msg.writeByte((i + 4096) >> 5);
        } else {
            msg.writeBit1();
            msg.writeLong(v);
        }
        return;
    }
    if (f.bits == kFieldAngle) {
        if (v == 0) {
            msg.writeBit0();
        } else {
            msg.writeBit1();
            float fl;
            std::memcpy(&fl, &v, 4);
            msg.writeAngle16(fl);
        }
        return;
    }
    int bits = f.bits < 0 ? -f.bits : f.bits;
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

void readPsField(Msg& msg, const uint8_t* from, uint8_t* to, const NetField& f) {
    if (msg.readBit() == 0) {
        st(to, f.offset, ld(from, f.offset));
        return;
    }
    if (f.bits == kFieldFloat) {
        if (msg.readBit() == 0) {
            int low = msg.readBits(5) & 31;
            int high = msg.readByte();
            float fl = static_cast<float>(low + (high - 128) * 32);
            std::memcpy(to + f.offset, &fl, 4);
        } else {
            st(to, f.offset, msg.readLong());
        }
        return;
    }
    if (f.bits == kFieldAngle) {
        float fl = msg.readBit() == 0 ? 0.0f : msg.readAngle16();
        std::memcpy(to + f.offset, &fl, 4);
        return;
    }
    bool sign = f.bits < 0;
    int bits = sign ? -f.bits : f.bits;
    int low = bits & 7;
    uint32_t v = low ? static_cast<uint32_t>(msg.readBits(low)) & ((1u << low) - 1u) : 0u;
    for (int shift = low; shift < bits; shift += 8) v |= static_cast<uint32_t>(msg.readByte() & 0xff) << shift;
    if (sign && bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1u);
    st(to, f.offset, static_cast<int32_t>(v));
}

// Hud elements: count (5 bits) of leading elements whose type is non-zero, then per element the index of the
// last changed field (5 bits) and the fields up to it with the entity field coder.
void writeHudElems(Msg& msg, const uint8_t* from, const uint8_t* to) {
    int count = 0;
    while (count < kPsHudElemCount && ld(to, count * kHudElemSize) != 0) ++count;
    msg.writeBits(count, 5);
    for (int e = 0; e < count; ++e) {
        const uint8_t* a = from + e * kHudElemSize;
        const uint8_t* b = to + e * kHudElemSize;
        int last = 0;
        for (int i = 0; i < 32; ++i)
            if (ld(a, kHudElemFields[i].offset) != ld(b, kHudElemFields[i].offset)) last = i;
        msg.writeBits(last, 5);
        for (int i = 0; i <= last; ++i) writeDeltaField(msg, a, b, kHudElemFields[i]);
    }
}

void readHudElems(Msg& msg, const uint8_t* from, uint8_t* to) {
    int count = msg.readBits(5) & 31;
    for (int e = 0; e < count && e < kPsHudElemCount; ++e) {
        const uint8_t* a = from + e * kHudElemSize;
        uint8_t* b = to + e * kHudElemSize;
        int last = msg.readBits(5) & 31;
        for (int i = 0; i <= last; ++i) readDeltaField(msg, a, b, kHudElemFields[i]);
    }
    // TODO(spec): elements at and after count are cleared by the reader (type 0).
    for (int e = count; e < kPsHudElemCount; ++e) std::memset(to + e * kHudElemSize, 0, kHudElemSize);
}

void writeShortGroups(Msg& msg, const uint8_t* from, const uint8_t* to, int offset) {
    for (int g = 0; g < 4; ++g) {
        int mask = 0;
        for (int i = 0; i < 16; ++i) {
            int off = offset + (g * 16 + i) * 4;
            if (ld(from, off) != ld(to, off)) mask |= 1 << i;
        }
        if (!mask) {
            msg.writeBit0();
            continue;
        }
        msg.writeBit1();
        msg.writeShort(mask);
        for (int i = 0; i < 16; ++i)
            if (mask & (1 << i)) msg.writeShort(ld(to, offset + (g * 16 + i) * 4));
    }
}

void readShortGroups(Msg& msg, uint8_t* to, int offset) {
    for (int g = 0; g < 4; ++g) {
        if (!msg.readBit()) continue;
        int mask = msg.readShort();
        for (int i = 0; i < 16; ++i)
            if (mask & (1 << i)) st(to, offset + (g * 16 + i) * 4, msg.readShort());
    }
}

} // namespace

UserCmd usercmdBase(const uint8_t* ps) {
    UserCmd base;
    base.weapon = ps[212];
    base.offhand = ps[208];
    for (int i = 0; i < 2; ++i) {
        float view;
        std::memcpy(&view, ps + 232 + 4 * i, 4);
        const int a = static_cast<int>(view * (65536.0f / 360.0f)) & 0xffff;
        base.angles[i] = (a - ld(ps, 84 + 4 * i)) & 0xffff;
    }
    if (ld(ps, 12) & 0x800000) {
        const int32_t eflags = ld(ps, 160);
        if (eflags & 8) base.buttons |= 0x100;
        else if (eflags & 4) base.buttons |= 0x200;
    }
    float lean, posFrac;
    std::memcpy(&lean, ps + 76, 4);
    std::memcpy(&posFrac, ps + 220, 4);
    if (lean > 0.0f) base.buttons |= 0x80;
    else if (lean < 0.0f) base.buttons |= 0x40;
    if (posFrac != 0.0f) base.buttons |= 0x1000;
    return base;
}

void writeDeltaPlayerState(Msg& msg, const uint8_t* fromIn, const uint8_t* to) {
    std::vector<uint8_t> zero;
    const uint8_t* from = fromIn;
    if (!from) {
        zero.assign(kPlayerStateSize, 0);
        from = zero.data();
    }
    int lc = 0;
    for (int i = 0; i < 105; ++i)
        if (ld(from, kPlayerStateFields[i].offset) != ld(to, kPlayerStateFields[i].offset)) lc = i + 1;
    msg.writeByte(lc);
    for (int i = 0; i < lc; ++i) writePsField(msg, from, to, kPlayerStateFields[i]);

    // Stats: 6-bit mask; 0-2 and 4 as shorts, 3 in 6 bits, 5 as a byte.
    int statsMask = 0;
    for (int i = 0; i < 6; ++i)
        if (ld(from, kPsStatsOffset + 4 * i) != ld(to, kPsStatsOffset + 4 * i)) statsMask |= 1 << i;
    if (!statsMask) {
        msg.writeBit0();
    } else {
        msg.writeBit1();
        msg.writeBits(statsMask, 6);
        for (int i = 0; i < 6; ++i) {
            if (!(statsMask & (1 << i))) continue;
            int32_t v = ld(to, kPsStatsOffset + 4 * i);
            if (i == 3) msg.writeBits(v, 6);
            else if (i == 5) msg.writeByte(v);
            else msg.writeShort(v);
        }
    }
    // Ammo: one bit for the whole array, then the groups. Clips: the groups only.
    if (std::memcmp(from + kPsAmmoOffset, to + kPsAmmoOffset, 256) == 0) {
        msg.writeBit0();
    } else {
        msg.writeBit1();
        writeShortGroups(msg, from, to, kPsAmmoOffset);
    }
    writeShortGroups(msg, from, to, kPsAmmoClipOffset);
    // Objectives.
    if (std::memcmp(from + kPsObjectivesOffset, to + kPsObjectivesOffset, kPsObjectiveCount * kObjectiveSize) == 0) {
        msg.writeBit0();
    } else {
        msg.writeBit1();
        for (int o = 0; o < kPsObjectiveCount; ++o) {
            const uint8_t* a = from + kPsObjectivesOffset + o * kObjectiveSize;
            const uint8_t* b = to + kPsObjectivesOffset + o * kObjectiveSize;
            msg.writeBits(ld(b, 0), 3);
            writeDeltaFieldList(msg, a, b, kObjectiveFields, 6);
        }
    }
    // Hud elements: archival array first.
    if (std::memcmp(from + kPsHudElemsOffset, to + kPsHudElemsOffset, 2 * kPsHudElemCount * kHudElemSize) == 0) {
        msg.writeBit0();
    } else {
        msg.writeBit1();
        writeHudElems(msg, from + kPsArchivalHudElemsOffset, to + kPsArchivalHudElemsOffset);
        writeHudElems(msg, from + kPsHudElemsOffset, to + kPsHudElemsOffset);
    }
}

void readDeltaPlayerState(Msg& msg, const uint8_t* fromIn, uint8_t* to) {
    std::vector<uint8_t> zero;
    const uint8_t* from = fromIn;
    if (!from) {
        zero.assign(kPlayerStateSize, 0);
        from = zero.data();
    }
    std::memcpy(to, from, kPlayerStateSize);
    int lc = msg.readByte();
    if (lc < 0) lc = 0;
    if (lc > 105) lc = 105;  // TODO(spec): reader behaviour for a count above the table size
    for (int i = 0; i < lc; ++i) readPsField(msg, from, to, kPlayerStateFields[i]);
    if (msg.readBit()) {
        int statsMask = msg.readBits(6);
        for (int i = 0; i < 6; ++i) {
            if (!(statsMask & (1 << i))) continue;
            int32_t v = i == 3 ? (msg.readBits(6) & 63) : i == 5 ? msg.readByte() : msg.readShort();
            st(to, kPsStatsOffset + 4 * i, v);
        }
    }
    if (msg.readBit()) readShortGroups(msg, to, kPsAmmoOffset);
    readShortGroups(msg, to, kPsAmmoClipOffset);
    if (msg.readBit()) {
        for (int o = 0; o < kPsObjectiveCount; ++o) {
            const uint8_t* a = from + kPsObjectivesOffset + o * kObjectiveSize;
            uint8_t* b = to + kPsObjectivesOffset + o * kObjectiveSize;
            st(b, 0, msg.readBits(3) & 7);
            readDeltaFieldList(msg, a, b, kObjectiveFields, 6);
        }
    }
    if (msg.readBit()) {
        readHudElems(msg, from + kPsArchivalHudElemsOffset, to + kPsArchivalHudElemsOffset);
        readHudElems(msg, from + kPsHudElemsOffset, to + kPsHudElemsOffset);
    }
}

} // namespace iw2::net
