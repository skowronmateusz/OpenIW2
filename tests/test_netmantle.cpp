// A network player mantles: the shared server / prediction move (netgame::runUsercmd) climbs a mantle link of mp_toujane
// with the player_mantle_* animations, and does not without them (docs/spec/mp-pmove.md "Mantle").
#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/navgen.h"
#include "game/world.h"
#include "netgame/netmove.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

namespace {

// Runs a player from node a towards b with forward and jump held for up to 4 s; true when a mantle ran and it ended on b's level.
bool climbs(iw2::game::World& w, const std::array<float, 3>& a, const std::array<float, 3>& b) {
    iw2::game::PlayerState ps;
    ps.origin = {a[0], a[1], a[2]};
    const float yaw = std::atan2(b[1] - a[1], b[0] - a[0]) * 180.0f / 3.14159265f;
    int prev = 1000;
    bool mantled = false;
    for (int t = prev + 50; t <= prev + 4000; t += 50) {
        const auto cmd = iw2::netgame::makeUserCmd(t, iw2::game::Button::Jump, 1.0f, 0.0f, 0.0f, yaw, 0);
        iw2::netgame::runUsercmd(w.collision(), ps, cmd, t - 50);
        mantled = mantled || ps.mantling;
        if (mantled && std::fabs(ps.origin[2] - b[2]) < 24 && std::hypot(ps.origin[0] - b[0], ps.origin[1] - b[1]) < 48) return true;
    }
    return false;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    const iw2::game::MantleAnims* anims = w.mpMantleAnims();
    CHECK(anims != nullptr);
    const auto& g = w.mpNav();
    namespace F = iw2::game::NavLinkFlag;
    int tried = 0, withAnims = 0, without = 0;
    for (size_t i = 0; i < g.size() && tried < 3; ++i)
        for (const auto& l : g.links[i]) {
            if ((l.flags & F::Special) != F::Mantle) continue;
            const auto a = g.nodes[i], b = g.nodes[l.target];
            if (b[2] - a[2] < 45) continue; // a climb up, higher than a jump reaches (about 39 units)
            ++tried;
            iw2::netgame::setMantleAnims(anims);
            withAnims += climbs(w, a, b);
            iw2::netgame::setMantleAnims(nullptr);
            without += climbs(w, a, b);
            i += 40;
            break;
        }
    std::cout << "mantle links " << tried << ", climbed with anims " << withAnims << ", without " << without << "\n";
    CHECK(tried > 0);
    CHECK(withAnims == tried);
    CHECK(without == 0);
    return failures ? 1 : 0;
}
