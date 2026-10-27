// Bullet damage by distance and target (exe 0x531090).

#include "game/bulletdamage.h"

#include <cmath>
#include <iostream>
#include <map>
#include <string>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n";               \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
using Def = std::map<std::string, std::string>;
} // namespace

int main() {
    using iw2::game::bulletDamageAtDistance;
    // g43: damage 100, minDamage 45, ranges 768..3000
    const Def g43{{"weaponclass", "rifle"}, {"damage", "100"}, {"mindamage", "45"}, {"maxdamagerange", "768"}, {"mindamagerange", "3000"}};
    CHECK(bulletDamageAtDistance(g43, true, 100) == 100);
    CHECK(bulletDamageAtDistance(g43, true, 767.9f) == 100);
    CHECK(bulletDamageAtDistance(g43, true, 3000) == 45);
    CHECK(bulletDamageAtDistance(g43, false, 5000) == 45);
    // halfway (1884): 100*0.5 + 45*0.5 = 72.5 -> truncated 72
    CHECK(bulletDamageAtDistance(g43, false, 1884) == 72);
    // mg42 turret: damage 250, playerDamage 30; the client uses playerDamage, others damage
    const Def mg{{"weaponclass", "turret"}, {"damage", "250"}, {"playerdamage", "30"}, {"mindamage", "250"},
                 {"minplayerdamage", "30"}, {"maxdamagerange", "15000"}, {"mindamagerange", "16000"}};
    CHECK(bulletDamageAtDistance(mg, true, 5000) == 30);
    CHECK(bulletDamageAtDistance(mg, false, 5000) == 250);
    CHECK(bulletDamageAtDistance(mg, true, 15500) == 30);
    // a non-turret weapon ignores playerDamage
    Def smg = mg;
    smg["weaponclass"] = "smg";
    CHECK(bulletDamageAtDistance(smg, true, 5000) == 250);
    // equal ranges: no blend
    const Def eq{{"damage", "90"}, {"mindamage", "10"}, {"maxdamagerange", "500"}, {"mindamagerange", "500"}};
    CHECK(bulletDamageAtDistance(eq, false, 100) == 90);
    CHECK(bulletDamageAtDistance(eq, false, 600) == 10);
    // accuracy graph (g43 aivsplayer.accu)
    iw2::game::AccuracyGraph g;
    CHECK(iw2::game::parseAccuracyGraph("WEAPONACCUFILE\n\n6\n0.0000 0.7148\n0.1481 0.6615\n0.2379 0.4027\n0.2994 0.1003\n0.3750 0.0000\n1.0000 0.0000\n", g));
    CHECK(g.points.size() == 6);
    CHECK(std::fabs(iw2::game::evalAccuracyGraph(g, 0) - 0.7148f) < 1e-5f);
    CHECK(std::fabs(iw2::game::evalAccuracyGraph(g, 0.1481f / 0.00025f) - 0.6615f) < 1e-4f);
    CHECK(iw2::game::evalAccuracyGraph(g, 5000) == 0.0f); // x clamped to 1
    CHECK(std::fabs(iw2::game::aiHitChance(g, 0, 1.8f, 1.0f) - 1.0f) < 1e-6f); // 0.7148 * 1.8 > 1 -> 1
    CHECK(std::fabs(iw2::game::aiHitChance(g, 0, 0.5f, 1.0f) - 0.3574f) < 1e-5f);
    CHECK(!iw2::game::parseAccuracyGraph("nonsense", g));
    if (failures == 0) std::cout << "bulletdamage ok\n";
    return failures ? 1 : 0;
}
