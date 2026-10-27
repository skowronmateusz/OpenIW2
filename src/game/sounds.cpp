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
    // setreverb(priority, roomtype, drylevel, wetlevel, fadetime) (0x539380) and deactivatereverb(priority, fadetime) (0x539560):
    // player-only (error 0x590ab8 for another entity); the original formats the arguments (string 0x59098c; defaults dry 0.5,
    // wet 1.0, fade 0) and sends them to the client as a server command (0x44a3b0). They only drive the client's reverb
    // effect, which the port has no counterpart for, so the call is accepted and ignored.
    // TODO(spec): the command string and the client-side reverb.
    for (const char* name : {"setreverb", "deactivatereverb"})
        vm_.registerBuiltin(name, [this, name](CallContext& c) {
            Entity* e = entityOf(c.self);
            if (!e || !e->isPlayer) throw std::runtime_error(std::string(name) + ": entity is not a player");
            return Value();
        });
    // setcullfog(near, far, r, g, b, time) (0x4f5580 -> 0x4f53f0): sends "fog <near> <far> 1 <r> <g> <b> <time*1000>" to the client
    // (format "%g %g %g %g %g %g %.0f" 0x585bd4). TODO(spec): the original raises script errors for near < 0, far <= near,
    // colour components outside 0..1 and time < 0 (0x4f53f0); setexpfog (0x4f5630, no SP script uses it) is not implemented.
    // setexpfog(density, r, g, b, time) (0x4f5630 -> the same 0x4f53f0): sends "fog 0 1 <density> <r> <g> <b> <time*1000>";
    // the client takes a third value < 1 as the exponential density (R_SetFog arg7).
    // TODO(spec): the renderer's exponential fog (iw2view ignores such a command).
    vm_.registerBuiltin("setexpfog", [this](CallContext& c) {
        if (c.args.size() < 5) return Value();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%g %g %g %g %g %g %.0f", 0.0, 1.0, double(c.arg(0).asFloat()), double(c.arg(1).asFloat()),
                      double(c.arg(2).asFloat()), double(c.arg(3).asFloat()), double(c.arg(4).asFloat() * 1000.0f));
        clientCommands_.push_back({"fog", buf});
        return Value();
    });
    // setculldist(distance) (0x4f5700): configstring 8 = "%g" (the renderer's far cull distance). TODO(spec): not applied.
    vm_.registerBuiltin("setculldist", [this](CallContext& c) {
        cullDist_ = c.arg(0).asFloat();
        return Value();
    });
    // soundexists(alias) (0x4ef380 -> 0x428320): is there a sound alias of that name (hash lookup, case-insensitive).
    vm_.registerBuiltin("soundexists", [this](CallContext& c) { return Value(soundExists(c.arg(0).toString())); });
    // setshadowhint(name) (0x4ef700): sets entity state bits 0x2000..0xe000 (shadow hint, render only). TODO(spec): not applied.
    vm_.registerBuiltin("setshadowhint", [](CallContext&) { return Value(); });
    // saveGame(name, description, image) (0x4f3a30): writes a save game. Not modelled (no save system).
    vm_.registerBuiltin("savegame", [](CallContext&) { return Value(); });
    // savegamenocommit (0x4f3bd0) returns the save id (int, < 0 on failure; _autosave.gsc tryAutoSave); commitsave (0x4f3db0) and
    // issaverecentlyloaded (0x4f3da0, a byte set by a load) have no save system behind them.
    vm_.registerBuiltin("savegamenocommit", [](CallContext&) { return Value(1); });
    vm_.registerBuiltin("commitsave", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("issaverecentlyloaded", [](CallContext&) { return Value(false); });
    // playrumble (0x4f8bf0) and stoprumble (0x4f8c10) are bare `ret`s in the original.
    vm_.registerBuiltin("stoprumble", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("playrumble", [](CallContext&) { return Value(); });
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
