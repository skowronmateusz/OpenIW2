# SDL patch: native Metal handles

`sdl-3.4.20-metal-handles.patch` adds four functions to SDL 3.4.20 (SDL's GPU API has no access to the Metal objects behind
its handles):

- `SDL_GetGPUDeviceMetalDevice`: the `id<MTLDevice>` of a GPU device,
- `SDL_GetGPUCommandBufferMetalCommandBuffer`: the `id<MTLCommandBuffer>` of a command buffer,
- `SDL_GetGPUTextureMetalTexture` and `SDL_GetGPUBufferMetalBuffer`: the `id<MTLTexture>` / `id<MTLBuffer>` a texture or buffer
  currently refers to,

and the macro `SDL_GPU_OPENIW2_METAL_HANDLES`. Other backends return NULL. With them the enhanced renderer encodes MetalFX and
ray tracing work into SDL's command buffers between SDL passes (docs/render-enhanced.md "Stage 5"). The patch contains only
our own code; with an unpatched SDL (e.g. Homebrew's) those features are compiled out and the rest of the game is unchanged.

The patched library is an altered version of SDL, which is under the zlib licence (`LICENSE.txt` in SDL's source, shipped in
the app as `LICENSE-SDL3.txt`): the patch marks its additions in the source.

Build (the README's "Build from source" uses it):

```bash
curl -fsSLO https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
tar -xzf SDL3-3.4.20.tar.gz
patch -d SDL3-3.4.20 -p1 < tools/sdl/sdl-3.4.20-metal-handles.patch
cmake -S SDL3-3.4.20 -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_INSTALL_PREFIX="$PWD/sdl-prefix" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build sdl-build --target install
```
