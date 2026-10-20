// Engine-independent script built-ins.
#pragma once

#include "script/vm.h"

#include <cstdint>

namespace iw2::script {

// Registers math, vector, string and random-number built-ins. Angles are in degrees.
void registerCoreBuiltins(VM& vm, uint32_t seed = 1);

} // namespace iw2::script
