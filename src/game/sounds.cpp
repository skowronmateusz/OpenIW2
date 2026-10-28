// Script sound built-ins and weapon sounds (docs/spec/sound.md). Mixing and output belong to the front end,
// which installs World::playSound / World::stopSound.
#include <cstdio>
#include "game/fog.h"
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
        // Argument errors (0x4f3358..0x4f33c9, script error 0x468ed0 with the argument number in 0x9bef1c): time < 0 ->
        // "Time must be positive" (0x586600, arg 2), then amount < 0 -> "Blur value must be greater than 0" (0x5865dc, arg 1).
        if (time < 0) throw std::runtime_error("Time must be positive");
        if (amount < 0) throw std::runtime_error("Blur value must be greater than 0");
        clientCommands_.push_back({"scr_blur", std::to_string(int(time * 1000.0f)) + " " + std::to_string(amount) + " 0 1"});
        return Value();
    });
    // setreverb(priority, roomtype [, dry [, wet [, fade]]]) (0x539380; 2..5 arguments, defaults dry 1.0, wet 0.5, fade 0) and
    // deactivatereverb(priority [, fade]) (0x539560): player-only (error 0x590ab8 for another entity); the priority must be one of the
    // two snd_enveffectsprio_* names (level -> 1, shellshock -> 2). They send the player the client command
    // `reverb %i "%s" %g %g %g` / `deactivatereverb %i %g` (0x44a3b0). The client state is audio/reverb.h.
    auto reverbPrio = [](const CallContext& c) {
        const std::string p = c.arg(0).toString();
        if (p == "snd_enveffectsprio_level") return 1;
        if (p == "snd_enveffectsprio_shellshock") return 2;
        throw std::runtime_error("priority must be 'snd_enveffectsprio_level' or 'snd_enveffectsprio_shellshock'\n");
    };
    for (const char* name : {"setreverb", "deactivatereverb"}) {
        const bool set = name[0] == 's';
        vm_.registerBuiltin(name, [this, name, set, reverbPrio](CallContext& c) {
            Entity* e = entityOf(c.self);
            if (!e || !e->isPlayer) throw std::runtime_error(std::string(name) + ": entity is not a player");
            const size_t n = c.args.size();
            if (set ? (n < 2 || n > 5) : (n < 1 || n > 2)) throw std::runtime_error("Incorrect number of parameters\n");
            const int prio = reverbPrio(c);
            char buf[256];
            if (set) {
                const float dry = n > 2 ? c.arg(2).asFloat() : 1.0f, wet = n > 3 ? c.arg(3).asFloat() : 0.5f,
                            fade = n > 4 ? c.arg(4).asFloat() : 0.0f;
                std::snprintf(buf, sizeof buf, "%i \"%s\" %g %g %g", prio, c.arg(1).toString().c_str(), double(dry), double(wet), double(fade));
                clientCommands_.push_back({"reverb", buf});
            } else {
                std::snprintf(buf, sizeof buf, "%i %g", prio, double(n > 1 ? c.arg(1).asFloat() : 0.0f));
                clientCommands_.push_back({"deactivatereverb", buf});
            }
            return Value();
        });
    }
    // setexpfog(density, r, g, b, time) (0x4f5630 -> the shared 0x4f53f0): exactly 5 arguments, density in (0, 1) (error 0x585b8c), then the
    // shared colour / time checks; sends "fog 0 1 <density> <r> <g> <b> <time*1000>" (the client takes a third value < 1 as the exponential
    // density: FogState::commandExp, R_SetFog arg7). The shared sender (0x4f5512) skips the command and the
    // AI sight update when the global 0x1c61048 is 2. TODO(spec): that global's meaning (not modelled; the command is always sent).
    vm_.registerBuiltin("setexpfog", [this](CallContext& c) {
        if (c.args.size() != 5) throw std::runtime_error("Incorrect number of parameters\n");
        const float d = c.arg(0).asFloat(), r = c.arg(1).asFloat(), g = c.arg(2).asFloat(), b = c.arg(3).asFloat(), t = c.arg(4).asFloat();
        if (!fogDensityValid(d)) throw std::runtime_error("setExpFog: distance must be greater than 0 and less than 1");
        if (const std::string e = fogArgError("setExpFog", 0.0f, 1.0f, r, g, b, t); !e.empty()) throw std::runtime_error(e);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%g %g %g %g %g %g %.0f", 0.0, 1.0, double(d), double(r), double(g), double(b), double(t * 1000.0f));
        clientCommands_.push_back({"fog", buf});
        // 0x4e96e0 on "0 1 <density> ...": only a density of 1 or more sets the AI sight limit ((1 - 0) * 0.828)^2; a valid density is < 1,
        // so the limit is always cleared here.
        fogSightSq_ = d >= 1.0f ? 0.828f * 0.828f : 3.4028235e38f;
        return Value();
    });
    // setculldist(distance) (0x4f5700): exactly 1 argument; configstring 8 = "%g" (the renderer's far cull distance). Consumer: client
    // 0x4a56d0 -> R_SetCullDist (gfx dll 0x1000ab00) -> farPlaneDistance (fog.h, docs/spec/render-hints.md); read via World::cullDist().
    vm_.registerBuiltin("setculldist", [this](CallContext& c) {
        if (c.args.size() != 1) throw std::runtime_error("Incorrect number of parameters\n");
        cullDist_ = c.arg(0).asFloat();
        return Value();
    });
    // soundexists(alias) (0x4ef380 -> 0x428320): is there a sound alias of that name (hash lookup, case-insensitive).
    vm_.registerBuiltin("soundexists", [this](CallContext& c) { return Value(soundExists(c.arg(0).toString())); });
    // setshadowhint(name) (0x4ef700): replaces the entity state bits 0x2000..0xe000 by the hint's value (shadowHintBits in fog.h); an unknown
    // name is the script error 0x587058. Render only: the bits are stored in Entity::shadowHint.
    // Consumers: the client maps the bits to refEntity render flags (0x4c1e2a, 0x4aacb0) and the gfx dll's shadow-caster selection
    // scores them (shadowCasterScore in fog.h, docs/spec/render-hints.md). TODO(spec): their savegame round trip; no shadow pass here.
    vm_.registerBuiltin("setshadowhint", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("setshadowhint: not an entity");
        uint32_t bits = 0;
        if (!shadowHintBits(c.arg(0).toString(), bits))
            throw std::runtime_error("setshadowhint argument must be \"normal\", \"never\", \"high_priority\", \"low_priority\", \"always\", or \"receiver\".");
        e->shadowHint = bits;
        return Value();
    });
    // savegame / savegamenocommit / commitsave / issaverecentlyloaded / issavesuccessful: src/game/savestate.cpp.
    // Proven no-ops: playrumble 0x4f8bf0, stoprumble 0x4f8c10, playrumbleonpos 0x4f8b20 and giveachievement 0x4f8b50 are each a single
    // `ret` (byte c3) in CoD2SP_s.exe 1.3; the arguments are not even read (the interpreter drops them).
    vm_.registerBuiltin("stoprumble", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("playrumble", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("playrumbleonpos", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("giveachievement", [](CallContext&) { return Value(); });
    // setcullfog(near, far, r, g, b, time) (0x4f5580 -> 0x4f53f0): exactly 6 arguments, then the fogArgError checks; sends
    // "fog <near> <far> 1 <r> <g> <b> <time*1000>" (format "%g %g %g %g %g %g %.0f" 0x585bd4). Same 0x1c61048 TODO(spec) as setexpfog.
    vm_.registerBuiltin("setcullfog", [this](CallContext& c) {
        if (c.args.size() != 6) throw std::runtime_error("Incorrect number of parameters\n");
        const float nr = c.arg(0).asFloat(), fr = c.arg(1).asFloat(), r = c.arg(2).asFloat(), g = c.arg(3).asFloat(), b = c.arg(4).asFloat(),
                    t = c.arg(5).asFloat();
        if (const std::string e = fogArgError("setCullFog", nr, fr, r, g, b, t); !e.empty()) throw std::runtime_error(e);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%g %g %g %g %g %g %.0f", double(nr), double(fr), 1.0, double(r), double(g), double(b), double(t * 1000.0f));
        clientCommands_.push_back({"fog", buf});
        const float lim = (fr - nr) * 0.828f + nr;
        fogSightSq_ = lim * lim; // 0x4e96e0: the AI sight limit of linear fog (0x1c61038)
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
