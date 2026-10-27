#include "game/watersim.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace iw2::game {
namespace {

// MSVC CRT rand(). TODO(spec): the CRT seed state when the original calls the setup (0x1002cf60) is unknown; seed 1.
struct CrtRand {
    uint32_t s = 1;
    int next() {
        s = s * 214013u + 2531011u;
        return int((s >> 16) & 0x7fff);
    }
};

// 0x100587d0: polar Box-Muller; (rand / 32768) * 2 - 1 twice, retried while x^2 + y^2 > 1, f = sqrt(-2 ln s / s).
void gaussianPair(CrtRand& r, float& a, float& b) {
    float x, y, s;
    do {
        x = float(r.next()) / 32768.0f * 2.0f - 1.0f;
        y = float(r.next()) / 32768.0f * 2.0f - 1.0f;
        s = y * y + x * x;
    } while (s > 1.0f || s == 0.0f);
    const float f = std::sqrt(-2.0f * std::log(s) / s);
    a = f * x;
    b = f * y;
}

// In-place unnormalised FFT of n points with the given stride (radix-2). The original (0x1005f260) is a radix-4 transform
// with a 256-entry twiddle table; only the magnitude is used later, so the direction does not matter (mirrors the field).
void fft1d(std::complex<float>* d, int n, int stride) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(d[i * stride], d[j * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * std::numbers::pi / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> w(float(std::cos(ang * k)), float(std::sin(ang * k)));
                auto& a = d[(i + k) * stride];
                auto& b = d[(i + k + len / 2) * stride];
                const std::complex<float> t = b * w;
                b = a - t;
                a = a + t;
            }
    }
}

} // namespace

WaterSim::WaterSim(const WaterParams& p) : p_(p), sinTable_(1024) {
    for (int i = 0; i < 1024; ++i) sinTable_[size_t(i)] = std::sin(float(i) * (2.0f * std::numbers::pi_v<float> / 1024.0f));
    const int m = p.size, n = p.size;
    h0_.assign(size_t(m) * size_t(n), {});
    omega_.assign(size_t(m) * size_t(n), 0.0f);
    CrtRand rng;
    const float x4 = p.v * p.v * (p.v * p.v / p.g); // V^4 / g (0x1002cf69..0x1002cf96)
    const float twoPi = 2.0f * std::numbers::pi_v<float>;
    const float ks1 = twoPi / (float(n) * p.lx), ks2 = twoPi / (float(m) * p.ly);
    size_t idx = 0;
    for (int iy = -n / 2; iy < n / 2; ++iy) {
        const float a = float(iy) * ks1;
        for (int ix = -m / 2; ix < m / 2; ++ix, ++idx) {
            const float b = float(ix) * ks2;
            float g1, g2;
            gaussianPair(rng, g1, g2);
            const float k2 = a * a + b * b;
            const float d = a * p.windX + b * p.windY;
            if (!(d > 0.0f)) continue;
            const float e = std::exp(-1.0f / (k2 * x4)) * p.amplitude;
            const float s = std::sqrt(0.5f * (e / (k2 * k2 * k2)) * d * d) * p.amplitude;
            h0_[idx] = {g1 * s, g2 * s};
            omega_[idx] = std::sqrt(std::sqrt(k2) * p.g);
        }
    }
}

std::vector<float> WaterSim::heightMagnitude(float t) {
    const int m = p_.size, n = p_.size;
    const size_t count = size_t(m) * size_t(n);
    std::vector<std::complex<float>> h(count);
    for (size_t i = 0; i < count; ++i) {
        if (omega_[i] == 0.0f) continue;
        const int ph = int(std::nearbyint(omega_[i] * t * 162.97466f + 9.3132257e-10f)); // 1024 / 2 pi (0x1019be38)
        // The original scales re by T[ph + 255] and im by T[ph] (not a complex rotation), 0x1002c8f9.
        h[i] = {sinTable_[size_t((ph + 255) & 0x3ff)] * h0_[i].real(), sinTable_[size_t(ph & 0x3ff)] * h0_[i].imag()};
    }
    for (int y = 0; y < n; ++y) fft1d(&h[size_t(y) * size_t(m)], m, 1);
    for (int x = 0; x < m; ++x) fft1d(&h[size_t(x)], n, m);
    std::vector<float> mag(count);
    for (size_t i = 0; i < count; ++i) mag[i] = std::abs(h[i]);
    return mag;
}

void WaterSim::updateL8(float t, std::vector<std::vector<uint8_t>>& levels) {
    const std::vector<float> mag = heightMagnitude(t);
    const float inv = 1.0f / float(mag.size());
    levels.clear();
    levels.emplace_back(mag.size());
    for (size_t i = 0; i < mag.size(); ++i)
        levels[0][i] = uint8_t(std::nearbyint(std::min(mag[i] * inv, 1.0f) * 255.0f + 9.3132257e-10f));
    for (int w = p_.size; w > 1;) {
        const int nw = w / 2;
        const auto& src = levels.back();
        std::vector<uint8_t> dst(size_t(nw) * size_t(nw));
        for (int y = 0; y < nw; ++y)
            for (int x = 0; x < nw; ++x) {
                const int s = src[size_t(2 * y) * size_t(w) + size_t(2 * x)] + src[size_t(2 * y) * size_t(w) + size_t(2 * x + 1)] +
                              src[size_t(2 * y + 1) * size_t(w) + size_t(2 * x)] + src[size_t(2 * y + 1) * size_t(w) + size_t(2 * x + 1)];
                dst[size_t(y) * size_t(nw) + size_t(x)] = uint8_t(s >> 2);
            }
        levels.push_back(std::move(dst));
        w = nw;
    }
}

void WaterSim::update(float t, std::vector<uint8_t>& bgra) {
    const int m = p_.size, n = p_.size;
    const size_t count = size_t(m) * size_t(n);
    const std::vector<float> mag = heightMagnitude(t);
    bgra.resize(count * 4);
    const float dz = float(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t x = i % size_t(m);
        const size_t prevX = i == 0 ? size_t(m) - 1 : i - 1; // linear predecessor (0x1002ca94)
        const size_t row = i / size_t(m);
        const size_t prevRow = (row == 0 ? size_t(n) - 1 : row - 1) * size_t(m) + x;
        const float dx = mag[i] - mag[prevX], dy = mag[i] - mag[prevRow];
        const float nrm = 127.5f / std::sqrt(dx * dx + dy * dy + dz * dz);
        auto q = [](float v) { return uint8_t(std::clamp(std::nearbyint(v), 0.0f, 255.0f)); };
        bgra[i * 4 + 0] = q(nrm * dz + 127.5f);
        bgra[i * 4 + 1] = q(nrm * dy + 127.5f);
        bgra[i * 4 + 2] = q(nrm * dx + 127.5f);
        bgra[i * 4 + 3] = 255;
    }
}

} // namespace iw2::game
