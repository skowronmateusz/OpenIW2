// Values the engine gives an AI actor's native fields when it spawns (docs/spec/ai.md, "Native actor field
// defaults").
#pragma once

#include "script/value.h"

namespace iw2::game {

// Sets the spawn-time values of the native actor fields on a newly spawned actor's script object.
void applyActorFieldDefaults(script::Object& actor);

// The sentient fields (table 0x570800) of a new sentient, actor or player: threatbias and ignoreme start at 0 (allocation
// 0x514ce0 zeroes the slot; actor spawn 0x4fc640 stores threatbias 0) (docs/spec/ai.md "Native actor fields", sentient fields).
void applySentientFieldDefaults(script::Object& sentient);

} // namespace iw2::game
