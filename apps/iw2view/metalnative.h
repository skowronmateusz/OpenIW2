// Native Metal work inside SDL's GPU command buffers (docs/render-enhanced.md "Stage 5: MetalFX and ray tracing"). Needs SDL3
// built with tools/sdl/sdl-3.4.20-metal-handles.patch; with any other SDL the native features are compiled out and every query
// reports them unavailable, so the enhanced renderer keeps its own paths.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUBuffer;
struct SDL_GPURenderPass;

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

// The scene for ray queries in the enhanced fragment shaders (docs/render-enhanced.md "Stage 5"): a two-level structure, the static
// world (built once per map) plus moving objects (instances, rebuilt per frame), bound at fragment buffer kSceneBufferIndex of
// every scene render pass (above SDL's uniform and storage buffers). Without a scene a placeholder that nothing hits is bound.
class SceneRT {
public:
    static constexpr unsigned kSceneBufferIndex = 6;
    // instance masks: what a ray may hit
    static constexpr uint32_t kMaskStatic = 1, kMaskDynamic = 2, kMaskAlpha = 4;
    // the alpha-tested world: index range, its alpha mask layer and AlphaFunc (assets::AlphaFunc as a number)
    struct AlphaRange { uint32_t first, count, layer, func; };
    SceneRT();
    ~SceneRT();
    SceneRT(const SceneRT&) = delete;
    SceneRT& operator=(const SceneRT&) = delete;
    // The static world: index ranges (first, count) of one vertex / index buffer (32-bit indices, the position first in a vertex).
    // alpha: the alpha-tested ranges, tested per ray candidate against alphaMasks (an R8 2D array, one layer per material).
    bool buildStatic(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* vertices, uint32_t vertexStride, SDL_GPUBuffer* indices,
                     const std::vector<std::pair<uint32_t, uint32_t>>& ranges, const std::vector<AlphaRange>& alpha = {},
                     SDL_GPUTexture* alphaMasks = nullptr);
    void clearStatic();
    bool hasStatic() const;
    // The frame's top level (static world plus this frame's moving objects); on the command buffer between SDL passes.
    // The frame's moving objects: one instance each, model-to-world transform (column-major 4 x 4), index ranges of its own
    // vertex (at a byte offset) / index buffers. key: the model (its bottom level is kept); deforming: its vertices are rewritten
    // every frame (skinned), so the kept bottom level is refit each frame.
    void beginItems();
    void addItem(SDL_GPUBuffer* vertices, uint32_t vertexOffset, uint32_t vertexStride, SDL_GPUBuffer* indices,
                 const std::vector<std::pair<uint32_t, uint32_t>>& ranges, const float transform[16], const void* key, bool deforming);
    // active false: no top level this frame (the placeholder is bound).
    bool buildFrame(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, bool active);
    // Shadows and contact occlusion of the moving objects (only those: kMaskDynamic) for the pixels of a depth image ($floatz,
    // clip z), into mask (RGBA16F with compute write usage, w x h): r the sun's visibility (a cone of rays over the frames: soft),
    // g the lamps' visibility (weighted by each lamp's light at the pixel), b the contact occlusion; lights (same size) the
    // visibility of up to four effect lights, one channel each, traced against the whole scene. u: 68 floats, see
    // metalnative.mm (RtShadowU).
    bool shadowPass(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* mask, SDL_GPUTexture* lights,
                    uint32_t w, uint32_t h, const float* u);
    // Binds the frame's top level (or the placeholder) to an open render pass.
    void bind(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);
    std::string describe() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iw2::metal
