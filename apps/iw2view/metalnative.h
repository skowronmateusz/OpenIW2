// Native Metal work inside SDL's GPU command buffers (docs/render-enhanced.md "Stage 5: MetalFX and ray tracing"). Needs SDL3
// built with tools/sdl/sdl-3.4.20-metal-handles.patch; with any other SDL the native features are compiled out and every query
// reports them unavailable, so the enhanced renderer keeps its own paths.
#pragma once

#include <string>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;

namespace iw2::metal {

struct Caps {
    bool handles = false;    // SDL gives the native Metal objects (the patch)
    bool metalFx = false;    // MetalFX temporal scaling is available on this GPU
    bool rayTracing = false; // hardware ray tracing (Apple GPU family 9: M3 and later)
    std::string gpu;         // the device name
};

// What this build and GPU allow; IW2_NO_METALFX / IW2_NO_METAL_RT force the features off (fallback checks).
Caps query(SDL_GPUDevice* dev);

// MetalFX temporal scaling into SDL's command buffer (between SDL passes). Inputs at the render size, single-sample: the linear
// HDR colour, a depth texture (R32F device depth), motion (RG16F, NDC units, current -> previous unjittered NDC, as the
// enhanced TAA's), the output at the output size with compute write usage.
class TemporalScaler {
public:
    TemporalScaler();
    ~TemporalScaler();
    TemporalScaler(const TemporalScaler&) = delete;
    TemporalScaler& operator=(const TemporalScaler&) = delete;
    // Encodes one frame; false if the scaler could not be made (then the caller resolves with its own TAA). jitter: the
    // projection's sub-pixel offset in NDC (Camera::jitterX / Y); reset: no valid history (first frame, a cut, a resize).
    bool encode(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* color, SDL_GPUTexture* depth, SDL_GPUTexture* motion,
                SDL_GPUTexture* output, unsigned inW, unsigned inH, unsigned outW, unsigned outH, float jitterNdcX, float jitterNdcY,
                bool reset);
    std::string describe() const; // the scaler's size and the texture usages it needs (log)

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iw2::metal
