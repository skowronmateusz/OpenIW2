// Fire-rate monitor counters (src/game/firemonitor.h): team shots, combat seconds, shots per combat minute, and the silent-shooter
// rule (an enemy in sight for 10 s without a shot; short gaps of sight do not end the stretch, a longer one does).
#include "game/firemonitor.h"

#include <cmath>
#include <iostream>

using iw2::game::FireMonitor;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

FireMonitor::Sample actor(int n, const char* team, bool enemy, bool visible, int starts = 0) {
    FireMonitor::Sample s;
    s.number = n;
    s.classname = "actor_test";
    s.team = team;
    s.hasEnemy = enemy;
    s.enemyVisible = visible;
    s.fireAnimStarts = starts;
    return s;
}
} // namespace

int main() {
    {
        // Counters: 20 s of combat for an axis actor, 3 s for an allied one; 5 axis shots.
        FireMonitor m;
        for (int i = 0; i < 400; ++i) m.sample(actor(1, "axis", true, false), 0.05);
        for (int i = 0; i < 60; ++i) m.sample(actor(2, "allies", true, false), 0.05);
        for (int i = 0; i < 100; ++i) m.sample(actor(3, "axis", false, false), 0.05); // no enemy: no combat time
        for (int i = 0; i < 5; ++i) m.shot(1, "axis");
        const auto& t = m.teams();
        CHECK(t.at("axis").shots == 5);
        CHECK(std::fabs(t.at("axis").combatSecs - 20.0) < 0.01);
        CHECK(std::fabs(t.at("axis").shotsPerMinute() - 15.0) < 0.01);
        CHECK(t.at("allies").shots == 0 && std::fabs(t.at("allies").combatSecs - 3.0) < 0.01);
        CHECK(t.at("allies").shotsPerMinute() == 0.0);
        CHECK(m.silent().empty());
        CHECK(t.at("axis").sightSecs == 0.0 && t.at("allies").sightSecs == 0.0); // enemy known but never in sight: no sight time
    }
    {
        // Sight seconds count only the samples with the enemy in line of sight, a subset of the combat seconds.
        FireMonitor m;
        for (int i = 0; i < 100; ++i) m.sample(actor(1, "axis", true, false), 0.05); // 5 s known only
        for (int i = 0; i < 60; ++i) m.sample(actor(1, "axis", true, true), 0.05);   // 3 s in sight
        for (int i = 0; i < 40; ++i) m.sample(actor(2, "axis", false, true), 0.05);  // no enemy: no sight
        const auto& t = m.teams();
        CHECK(std::fabs(t.at("axis").combatSecs - 8.0) < 0.01);
        CHECK(std::fabs(t.at("axis").sightSecs - 3.0) < 0.01);
    }
    {
        // 12 s of sight without a shot: silent; its animation starts are counted.
        FireMonitor m;
        for (int i = 0; i < 240; ++i) {
            m.tick(0.05);
            m.sample(actor(7, "allies", true, true, i / 60), 0.05);
        }
        CHECK(m.silent().size() == 1);
        CHECK(m.silent()[0].number == 7 && m.silent()[0].visibleSecs >= 11.9 && m.silent()[0].animStarts == 3);
    }
    {
        // A shot inside the stretch, 9 s of sight only, or a gap of 1 s or more all keep an actor from being flagged.
        FireMonitor m;
        for (int i = 0; i < 240; ++i) {
            if (i == 100) m.shot(1, "axis");
            m.sample(actor(1, "axis", true, true), 0.05);
        }
        for (int i = 0; i < 180; ++i) m.sample(actor(2, "axis", true, true), 0.05);
        for (int rep = 0; rep < 3; ++rep) { // 7 s of sight, 1 s without: never 10 s in a row
            for (int i = 0; i < 140; ++i) m.sample(actor(3, "axis", true, true), 0.05);
            for (int i = 0; i < 20; ++i) m.sample(actor(3, "axis", true, false), 0.05);
        }
        CHECK(m.silent().empty());
    }
    {
        // A gap under a second does not end the stretch: 6 s, 0.5 s hidden, 6 s.
        FireMonitor m;
        for (int i = 0; i < 120; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        for (int i = 0; i < 10; ++i) m.sample(actor(4, "axis", true, false), 0.05);
        for (int i = 0; i < 120; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        CHECK(m.silent().size() == 1);
        // A death closes the stretch; the next one starts from zero.
        m.closeActor(4);
        for (int i = 0; i < 100; ++i) m.sample(actor(4, "axis", true, true), 0.05);
        CHECK(m.silent().size() == 1);
    }
    {
        // A shot ends the stretch: 5 s, a shot, then 12 s without one is a silent stretch of its own.
        FireMonitor m;
        for (int i = 0; i < 340; ++i) {
            if (i == 100) m.shot(9, "axis");
            m.tick(0.05);
            m.sample(actor(9, "axis", true, true), 0.05);
        }
        CHECK(m.silent().size() == 1);
        if (m.silent().size() == 1) CHECK(std::fabs(m.silent()[0].visibleSecs - 12.0) < 0.06 && std::fabs(m.silent()[0].startSecs - 5.0) < 0.06);
        // A flagged stretch stops growing at the first shot.
        for (int i = 0; i < 40; ++i) {
            if (i == 20) m.shot(9, "axis");
            m.tick(0.05);
            m.sample(actor(9, "axis", true, true), 0.05);
        }
        if (m.silent().size() == 1) CHECK(std::fabs(m.silent()[0].visibleSecs - 13.0) < 0.06);
    }
    {
        // Reasons. Each sample is explained by the move script (running) or by suppression the scripts were told about; a stretch is
        // unexplained when the rest still adds up to 10 s.
        FireMonitor m;
        auto stretch = [&](int n, auto&& set) {
            for (int i = 0; i < 240; ++i) { // 12 s
                FireMonitor::Sample s = actor(n, "axis", true, true);
                s.animScript = "combat";
                set(s, i);
                m.sample(s, 0.05);
            }
        };
        stretch(5, [](FireMonitor::Sample& s, int i) { // suppressed and told so throughout
            s.suppressed = s.toldSuppressed = true;
            s.animScript = i < 100 ? "cover_crouch" : "combat";
        });
        stretch(6, [](FireMonitor::Sample& s, int i) { s.animScript = i < 200 ? "move" : "stop"; }); // 10 s moving, 2 s in stop
        stretch(8, [](FireMonitor::Sample& s, int) { s.suppressed = true; });                       // suppressed, scripts told no
        stretch(10, [](FireMonitor::Sample& s, int i) { s.animScript = i < 80 ? "move" : "combat"; }); // 4 s moving, 8 s left
        stretch(11, [](FireMonitor::Sample& s, int i) { s.toldSuppressed = true; s.suppressed = i < 20; }); // told yes, engine 1 s only
        stretch(12, [](FireMonitor::Sample& s, int) { s.scriptState = "melee"; }); // the melee script state throughout
        CHECK(m.silent().size() == 6);
        if (m.silent().size() == 6) CHECK(m.silent()[5].reason() == "melee" && std::fabs(m.silent()[5].meleeSecs - 12.0) < 0.06);
        if (m.silent().size() == 6) {
            CHECK(std::fabs(m.silent()[0].suppressedSecs - 12.0) < 0.06 && m.silent()[0].mainScript == "combat");
            CHECK(m.silent()[0].reason() == "suppressed");
            CHECK(m.silent()[1].mainScript == "move" && m.silent()[1].reason() == "running");
            CHECK(m.silent()[2].reason().empty() && m.silent()[2].suppressedSecs >= 11.9);
            CHECK(m.silent()[3].reason() == "running" && m.silent()[3].mainScript == "combat");
            CHECK(m.silent()[4].reason().empty());
        }
        CHECK(m.unexplainedSilent() == 2);
    }
    if (failures) return 1;
    std::cout << "firemonitor OK\n";
    return 0;
}
