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
    double suppressedSecs = 0; // seconds of the stretch the engine held the actor suppressed (issuppressed, 0x53e6a0)
    double movingSecs = 0;     // seconds in the move animscript
    double hidingSecs = 0;     // seconds outside it while suppressed and the scripts' last issuppressed answer was yes
    double meleeSecs = 0;      // seconds outside both in the melee script state (trackscriptstate "melee")
    std::string mainScript;    // the animscript the actor spent most of the stretch in (e.g. mg42/stand, cover_crouch)
    // A reason from the shipped animscripts why the actor does not shoot, or "" when the stretch is unexplained. Each sample
    // is explained by "running" when the actor is in the move animscript (its run and walk handlers never shoot: the run
    // script's shoot-while-running test is commented out, animscripts/run.gsc:13-42, and walk.gsc has no fire call;
    // move.gsc:119-129 picks one of the two), or by "suppressed" when the engine holds it suppressed and the last answer the
    // animscripts got from issuppressed was yes (the cover scripts hide or stop firing on it, e.g. cover_crouch.gsc:1179,
    // 1431), or by "melee" when the animscripts' own state (trackscriptstate, 0x540280) is "melee": animscripts/melee.gsc:6-8
    // MeleeCombat strikes instead of shooting as long as the enemy stays in melee range (melee.gsc:19-25; against a god-mode
    // player that is for ever). The stretch is unexplained when the samples explained by neither still add up to the silent threshold
    // (FireMonitor::kSilentVisibleSecs, the same test that flags a stretch); otherwise the larger explained part names it.
    std::string reason() const;
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
        bool suppressed = false;   // the engine's issuppressed state (World::isSuppressed)
        bool toldSuppressed = false; // the animscripts' last issuppressed call answered yes
        std::string scriptState;   // the last trackscriptstate state of the running animscript (e.g. "melee")
        std::string animScript;    // the running animscript
    };
    struct TeamTotals {
        int shots = 0;
        int animStarts = 0;
        double combatSecs = 0;
        double sightSecs = 0; // seconds an actor of the team had its enemy in line of sight (a subset of combatSecs)
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
    int unexplainedSilent() const {
        int n = 0;
        for (const auto& ss : silent_) n += ss.reason().empty();
        return n;
    }
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
        double suppressedSecs = 0;     // suppressed seconds in the stretch
        double movingSecs = 0, hidingSecs = 0, meleeSecs = 0;
        std::map<std::string, double> scriptSecs; // seconds per animscript in the stretch
    };
    void finishStretch(Actor& a);
    std::map<int, Actor> actors_;
    std::map<std::string, TeamTotals> teams_;
    std::vector<SilentShooter> silent_;
    double elapsed_ = 0;
};

} // namespace iw2::game
