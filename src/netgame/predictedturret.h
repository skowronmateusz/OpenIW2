// The own map-turret fire of a network client, predicted (docs/spec/net.md section 10 "Predicted turret fire"; an
// openiw2 rule, the 1.3 client waits for the server's shot events). The server fires a client's turret in its game
// frame: the fire timer drops by 50 every frame, and with attack held a timer below 1 restarts at fireTime and shoots
// (docs/spec/turrets.md "Multiplayer"), so shots come every 50 * ceil(fireTime / 50) ms while attack is held and the
// fire loop runs on for fireTime * 3 after the last one. The client runs the same rule on its own commands' times:
// a press shoots at once when the timer has run out, the shots follow at the server's interval while attack is held,
// and the server's shot events of that turret are counted against the predicted ones instead of being played again.
#pragma once

#include <deque>

namespace iw2::netgame {

class PredictedTurret {
public:
    // The server's interval between two shots of a turret with this fireTime (ms) while attack is held.
    static int shotIntervalMs(int fireTimeMs) { return 50 * (fireTimeMs <= 1 ? 1 : (fireTimeMs - 1) / 50 + 1); }

    // One command on the turret `turret` (the client world's entity number) at commandTime with attack held or not.
    // Returns the shots to play now. A command not newer than the last one is skipped (the server skips it too).
    int command(int turret, int commandTime, bool attack, int fireTimeMs) {
        if (turret != turret_) {
            clear();
            turret_ = turret;
        }
        if (haveTime_ && commandTime <= lastTime_) return 0;
        haveTime_ = true;
        lastTime_ = commandTime;
        fireTimeMs_ = fireTimeMs;
        int shots = 0;
        if (attack) {
            const int interval = shotIntervalMs(fireTimeMs);
            // A press after the timer ran out shoots at once; held, the shots follow at the interval (a long command
            // gets the shots of the frames it spans, as many as the event ring holds).
            if (!held_ && (nextShot_ < 0 || commandTime >= nextShot_)) nextShot_ = commandTime;
            while (commandTime >= nextShot_ && shots < 4) {
                ++shots;
                lastShot_ = commandTime;
                pending_.push_back(commandTime);
                nextShot_ += interval;
            }
            if (commandTime >= nextShot_) nextShot_ = commandTime + interval;
        }
        held_ = attack;
        // Predicted shots the server has not confirmed within a second of command time are given up.
        while (!pending_.empty() && pending_.front() < commandTime - 1000) pending_.pop_front();
        return shots;
    }
    // The fire loop runs: within fireTime * 3 of the last predicted shot.
    bool looping() const { return lastShot_ >= 0 && lastTime_ < lastShot_ + 3 * fireTimeMs_; }
    // A shot event of the server on `turret`: true when it stands for a predicted shot (not played again).
    bool serverShot(int turret) {
        if (turret != turret_ || pending_.empty()) return false;
        pending_.pop_front();
        return true;
    }
    int turret() const { return turret_; }
    int pending() const { return int(pending_.size()); }
    void clear() { *this = PredictedTurret{}; }

private:
    int turret_ = -1;
    bool haveTime_ = false;
    bool held_ = false;   // attack held in the last command
    int lastTime_ = 0;
    int fireTimeMs_ = 50;
    int nextShot_ = -1;   // command time of the next possible shot (-1: none yet)
    int lastShot_ = -1;   // command time of the last predicted shot
    std::deque<int> pending_; // command times of the predicted shots the server's events have not matched yet
};

} // namespace iw2::netgame
