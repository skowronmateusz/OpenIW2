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
    // Proven no-ops: playrumble 0x4f8bf0, stoprumble 0x4f8c10, playrumbleonpos 0x4f8b20 and giveachievement 0x4f8b50 are each a single
    // `ret` (byte c3) in CoD2SP_s.exe 1.3; the arguments are not even read (the interpreter drops them).
    vm_.registerBuiltin("stoprumble", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("playrumble", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("playrumbleonpos", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("giveachievement", [](CallContext&) { return Value(); });
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
        // 0x4ef3f0 only stores the alias' configstring index in the entity state (ent+6) and clears flag bit 0 of ent+0xa2; the
        // client keeps one running loop, so the same alias again does not restart it.
        const std::string alias = c.arg(0).toString();
        if (const Value h = obj->get("$loopsound"); h.isNumber()) {
            if (obj->get("$loopalias").toString() == alias) return Value();
            stopSound(h.asInt());
        }
        const auto o = originOf(c);
        obj->set("$loopsound", Value(int32_t(startSound(alias, o ? &*o : nullptr))));
        obj->set("$loopalias", Value(alias));
        return Value();
    });
    vm_.registerBuiltin("stoploopsound", [this](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const auto obj = c.self.asObject();
        if (const Value h = obj->get("$loopsound"); h.isNumber()) stopSound(h.asInt());
        obj->set("$loopsound", Value());
        obj->set("$loopalias", Value());
        return Value();
    });
    // Fade seconds (argument `i`, default 0) as milliseconds, truncated like the configstring time (0x4f3800: x1000 via 0x565994).
    auto fadeMs = [](const CallContext& c, size_t i) { return c.args.size() > i ? int(c.arg(i).asFloat() * 1000.0f) : 0; };
    // soundfade(volume [, time]) (0x4f3520): sends every client "snd_fade <volume> <ms>"; ms is 0 without the time argument.
    vm_.registerBuiltin("soundfade", [this, fadeMs](CallContext& c) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%f %i", double(c.arg(0).asFloat()), fadeMs(c, 1));
        clientCommands_.push_back({"snd_fade", buf});
        return Value();
    });
    // ambientplay(alias [, fade]) (0x4f3800): 1 or 2 arguments; errors for an empty alias, a negative fade and an unknown
    // alias. The original publishes configstring 3 "n\<alias>\t\<levelTime + fade ms>" and the client starts the ambient
    // loop. The configstring is modelled as client commands ambient_play <alias> <fade ms> and ambient_stop <fade ms> (client 0x443080: a new alias fades the old track out and itself in, the same alias is kept).
    vm_.registerBuiltin("ambientplay", [this, fadeMs](CallContext& c) {
        if (c.args.size() < 1 || c.args.size() > 2) throw std::runtime_error("Incorrect number of parameters\n");
        const std::string alias = c.arg(0).toString();
        if (alias.empty())
            throw std::runtime_error("ambientPlay: alias name cannot be the empty string... use stop or fade version\n");
        if (c.args.size() == 2 && c.arg(1).asFloat() < 0) throw std::runtime_error("ambientPlay: fade time must be >= 0\n");
        if (!soundExists(alias)) throw std::runtime_error("unknown sound alias '" + alias + "'");
        clientCommands_.push_back({"ambient_play", alias + " " + std::to_string(fadeMs(c, 1))});
        return Value();
    });
    // ambientstop([fade]) (0x4f3930): negative fade is an error; configstring 3 "t\<levelTime + fade ms>".
    vm_.registerBuiltin("ambientstop", [this, fadeMs](CallContext& c) {
        if (c.args.size() > 1) throw std::runtime_error("Incorrect number of parameters\n");
        if (c.args.size() == 1 && c.arg(0).asFloat() < 0) throw std::runtime_error("ambientStop: fade time must be >= 0\n");
        clientCommands_.push_back({"ambient_stop", std::to_string(fadeMs(c, 0))});
        return Value();
    });
    // musicplay(alias) (0x4f3400): unknown alias is a script error; sends the client "mu_play <alias>".
    vm_.registerBuiltin("musicplay", [this](CallContext& c) {
        const std::string alias = c.arg(0).toString();
        if (!soundExists(alias)) throw std::runtime_error("unknown sound alias '" + alias + "'");
        clientCommands_.push_back({"mu_play", alias});
        return Value();
    });
    // musicstop([fade seconds]) (0x4f3470): sends the client "mu_stop <ms>" with ms = int(fade * 1000 + 0.5); negative is an error.
    vm_.registerBuiltin("musicstop", [this](CallContext& c) {
        if (c.args.size() > 1) throw std::runtime_error("Incorrect number of parameters\n");
        int ms = 0;
        if (c.args.size() == 1) {
            const float f = c.arg(0).asFloat() * 1000.0f;
            ms = int(f + 0.5f);
            if (ms < 0) throw std::runtime_error("musicStop: fade time must be >= 0\n");
        }
        clientCommands_.push_back({"mu_stop", std::to_string(ms)});
        return Value();
    });
}

} // namespace iw2::game
