// Effect files (fx/*.efx): text definitions of particle and other elements (docs/spec/effects.md).
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

// A channel graph (rgb, alpha, size, velocityX, …): rows of {time, value…} with 3 values for rgb/rgbRand and 1
// otherwise (FUN_00489270 / FUN_00489340), and a scale range (default 1 1). The engine adds the end points at
// time 0 and 1 when they are missing, both with the values of the first row (FUN_0047ed80).
struct EfxGraph {
    int dims = 1;
    std::vector<std::vector<float>> points; // {time, v0[, v1, v2]}
    float scaleMin = 1, scaleMax = 1;
    // Linearly interpolated value at t (0..1) of component c.
    float sample(float t, int c = 0) const;
    // Area under component c from 0 to t (FUN_0047ecf0: trapezoids over the piecewise-linear curve).
    float integral(float t, int c = 0) const;
};

struct EfxElement {
    std::string type;                                   // Particle, Tail, Emitter, FxRunner, Decal, Light, …
    std::string name;
    std::map<std::string, std::vector<std::string>> keys; // other keys with their whitespace-separated values
    std::map<std::string, EfxGraph> graphs;               // lower-case channel name → graph
    std::vector<std::string> flags() const { return list("flags"); }
    // Bits of the `flags` and `spawnFlags` names (tables at 0x56d528 and 0x56d648 in CoD2SP_s.exe).
    uint32_t flagBits() const;
    uint32_t spawnFlagBits() const;
    std::vector<std::string> list(const std::string& key) const;
    // Numbers of a key (missing or non-numeric values read as 0).
    std::vector<float> numbers(const std::string& key) const;

private:
    mutable int64_t flagBitsCache_ = -1, spawnFlagBitsCache_ = -1; // the name lookups are too slow per particle and frame
};

struct EfxEffect {
    std::vector<EfxElement> elements;
};

// The effect system's random generator (CoD2SP_s.exe 0x59b2a8: seed = seed · 0x343fd + 0x269ec3, draw = seed >> 17). One
// global shared by the parser (legacy `random` channels, 0x47e870) and the runtime; it starts at 0x89abcdef (the data
// value) and nothing in the exe reseeds it (its setter 0x426240 has no caller).
uint32_t& fxGeneratorSeed();

// Throws std::runtime_error on malformed text.
EfxEffect parseEfx(std::string_view text);

} // namespace iw2::assets
