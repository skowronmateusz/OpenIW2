// Native Metal work inside SDL's GPU command buffers (docs/render-enhanced.md "Stage 5: MetalFX and ray tracing"). Needs SDL3
// built with tools/sdl/sdl-3.4.20-metal-handles.patch; with any other SDL the native features are compiled out and every query
// reports them unavailable, so the enhanced renderer keeps its own paths.
#pragma once

#include <string>

struct SDL_GPUDevice;

namespace iw2::metal {

struct Caps {
    bool handles = false;    // SDL gives the native Metal objects (the patch)
    bool metalFx = false;    // MetalFX temporal scaling is available on this GPU
    bool rayTracing = false; // hardware ray tracing (Apple GPU family 9: M3 and later)
    std::string gpu;         // the device name
};

// What this build and GPU allow; IW2_NO_METALFX / IW2_NO_METAL_RT force the features off (fallback checks).
Caps query(SDL_GPUDevice* dev);

} // namespace iw2::metal
