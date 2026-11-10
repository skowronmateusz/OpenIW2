// Join-server screen (docs/spec/mp-ui.md "Join server", "Addresses and name lookup"): name lookup, favorites and their file,
// filters, sort, columns, the favorites source talking to a real UDP responder, and the ownerdraw rules.
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "net/lanbrowser.h"
#include "net/oob.h"
#include "net/resolve.h"
#include "ui/mpserver.h"

using namespace iw2;
using namespace iw2::net;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; ++failures; } } while (0)

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void testAddresses() {
    std::string h; uint16_t p = 0;
    CHECK(splitHostPort("10.0.0.1", h, p, 28960) && h == "10.0.0.1" && p == 28960);
    CHECK(splitHostPort("cod.example.com:28961", h, p, 28960) && h == "cod.example.com" && p == 28961);
    CHECK(!splitHostPort("", h, p, 28960));
    CHECK(!splitHostPort(":123", h, p, 28960));
    CHECK(!splitHostPort("host:abc", h, p, 28960));
    CHECK(!splitHostPort("host:0", h, p, 28960));
    CHECK(!splitHostPort("host:70000", h, p, 28960));
    CHECK(!splitHostPort("a b:1", h, p, 28960));

    Resolver r;
    r.start("10.1.2.3:28965", 28960, 0); // numeric: no thread, complete at once
    CHECK(r.poll(0) == Resolver::Status::Done);
    CHECK(r.address().ip == 0x0a010203 && r.address().port == 28965);
    r.start("bad host", 28960, 0);
    CHECK(r.poll(0) == Resolver::Status::Failed);

    const int64_t t0 = nowMs();
    r.start("localhost:28970", 28960, t0);
    CHECK(nowMs() - t0 < 100); // start never waits for the lookup
    Resolver::Status st = r.poll(t0);
    while (st == Resolver::Status::Pending && nowMs() - t0 < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        st = r.poll(nowMs());
    }
    CHECK(st == Resolver::Status::Done);
    CHECK(r.address().ip == 0x7f000001 && r.address().port == 28970);

    NetAddress a;
    CHECK(resolveBlocking("127.0.0.1", a, 28960, 100) && a.port == 28960);
    CHECK(!resolveBlocking("x y", a, 28960, 100));
    r.start("slow.invalid", 28960, 0); // the timeout ends a lookup that never answers, whatever the resolver does
    CHECK(r.poll(Resolver::kTimeoutMs + 1) != Resolver::Status::Pending);
}

void testFavorites() {
    FavoriteList l;
    CHECK(l.add("a", "10.0.0.1", 28960) == 1);
    CHECK(l.add("dup", "10.0.0.1:28960", 28960) == 0);
    CHECK(l.add("b", "srv.example.com:28961", 28960) == 1);
    CHECK(l.add("bad", "host:port", 28960) == -2);
    FavoriteList l2;
    l2.parse(l.format(), 28960);
    CHECK(l2.items().size() == 2 && l2.items()[1].name == "b" && l2.items()[1].address == "srv.example.com:28961");
    CHECK(l2.remove("SRV.example.com:28961") && l2.items().size() == 1);
    CHECK(!l2.remove("nope"));
    FavoriteList full;
    for (int i = 0; i < FavoriteList::kMax; ++i) CHECK(full.add("n", "10.0.0.1:" + std::to_string(i + 1), 28960) == 1);
    CHECK(full.add("n", "10.0.0.2", 28960) == -1);

    FavoriteList m;
    CHECK(favoriteMessage(m, "", "1.2.3.4", 28960) == "@EXE_FAVORITENAMEEMPTY");
    CHECK(favoriteMessage(m, "x", "", 28960) == "@EXE_FAVORITEADDRESSEMPTY");
    CHECK(favoriteMessage(m, "x", "a b", 28960) == "@EXE_BADSERVERADDRESS");
    CHECK(favoriteMessage(m, "x", "1.2.3.4", 28960) == "@EXE_FAVORITEADDED");
    CHECK(favoriteMessage(m, "x", "1.2.3.4", 28960) == "@EXE_FAVORITEINLIST");
}

LanServer server(const std::string& info) { LanServer s; s.info = info; s.pingMs = 10; return s; }

void testFilter() {
    BrowserFilter f;
    const LanServer ok = server("\\clients\\2\\sv_maxclients\\8\\pure\\1\\hw\\1\\gametype\\dm\\mod\\0");
    CHECK(passesFilter(ok, f));
    CHECK(passesFilter(server("\\clients\\2\\sv_maxclients\\8"), f)); // an openiw2 server does not claim pure: shown by default
    { BrowserFilter g; g.showPure = true; CHECK(!passesFilter(server("\\clients\\2\\sv_maxclients\\8"), g)); }
    { BrowserFilter g; g.showFull = false; CHECK(!passesFilter(server("\\clients\\8\\sv_maxclients\\8\\pure\\1"), g)); }
    { BrowserFilter g; g.showEmpty = false; CHECK(!passesFilter(server("\\sv_maxclients\\8\\pure\\1"), g)); }
    { BrowserFilter g; g.showPassword = false; CHECK(!passesFilter(server("\\pswrd\\1\\pure\\1"), g)); }
    { BrowserFilter g; g.showNoPassword = false; CHECK(!passesFilter(server("\\pure\\1"), g)); }
    { BrowserFilter g; g.gametype = "tdm"; CHECK(!passesFilter(ok, g)); g.gametype = "DM"; CHECK(passesFilter(ok, g)); }
    { BrowserFilter g; g.killcam = 1; CHECK(!passesFilter(ok, g)); }
    { BrowserFilter g; g.showDedicated = true; CHECK(passesFilter(ok, g)); CHECK(!passesFilter(server("\\pure\\1\\hw\\0"), g)); }
    LanServer fav; fav.favorite = true; fav.favoriteName = "x";
    CHECK(passesFilter(fav, f)); // a favorite without an answer stays listed
}

