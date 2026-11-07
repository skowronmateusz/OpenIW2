#include "game/firemonitor.h"

namespace iw2::game {

void FireMonitor::shot(int shooter, const std::string& team) {
    ++teams_[team].shots;
    ++actors_[shooter].shots;
}

void FireMonitor::sample(const Sample& s, double dt) {
    Actor& a = actors_[s.number];
    a.classname = s.classname;
    a.team = s.team;
    a.pos = s.pos;
    const int started = a.lastStarts < 0 ? 0 : s.fireAnimStarts - a.lastStarts;
    a.lastStarts = s.fireAnimStarts;
    teams_[s.team].animStarts += started;
    if (s.hasEnemy) teams_[s.team].combatSecs += dt;
    if (s.hasEnemy && s.enemyVisible) teams_[s.team].sightSecs += dt;
    if (!(s.hasEnemy && s.enemyVisible)) {
        a.hidden += dt;
        if (a.hidden >= kGraceSecs) finishStretch(a);
        return;
    }
    a.hidden = 0;
    if (a.visible == 0) a.shotsAtStart = a.shots;
    a.visible += dt;
    a.stretchStarts += started;
    // Flag as soon as the stretch qualifies (a shot afterwards does not undo it: the report lists the stretch).
    if (a.silentIdx < 0 && a.visible >= kSilentVisibleSecs && a.shots == a.shotsAtStart) {
        a.silentIdx = int(silent_.size());
        silent_.push_back({s.number, a.classname, a.team, a.pos, elapsed_ - a.visible, a.visible, a.stretchStarts});
    }
    if (a.silentIdx >= 0) {
        silent_[size_t(a.silentIdx)].visibleSecs = a.visible;
        silent_[size_t(a.silentIdx)].animStarts = a.stretchStarts;
    }
}

void FireMonitor::finishStretch(Actor& a) {
    a.visible = 0;
    a.stretchStarts = 0;
    a.hidden = 0;
    a.silentIdx = -1;
}

void FireMonitor::closeActor(int number) {
    if (auto it = actors_.find(number); it != actors_.end()) finishStretch(it->second);
}

void FireMonitor::endRun() {
    for (auto& [n, a] : actors_) finishStretch(a);
}

} // namespace iw2::game
