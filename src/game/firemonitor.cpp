#include "game/firemonitor.h"

namespace iw2::game {

std::string SilentShooter::reason() const {
    if (visibleSecs - movingSecs - hidingSecs - meleeSecs >= FireMonitor::kSilentVisibleSecs) return "";
    if (movingSecs >= hidingSecs && movingSecs >= meleeSecs) return "running";
    return hidingSecs >= meleeSecs ? "suppressed" : "melee";
}

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
    // A shot ends the stretch (a flagged one keeps the seconds up to the shot) and the next silent stretch starts here.
    if (a.visible > 0 && a.shots != a.shotsAtStart) finishStretch(a);
    a.hidden = 0;
    if (a.visible == 0) a.shotsAtStart = a.shots;
    a.visible += dt;
    a.stretchStarts += started;
    if (s.suppressed) a.suppressedSecs += dt;
    if (s.animScript == "move") a.movingSecs += dt;
    else if (s.suppressed && s.toldSuppressed) a.hidingSecs += dt;
    else if (s.scriptState == "melee") a.meleeSecs += dt;
    a.scriptSecs[s.animScript] += dt;
    // Flag as soon as the stretch qualifies (a later shot ends it but does not undo it: the report lists the stretch).
    if (a.silentIdx < 0 && a.visible >= kSilentVisibleSecs) {
        a.silentIdx = int(silent_.size());
        SilentShooter ss;
        ss.number = s.number;
        ss.classname = a.classname;
        ss.team = a.team;
        ss.pos = a.pos;
        ss.startSecs = elapsed_ - a.visible;
        silent_.push_back(ss);
    }
    if (a.silentIdx >= 0) {
        silent_[size_t(a.silentIdx)].visibleSecs = a.visible;
        SilentShooter& ss = silent_[size_t(a.silentIdx)];
        ss.animStarts = a.stretchStarts;
        ss.suppressedSecs = a.suppressedSecs;
        ss.movingSecs = a.movingSecs;
        ss.hidingSecs = a.hidingSecs;
        ss.meleeSecs = a.meleeSecs;
        double best = -1;
        for (const auto& [name, secs] : a.scriptSecs)
            if (secs > best) best = secs, ss.mainScript = name;
    }
}

void FireMonitor::finishStretch(Actor& a) {
    a.visible = 0;
    a.stretchStarts = 0;
    a.hidden = 0;
    a.silentIdx = -1;
    a.suppressedSecs = 0;
    a.movingSecs = a.hidingSecs = a.meleeSecs = 0;
    a.scriptSecs.clear();
}

void FireMonitor::closeActor(int number) {
    if (auto it = actors_.find(number); it != actors_.end()) finishStretch(it->second);
}

void FireMonitor::endRun() {
    for (auto& [n, a] : actors_) finishStretch(a);
}

} // namespace iw2::game
