#pragma once

namespace iw2::game {

// The view weapon's muzzle flash flag (cgame entity byte +0x142, set by EV_FIRE_WEAPON 0x4b7800). The view weapon pass
// (0x4b69b0) clears it in every frame it poses the gun and plays viewFlashEffect only when that pass is allowed to draw
// (third argument non-zero; 0x4b6d40 passes zero while the scope overlay 0x4bc8a0 is up). A shot fired under the overlay
// therefore never produces a flash later. Returns whether the effect must be played now.
inline bool consumeViewFlash(bool& pending, bool drawn) {
    const bool had = pending;
    pending = false;
    return had && drawn;
}

} // namespace iw2::game
