// Water normal-map generator (FFT ocean) of the 1.3 renderer: setup 0x1002d120 + spectrum 0x1002cf60, per frame
// h(k,t) 0x1002c880, FFT 0x1002c990, |h| -> BGRA slope map 0x1002ca40.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace iw2::game {

struct WaterParams {
    int size = 64;            // width = height (u16 of the material's water description)
    float lx = 37, ly = 37;   // desc +4 / +8
    float amplitude = 0.06f;  // desc +0xc (struct +0x2c)
    float v = 76;             // desc +0x10 (struct +0x20)
    float windX = 0, windY = 0;
    float g = 800.0f;         // constant stored by the loader (0x1001bf64: 0x44480000)
};

class WaterSim {
public:
    explicit WaterSim(const WaterParams& p);
    // Fills size * size BGRA bytes for the given time in seconds (refdef floatTime).
    void update(float timeSeconds, std::vector<uint8_t>& bgra);
    // dx9 path (r_rendererInUse != dx7): L8 height map = min(|h| / (M*N), 1) * 255 (0x1002cbd0) with a full mip chain of 2x2 box
    // averages (sum of four >> 2, 0x1000de90) from 0x1002cc70. levels[0] is size x size.
    void updateL8(float timeSeconds, std::vector<std::vector<uint8_t>>& levels);
    int size() const { return p_.size; }

private:
    std::vector<float> heightMagnitude(float t); // h(k,t) + FFT + |h|
    WaterParams p_;
    std::vector<std::complex<float>> h0_;
    std::vector<float> omega_;
    std::vector<float> sinTable_; // 1024 entries, T[i] = sin(i * 2 pi / 1024) (0x10012fe0)
};

} // namespace iw2::game
