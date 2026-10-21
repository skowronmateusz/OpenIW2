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
    vm_.registerBuiltin("setclock", timer);
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
