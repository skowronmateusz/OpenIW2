// Our own lighting re-bake (docs/render-rebake.md): the pure parts, i.e. everything except the ray tracing itself. An openiw2
// addition with no counterpart in the original game; it reads the lightmap layout and its directional basis from docs/spec
// (material.md "Lightmaps", "$lightmapweights") and is not a specification of the original compiler.
//
//   inputs           collectLightmapTriangles   the world triangles of each lightmap with their lightmap coordinates
//   texel set-up     rasterizeTexels            world position, normal and tangent frame of every lightmap texel
//   direct sun, sky  bakeMap                    traced by a `Tracer` (the Metal ray tracing in apps/iw2view/rebake.mm)
//   projection       skyProjection basis        the four `$lightmapweights` directions in the tangent frame
//   calibration      RatioHistogram              per colour channel gain against the shipped coefficient 0
//   cache            cacheKey, save / load       one file per map in the user's cache folder
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/renderenhanced.h"

namespace iw2::assets {
class D3dBsp;
}

namespace iw2::game::rebake {

constexpr int kCoeffRes = 512;   // the shipped coefficient images (three per lightmap, one per colour channel)
constexpr int kSunRes = 1024;    // the shipped sun visibility mask
constexpr uint32_t kFormatVersion = 8; // cache format and bake algorithm; bump with any change of the numbers

// ---- the directional basis ---------------------------------------------------------------------------------------------------------

// The weights `$lightmapweights` gives a direction (x, y, z) in tangent space (x along the tangent, y along the binormal, z along the
// normal; docs/spec/material.md): w0 along the normal, w1..w3 around it, 120 degrees apart. Outside the unit disc x, y are
// normalised and z is 0.
inline std::array<float, 4> lightmapWeights(float x, float y) {
    float z2 = 1.0f - (x * x + y * y);
    if (!(z2 > 0.0f)) {
        const float len = std::sqrt(x * x + y * y);
        if (len != 0.0f) x /= len, y /= len;
        z2 = 0.0f;
    }
    const float z = std::sqrt(z2);
    float a = std::atan2(y, x) * 0.47746482f - 0.75f;
    if (a < 0.0f) a += 3.0f;
    else if (a > 3.0f) a -= 3.0f;
    std::array<float, 4> w{};
    if (a < 1.0f) w[1] = 1.0f - a, w[2] = a;
    else if (a < 2.0f) w[2] = 2.0f - a, w[3] = a - 1.0f;
    else w[1] = a - 2.0f, w[3] = 3.0f - a;
    w[0] = std::clamp(1.0f - std::acos(z) * 1.0467734f, 0.0f, 1.0f);
    for (int k = 1; k < 4; ++k) w[size_t(k)] *= 1.0f - w[0];
    return w;
}

// The four directions the coefficients stand for (the direction whose weights are the unit vector k), in tangent space. Our reading of
// the spec: coefficient k is the light arriving at a surface whose (perturbed) normal points along basis direction k.
// TODO(spec): that the shipped coefficients are irradiance for these normals is inferred from the weight function; the shipped
// numbers were not fitted to confirm it.
inline std::array<std::array<float, 3>, 4> lightmapBasis() {
    constexpr float kPi = 3.14159265358979f;
    const float s = std::sqrt(2.0f / 3.0f), z = std::sqrt(1.0f / 3.0f);
    std::array<std::array<float, 3>, 4> d{};
    d[0] = {0.0f, 0.0f, 1.0f};
    for (int k = 1; k < 4; ++k) {
        const float az = (0.75f + float(k - 1)) * 2.0f * kPi / 3.0f; // weight k is 1 at azimuth atan2(y, x) = (k - 1 + 0.75) 2 pi / 3
        d[size_t(k)] = {s * std::cos(az), s * std::sin(az), z};
    }
    return d;
}

// The eight directions the light grid's coefficients stand for (docs/spec/lightgrid.md): the corners of the cube in the grid's t space
// (three axes 54.7 degrees from vertical, 120 degrees apart), coefficient k at the corner whose bit 0 / 1 / 2 is the sign of t0 / t1 / t2.
// Our reading, like lightmapBasis(): the coefficient is the light arriving at a normal pointing along that corner.
// TODO(spec): inferred from the weight function, checked only through the statistics of the grid bake.
inline std::array<std::array<float, 3>, 8> gridBasis() {
    const float a0[3] = {0.81649655f, 0.0f, 0.57735026f}, a1[3] = {-0.40824828f, 0.70710677f, 0.57735026f}, a2[3] = {-0.40824828f, -0.70710677f, 0.57735026f};
    std::array<std::array<float, 3>, 8> d{};
    for (int k = 0; k < 8; ++k) {
        const float s0 = (k & 1) ? 1.0f : -1.0f, s1 = (k & 2) ? 1.0f : -1.0f, s2 = (k & 4) ? 1.0f : -1.0f;
        float v[3];
        for (int e = 0; e < 3; ++e) v[e] = s0 * a0[e] + s1 * a1[e] + s2 * a2[e];
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int e = 0; e < 3; ++e) d[size_t(k)][size_t(e)] = v[e] / l;
    }
    return d;
}

