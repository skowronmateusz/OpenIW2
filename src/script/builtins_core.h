// Engine-independent script built-ins.
#pragma once

#include "script/vm.h"

#include <cstdint>
#include <memory>
#include <random>

namespace iw2::script {

// Registers math, vector, string and random-number built-ins. Angles are in degrees.
// Returns the random generator of randomint / randomfloat (its state is part of a savegame).
std::shared_ptr<std::mt19937> registerCoreBuiltins(VM& vm, uint32_t seed = 1);

} // namespace iw2::script
