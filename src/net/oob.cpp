// Implemented from the spec (docs/spec/net.md section 5).
#include "net/oob.h"

#include <cstdlib>
#include <cstring>

#include "net/msg.h"

namespace iw2::net {

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = line.size();
    while (out.size() < 512) {
        // Skip whitespace and comments.
        for (;;) {
            while (i < n && static_cast<uint8_t>(line[i]) <= ' ') ++i;
            if (i + 1 < n && line[i] == '/' && line[i + 1] == '/') return out;
            if (i + 1 < n && line[i] == '/' && line[i + 1] == '*') {
                i += 2;
                while (i < n && !(i + 1 < n && line[i] == '*' && line[i + 1] == '/')) ++i;
                if (i < n) i += 2;
                continue;
            }
            break;
        }
        if (i >= n) return out;
        std::string tok;
        if (line[i] == '"') {
            ++i;
            // A backslash before a quote is dropped and the quote stays in the token (docs/spec/net.md section 5).
            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n && line[i + 1] == '"') ++i;
                tok.push_back(line[i++]);
            }
            if (i < n) ++i;
            out.push_back(tok);
            continue;
        }
        while (i < n && static_cast<uint8_t>(line[i]) > ' ' && line[i] != '"' &&
               !(i + 1 < n && line[i] == '/' && (line[i + 1] == '/' || line[i + 1] == '*')))
            tok.push_back(line[i++]);
        out.push_back(tok);
    }
    return out;
}

std::string infoValueForKey(const std::string& info, const std::string& key) {
    size_t i = 0;
    if (i < info.size() && info[i] == '\\') ++i;
    while (i < info.size()) {
        size_t ke = info.find('\\', i);
        if (ke == std::string::npos) return {};
        std::string k = info.substr(i, ke - i);
        size_t ve = info.find('\\', ke + 1);
        std::string v = info.substr(ke + 1, ve == std::string::npos ? std::string::npos : ve - ke - 1);
        if (k == key) return v;
        if (ve == std::string::npos) break;
        i = ve + 1;
    }
    return {};
}

void infoSetValueForKey(std::string& info, const std::string& key, const std::string& value) {
    // Remove an existing key, then append.
    std::string rebuilt;
    size_t i = 0;
    if (i < info.size() && info[i] == '\\') ++i;
    while (i < info.size()) {
        size_t ke = info.find('\\', i);
        if (ke == std::string::npos) break;
        size_t ve = info.find('\\', ke + 1);
        std::string k = info.substr(i, ke - i);
        std::string v = info.substr(ke + 1, ve == std::string::npos ? std::string::npos : ve - ke - 1);
        if (k != key) rebuilt += "\\" + k + "\\" + v;
        if (ve == std::string::npos) break;
        i = ve + 1;
    }
    if (!value.empty()) rebuilt += "\\" + key + "\\" + value;
    info = rebuilt;
}

std::vector<uint8_t> makeOutOfBand(const std::string& text) {
    std::vector<uint8_t> out(4, 0xff);
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

std::optional<std::string> outOfBandText(const uint8_t* data, int len) {
    if (len < 4 || data[0] != 0xff || data[1] != 0xff || data[2] != 0xff || data[3] != 0xff) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(data + 4), static_cast<size_t>(len - 4));
}

std::optional<std::string> parseOutOfBand(const uint8_t* data, int len) {
    if (len < 4 || data[0] != 0xff || data[1] != 0xff || data[2] != 0xff || data[3] != 0xff) return std::nullopt;
    std::vector<uint8_t> copy(data, data + len);
    Msg m(copy.data(), len);
    m.setCursize(len);
    m.beginReading();
    m.readLong();
    return m.readStringLine();
}

namespace {
std::string str(int v) { return std::to_string(v); }
} // namespace

std::string buildInfoResponse(const ServerInfo& s, const std::string& challenge) {
    std::string info;
    infoSetValueForKey(info, "challenge", challenge);
    infoSetValueForKey(info, "protocol", str(kProtocolVersion));
    infoSetValueForKey(info, "hostname", s.hostname);
    infoSetValueForKey(info, "mapname", s.mapname);
    if (s.clients != 0) infoSetValueForKey(info, "clients", str(s.clients));
    if (s.maxClients > 0) infoSetValueForKey(info, "sv_maxclients", str(s.maxClients));
    infoSetValueForKey(info, "gametype", s.gametype);
    if (s.pure) infoSetValueForKey(info, "pure", "1");
    if (s.minPing != 0) infoSetValueForKey(info, "minPing", str(s.minPing));
    if (s.maxPing != 0) infoSetValueForKey(info, "maxPing", str(s.maxPing));
    if (!s.fsGame.empty()) infoSetValueForKey(info, "game", s.fsGame);
    if (s.allowAnonymous) infoSetValueForKey(info, "sv_allowAnonymous", "1");
    if (s.conDisabled) infoSetValueForKey(info, "con_disabled", "1");
    if (s.password) infoSetValueForKey(info, "pswrd", "1");
    if (s.friendlyFire != 0) infoSetValueForKey(info, "ff", str(s.friendlyFire));
    if (s.killcam != 0) infoSetValueForKey(info, "kc", str(s.killcam));
    infoSetValueForKey(info, "hw", "1");
    infoSetValueForKey(info, "mod", s.mod ? "1" : "0");
    infoSetValueForKey(info, "voice", s.voice ? "1" : "0");
    infoSetValueForKey(info, "pb", "0");
    return "infoResponse\n" + info;
}

