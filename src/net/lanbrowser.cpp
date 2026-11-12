// Server browser; implemented from the spec (docs/spec/net.md sections 5 and 12, docs/spec/mp-ui.md "Join server").
#include "net/lanbrowser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <strings.h>

#include "net/oob.h"

namespace iw2::net {

std::string LanServer::hostname() const { return infoValueForKey(info, "hostname"); }
std::string LanServer::map() const { return infoValueForKey(info, "mapname"); }
std::string LanServer::gametype() const { return infoValueForKey(info, "gametype"); }
int LanServer::clients() const { return std::atoi(infoValueForKey(info, "clients").c_str()); }
int LanServer::maxClients() const { return std::atoi(infoValueForKey(info, "sv_maxclients").c_str()); }
bool LanServer::password() const { return std::atoi(infoValueForKey(info, "pswrd").c_str()) != 0; }

namespace {
int key(const LanServer& s, const char* k) { return std::atoi(infoValueForKey(s.info, k).c_str()); }
std::string lowerKey(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// The canonical form of an address for the duplicate test: lower case with the port written out.
std::string canon(const std::string& a, uint16_t defaultPort) {
    std::string host;
    uint16_t port = 0;
    if (!splitHostPort(a, host, port, defaultPort)) return lowerKey(a);
    return lowerKey(host) + ":" + std::to_string(port);
}
} // namespace

bool passesFilter(const LanServer& s, const BrowserFilter& f) {
    if (s.favorite && s.pingMs <= 0) return true; // a favorite that has not answered stays listed
    if (!f.showFull && s.maxClients() > 0 && s.clients() == s.maxClients()) return false;
    if (!f.showEmpty && s.clients() == 0) return false;
    if (!f.showPassword && key(s, "pswrd") != 0) return false;
    if (!f.showNoPassword && key(s, "pswrd") == 0) return false;
    if (f.showPure && key(s, "pure") == 0) return false;
    if (f.showDedicated) { const int hw = key(s, "hw"); if (hw < 1 || hw > 3) return false; }
    if (f.mod >= 0 && key(s, "mod") != f.mod) return false;
    if (f.friendlyFire >= 0 && key(s, "ff") != f.friendlyFire) return false;
    if (f.killcam >= 0 && key(s, "kc") != f.killcam) return false;
    if (f.punkBuster >= 0 && key(s, "pb") != f.punkBuster) return false;
    if (!f.gametype.empty() && strcasecmp(f.gametype.c_str(), s.gametype().c_str()) != 0) return false;
    return true;
}

int FavoriteList::add(const std::string& name, const std::string& address, uint16_t defaultPort) {
    std::string host;
    uint16_t port = 0;
    if (!splitHostPort(address, host, port, defaultPort)) return -2;
    if (contains(address, defaultPort)) return 0;
    if (int(items_.size()) >= kMax) return -1;
    items_.push_back(Favorite{name, address});
    return 1;
}

bool FavoriteList::contains(const std::string& address, uint16_t defaultPort) const {
    const std::string c = canon(address, defaultPort);
    return std::any_of(items_.begin(), items_.end(), [&](const Favorite& f) { return canon(f.address, defaultPort) == c; });
}

bool FavoriteList::remove(const std::string& address) {
    const auto it = std::find_if(items_.begin(), items_.end(), [&](const Favorite& f) { return strcasecmp(f.address.c_str(), address.c_str()) == 0; });
    if (it == items_.end()) return false;
    items_.erase(it);
    return true;
}

std::string FavoriteList::format() const {
    std::string out;
    for (const Favorite& f : items_) out += f.address + "\t" + f.name + "\n";
    return out;
}

void FavoriteList::parse(const std::string& text, uint16_t defaultPort) {
    items_.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        add(line.substr(tab + 1), line.substr(0, tab), defaultPort);
    }
}

std::string favoriteMessage(FavoriteList& list, const std::string& name, const std::string& address, uint16_t defaultPort) {
    if (name.empty()) return "@EXE_FAVORITENAMEEMPTY";
    if (address.empty()) return "@EXE_FAVORITEADDRESSEMPTY";
    switch (list.add(name, address, defaultPort)) {
    case 0: return "@EXE_FAVORITEINLIST";
    case -1: return "@EXE_FAVORITELISTFULL";
    case -2: return "@EXE_BADSERVERADDRESS";
    default: return "@EXE_FAVORITEADDED";
    }
}

bool LanBrowser::ensureSocket() {
    if (sock_.isOpen()) return true;
    if (!sock_.open(0, 0)) return false;
    sock_.enableBroadcast();
    return true;
}

bool LanBrowser::refresh(int64_t nowMs, const std::vector<NetAddress>& extra) {
    servers_.clear();
    rows_.clear();
    lookups_.clear();
    selected = -1;
    hasSelection_ = false;
    if (!ensureSocket()) return false;
    refreshMs_ = nowMs;
    const auto pkt = makeOutOfBand("getinfo " + challenge);
    if (source_ == kSourceLocal) {
        for (int i = 0; i < kPortCount; ++i) {
            NetAddress to{0xffffffffu, uint16_t(kFirstPort + i)};
            sock_.sendTo(to, pkt.data(), int(pkt.size()));
        }
        for (const NetAddress& a : extra) sock_.sendTo(a, pkt.data(), int(pkt.size()));
    } else if (source_ == kSourceFavorites) {
        for (const Favorite& f : favorites_) {
            LanServer s;
            s.favorite = true;
            s.favoriteName = f.name;
            s.addressText = f.address;
            servers_.push_back(s);
            lookups_.emplace_back();
            lookups_.back().start(f.address, kFirstPort, nowMs);
        }
    }
    poll(nowMs);
    rebuild();
    return true;
}

void LanBrowser::poll(int64_t nowMs) {
    if (!sock_.isOpen()) return;
    bool changed = false;
    if (source_ == kSourceFavorites) {
        const auto pkt = makeOutOfBand("getinfo " + challenge);
        for (size_t i = 0; i < lookups_.size() && i < servers_.size(); ++i) {
            if (lookups_[i].poll(nowMs) != Resolver::Status::Done) continue;
            servers_[i].address = lookups_[i].address();
            lookups_[i].reset();
            sock_.sendTo(servers_[i].address, pkt.data(), int(pkt.size()));
        }
    }
    static uint8_t buf[0x4000];
    NetAddress from;
    for (int n; (n = sock_.receive(buf, sizeof buf, from, 0)) > 0;) {
        const auto text = outOfBandText(buf, n);
        if (!text) continue;
        if (text->rfind("statusResponse\n", 0) == 0) {
            if (!(from == statusAddress_)) continue;
            status_.clear();
            std::istringstream in(text->substr(15));
            std::string cvars;
            std::getline(in, cvars);
            size_t p = 0;
            while (p < cvars.size() && cvars[p] == '\\') {
                const size_t ke = cvars.find('\\', p + 1);
                if (ke == std::string::npos) break;
                size_t ve = cvars.find('\\', ke + 1);
                if (ve == std::string::npos) ve = cvars.size();
                status_.push_back({cvars.substr(p + 1, ke - p - 1), "", "", cvars.substr(ke + 1, ve - ke - 1)});
                p = ve;
            }
            std::vector<std::vector<std::string>> players;
            int num = 0;
            for (std::string line; std::getline(in, line);) {
                std::istringstream ls(line);
                std::string score, ping, name;
                ls >> score >> ping;
                std::getline(ls >> std::ws, name);
                if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
                players.push_back({std::to_string(num++), score, ping, name});
            }
            if (!players.empty()) {
                status_.push_back({"", "", "", ""});
                status_.push_back({"num", "score", "ping", "name"});
                status_.insert(status_.end(), players.begin(), players.end());
            }
            continue;
        }
        if (text->rfind("infoResponse\n", 0) != 0) continue;
        LanServer* row = nullptr;
        for (LanServer& o : servers_) if (o.address == from) row = &o;
        if (!row && source_ == kSourceFavorites) continue; // only the favorites are listed there
        if (row && row->pingMs > 0) continue;
        if (!row) { servers_.emplace_back(); row = &servers_.back(); }
        row->address = from;
        row->info = text->substr(13);
        row->pingMs = std::max(1, int(nowMs - refreshMs_));
        changed = true;
    }
    if (changed) rebuild();
}

void LanBrowser::setFilter(const BrowserFilter& f) {
    if (f == filter_) return;
    filter_ = f;
    rebuild();
}

void LanBrowser::sortBy(int column) {
    if (column == sortColumn_) sortDescending_ = !sortDescending_;
    sortColumn_ = column;
    rebuild();
}

void LanBrowser::rebuild() {
    rows_.clear();
    for (int i = 0; i < int(servers_.size()); ++i)
        if (passesFilter(servers_[size_t(i)], filter_)) rows_.push_back(i);
    const auto cmp = [&](int ia, int ib) {
        const LanServer& a = servers_[size_t(ia)];
        const LanServer& b = servers_[size_t(ib)];
        const auto num = [&](const char* k) { return key(a, k) - key(b, k); };
        int c = 0;
        switch (sortColumn_) {
        case 0: c = num("pswrd"); break;
        case 1: c = num("hw"); break;
        case 2: c = strcasecmp(a.hostname().c_str(), b.hostname().c_str()); break;
        case 3: c = strcasecmp(a.map().c_str(), b.map().c_str()); break;
        case 4: c = a.clients() - b.clients(); break;
        case 5: c = strcasecmp(a.gametype().c_str(), b.gametype().c_str()); break;
        case 6: c = num("voice"); break;
        case 7: c = num("pure"); break;
        case 8: c = num("mod"); break;
        case 9: c = num("pb"); break;
        default: c = a.pingMs - b.pingMs; break;
        }
        return sortDescending_ ? c > 0 : c < 0;
    };
    std::stable_sort(rows_.begin(), rows_.end(), cmp);
    if (hasSelection_) {
        selected = -1;
        for (int r = 0; r < int(rows_.size()); ++r)
            if (servers_[size_t(rows_[size_t(r)])].address == selectedAddress_) selected = r;
    }
}

void LanBrowser::select(int row) {
    selected = row;
    if (const LanServer* s = rowServer(row)) {
        selectedAddress_ = s->address;
        hasSelection_ = true;
    } else {
        hasSelection_ = false;
    }
}

const LanServer* LanBrowser::rowServer(int row) const {
    if (row < 0 || size_t(row) >= rows_.size()) return nullptr;
    return &servers_[size_t(rows_[size_t(row)])];
}

std::string LanBrowser::column(int row, int col) const {
    const LanServer* sp = rowServer(row);
    if (!sp) return {};
    const LanServer& s = *sp;
    const auto flag = [&](const char* k) { return key(s, k) != 0 ? std::string("X") : std::string(); };
    switch (col) {
    case 0: return flag("pswrd");
    case 2: {
        // MP 0x537640: before an answer the address, after it the server's own hostname cut to 19 characters; a favorite's
        // stored name is overwritten by the answer (0x4b3300) and never shown in this column.
        if (s.pingMs <= 0) return s.address.ip ? s.address.toString() : s.addressText;
        return s.hostname().substr(0, 19);
    }
    case 3: return s.map();
    case 4: return s.info.empty() ? std::string() : std::to_string(s.clients()) + " (" + std::to_string(s.maxClients()) + ")";
    case 5: { // the raw gametype name as the server sent it (stored up to 15 characters), "?" when it sent none (MP 0x537640)
        const std::string g = s.gametype().substr(0, 15);
        return g.empty() ? std::string("?") : g;
    }
    case 6: return flag("voice");
    case 7: return flag("pure");
    case 8: return flag("mod");
    case 9: return flag("pb");
    case 10: return s.pingMs > 0 ? std::to_string(s.pingMs) : std::string("...");
    default: return {};
    }
}

bool LanBrowser::queryStatus(int64_t) {
    status_.clear();
    const LanServer* s = rowServer(selected >= 0 ? selected : 0);
    if (!s || !ensureSocket()) return false;
    statusAddress_ = s->address;
    const auto pkt = makeOutOfBand("getstatus " + challenge);
    return sock_.sendTo(statusAddress_, pkt.data(), int(pkt.size()));
}

std::string LanBrowser::statusColumn(int row, int col) const {
    if (row < 0 || size_t(row) >= status_.size() || col < 0 || col >= 4) return {};
    return status_[size_t(row)][size_t(col)];
}

} // namespace iw2::net