// ---- floats ------------------------------------------------------------------------------------------------------------------------

uint16_t floatToHalf(float f);
float halfToFloat(uint16_t h);

// ---- inputs and texel set-up -------------------------------------------------------------------------------------------------------

struct LmVertex {
    float pos[3], normal[3], tangent[3], binormal[3], uv[2];
};
struct LmTriangle {
    LmVertex v[3];
};

enum class TexelState : uint8_t {
    Unset = 0,
    Inside,  // the texel centre lies inside a triangle
    Edge,    // only the texel square touches a triangle (conservative coverage): the point is the closest one on the triangle
    Dilated, // one texel around the charts, copied from a neighbour so bilinear filtering never reads unset texels
    Shared,  // two triangles of different surfaces claim the centre (overlapping or mirrored charts): the shipped data stays
};

struct Texel {
    float pos[3] = {0, 0, 0}, n[3] = {0, 0, 1}, t[3] = {1, 0, 0}, b[3] = {0, 1, 0};
    TexelState state = TexelState::Unset;
    int32_t source = -1; // Dilated: the texel it was copied from
};

struct TexelSet {
    int res = 0;
    std::vector<Texel> texels; // res * res, row y * res + x; texel (x, y) has its centre at lightmap uv ((x + 0.5) / res, (y + 0.5) / res)
    size_t inside = 0, edge = 0, dilated = 0, shared = 0;
};

// Rasterises the triangles in lightmap space at res x res: conservative coverage, one texel of dilation, and the overlap detection (a
// texel whose centre two triangles with different position or normal claim becomes Shared).
TexelSet rasterizeTexels(int res, const std::vector<LmTriangle>& tris);

// The triangles of each lightmap of the world (model 0; brush submodels move). Soups with lightmap index 31 or beyond the lump are left out.
std::vector<std::vector<LmTriangle>> collectLightmapTriangles(const assets::D3dBsp& bsp);

// A texel as the tracer needs it (a surface point and its tangent frame).
struct TexelPoint {
    float pos[3], n[3], t[3], b[3];
};

// The scene the tracer needs beyond positions (stage 3): per triangle what a bounce ray finds, geometry groups with their alpha
// tests, the lamps and, for the CPU tracer only, the textures' data.
struct SceneTri {
    float uv[6] = {0, 0, 0, 0, 0, 0}; // lightmap coordinates of the three vertices
    int32_t lm = -1;                  // lightmap index, -1: none (static models and the like)
    int32_t tex = -1;                 // texture slot (albedo)
    float light[3] = {0, 0, 0};       // lm < 0: the light there (baked into the model's vertices), displayed units
    float pad = 0;
};
struct GeomRange {
    uint32_t first = 0, count = 0;    // a range of the index list (indices, a multiple of 3)
    int32_t layer = -1;               // alpha mask layer, -1: opaque
    uint32_t func = 1;                // alpha function when tested: 1 GE128, 2 GT0, 3 LT128 (assets::AlphaFunc as in docs/spec/material.md)
    int32_t tex = -1;                 // texture slot of a tested range
};
struct TraceScene {
    std::vector<float> positions, vertexUv; // 3 and 2 floats per vertex
    std::vector<uint32_t> indices;
    std::vector<GeomRange> ranges;          // cover `indices` in order; opaque and tested groups
    std::vector<SceneTri> tris;             // indices.size() / 3
    std::vector<MapLight> lamps;
    int lightmaps = 0;
    std::vector<void*> textures;            // per texture slot the native image (Metal tracer; SDL_GPUTexture*), may be null
    // CPU tracer and tests: 3 floats of albedo per slot, 64 x 64 alpha bytes per mask layer
    std::vector<float> albedo;
    std::vector<std::vector<uint8_t>> masks;
};

