// Host name lookup; implemented from the spec (docs/spec/mp-ui.md "Addresses and name lookup").
#include "net/resolve.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace iw2::net {

bool splitHostPort(const std::string& text, std::string& host, uint16_t& port, uint16_t defaultPort) {
    host = text;
    port = defaultPort;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        const std::string p = text.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        const int v = std::atoi(p.c_str());
        if (v < 1 || v > 65535) return false;
        port = uint16_t(v);
    }
    if (host.empty()) return false;
    for (const char c : host)
        if (c == ' ' || c == '\t' || c == ':' || c == '\\' || c == '"' || c == ';') return false;
    return true;
}

struct Resolver::State {
    std::atomic<int> result{0}; // 0 running, 1 found, -1 not found
    uint32_t ip = 0;
};

namespace {
bool numeric(const std::string& host, uint32_t& ip) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
    ip = ntohl(a.s_addr);
    return true;
}
} // namespace

void Resolver::start(const std::string& text, uint16_t defaultPort, int64_t nowMs) {
    text_ = text;
    startMs_ = nowMs;
    state_.reset();
    std::string host;
    uint16_t port = 0;
    if (!splitHostPort(text, host, port, defaultPort)) {
        status_ = Status::Failed;
        return;
    }
    address_ = NetAddress{0, port};
    if (numeric(host, address_.ip)) {
        status_ = Status::Done;
        return;
    }
    status_ = Status::Pending;
    state_ = std::make_shared<State>();
    std::thread([s = state_, host] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        int r = -1;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            s->ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
            r = 1;
        }
        if (res) freeaddrinfo(res);
        s->result.store(r);
    }).detach();
}

Resolver::Status Resolver::poll(int64_t nowMs) {
    if (status_ != Status::Pending) return status_;
    const int r = state_->result.load();
    if (r > 0) {
        address_.ip = state_->ip;
        status_ = Status::Done;
    } else if (r < 0 || nowMs - startMs_ >= kTimeoutMs) {
        status_ = Status::Failed;
    }
    if (status_ != Status::Pending) state_.reset();
    return status_;
}

bool resolveBlocking(const std::string& text, NetAddress& out, uint16_t defaultPort, int timeoutMs) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto ms = [&] { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count()); };
    Resolver r;
    r.start(text, defaultPort, 0);
    Resolver::Status s = r.poll(0);
    while (s == Resolver::Status::Pending && ms() < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        s = r.poll(ms());
    }
    if (s != Resolver::Status::Done) return false;
    out = r.address();
    return true;
}

} // namespace iw2::net
