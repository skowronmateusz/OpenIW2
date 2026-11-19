// The Metal ray tracing behind the lighting re-bake (docs/render-rebake.md): a game::rebake::Tracer over the static world. It owns its
// acceleration structure and command queue and works synchronously (the bake runs once per map and is then cached), so it needs
// neither SDL's command buffers nor the GPU family of the runtime ray tracing features. Needs SDL3 built with the Metal handles patch
// (tools/sdl/sdl-3.4.20-metal-handles.patch); with any other SDL nothing is available and r_enhRebake has no effect.
#pragma once

#include "game/rebake.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_GPUDevice;

namespace iw2::rebake {

// The build has the native Metal objects and the GPU can run ray queries (hardware or not: M1 and M2 run them in software).
bool available(SDL_GPUDevice* dev);

// nullptr (and a message in `error`) when the structure or the kernels could not be made.
// `scene.textures` holds SDL_GPUTexture* (colour maps) for the albedo and the alpha masks.
std::unique_ptr<game::rebake::Tracer> makeTracer(SDL_GPUDevice* dev, const game::rebake::TraceScene& scene, const game::rebake::Config& cfg, std::string& error);

} // namespace iw2::rebake