// Mean colour of rgba samples weighted by alpha (the albedo of a material from a small mip level, docs/render-rebake.md step 1); 0 if no
// sample has alpha. TODO(spec): the stored encoding is left alone (the colour map's gamma is not undone).
inline void meanAlbedo(const float* rgba, size_t n, float out[3]) {
    double acc[3] = {0, 0, 0}, w = 0;
    for (size_t i = 0; i < n; ++i) {
        const double a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; ++c) acc[c] += double(rgba[i * 4 + size_t(c)]) * a;
        w += a;
    }
    for (int c = 0; c < 3; ++c) out[c] = w > 0 ? float(acc[c] / w) : 0.0f;
}

// The tracer: visibility of the sun and of the sky from surface points, against the static world.
class Tracer {
public:
    virtual ~Tracer() = default;
    // Lamps (TraceScene::lamps): per point 12 values, [channel * 4 + k] = sum over lamps of colour_channel * falloff * visibility *
    // max(0, n_k . l) with the light direction l, k the basis direction (mapLightFalloff; a lamp behind the surface gives 0; the visibility
    // averages a few rays towards points around the lamp). The default says no lamps.
    virtual bool lamps(const std::vector<TexelPoint>& pts, std::vector<float>& out12) { out12.assign(pts.size() * 12, 0.0f); return true; }
    // The irradiance cache: rgba floats per texel of every lightmap's kCoeffRes grid (lightmap-major, row-major), the light that arrives
    // at a surface in displayed units. bounce() reads it.
    virtual void setCache(const std::vector<float>&) {}
    // The light grid's points (stage 4): the same terms over the whole sphere around a free point (pts[].pos; the frame is ignored),
    // projected onto gridBasis(): 8 values per point for the sky, 24 ([channel * 8 + k]) for lamps and bounce. Sphere sampling is uniform, so
    // the sky gives (4 / N) sum of max(0, d . basis_k) over the rays that escape upwards (the sky is a dome: rays with d.z <= 0 never see it; 1 for the upward corner in the open).
    virtual bool gridSky(const std::vector<TexelPoint>& pts, std::vector<float>& out8) { out8.assign(pts.size() * 8, 0.0f); return true; }
    virtual bool gridLamps(const std::vector<TexelPoint>& pts, std::vector<float>& out24) { out24.assign(pts.size() * 24, 0.0f); return true; }
    virtual bool gridBounce(const std::vector<TexelPoint>& pts, std::vector<float>& out24) { out24.assign(pts.size() * 24, 0.0f); return true; }
    // One bounce: per point 12 values as lamps(), the light that comes from the surfaces the hemisphere rays hit: albedo * the cache at the
    // hit's lightmap coordinates (or SceneTri::light where there is no lightmap), projected like the sky.
    virtual bool bounce(const std::vector<TexelPoint>& pts, std::vector<float>& out12) { out12.assign(pts.size() * 12, 0.0f); return true; }
    // Fraction of the sun's disc (a cone around the direction towards the sun) a point sees, 0..1, one value per point.
    virtual bool sun(const std::vector<TexelPoint>& pts, const float towardsSun[3], std::vector<float>& vis) = 0;
    // The four basis projections of the sky's visibility, four values per point: v_k = (2 / N) sum over the hemisphere samples that
    // reach the sky of max(0, d . basis_k) (1 for flat open ground along basis 0).
    virtual bool sky(const std::vector<TexelPoint>& pts, std::vector<float>& v4) = 0;
};

// ---- calibration -------------------------------------------------------------------------------------------------------------------

// Median of positive ratios from a fixed logarithmic histogram (1/4096 .. 4096, 3 % bins): exact enough for a gain, no per-texel storage.
class RatioHistogram {
public:
    void add(float ratio);
    size_t count() const { return n_; }
    float median() const; // 0 when empty
private:
    std::vector<uint32_t> bins_ = std::vector<uint32_t>(kBins, 0);
    size_t n_ = 0;
    static constexpr int kBins = 600;
};

// Intersection over union of two binary masks (value > threshold) over the texels `use` selects; 1 when both are empty.
struct MaskOverlap {
    uint64_t both = 0, either = 0;
    uint64_t onlyA = 0, onlyB = 0; // lit in a (the shipped mask) only / in b (the re-bake) only
    double iou() const { return either ? double(both) / double(either) : 1.0; }
};
inline MaskOverlap maskOverlap(uint8_t a, uint8_t b, uint8_t threshold, MaskOverlap acc = {}) {
    const bool x = a > threshold, y = b > threshold;
    acc.both += (x && y), acc.either += (x || y), acc.onlyA += (x && !y), acc.onlyB += (!x && y);
    return acc;
}

