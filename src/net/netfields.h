// Field tables of the patch 1.3 delta coders (docs/spec/net.md section 7).
#pragma once

#include "net/delta.h"

namespace iw2::net {

// Structure sizes in bytes (the offsets in the tables index into these layouts).
constexpr int kEntityStateSize = 240;
constexpr int kArchivedEntitySize = 276;
constexpr int kClientStateSize = 92;
constexpr int kPlayerStateSize = 9896;
constexpr int kObjectiveSize = 28;
constexpr int kHudElemSize = 128;

constexpr int kEntityNumberBits = 10;
constexpr int kClientNumberBits = 6;
constexpr int kMaxEntities = 1024;
constexpr int kEntityListEnd = kMaxEntities - 1;

extern const NetField kEntityStateFields[59];
extern const NetField kArchivedEntityFields[68];
extern const NetField kClientStateFields[22];
extern const NetField kPlayerStateFields[105];
extern const NetField kObjectiveFields[6];
extern const NetField kHudElemFields[32];

} // namespace iw2::net
