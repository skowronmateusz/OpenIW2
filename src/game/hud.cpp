// Script HUD elements (newhudelem and friends; docs/spec/ui.md). The world keeps their state; the front end
// draws them. Timed changes (fade, move, scale) keep their start values so the current value can be
// interpolated at draw time.
#include "game/world.h"

#include <algorithm>

namespace iw2::game {

using namespace script;

void World::registerHudBuiltins() {
    auto create = [this](CallContext&) {
        ObjectPtr h = vm_.newObject("hudelem");
        h->set("x", Value(0.0f));
        h->set("y", Value(0.0f));
        h->set("alpha", Value(1.0f));
        h->set("color", Value(Vec3{1, 1, 1}));
        h->set("fontscale", Value(1.0f));
        h->set("alignx", Value("left"));
        h->set("aligny", Value("top"));
        h->set("horzalign", Value("left"));
        h->set("vertalign", Value("top"));
        h->set("sort", Value(0.0f));
        hudElems_.push_back(h);
        return Value(h);
    };
    vm_.registerBuiltin("newhudelem", create);
    vm_.registerBuiltin("newclienthudelem", create);
    vm_.registerBuiltin("newteamhudelem", create);
    vm_.registerBuiltin("setshader", [](CallContext& c) {
        const auto h = c.self.asObject();
        h->set("$shader", Value(c.arg(0).toString()));
        h->set("$width", c.args.size() > 1 ? c.arg(1) : Value(int32_t(0)));
        h->set("$height", c.args.size() > 2 ? c.arg(2) : Value(int32_t(0)));
        h->set("$text", Value());
        return Value();
    });
    vm_.registerBuiltin("settext", [](CallContext& c) {
        c.self.asObject()->set("$text", c.arg(0));
        c.self.asObject()->set("$shader", Value());
        return Value();
    });
    vm_.registerBuiltin("setvalue", [](CallContext& c) {
        c.self.asObject()->set("$value", c.arg(0));
        c.self.asObject()->set("$shader", Value());
        return Value();
    });
    auto timer = [this](CallContext& c) {
        c.self.asObject()->set("$timerend", Value(float(timeMs_) + c.arg(0).asFloat() * 1000.0f));
        c.self.asObject()->set("$shader", Value());
        return Value();
    };
    vm_.registerBuiltin("settimer", timer);
    vm_.registerBuiltin("settenthstimer", timer);
    // settimerup / settenthstimerup (0x530780, 0x5307a0, types 5 and 7): the counter shows now - start (0x4c1f30, 0x4c1f6a).
    auto timerUp = [this](CallContext& c) {
        c.self.asObject()->set("$timerend", Value(float(timeMs_) + c.arg(0).asFloat() * 1000.0f));
        c.self.asObject()->set("$timerup", Value(int32_t(1)));
        c.self.asObject()->set("$shader", Value());
        return Value();
    };
    vm_.registerBuiltin("settimerup", timerUp);
    vm_.registerBuiltin("settenthstimerup", timerUp);
    // setclock / setclockup (0x5307b0, 0x5307c0 -> 0x530510, types 8 and 9): setClock(seconds, duration, material, width, height).
    // The element end (+0x68) is now + seconds, +0x6c the duration in ms (rounded), +0x34 / +0x38 the size, the material is the
    // face; cgame 0x4c2c30 draws it and the material named material + "needle" rotated by the clock angle. A clock counts
    // down (end - now) or up (now - end).
    auto clock = [this](bool up) {
        return [this, up](CallContext& c) {
            const auto h = c.self.asObject();
            h->set("$timerend", Value(float(timeMs_) + float(int(c.arg(0).asFloat() * 1000.0f + 0.5f))));
            h->set("$clockdur", Value(int32_t(c.arg(1).asFloat() * 1000.0f + 0.5f)));
            h->set("$clockup", Value(int32_t(up ? 1 : 0)));
            h->set("$shader", Value(c.arg(2).toString()));
            h->set("$width", c.args.size() > 3 ? c.arg(3) : Value(int32_t(0)));
            h->set("$height", c.args.size() > 4 ? c.arg(4) : Value(int32_t(0)));
            h->set("$text", Value());
            return Value();
        };
    };
    vm_.registerBuiltin("setclock", clock(false));
    vm_.registerBuiltin("setclockup", clock(true));
    vm_.registerBuiltin("fadeovertime", [this](CallContext& c) {
        const auto h = c.self.asObject();
        h->set("$fadestart", Value(float(timeMs_)));
        h->set("$fadetime", Value(c.arg(0).asFloat() * 1000.0f));
        h->set("$fadealpha", h->get("alpha"));
        h->set("$fadecolor", h->get("color"));
        return Value();
    });
    vm_.registerBuiltin("moveovertime", [this](CallContext& c) {
        const auto h = c.self.asObject();
        h->set("$movestart", Value(float(timeMs_)));
        h->set("$movetime", Value(c.arg(0).asFloat() * 1000.0f));
        h->set("$movex", h->get("x"));
        h->set("$movey", h->get("y"));
        return Value();
    });
    vm_.registerBuiltin("scaleovertime", [this](CallContext& c) {
        const auto h = c.self.asObject();
        h->set("$scalestart", Value(float(timeMs_)));
        h->set("$scaletime", Value(c.arg(0).asFloat() * 1000.0f));
        h->set("$scalew", h->get("$width"));
        h->set("$scaleh", h->get("$height"));
        h->set("$width", c.arg(1));
        h->set("$height", c.arg(2));
        return Value();
    });
    vm_.registerBuiltin("destroy", [this](CallContext& c) {
        if (!c.self.isObject()) return Value();
        const auto h = c.self.asObject();
        h->removed = true;
        std::erase(hudElems_, h);
        return Value();
    });
}

} // namespace iw2::game