inline float luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// ---- the bake ----------------------------------------------------------------------------------------------------------------------

struct Config {
    float sunConeDegrees = 0.5f; // half angle of the sun's cone in the tracer; TODO(spec): the shipped mask's softness
    int sunRays = 16;            // rays a sun grid texel casts into the cone
    int skyRays = 128;           // hemisphere rays of a coefficient texel
    float minShipped = 3.0f / 255.0f; // calibration fit: shipped coefficient 0 above this (not near black)
    float minBaked = 0.05f;           // and the re-baked visibility projection above this
    float lampMargin = 1.25f;         // (stage 2: texels near lamps stayed out of the sky fit; now a texel is out of it when a lamp reaches it)
    int lampRays = 4;                 // visibility rays per lamp and texel
    float lampSize = 6.0f;            // the lamp's extent the visibility rays spread over (units)
    int bounceRays = 64;              // hemisphere rays per bounce pass and texel
    int bounces = 2;                  // bounce passes (0: direct light only)
    bool lightmapGain = true;         // a last correction per lightmap and colour channel (the median shipped / re-baked over the lightmap's texels)
    bool bounceGainPerChannel = true; // false: one bounce gain for the three channels (the colour then comes from the albedo)
    float albedoGamma = 1.0f;         // the albedo is raised to this power (1: the colour map as stored, 2.2: its gamma undone)
    float albedoSat = 0.0f;           // and moved from its luminance towards its colour by this factor (0: grey, the default, 1: unchanged; measured in docs/render-rebake.md)
    int fitRounds = 3;                // fit, bounce, refit rounds
    float minLamp = 0.02f;            // lamp fit: the lamp's coefficient 0 (luminance) above this
    bool residualView = false;        // keep a ratio image per lightmap (LightmapResult::ratio); the caller leaves the cache out
    bool alphaCasters = true;         // set by the caller: whether the scene has the alpha-tested surfaces (settings string only)
    float facingMin = 0.05f;          // sun IoU over texels with N.L above this
    uint8_t maskThreshold = 127;      // binary threshold of both sun masks
};

// The albedo as the bounce uses it: raised to `gamma`, then moved from its luminance towards its colour by `sat`.
void shapeAlbedo(float a[3], float gamma, float sat);

// A brute-force CPU tracer with the same sampling as the Metal kernels (apps/iw2view/rebake.mm): the reference of the kernels' maths for
// the tests and for IW2_REBAKE_VERIFY. Linear in the triangle count per ray: only for small scenes and a few texels.
std::unique_ptr<Tracer> makeCpuTracer(TraceScene scene, const Config& cfg);

// What the shipped lightmap holds, as in the file (coefficient images B, G, R, A byte order = c0, c1, c2, c3; the sampled channel
// order is (c2, c1, c0, c3)).
struct ShippedLightmap {
    const uint8_t* coeff[3] = {nullptr, nullptr, nullptr}; // red, green, blue image, kCoeffRes^2 * 4 bytes
    const uint8_t* sun = nullptr;                          // kSunRes^2 bytes
};

struct LightmapResult {
    bool baked = false;                 // false: nothing of this lightmap was re-baked (the shipped data stays)
    std::vector<uint16_t> coeffs;       // 3 layers (red, green, blue) x kCoeffRes^2 x 4 half floats, sampled channel order (c2, c1, c0, c3)
    std::vector<uint8_t> ratio;         // Config::residualView: kCoeffRes^2 x 3 bytes, log2 of the re-baked / shipped c0 luminance (blue below, red above, grey = equal; black = not traced)
    std::vector<uint8_t> sun;           // kSunRes^2, the re-baked mask where texels were traced, the shipped one elsewhere
    size_t inside = 0, edge = 0, dilated = 0, shared = 0; // of the sun grid (kSunRes)
};

