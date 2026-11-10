// Host name lookup for addresses the player types (docs/spec/mp-ui.md "Addresses and name lookup"): a numeric IPv4 address or
// a DNS name (A record) with an optional port. The lookup runs on a thread of its own and is polled, so the UI never waits.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "net/udp.h"

namespace iw2::net {

// Splits "host[:port]". False when the host is empty or the port is not a number in 1..65535.
bool splitHostPort(const std::string& text, std::string& host, uint16_t& port, uint16_t defaultPort);

class Resolver {
public:
    enum class Status { Idle, Pending, Done, Failed };
    static constexpr int kTimeoutMs = 4000;

    // Starts a lookup (replaces one still running, whose thread ends on its own). A numeric address completes at once.
    void start(const std::string& text, uint16_t defaultPort, int64_t nowMs);
    Status poll(int64_t nowMs);
    const NetAddress& address() const { return address_; }
    const std::string& text() const { return text_; }
    void reset() { state_.reset(); status_ = Status::Idle; }

private:
    struct State;
    std::shared_ptr<State> state_;
    Status status_ = Status::Idle;
    NetAddress address_;
    std::string text_;
    int64_t startMs_ = 0;
};

// Waits for the lookup, at most timeoutMs. For the command line, before there is a window.
bool resolveBlocking(const std::string& text, NetAddress& out, uint16_t defaultPort, int timeoutMs);

} // namespace iw2::net
