// Model LOD selection and the static model distance cull (game/modellod.h, docs/spec/render-hints.md "Model LOD").
#include "game/modellod.h"
#include <cmath>
#include <cstdio>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    // LOD for a distance: first LOD with distance 0 or greater than the distance.
    const float three[3] = {100, 250, 600};
    chk(lodForDistance(three, 3, 0.0f) == 0, __LINE__);
    chk(lodForDistance(three, 3, 99.9f) == 0, __LINE__);
    chk(lodForDistance(three, 3, 100.0f) == 1, __LINE__); // equal is not below: next LOD
    chk(lodForDistance(three, 3, 599.0f) == 2, __LINE__);
    chk(lodForDistance(three, 3, 600.0f) == -1, __LINE__); // beyond the last LOD: not drawn
    const float openEnd[4] = {100, 250, 600, 0};           // a last distance of 0 never ends
    chk(lodForDistance(openEnd, 4, 1e9f) == 3, __LINE__);
    const float zeroFirst[2] = {0, 500};                   // a 0 distance qualifies at once
    chk(lodForDistance(zeroFirst, 2, 1e6f) == 0, __LINE__);
    chk(lodForDistance(three, 0, 1.0f) == -1, __LINE__);
    chk(lodOutDistance(three, 3) == 600.0f && lodOutDistance(openEnd, 4) == 0.0f && lodOutDistance(three, 0) == 0.0f, __LINE__);

    // Scene LOD parameters: fov 80 leaves r_lodScale / r_lodBias as they are; a narrower fov scales both by tan(fov/2) / tan(40 deg).
    const float eye[3] = {0, 0, 0};
    {
        const LodView v = lodView(eye, 80.0f, 1.0f, -10.0f);
        chk(v.scale == 1.0f && v.bias == -10.0f, __LINE__);
        const LodView z = lodView(eye, 20.0f, 1.0f, -10.0f);
        const float k = std::tan(10.0f * 0.0174532925f) / std::tan(40.0f * 0.0174532925f);
        chk(std::abs(z.scale - k) < 1e-5f && std::abs(z.bias + 10.0f * k) < 1e-4f, __LINE__);
        chk(z.scale < 1.0f, __LINE__); // zooming in lowers the distance: more detail
        const LodView w = lodView(eye, 90.0f);
        chk(w.scale > 1.0f, __LINE__);
    }

    // Instance LOD distance: the scaled distance divided by the instance scale, not divided for scale 0.
    {
        const LodView v = lodView(eye, 80.0f);
        const float p[3] = {300, 400, 0}; // 500 units away
        chk(std::abs(lodDistance(v, p, 1.0f) - 500.0f) < 1e-3f, __LINE__);
        chk(std::abs(lodDistance(v, p, 2.0f) - 250.0f) < 1e-3f, __LINE__);
        chk(std::abs(lodDistance(v, p, 0.0f) - 500.0f) < 1e-3f, __LINE__);
        LodView b = v;
        b.bias = -100.0f;
        chk(std::abs(lodDistance(b, p, 1.0f) - 400.0f) < 1e-3f, __LINE__);
    }

    // Static model distance cull: last LOD distance times the model scale; equality keeps the model; 0 never culls.
    {
        const LodView v = lodView(eye, 80.0f);
        const float grass[1] = {800};
        chk(staticModelCullDistance(grass, 1, 1.5f) == 1200.0f, __LINE__);
        const float at[3] = {1200, 0, 0}, past[3] = {1200.5f, 0, 0};
        chk(!staticModelDistanceCulled(v, at, 1200.0f), __LINE__);
        chk(staticModelDistanceCulled(v, past, 1200.0f), __LINE__);
        chk(!staticModelDistanceCulled(v, past, 0.0f), __LINE__);
        // A scaled model: culled by distance * scale, LOD picked by distance / scale.
        // Exactly at the cull distance the distance cull keeps the model, but its LOD distance (1200 / 1.5 = 800) is not below the last
        // LOD distance, so no LOD qualifies and it is not drawn either.
        chk(staticModelLod(v, at, 1.5f, grass, 1) == -1, __LINE__);
        const float near[3] = {1199, 0, 0};
        chk(staticModelLod(v, near, 1.5f, grass, 1) == 0, __LINE__);
        chk(staticModelLod(v, past, 1.5f, grass, 1) == -1, __LINE__);
        const float sandbag[3] = {100, 350, 0};
        const float p150[3] = {150, 0, 0}, far[3] = {1e6f, 0, 0};
        chk(staticModelLod(v, p150, 1.0f, sandbag, 3) == 1, __LINE__);
        chk(staticModelLod(v, p150, 2.0f, sandbag, 3) == 0, __LINE__); // 150 / 2 = 75 < 100
        chk(staticModelLod(v, far, 1.0f, sandbag, 3) == 2, __LINE__);  // open-ended last LOD: never culled
        // Zoomed view: the same model 150 units away draws a finer LOD.
        const LodView zoom = lodView(eye, 20.0f);
        chk(staticModelLod(zoom, p150, 1.0f, sandbag, 3) == 0, __LINE__);
    }
    if (bad == 0) std::printf("modellod: ok\n");
    return bad ? 1 : 0;
}
