// Text dump of decoded wire structures for the differential tests (docs/spec/net.md section 9).
#pragma once

#include <cstdint>
#include <string>

#include "net/delta.h"

namespace iw2::net {

// " name=value" for every field of the table that differs from base; fields whose name ends in "Time" / "time"
// are printed relative to timeBase (0 keeps them).
std::string dumpFields(const uint8_t* s, const uint8_t* base, const NetField* fields, int count, int timeBase);
// Every non-zero player-state field, then every non-zero 32-bit word outside the field table as "@offset=value".
std::string dumpPlayerState(const uint8_t* ps, int timeBase);

} // namespace iw2::net
