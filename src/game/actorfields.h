// Values the engine gives an AI actor's native fields when it spawns (docs/spec/ai.md, "Native actor field
// defaults").
#pragma once

#include "script/value.h"

namespace iw2::game {

// Sets the spawn-time values of the native actor fields on a newly spawned actor's script object.
void applyActorFieldDefaults(script::Object& actor);

} // namespace iw2::game
