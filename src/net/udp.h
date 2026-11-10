// Non-blocking IPv4 UDP socket (macOS / Linux).
#pragma once

#include <cstdint>
#include <string>

namespace iw2::net {

struct NetAddress {
    uint32_t ip = 0;     // host byte order
    uint16_t port = 0;   // host byte order
    bool operator==(const NetAddress& o) const { return ip == o.ip && port == o.port; }
    std::string toString() const;
    static bool parse(const std::string& text, NetAddress& out, uint16_t defaultPort);
};

// An in-process network for single-threaded tests with an injected clock: while one is installed, every socket opened
// exchanges datagrams through its queues, delivered at once and in order, so a test does not depend on the scheduler.
class MemoryUdp {
public:
    MemoryUdp();
    ~MemoryUdp();
    MemoryUdp(const MemoryUdp&) = delete;
    MemoryUdp& operator=(const MemoryUdp&) = delete;

private:
    friend class UdpSocket;
    struct Datagram;
    struct Queues;
    Queues* queues_;
    uint16_t nextPort_ = 50000;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds to ip:port (port 0 = ephemeral). Returns false on error.
    bool open(uint32_t ip, uint16_t port);
    void close();
    // Allows sending to the broadcast address (LAN server browser).
    bool enableBroadcast();
    bool isOpen() const { return fd_ >= 0 || memoryPort_ != 0; }
    uint16_t localPort() const;
    uint32_t localIp() const; // the bound address (0: every interface)

    bool sendTo(const NetAddress& to, const void* data, int len);
    // Waits up to timeoutMs (0 = poll) for a datagram. Returns its length, 0 when none, -1 on error.
    int receive(void* buf, int cap, NetAddress& from, int timeoutMs);

private:
    int fd_ = -1;
    uint16_t memoryPort_ = 0;  // non-zero: bound on the installed MemoryUdp
};

} // namespace iw2::net
