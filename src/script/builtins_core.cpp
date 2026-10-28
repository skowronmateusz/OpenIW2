// Engine-independent built-ins: math, vectors, strings, random numbers (docs/spec/gsc-builtins.md).

#include "script/builtins_core.h"

#include <cmath>
#include <random>

namespace iw2::script {
namespace {

constexpr float kDegToRad = float(M_PI) / 180.0f;

Vec3 vec(const Value& v) { return v.asVector(); }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void angleVectors(const Vec3& angles, Vec3* forward, Vec3* right, Vec3* up) {
    const float sp = std::sin(angles[0] * kDegToRad), cp = std::cos(angles[0] * kDegToRad);
    const float sy = std::sin(angles[1] * kDegToRad), cy = std::cos(angles[1] * kDegToRad);
    const float sr = std::sin(angles[2] * kDegToRad), cr = std::cos(angles[2] * kDegToRad);
    if (forward) *forward = {cp * cy, cp * sy, -sp};
    if (right) *right = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    if (up) *up = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
}

} // namespace

std::shared_ptr<std::mt19937> registerCoreBuiltins(VM& vm, uint32_t seed) {
    auto rng = std::make_shared<std::mt19937>(seed);

    vm.registerBuiltin("sin", [](CallContext& c) { return Value(std::sin(c.arg(0).asFloat() * kDegToRad)); });
    vm.registerBuiltin("cos", [](CallContext& c) { return Value(std::cos(c.arg(0).asFloat() * kDegToRad)); });
    vm.registerBuiltin("tan", [](CallContext& c) { return Value(std::tan(c.arg(0).asFloat() * kDegToRad)); });
    vm.registerBuiltin("asin", [](CallContext& c) { return Value(std::asin(c.arg(0).asFloat()) / kDegToRad); });
    vm.registerBuiltin("acos", [](CallContext& c) { return Value(std::acos(c.arg(0).asFloat()) / kDegToRad); });
    vm.registerBuiltin("atan", [](CallContext& c) { return Value(std::atan(c.arg(0).asFloat()) / kDegToRad); });
    vm.registerBuiltin("abs", [](CallContext& c) { return c.arg(0).isInt() ? Value(std::abs(c.arg(0).asInt())) : Value(std::fabs(c.arg(0).asFloat())); });

    vm.registerBuiltin("distance", [](CallContext& c) {
        const Vec3 a = vec(c.arg(0)), b = vec(c.arg(1));
        const Vec3 d{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        return Value(std::sqrt(dot(d, d)));
    });
    vm.registerBuiltin("distancesquared", [](CallContext& c) {
        const Vec3 a = vec(c.arg(0)), b = vec(c.arg(1));
        const Vec3 d{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        return Value(dot(d, d));
    });
    vm.registerBuiltin("length", [](CallContext& c) { const Vec3 a = vec(c.arg(0)); return Value(std::sqrt(dot(a, a))); });
    vm.registerBuiltin("lengthsquared", [](CallContext& c) { const Vec3 a = vec(c.arg(0)); return Value(dot(a, a)); });
    vm.registerBuiltin("vectordot", [](CallContext& c) { return Value(dot(vec(c.arg(0)), vec(c.arg(1)))); });
    vm.registerBuiltin("vectornormalize", [](CallContext& c) {
        Vec3 a = vec(c.arg(0));
        const float l = std::sqrt(dot(a, a));
        if (l > 0) for (float& x : a) x /= l;
        return Value(a);
    });
    vm.registerBuiltin("vectortoangles", [](CallContext& c) {
        const Vec3 v = vec(c.arg(0));
        float yaw = 0, pitch = 0;
        if (v[1] == 0 && v[0] == 0) {
            pitch = v[2] > 0 ? 270.0f : 90.0f;
        } else {
            yaw = v[0] ? std::atan2(v[1], v[0]) / kDegToRad : (v[1] > 0 ? 90.0f : 270.0f);
            if (yaw < 0) yaw += 360.0f;
            const float forward = std::sqrt(v[0] * v[0] + v[1] * v[1]);
            pitch = std::atan2(v[2], forward) / kDegToRad;
            if (pitch < 0) pitch += 360.0f;
            pitch = -pitch;
            if (pitch < 0) pitch += 360.0f;
        }
        return Value(Vec3{pitch, yaw, 0.0f});
    });
    vm.registerBuiltin("anglestoforward", [](CallContext& c) { Vec3 f; angleVectors(vec(c.arg(0)), &f, nullptr, nullptr); return Value(f); });
    vm.registerBuiltin("anglestoright", [](CallContext& c) { Vec3 r; angleVectors(vec(c.arg(0)), nullptr, &r, nullptr); return Value(r); });
    vm.registerBuiltin("anglestoup", [](CallContext& c) { Vec3 u; angleVectors(vec(c.arg(0)), nullptr, nullptr, &u); return Value(u); });

    vm.registerBuiltin("randomint", [rng](CallContext& c) {
        const int32_t n = c.arg(0).asInt();
        if (n <= 0) return Value(int32_t(0));
        return Value(int32_t(std::uniform_int_distribution<int32_t>(0, n - 1)(*rng)));
    });
    vm.registerBuiltin("randomintrange", [rng](CallContext& c) {
        const int32_t lo = c.arg(0).asInt(), hi = c.arg(1).asInt();
        if (hi <= lo) return Value(lo);
        return Value(int32_t(std::uniform_int_distribution<int32_t>(lo, hi - 1)(*rng)));
    });
    vm.registerBuiltin("randomfloat", [rng](CallContext& c) {
        return Value(std::uniform_real_distribution<float>(0.0f, c.arg(0).asFloat())(*rng));
    });
    vm.registerBuiltin("randomfloatrange", [rng](CallContext& c) {
        const float lo = c.arg(0).asFloat(), hi = c.arg(1).asFloat();
        if (hi <= lo) return Value(lo);
        return Value(std::uniform_real_distribution<float>(lo, hi)(*rng));
    });

    vm.registerBuiltin("tolower", [](CallContext& c) {
        std::string s = c.arg(0).toString();
        for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return Value(s);
    });
    vm.registerBuiltin("getsubstr", [](CallContext& c) {
        const std::string s = c.arg(0).toString();
        const int32_t start = std::max(0, c.arg(1).asInt());
        const int32_t end = c.arg(2).isDefined() ? std::min<int32_t>(c.arg(2).asInt(), int32_t(s.size())) : int32_t(s.size());
        if (start >= end) return Value(std::string());
        return Value(s.substr(size_t(start), size_t(end - start)));
    });
    vm.registerBuiltin("issubstr", [](CallContext& c) {
        return Value(c.arg(0).toString().find(c.arg(1).toString()) != std::string::npos);
    });
    vm.registerBuiltin("strtok", [](CallContext& c) {
        const std::string s = c.arg(0).toString(), delims = c.arg(1).toString();
        auto out = std::make_shared<Array>();
        std::string cur;
        int32_t i = 0;
        auto flush = [&] {
            if (cur.empty()) return;
            ArrayKey k;
            k.i = i++;
            out->set(k, Value(cur));
            cur.clear();
        };
        for (char ch : s) {
            if (delims.find(ch) != std::string::npos) flush();
            else cur += ch;
        }
        flush();
        return Value(out);
    });
    return rng;
}

} // namespace iw2::script