// A UDP responder on a test port answers getinfo like a server.
void testFavoritesSource() {
    UdpSocket srv;
    CHECK(srv.open(0x7f000001, 28987));
    FavoriteList favs;
    CHECK(favs.add("unreachable", "127.0.0.1:28988", 28960) == 1);
    CHECK(favs.add("mine", "localhost:28987", 28960) == 1);
    LanBrowser b;
    b.setSource(kSourceFavorites);
    b.setFavorites(favs.items());
    CHECK(b.refresh(nowMs()));
    CHECK(b.rowCount() == 2); // rows exist before any answer
    CHECK(b.column(0, 2) == "unreachable" || b.column(1, 2) == "unreachable");
    CHECK(b.column(0, 10) == "...");
    const int64_t t0 = nowMs();
    bool got = false;
    uint8_t buf[2048];
    NetAddress from;
    while (nowMs() - t0 < 3000 && !got) {
        b.poll(nowMs());
        if (int n = srv.receive(buf, sizeof buf, from, 5); n > 0) {
            const auto text = outOfBandText(buf, n);
            CHECK(text && text->rfind("getinfo ", 0) == 0);
            const auto pkt = makeOutOfBand("infoResponse\n\\hostname\\Test Host\\mapname\\mp_toujane\\gametype\\dm\\clients\\3\\sv_maxclients\\16\\pure\\1\\voice\\1");
            srv.sendTo(from, pkt.data(), int(pkt.size()));
            for (int i = 0; i < 100 && !got; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                b.poll(nowMs());
                for (int r = 0; r < b.rowCount(); ++r) got = got || b.column(r, 2) == "Test Host";
            }
        }
    }
    CHECK(got);
    int row = -1;
    for (int r = 0; r < b.rowCount(); ++r) if (b.column(r, 2) == "Test Host") row = r;
    CHECK(row >= 0);
    if (row < 0) return;
    CHECK(b.column(row, 3) == "mp_toujane");
    CHECK(b.column(row, 4) == "3 (16)");
    CHECK(b.column(row, 6) == "X");
    CHECK(b.column(row, 10) != "...");
    b.select(row);
    CHECK(b.queryStatus(nowMs()));
    int n = srv.receive(buf, sizeof buf, from, 500);
    CHECK(n > 0);
    if (n > 0) CHECK(outOfBandText(buf, n)->rfind("getstatus", 0) == 0);
    const auto st = makeOutOfBand("statusResponse\n\\sv_hostname\\Test\\g_gametype\\dm\n5 40 \"Player One\"\n");
    srv.sendTo(from, st.data(), int(st.size()));
    for (int i = 0; i < 200 && b.statusCount() == 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); b.poll(nowMs()); }
    CHECK(b.statusCount() == 5); // 2 cvars, a blank row, the header, 1 player
    CHECK(b.statusColumn(0, 0) == "sv_hostname" && b.statusColumn(0, 3) == "Test");
    CHECK(b.statusColumn(4, 3) == "Player One" && b.statusColumn(4, 1) == "5" && b.statusColumn(4, 2) == "40");
}

void testSort() {
    UdpSocket srv;
    CHECK(srv.open(0x7f000001, 28989));
    LanBrowser b;
    b.setSource(kSourceLocal);
    CHECK(b.refresh(nowMs(), {NetAddress{0x7f000001, 28989}}));
    uint8_t buf[2048];
    NetAddress from;
    int n = 0;
    for (int i = 0; i < 100 && n <= 0; ++i) n = srv.receive(buf, sizeof buf, from, 10);
    CHECK(n > 0);
    if (n <= 0) return;
    UdpSocket srv2;
    CHECK(srv2.open(0x7f000001, 28990));
    for (auto [sock, name] : {std::pair{&srv, "bravo"}, std::pair{&srv2, "alpha"}}) {
        const auto pkt = makeOutOfBand(std::string("infoResponse\n\\pure\\1\\hostname\\") + name);
        sock->sendTo(from, pkt.data(), int(pkt.size()));
    }
    for (int i = 0; i < 200 && b.rowCount() < 2; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); b.poll(nowMs()); }
    CHECK(b.rowCount() == 2);
    b.sortBy(2);
    CHECK(b.column(0, 2) == "alpha");
    b.select(0);
    b.sortBy(2); // the same column again reverses
    CHECK(b.column(0, 2) == "bravo");
    CHECK(b.selected == 1); // the selection follows the server
}

void testOwnerDraws() {
    using namespace iw2::ui;
    CHECK(mpOwnerDrawFlagShown(kShowFavoriteServers, 2) && !mpOwnerDrawFlagShown(kShowFavoriteServers, 0));
    CHECK(mpOwnerDrawFlagShown(kShowNotFavoriteServers, 0) && !mpOwnerDrawFlagShown(kShowNotFavoriteServers, 2));
    CHECK(mpCycleNetSource(0, 1) == 1 && mpCycleNetSource(2, 1) == 0 && mpCycleNetSource(0, -1) == 2);
    CHECK(mpNetSourceLabel(2) == "EXE_FAVORITES");
    CHECK(mpRegisteredDefault("ui_browserShowDedicated") == "0" && mpRegisteredDefault("ui_browserMod") == "-1");
}
} // namespace

int main() {
    testAddresses();
    testFavorites();
    testFilter();
    testFavoritesSource();
    testSort();
    testOwnerDraws();
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "mpbrowser ok\n";
    return 0;
}
