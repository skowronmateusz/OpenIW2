#include "net/udp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace iw2::net {

std::string NetAddress::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
                  static_cast<unsigned>(port));
    return buf;
}

bool NetAddress::parse(const std::string& text, NetAddress& out, uint16_t defaultPort) {
    std::string host = text;
    uint16_t port = defaultPort;
    size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(text.c_str() + colon + 1));
    }
    if (host == "localhost") host = "127.0.0.1";
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
    out.ip = ntohl(a.s_addr);
    out.port = port;
    return true;
}

struct MemoryUdp::Datagram {
    NetAddress from;
    std::vector<uint8_t> data;
};
struct MemoryUdp::Queues {
    std::map<uint16_t, std::deque<Datagram>> byPort;
};

namespace {
MemoryUdp* gMemoryUdp = nullptr;
constexpr uint32_t kLocalhost = 0x7f000001;
} // namespace

MemoryUdp::MemoryUdp() : queues_(new Queues) { gMemoryUdp = this; }
MemoryUdp::~MemoryUdp() {
    gMemoryUdp = nullptr;
    delete queues_;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint32_t ip, uint16_t port) {
    close();
    if (gMemoryUdp) {
        memoryPort_ = port ? port : gMemoryUdp->nextPort_++;
        gMemoryUdp->queues_->byPort[memoryPort_].clear();
        return true;
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) return false;
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ip);
    sa.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        close();
        return false;
    }
    return true;
}

bool UdpSocket::enableBroadcast() {
    int on = 1;
    return fd_ >= 0 && ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) == 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (memoryPort_ && gMemoryUdp) gMemoryUdp->queues_->byPort.erase(memoryPort_);
    memoryPort_ = 0;
}

uint16_t UdpSocket::localPort() const {
    if (memoryPort_) return memoryPort_;
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    if (fd_ < 0 || getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

uint32_t UdpSocket::localIp() const {
    if (memoryPort_) return 0;
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    if (fd_ < 0 || getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0xffffffffu;
    return ntohl(sa.sin_addr.s_addr);
}

bool UdpSocket::sendTo(const NetAddress& to, const void* data, int len) {
    if (memoryPort_) {
        auto q = gMemoryUdp->queues_->byPort.find(to.port);
        if (q != gMemoryUdp->queues_->byPort.end()) {
            const auto* d = static_cast<const uint8_t*>(data);
            q->second.push_back({NetAddress{kLocalhost, memoryPort_}, std::vector<uint8_t>(d, d + len)});
        }
        return true;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(to.ip);
    sa.sin_port = htons(to.port);
    return ::sendto(fd_, data, static_cast<size_t>(len), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == len;
}

int UdpSocket::receive(void* buf, int cap, NetAddress& from, int timeoutMs) {
    if (memoryPort_) {
        auto& q = gMemoryUdp->queues_->byPort[memoryPort_];
        if (q.empty()) return 0;
        const int n = std::min(cap, static_cast<int>(q.front().data.size()));
        std::memcpy(buf, q.front().data.data(), static_cast<size_t>(n));
        from = q.front().from;
        q.pop_front();
        return n;
    }
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeoutMs);
    if (r < 0) return -1;
    if (r == 0) return 0;
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    ssize_t n = ::recvfrom(fd_, buf, static_cast<size_t>(cap), 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return 0;
    from.ip = ntohl(sa.sin_addr.s_addr);
    from.port = ntohs(sa.sin_port);
    return static_cast<int>(n);
}

} // namespace iw2::net
