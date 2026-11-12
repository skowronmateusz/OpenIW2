// A client's commands on a bursty path (docs/spec/net.md section 10, "Commands on a bursty path"; openiw2, no 1.3
// counterpart): while the command packets keep arriving in late stretches, the accepted commands wait in a queue and
// the server runs them on an even schedule before each game frame, held back by the size of the late stretches.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "net/delaywindow.h"
#include "net/delta.h"

namespace iw2::net {

class CommandPlayout {
public:
    DelayWindow window;
    int referenceMs = 500;     // a packet is late against the least delayed one of this long before it
    int maxHoldMs = 100;       // the hold never exceeds this (every shot, use and turret command waits as long)
    int riseDivisor = 2;       // the hold rises by at most 1 / riseDivisor of the real time that passes
    int fallDivisor = 10;      // and falls by at most 1 / fallDivisor of it
    size_t maxQueue = 64;      // more waiting commands than this run at the next frame whatever their time (a 1.3 client keeps 128)

    void clear() { *this = CommandPlayout{}; }

    // A command packet arrived at nowMs; newestTime is the time of its newest command.
    void packet(int64_t nowMs, int newestTime) {
        if (baseMs_ < 0) baseMs_ = nowMs;
        window.add(nowMs, newestTime - rel(nowMs));
    }
    // The commands wait (push) instead of running on arrival: the path is bursty (as of the last frame), or the hold
    // of a path that was has not run down yet.
    bool holding() const { return hold_ > 0 || !queue_.empty() || bursty_; }
    void push(const UserCmd& cmd) { queue_.push_back(cmd); }

    // Before the game frame at frameMs (real time): moves the hold and returns the commands to run now, in the order
    // they were accepted.
    std::vector<UserCmd> due(int64_t frameMs) {
        std::vector<UserCmd> out;
        const int dt = lastFrameMs_ < 0 ? 0 : static_cast<int>(std::clamp<int64_t>(frameMs - lastFrameMs_, 0, 200));
        lastFrameMs_ = frameMs;
        window.expire(frameMs);
        if (window.empty()) {  // no command packet for the whole window: nothing to schedule against
            hold_ = 0, restUp_ = restDown_ = 0;
            bursty_ = false;
            return flush();
        }
        const DelayWindow::Stats st = window.stats(frameMs, referenceMs);
        bursty_ = st.bursty;
        const int best = st.reference;
        const int target = bursty_ ? std::min(st.lateness, maxHoldMs) : 0;
        if (target > hold_) {
            restUp_ += dt;
            hold_ = std::min(target, hold_ + restUp_ / riseDivisor);
            restUp_ %= riseDivisor;
        } else if (target < hold_) {
            restDown_ += dt;
            hold_ = std::max(target, hold_ - restDown_ / fallDivisor);
            restDown_ %= fallDivisor;
        }
        // A command of time c on the least delayed path arrives at c - best; it is due hold_ later.
        const int64_t limit = static_cast<int64_t>(rel(frameMs)) + best - hold_;
        while (!queue_.empty() && (queue_.front().serverTime <= limit || queue_.size() > maxQueue)) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return out;
    }
    // Every waiting command (a jump of the server's time).
    std::vector<UserCmd> flush() {
        std::vector<UserCmd> out(queue_.begin(), queue_.end());
        queue_.clear();
        return out;
    }
    void reset() {
        hold_ = 0, restUp_ = restDown_ = 0;
        bursty_ = false;
        window.clear();
    }

    int holdMs() const { return hold_; }
    size_t waiting() const { return queue_.size(); }

private:
    int rel(int64_t ms) const { return static_cast<int>(ms - baseMs_); }
    std::deque<UserCmd> queue_;
    int64_t baseMs_ = -1;       // real time of the first packet: the window's offsets stay small
    int64_t lastFrameMs_ = -1;
    int hold_ = 0;
    bool bursty_ = false;       // the window at the last frame
    int restUp_ = 0, restDown_ = 0;  // real ms not yet turned into a change of the hold
};

} // namespace iw2::net
