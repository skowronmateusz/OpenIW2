// Tests for the grenade hint node geometry: pick 0x51ece0, MagicGrenade lob 0x51f050, sort comparator 0x51f280.

#include "assets/font.h"
#include "fs/vfs.h"
#include "game/grenadehint.h"

#include <cstdlib>

#include <cmath>
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

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

void testPick() {
    const HintVec start{0, 0, 0}, target{500, 0, 0};
    HintVec out{};
    // Nothing in an empty table.
    CHECK(!grenadeHintPick({}, start, target, kGrenadeHintArcDefault, out));
    // Too low (z − 10 < target z), beyond the target (same direction from both ends), too far (> 192 in 2D).
    CHECK(!grenadeHintPick({{480, 0, 5}, {520, 0, 60}, {250, 0, 60}}, start, target, kGrenadeHintArcDefault, out));
    // Between thrower and target, above it: the one nearest the target wins.
    CHECK(grenadeHintPick({{400, 0, 60}, {450, 0, 60}, {480, 0, 5}}, start, target, kGrenadeHintArcDefault, out));
    CHECK(out[0] == 450 && out[2] == 60);
    // Equal distances keep the first entry.
    CHECK(grenadeHintPick({{450, 10, 60}, {450, -10, 60}}, start, target, kGrenadeHintArcDefault, out));
    CHECK(out[1] == 10);
    // The cone: dot(target→hint, start→hint) ≤ cos(arc/2 + 180°). (450, 100) gives −0.2425: outside the default 150°
    // arc (limit −0.2588), inside arc 180 (limit cos 270° ≈ 0).
    const HintVec side{450, 100, 60};
    CHECK(!grenadeHintPick({side}, start, target, kGrenadeHintArcDefault, out));
    CHECK(grenadeHintPick({side}, start, target, 180.0f, out));
}

void testLob() {
    const float g = 800.0f;
    const HintVec start{0, 0, 0}, target{600, 0, 0};
    HintVec vel{}, land{};
    // Through (300, 150) and (600, 0): z(s) = s − s²/600, horizontal speed √240000, vertical the same.
    CHECK(grenadeHintLob({300, 0, 150}, start, target, g, vel, land));
    CHECK(near(vel[0], 489.898f) && near(vel[1], 0) && near(vel[2], 489.898f));
    CHECK(land[0] == 600 && land[1] == 0 && land[2] == 0);
    // The flight reaches the landing point at the landing height.
    const float t = land[0] / vel[0];
    CHECK(near(vel[2] * t - 0.5f * g * t * t, 0.0f, 0.1f));
    // Rejected: behind the thrower, below the straight line, beyond the target in 2D, landing > 192 from the target,
    // launch speed² above 810000.
    CHECK(!grenadeHintLob({-300, 0, 150}, start, target, g, vel, land));
    CHECK(!grenadeHintLob({300, 0, -10}, start, target, g, vel, land));
    CHECK(!grenadeHintLob({700, 0, 150}, start, target, g, vel, land));
    CHECK(!grenadeHintLob({300, 300, 150}, start, target, g, vel, land));
    CHECK(!grenadeHintLob({500, 0, 200}, start, HintVec{1000, 0, 0}, g, vel, land));
    // Directly above the start (L = 0) is not between.
    CHECK(!grenadeHintLob({0, 0, 150}, start, target, g, vel, land));
}

void testSort() {
    std::vector<HintVec> h{{100, 0, 0}, {0, 0, 10}, {0, 50, 0}, {0, -50, 0}};
    grenadeHintSortByDistance(h, {0, 0, 0});
    CHECK(h[0][2] == 10 && h[1][1] == 50 && h[2][1] == -50 && h[3][0] == 100);
}
}  // namespace

// With IW2_MAIN: the moscow potato's text keys resolve (weapon file displayName WEAPON_POTATO, the moscow hints), and
// its weapon file is the non-exploding kind the explosion code keys on (projExplosionType none, no inner damage).
void testPotatoGameFiles() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) return;
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    for (const char* lang : {"ENGLISH", "POLISH"}) {
        iw2::assets::Localization loc;
        loc.load(vfs, lang);
        CHECK(loc.find("WEAPON_POTATO") && !loc.find("WEAPON_POTATO")->empty());
        CHECK(loc.find("MOSCOW_GRAB_POTATO"));
        CHECK(loc.find("MOSCOW_PLATFORM_THROW_POTATO"));
        CHECK(loc.find("MOSCOW_OBJ_POTATO1"));
    }
    const auto w = vfs.read("weapons/sp/potato");
    CHECK(w.has_value());
    if (w) {
        const std::string t(w->begin(), w->end());
        CHECK(t.find("\\displayName\\WEAPON_POTATO\\") != std::string::npos);
        CHECK(t.find("\\projExplosionType\\none\\") != std::string::npos);
        CHECK(t.find("\\explosionInnerDamage\\0\\") != std::string::npos);
    }
}

int main() {
    testPick();
    testLob();
    testSort();
    testPotatoGameFiles();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "grenadehint: ok\n";
    return 0;
}
