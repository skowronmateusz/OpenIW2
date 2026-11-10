// Delta coders for user commands and field tables, implemented from the spec (docs/spec/net.md sections 6-7).
#pragma once

#include <cstdint>

#include "net/msg.h"

namespace iw2::net {

struct UserCmd {
    int32_t serverTime = 0;
    int32_t buttons = 0;
    uint8_t weapon = 0;
    uint8_t offhand = 0;  // TODO(spec): name inferred
    int32_t angles[3] = {0, 0, 0};
    int8_t forwardmove = 0;
    int8_t rightmove = 0;
};

// Movement is carried as a 4-bit direction class.
int moveClass(int forward, int right);
void moveFromClass(int cls, int8_t& forward, int8_t& right);

void writeDeltaUsercmd(Msg& msg, uint32_t key, const UserCmd& from, const UserCmd& to);
void readDeltaUsercmd(Msg& msg, uint32_t key, const UserCmd& from, UserCmd& to);

// Field description: byte offset into the structure and a bit code (0 float, -99 small float, -100 angle,
// otherwise an integer of |bits| bits).
struct NetField {
    const char* name;
    int offset;
    int bits;
};

constexpr int kFieldFloat = 0;
constexpr int kFieldSmallFloat = -99;
constexpr int kFieldAngle = -100;

void writeDeltaField(Msg& msg, const uint8_t* from, const uint8_t* to, const NetField& field);
void readDeltaField(Msg& msg, const uint8_t* from, uint8_t* to, const NetField& field);

// Structure frame: number (numberBits), removed bit, changed bit, last-changed count byte, fields.
// to == nullptr writes a removal. leadingBit adds the extra 1 bit used by client-state frames.
void writeDeltaStruct(Msg& msg, const uint8_t* from, const uint8_t* to, bool force, const NetField* fields,
                      int fieldCount, int number, int numberBits, bool leadingBit);

// Plain list (no number, no count): one bit "anything changed" (or forced), then every field.
void writeDeltaFieldList(Msg& msg, const uint8_t* from, const uint8_t* to, const NetField* fields, int fieldCount,
                         bool force = false);
void readDeltaFieldList(Msg& msg, const uint8_t* from, uint8_t* to, const NetField* fields, int fieldCount);

enum class StructDelta { Removed, Unchanged, Changed };
// Reads the part after the number: returns what the frame said and fills to (copied from from first).
StructDelta readDeltaStructBody(Msg& msg, const uint8_t* from, uint8_t* to, int structSize, const NetField* fields,
                                int fieldCount);

} // namespace iw2::net
