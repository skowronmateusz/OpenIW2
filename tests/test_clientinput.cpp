// Tests for the client commands and button bits (docs/spec/controls.md) with the shipped default.cfg bindings.

#include "game/clientinput.h"

#include <iostream>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2::game;

const char* kCfg = R"(unbindall
bind w	"+forward"
bind SHIFT	"+melee_breath"
bind MOUSE1	"+attack"
bind MOUSE2	"toggleads"
bind 1	"weapnext"
bind g		"+frag"
bind f		"+activate"
bind SPACE	"+gostand"
bind CTRL	"goprone"
bind c		"gocrouch"
// comment
)";

void bindings() {
    ClientInput in;
    in.loadConfig(kCfg);
    CHECK(in.bindings().at("W") == "+forward");
    CHECK(in.keysFor("+gostand") == std::vector<std::string>{"SPACE"});
    in.key("W", true);
    CHECK(in.forwardMove() == 127);
    in.key("W", false);
    CHECK(in.forwardMove() == 0);
}

void rebinding() {
    ClientInput in;
    in.loadConfig(kCfg);
    in.rebind("+gostand", "X"); // slot 1 free: second key
    CHECK((in.keysFor("+gostand") == std::vector<std::string>{"SPACE", "X"}));
    in.rebind("+gostand", "Y"); // both taken: replaced by the new key alone
    CHECK((in.keysFor("+gostand") == std::vector<std::string>{"Y"}));
    in.rebind("+forward", "Y"); // a key leaves its previous command
    CHECK(in.keysFor("+gostand").empty());
    CHECK((in.keysFor("+forward") == std::vector<std::string>{"W", "Y"}));
    in.rebind("+forward", ""); // Del clears
    CHECK(in.keysFor("+forward").empty());
    ClientInput again; // the saved text round-trips, slot order included
    in.rebind("+frag", "Z");
    again.loadConfig(in.formatBindings());
    CHECK((again.keysFor("+frag") == std::vector<std::string>{"G", "Z"}));
    CHECK(again.bindings().size() == in.bindings().size());
}

void stance() {
    ClientInput in;
    in.loadConfig(kCfg);
    in.key("C", true);
    CHECK(in.buttons(false) & Button::Crouch);
    in.key("C", false);
    in.key("CTRL", true);
    uint32_t b = in.buttons(false);
    CHECK((b & Button::Prone) && !(b & Button::Crouch));
    // SPACE from prone stands up without jumping; standing it jumps.
    in.key("SPACE", true);
    b = in.buttons(false);
    CHECK(!(b & (Button::Prone | Button::Crouch | Button::Jump)) && in.stance() == 0);
    in.key("SPACE", false);
    in.key("SPACE", true);
    CHECK(in.buttons(false) & Button::Jump);
    in.key("SPACE", false);
    CHECK(!(in.buttons(false) & Button::Jump));
}

void adsAndWeapons() {
    ClientInput in;
    in.loadConfig(kCfg);
    in.key("MOUSE2", true);
    in.key("MOUSE2", false);
    CHECK(in.buttons(false) & Button::Ads); // toggled on
    in.key("MOUSE2", true);
    CHECK(!(in.buttons(false) & Button::Ads));
    in.key("SHIFT", true);
    const uint32_t b = in.buttons(false);
    CHECK((b & Button::Melee) && (b & Button::HoldBreath));
    in.key("G", true);
    CHECK(in.buttons(false) & Button::Frag);
    in.key("1", true);
    CHECK(in.takeWeaponCycle() == 1 && in.takeWeaponCycle() == 0);
    // A press and release within one frame still reaches the command (wasPressed).
    in.key("F", true);
    in.key("F", false);
    CHECK(in.buttons(false) & Button::Activate);
    CHECK(!(in.buttons(false) & Button::Activate));
}

} // namespace

int main() {
    bindings();
    stance();
    adsAndWeapons();
    rebinding();
    if (failures) return 1;
    std::cout << "clientinput tests passed\n";
    return 0;
}
