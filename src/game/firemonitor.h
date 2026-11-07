// AI fire-rate monitor for iw2game --report (docs/debug-flags.md "Fire rate"): per team the shots fired by AI (the `shoot` built-in,
// turret gunners, vehicle turrets), the seconds an actor of the team was alive with a valid enemy ("combat seconds"), and the
// "silent shooters": actors that kept an enemy in line of sight for a long stretch without making a shot (the stretch also counts
// how often the actor started an animation with a "fire" notetrack meanwhile). Debug tooling only: the sampling reads the world
// and never changes it; it runs only when `enabled` is set.
#pragma once

#include "script/value.h"

#include <map>
#include <string>
#include <vector>

namespace iw2::game {

// One silent stretch: the enemy was visible for `visibleSecs` in a row (gaps under a second ignored), no shot; `animStarts` shooting animations started in it.
struct SilentShooter {
    int number = 0;
    std::string classname, team;
    script::Vec3 pos{0, 0, 0};
    double startSecs = 0, visibleSecs = 0;
    int animStarts = 0;
};

class FireMonitor {
public:
    // A visible stretch this long (the enemy out of sight for less than kGraceSecs does not end it) without a shot flags the actor.
    static constexpr double kSilentVisibleSecs = 10.0;
    static constexpr double kGraceSecs = 1.0;

    bool enabled = false;

    struct Sample {
        int number = 0;
        std::string classname, team;
        script::Vec3 pos{0, 0, 0};
        bool hasEnemy = false, enemyVisible = false;
        int fireAnimStarts = 0; // cumulative count of firing-animation starts of this actor
    };
    struct TeamTotals {
        int shots = 0;
        int animStarts = 0;
        double combatSecs = 0;
        double shotsPerMinute() const { return combatSecs > 0 ? shots * 60.0 / combatSecs : 0.0; }
    };

    // An AI shot (shooter = the actor, or the turret / vehicle with its gunner's team).
    void shot(int shooter, const std::string& team);
    // One time step of one living actor.
    void sample(const Sample& s, double dt);
    // The actor is gone (dead or removed): closes its running stretch.
    void closeActor(int number);
    void endRun();

    const std::map<std::string, TeamTotals>& teams() const { return teams_; }
    const std::vector<SilentShooter>& silent() const { return silent_; }
    void tick(double dt) { elapsed_ += dt; }

private:
    struct Actor {
        std::string classname, team;
        script::Vec3 pos{0, 0, 0};
        int shots = 0;
        int lastStarts = -1;           // starts seen at the previous sample
        double visible = 0;            // the running visible stretch
        int stretchStarts = 0;         // firing animation starts in it
        int shotsAtStart = 0;
        double hidden = 0;             // seconds without a visible enemy inside the stretch
        int silentIdx = -1;            // index into silent_ of the running stretch once flagged
    };
    void finishStretch(Actor& a);
    std::map<int, Actor> actors_;
    std::map<std::string, TeamTotals> teams_;
    std::vector<SilentShooter> silent_;
    double elapsed_ = 0;
};

} // namespace iw2::game
