// Script sound built-ins and weapon sounds (docs/spec/sound.md). Mixing and output belong to the front end,
// which installs World::playSound / World::stopSound.
#include <cstdio>
#include "game/world.h"

namespace iw2::game {

using namespace script;

int World::startSound(const std::string& alias, const Vec3* origin, ObjectPtr notifyObj, const std::string& notify) {
    float duration = 0;
    const int handle = playSound(alias, origin, &duration);
    // A notify name asks for "<name>" when the sound finishes (immediately if nothing played).
    if (notifyObj && !notify.empty())
        pendingNotifies_.push_back({timeMs_ + int64_t(std::max(0.0f, duration) * 1000.0f), notifyObj, notify});
    return handle;
}

void World::registerSoundBuiltins() {
    auto originOf = [](const CallContext& c) -> std::optional<Vec3> {
        if (!c.self.isObject()) return std::nullopt;
        const Value o = c.self.asObject()->get("origin");
        if (!o.isVector()) return std::nullopt;
        return o.asVector();
    };
    // setblur(amount, time) (0x4f3340): sends "scr_blur <time*1000> <amount> 0 1" to the client.
    vm_.registerBuiltin("setblur", [this](CallContext& c) {
        const float amount = c.arg(0).asFloat(), time = c.arg(1).asFloat();
        // TODO(spec): the original raises script errors "Time must be positive" / "Blur value must be greater than 0".
        if (time < 0 || amount < 0) return Value();
        clientCommands_.push_back({"scr_blur", std::to_string(int(time * 1000.0f)) + " " + std::to_string(amount) + " 0 1"});
        return Value();
    });
    // setcullfog(near, far, r, g, b, time) (0x4f5580 -> 0x4f53f0): sends "fog <near> <far> 1 <r> <g> <b> <time*1000>" to the client
    // (format "%g %g %g %g %g %g %.0f" 0x585bd4). TODO(spec): the original raises script errors for near < 0, far <= near,
    // colour components outside 0..1 and time < 0 (0x4f53f0); setexpfog (0x4f5630, no SP script uses it) is not implemented.
    vm_.registerBuiltin("setcullfog", [this](CallContext& c) {
        if (c.args.size() < 6) return Value();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%g %g %g %g %g %g %.0f", double(c.arg(0).asFloat()), double(c.arg(1).asFloat()), 1.0, double(c.arg(2).asFloat()),
                      double(c.arg(3).asFloat()), double(c.arg(4).asFloat()), double(c.arg(5).asFloat() * 1000.0f));
        clientCommands_.push_back({"fog", buf});
        return Value();
    });
    vm_.registerBuiltin("playsound", [this, originOf](CallContext& c) {
        const auto o = originOf(c);
        const bool local = c.self.isObject() && entityOf(c.self) && entityOf(c.self)->isPlayer;
        startSound(c.arg(0).toString(), local || !o ? nullptr : &*o, c.self.isObject() ? c.self.asObject() : nullptr,
                   c.args.size() > 1 ? c.arg(1).toString() : std::string());
        return Value();
    });
    vm_.registerBuiltin("playsoundasmaster", [this, originOf](CallContext& c) {
        const auto o = originOf(c);
        startSound(c.arg(0).toString(), o ? &*o : nullptr, c.self.isObject() ? c.self.asObject() : nullptr,
                   c.args.size() > 1 ? c.arg(1).toString() : std::string());
        return Value();
    });
    vm_.registerBuiltin("playlocalsound", [this](CallContext& c) {
        startSound(c.arg(0).toString(), nullptr);
        return Value();
    });
    vm_.registerBuiltin("playloopsound", [this, originOf](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const auto obj = c.self.asObject();
        if (const Value h = obj->get("$loopsound"); h.isNumber()) stopSound(h.asInt());
        const auto o = originOf(c);
        obj->set("$loopsound", Value(int32_t(startSound(c.arg(0).toString(), o ? &*o : nullptr))));
        return Value();
    });
    vm_.registerBuiltin("stoploopsound", [this](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const auto obj = c.self.asObject();
        if (const Value h = obj->get("$loopsound"); h.isNumber()) stopSound(h.asInt());
        obj->set("$loopsound", Value());
        return Value();
    });
    vm_.registerBuiltin("ambientplay", [this](CallContext& c) {
        if (ambientHandle_) stopSound(ambientHandle_);
        ambientHandle_ = startSound(c.arg(0).toString(), nullptr);
        return Value();
    });
    vm_.registerBuiltin("ambientstop", [this](CallContext&) {
        if (ambientHandle_) stopSound(ambientHandle_);
        ambientHandle_ = 0;
        return Value();
    });
    vm_.registerBuiltin("musicplay", [this](CallContext& c) {
        if (musicHandle_) stopSound(musicHandle_);
        musicHandle_ = startSound(c.arg(0).toString(), nullptr);
        return Value();
    });
    vm_.registerBuiltin("musicstop", [this](CallContext&) {
        if (musicHandle_) stopSound(musicHandle_);
        musicHandle_ = 0;
        return Value();
    });
}

} // namespace iw2::game