std::string buildStatusResponse(const ServerInfo& s, const std::string& challenge) {
    std::string info;
    for (const auto& kv : s.statusCvars) infoSetValueForKey(info, kv.first, kv.second);
    infoSetValueForKey(info, "challenge", challenge);
    infoSetValueForKey(info, "pswrd", s.password ? "1" : "0");
    infoSetValueForKey(info, "mod", s.mod ? "1" : "0");
    std::string players;
    for (const PlayerStatus& p : s.players) {
        std::string line = str(p.score) + " " + str(p.ping) + " \"" + p.name + "\"\n";
        if (players.size() + line.size() > 0x1ffff) break;
        players += line;
    }
    return "statusResponse\n" + info + "\n" + players;
}

int ConnectionlessServer::random15() {
    rng_ = rng_ * 1103515245u + 12345u;
    return static_cast<int>((rng_ >> 16) & 0x7fff);
}

int ConnectionlessServer::challengeFor(const Address& from) const {
    for (const Challenge& c : challenges_)
        if (c.used && c.adr == from) return c.challenge;
    return 0;
}

std::string ConnectionlessServer::handleGetChallenge(const Address& from, const std::vector<std::string>& args) {
    size_t slot = challenges_.size();
    size_t oldest = 0;
    int oldestTime = 0x7fffffff;
    for (size_t i = 0; i < challenges_.size(); ++i) {
        if (challenges_[i].used && challenges_[i].adr == from) {
            slot = i;
            break;
        }
        // A free entry is taken before any used one: the table time counts from the server start, so a challenge
        // issued in the first millisecond has time 0 and would otherwise tie with the free entries and be evicted
        // by the next address's getchallenge (EXE_BAD_CHALLENGE for the first client).
        const int t = challenges_[i].used ? challenges_[i].time : -1;
        if (t < oldestTime) {
            oldestTime = t;
            oldest = i;
        }
    }
    if (slot == challenges_.size()) {
        slot = oldest;
        Challenge& c = challenges_[slot];
        int hi = random15();
        int lo = random15();
        c = Challenge{};
        c.used = true;
        c.adr = from;
        c.challenge = lo ^ (hi << 16) ^ time;
        c.time = time;
    }
    if (args.size() == 3) challenges_[slot].guid = args[2].substr(0, 32);
    // openiw2 runs without authorisation: always the immediate (LAN) answer.
    return "challengeResponse " + str(challenges_[slot].challenge);
}

std::optional<std::string> ConnectionlessServer::handle(const Address& from, const std::string& line,
                                                        std::optional<ConnectRequest>* accepted) {
    if (line.compare(0, 3, "pb_") == 0) return std::nullopt;  // PunkBuster traffic is ignored
    std::vector<std::string> args = tokenize(line);
    if (args.empty()) return std::nullopt;
    const std::string arg1 = args.size() > 1 ? args[1] : std::string();
    const std::string& cmd = args[0];
    if (cmd == "getstatus") return buildStatusResponse(info, arg1);
    if (cmd == "getinfo") return buildInfoResponse(info, arg1);
    if (cmd == "getchallenge") return handleGetChallenge(from, args);
    if (cmd == "connect") {
        const std::string& userinfo = arg1;
        int protocol = std::atoi(infoValueForKey(userinfo, "protocol").c_str());
        if (protocol != kProtocolVersion)
            return std::string("error\nEXE_SERVER_IS_DIFFERENT_VER\x15") + kProtocolVersionString + "\n";
        int challenge = std::atoi(infoValueForKey(userinfo, "challenge").c_str());
        int qport = std::atoi(infoValueForKey(userinfo, "qport").c_str());
        bool ok = false;
        for (const Challenge& c : challenges_)
            if (c.used && c.adr == from && c.challenge == challenge) ok = true;
        if (!ok) return std::string("error\nEXE_BAD_CHALLENGE");
        // TODO(spec): ping limits, reconnect throttling, password, slots, game rejection (E2).
        if (accepted) *accepted = ConnectRequest{from, userinfo, qport, challenge};
        return std::string("connectResponse");
    }
    return std::nullopt;
}

} // namespace iw2::net