struct Stats {
    size_t lightmaps = 0, bakedLightmaps = 0, coeffTexels = 0, sunTexels = 0, sharedTexels = 0;
    MaskOverlap sunFacing;   // sun mask overlap over texels that face the sun
    MaskOverlap sunAll;      // over every covered texel
    float gain[3] = {1, 1, 1};     // sky gain per channel (after the final correction)
    float lampGain = 1, bounceGain = 1; // lamp and bounce gains (after the final correction)
    float correction[3] = {1, 1, 1}; // the last round's overall correction per channel (how far the terms were from the shipped light)
    size_t fitTexels = 0, lampFitTexels = 0;
    // mean(re-baked c0 luminance) / mean(shipped) by class of texel, and the share of texels within +-25 % of the shipped luminance
    double classRatio[3] = {0, 0, 0};   // 0 sky-lit (open sky, no lamp), 1 lit by a lamp, 2 the rest (indoor and bounced)
    size_t classTexels[3] = {0, 0, 0};
    double within25 = 0;
    double lumRatioNoBounce = 0;        // all texels, sky and lamps only
    // the light grid bake (stage 4): points, the median correction against the shipped grid, the mean ratio (sum re-baked / sum shipped over
    // channels and coefficients) before and after it, and the share of points within +-25 % of the shipped luminance (mean of the 8 coefficients)
    size_t gridPoints = 0;
    float gridCorrection[3] = {1, 1, 1};
    double gridOpenShare = 0, gridOpenRatio = 0; // points whose upward corner sees at least 0.9 of the sky dome: their share, and sum re-baked / sum shipped over them (before the grid correction)
    double gridShippedMean = 0, gridSkyMean = 0; // mean shipped coefficient, and the mean sky visibility projection (1 in open air)
    double gridRatioBefore = 0, gridRatio = 0, gridWithin25Before = 0, gridWithin25 = 0;
    double chromaErr = 0;               // mean |shipped - re-baked| of the red and blue shares of the luminance over the covered texels (the colour cast)
    float shippedDirRatio[3] = {0, 0, 0}, bakedDirRatio[3] = {0, 0, 0}; // median c_k / c_0 (k = 1..3, green channel) over the fit set: shipped and re-baked
    double lumRatioFit = 0, lumRatioAll = 0; // calibrated re-baked coefficient 0 luminance over the shipped one (mean)
    double seconds = 0;
};

struct BakeResult {
    std::vector<LightmapResult> lightmaps;
    Stats stats;
    std::vector<std::array<float, 24>> grid; // light grid points' coefficients [channel * 8 + k], per entry of assets::LightGrid::points(); empty: not baked
    // What the light grid bake needs from the lightmap bake (not stored in the cache; the grid is stored instead).
    struct Terms {
        bool valid = false;
        std::vector<float> cache; // the final irradiance cache
        float sky[3] = {1, 1, 1}, lamp = 1, bounce[3] = {1, 1, 1}, correction[3] = {1, 1, 1};
    } terms;
};

// The light grid: re-bakes every point with the lightmap bake's terms and gains (`r.terms`), then fits one more correction per channel
// against `shipped` (the shipped coefficients of each point, bytes / 255) and stores the result in r.grid and the statistics.
bool bakeGrid(Tracer& tracer, BakeResult& r, const std::vector<std::array<float, 3>>& points, const std::vector<std::array<float, 24>>& shipped,
              const Config& cfg = {});

// Bakes every lightmap that has triangles. `shipped[i]` is lightmap i's shipped data (used for the calibration, the IoU and to fill
// everything that was not re-baked). The tracer is asked once per lightmap and grid. Returns nullopt if the tracer fails.
std::optional<BakeResult> bakeMap(Tracer& tracer, const std::vector<std::vector<LmTriangle>>& tris, const std::vector<ShippedLightmap>& shipped,
                                  const float towardsSun[3], const float sunColor[3], const std::vector<MapLight>& lights, const Config& cfg = {});

// ---- cache -------------------------------------------------------------------------------------------------------------------------

uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 1469598103934665603ull);
// The key of a map's cache file: the map file's hash, the settings string and the format version.
std::string cacheKey(uint64_t mapHash, const std::string& settings);
std::string cacheDir(); // $IW2_REBAKE_CACHE, else the user cache folder (macOS ~/Library/Caches/OpenIW2/rebake); empty if unknown
bool saveCache(const std::string& path, const BakeResult& r);
std::optional<BakeResult> loadCache(const std::string& path);
// The bake settings as a string for cacheKey (the config's numbers, the sun direction and the map lights' hash).
std::string settingsString(const Config& cfg, const float towardsSun[3], const std::vector<MapLight>& lights, const std::string& extra = {});

// ---- debug export (stage 1) --------------------------------------------------------------------------------------------------------

// Writes <dir>/lmNN_pos.ppm and lmNN_normal.ppm (res x res; positions normalised to the box of all texels, normals as n * 0.5 + 0.5; unset
// texels black, dilated and shared ones dimmed / magenta) for every lightmap with triangles. Returns the number of lightmaps written.
size_t exportTexelImages(const std::string& dir, const std::vector<std::vector<LmTriangle>>& tris, int res);

} // namespace iw2::game::rebake
