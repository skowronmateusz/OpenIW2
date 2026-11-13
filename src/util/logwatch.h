#pragma once
// State-change watchers behind the event lines (docs/logging.md "Event lines"). Each is fed a few values per frame and
// returns the lines of the transitions since the previous call (nothing when nothing changed), or a summary every
// 10 s. They know no game types: the callers map their state into the samples. The caller passes the lines to
// iw2::log::event with the watcher's category, and feeds a watcher only while iw2::log::eventsEnabled().
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace iw2::log {

constexpr int64_t kSummaryMs = 10000;

inline const char* stanceName(int s) { return s == 1 ? "crouch" : s == 2 ? "prone" : "stand"; }

// The own player (category "player").
struct PlayerSample {
    bool alive = false;
    int stance = 0; // 0 stand, 1 crouch, 2 prone
    bool ladder = false;
    bool mantle = false;
    bool turret = false;
    std::string weapon;
    bool reloading = false;
};

class PlayerWatch {
public:
    void reset() { have_ = false; }
    // A footstep sound of the own player while in that stance (counted for the 10 s summary).
    void footstep(int stance) {
        if (stance >= 0 && stance < 3) ++steps_[stance];
    }
    std::vector<std::string> update(const PlayerSample& s, int64_t nowMs) {
        std::vector<std::string> out;
        if (windowStart_ < 0) windowStart_ = nowMs;
        if (!have_) {
            have_ = true;
            last_ = s;
            out.push_back(std::string(s.alive ? "alive" : "not alive") + ", " + stanceName(s.stance) + (s.weapon.empty() ? "" : ", weapon " + s.weapon) +
                          (s.turret ? ", on a turret" : "") + (s.ladder ? ", on a ladder" : ""));
        } else {
            const PlayerSample& p = last_;
            if (p.turret && !s.turret) out.push_back(std::string("turret drop (") + (s.alive ? "left" : "died") + ")");
            if (p.alive != s.alive) out.push_back(s.alive ? "spawned" : "died");
            if (s.alive) {
                if (p.stance != s.stance) out.push_back(std::string("stance ") + stanceName(p.stance) + " -> " + stanceName(s.stance));
                if (p.ladder != s.ladder) out.push_back(s.ladder ? "ladder on" : "ladder off");
                if (p.mantle != s.mantle) out.push_back(s.mantle ? "mantle start" : "mantle end");
                if (!p.turret && s.turret) out.push_back("turret use");
                if (p.weapon != s.weapon) out.push_back("weapon " + (p.weapon.empty() ? std::string("none") : p.weapon) + " -> " + (s.weapon.empty() ? std::string("none") : s.weapon));
                if (!p.reloading && s.reloading) out.push_back("reload start " + s.weapon);
            }
            last_ = s;
        }
        if (nowMs - windowStart_ >= kSummaryMs) {
            if (steps_[0] + steps_[1] + steps_[2] > 0)
                out.push_back("footsteps in " + std::to_string((nowMs - windowStart_ + 500) / 1000) + " s: stand " + std::to_string(steps_[0]) + ", crouch " +
                              std::to_string(steps_[1]) + ", prone " + std::to_string(steps_[2]));
            steps_[0] = steps_[1] = steps_[2] = 0;
            windowStart_ = nowMs;
        }
        return out;
    }

private:
    bool have_ = false;
    PlayerSample last_;
    int steps_[3] = {0, 0, 0};
    int64_t windowStart_ = -1;
};

// A network client's connection and clock (category "conn"): a summary every 10 s.
struct ConnSample {
    int snapshots = 0;        // received so far (a counter)
    int dropped = 0;          // undecodable so far (a counter)
    int newestMessage = 0;    // the newest snapshot's message number
    int64_t newestReceivedMs = 0;
    int timeDelta = 0;        // the clock's time delta
    bool extrapolating = false;
    int ping = -1;            // -1: not known
};

class ConnWatch {
public:
    void reset() { have_ = false; }
    std::vector<std::string> update(const ConnSample& s, int64_t nowMs) {
        std::vector<std::string> out;
        if (!have_) {
            have_ = true;
            start_ = s;
            windowStart_ = nowMs;
            lastMessage_ = s.newestMessage;
            lastReceivedMs_ = s.newestReceivedMs;
            lastDelta_ = minDelta_ = maxDelta_ = s.timeDelta;
            frames_ = extrapolated_ = late_ = resets_ = 0;
            return out;
        }
        ++frames_;
        if (s.extrapolating) ++extrapolated_;
        if (s.newestMessage != lastMessage_) {
            if (lastReceivedMs_ > 0 && s.newestReceivedMs - lastReceivedMs_ > kLateGapMs) ++late_;
            lastMessage_ = s.newestMessage;
            lastReceivedMs_ = s.newestReceivedMs;
        }
        if (std::abs(s.timeDelta - lastDelta_) > kResetJumpMs) ++resets_;
        lastDelta_ = s.timeDelta;
        minDelta_ = std::min(minDelta_, s.timeDelta);
        maxDelta_ = std::max(maxDelta_, s.timeDelta);
        if (nowMs - windowStart_ >= kSummaryMs) {
            const int snaps = s.snapshots - start_.snapshots;
            const double secs = double(nowMs - windowStart_) / 1000.0;
            char rate[32];
            std::snprintf(rate, sizeof rate, "%.1f", secs > 0 ? double(snaps) / secs : 0.0);
            out.push_back("client snapshots " + std::to_string(snaps) + " (" + rate + "/s), late " + std::to_string(late_) + ", dropped " +
                          std::to_string(s.dropped - start_.dropped) + ", ping " + (s.ping >= 0 ? std::to_string(s.ping) : std::string("?")) +
                          ", clock delta " + std::to_string(minDelta_) + ".." + std::to_string(maxDelta_) + ", resets " + std::to_string(resets_) +
                          ", extrapolated frames " + std::to_string(extrapolated_) + "/" + std::to_string(frames_));
            have_ = false;
            update(s, nowMs); // the next window starts here
        }
        return out;
    }
    static constexpr int64_t kLateGapMs = 50;  // a snapshot more than this after the previous one counts as late
    static constexpr int kResetJumpMs = 100;   // a time delta jump above this in one frame is a clock reset

private:
    bool have_ = false;
    ConnSample start_;
    int64_t windowStart_ = 0, lastReceivedMs_ = 0;
    int lastMessage_ = 0, lastDelta_ = 0, minDelta_ = 0, maxDelta_ = 0;
    int frames_ = 0, extrapolated_ = 0, late_ = 0, resets_ = 0;
};

// A server's frames and clients (category "conn"): a summary every 10 s.
class ServerWatch {
public:
    void frame(int64_t lateMs) {
        ++frames_;
        if (lateMs > kLateMs) ++late_;
        maxLate_ = std::max(maxLate_, lateMs);
    }
    void client(int slot, int commandLagMs, int ping) {
        auto& c = clients_[slot];
        c.first = std::max(c.first, commandLagMs);
        c.second = ping;
    }
    std::vector<std::string> update(int64_t nowMs) {
        std::vector<std::string> out;
        if (windowStart_ < 0) windowStart_ = nowMs;
        if (nowMs - windowStart_ < kSummaryMs) return out;
        std::string line = "server frames " + std::to_string(frames_) + ", late " + std::to_string(late_) + " (over " + std::to_string(kLateMs) +
                           " ms), latest " + std::to_string(maxLate_) + " ms";
        for (const auto& [slot, c] : clients_)
            line += ", slot " + std::to_string(slot) + " lag " + std::to_string(c.first) + " ping " + std::to_string(c.second);
        out.push_back(line);
        frames_ = late_ = 0;
        maxLate_ = 0;
        clients_.clear();
        windowStart_ = nowMs;
        return out;
    }
    static constexpr int64_t kLateMs = 10;

private:
    int64_t windowStart_ = -1, maxLate_ = 0;
    int frames_ = 0, late_ = 0;
    std::map<int, std::pair<int, int>> clients_; // slot -> largest command lag, last ping
};

// Entities of interest on a network client (category "ent"): +ent / -ent lines when they come and go.
class EntityWatch {
public:
    void reset() { have_ = false; }
    std::vector<std::string> update(const std::map<int, std::string>& kinds) {
        std::vector<std::string> out;
        if (!have_) {
            have_ = true;
            std::map<std::string, int> n;
            for (const auto& [num, k] : kinds) ++n[k];
            std::string line = "entities now:";
            for (const auto& [k, c] : n) line += " " + std::to_string(c) + " " + k;
            if (n.empty()) line += " none";
            out.push_back(line);
        } else {
            for (const auto& [num, k] : last_) {
                const auto it = kinds.find(num);
                if (it == kinds.end()) out.push_back("-ent " + std::to_string(num) + " " + k);
                else if (it->second != k) out.push_back("-ent " + std::to_string(num) + " " + k + " (now " + it->second + ")");
            }
            for (const auto& [num, k] : kinds) {
                const auto it = last_.find(num);
                if (it == last_.end() || it->second != k) out.push_back("+ent " + std::to_string(num) + " " + k);
            }
        }
        last_ = kinds;
        return out;
    }

private:
    bool have_ = false;
    std::map<int, std::string> last_;
};

} // namespace iw2::log
