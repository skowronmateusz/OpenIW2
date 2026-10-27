// Damped angle spring of the compass face (exe 0x4b09a0, called from the compass ownerdraw 0x4a90b0 with the target
// view yaw - north yaw; state 0xf79a20 time, 0xf79a28 angle, 0xf79a2c velocity).
#pragma once

#include <cmath>
#include <cstdint>

namespace iw2::game {

class CompassSpring {
public:
    // Returns the springy angle in degrees [0, 360) for the target angle (any range) at time nowMs.
    float update(int nowMs, float targetAngle) {
        const float t = quant(targetAngle);
        int dt = nowMs - lastMs_;
        if (lastMs_ > nowMs || float(dt) > 500.0f) { // reset: first use, time went back, or a gap over 500 ms
            lastMs_ = nowMs;
            angle_ = t;
            vel_ = 0.0f;
            return angle_;
        }
        lastMs_ = nowMs;
        float delta = angleSubtract(angle_, t); // offset from the target
        while (dt > 0) {
            int step;
            if (dt > 5) { step = 5; dt -= 5; }
            else { step = dt; dt = 0; }
            const float s = float(step) * 0.001f;
            if (std::fabs(delta) < 0.25f && std::fabs(vel_) < 1.0f) { // at rest on the target: snap
                vel_ = 0.0f;
                angle_ = t;
                return angle_;
            }
            float x = quant(vel_ * s + delta);
            if (x > 180.0f) x -= 360.0f;
            delta = x;
            if (x > 0.0f) vel_ -= s * 1000.0f;       // restoring acceleration 1000 deg/s^2 towards the target
            else if (x < 0.0f) vel_ += s * 1000.0f;
            vel_ -= 2.0f * vel_ * s;                  // damping 2 /s
            if (vel_ > 0.0f) {
                if (x > 0.0f) vel_ -= vel_ * s * 3.5f; // moving away from the target: extra damping 3.5 /s
                vel_ -= s;                             // constant friction 1 deg/s^2, never reversing the sign
                if (vel_ < 0.0f) vel_ = 0.0f;
            } else {
                if (x < 0.0f) vel_ -= vel_ * s * 3.5f;
                vel_ += s;
                if (vel_ > 0.0f) vel_ = 0.0f;
            }
            if (vel_ > 30000.0f) vel_ = 30000.0f;
            else if (vel_ < -30000.0f) vel_ = -30000.0f;
        }
        angle_ = quant(delta + t);
        return angle_;
    }
    float velocity() const { return vel_; }

private:
    // x * 65536/360 truncated to 16 bits, back to degrees (0x565994 + 0x591e68 / 0x591f10).
    static float quant(float a) { return float(int(a * 182.04444885253906f) & 0xffff) * 0.0054931640625f; }
    // 0x423f50: a - b brought into (-180, 180].
    static float angleSubtract(float a, float b) {
        float d = a - b;
        while (d > 180.0f) d -= 360.0f;
        while (d <= -180.0f) d += 360.0f;
        return d;
    }
    int lastMs_ = 0x7fffffff; // forces the reset on first use
    float angle_ = 0.0f, vel_ = 0.0f;
};

} // namespace iw2::game
