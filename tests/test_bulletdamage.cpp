// Bullet damage by distance and target (exe 0x531090).

#include "game/bulletdamage.h"

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
    if (failures == 0) std::cout << "bulletdamage ok\n";
    return failures ? 1 : 0;
}
