// Multiplayer chat and quick messages (docs/spec/mp-chat.md): name and text cleaning, the client chat buffer (wrap,
// colour carry, height, fade), the chat bindings, and with the game files (IW2_MAIN) the say rules (markers, team
// filter, dead chat, log lines) and the quick message scripts (sayTeam with a localized QUICKMESSAGE string, the 2 s
// spam delay, spectators ignored).

#include "game/clientinput.h"
#include "game/mpchat.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::MpChatBuffer;
using iw2::game::World;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}
// Chat commands each client received since the last call.
std::vector<std::pair<int, std::string>> takeChat(World& w) {
    auto v = w.mpState().chat;
    const_cast<iw2::game::MpState&>(w.mpState()).chat.clear();
    return v;
}
int countFor(const std::vector<std::pair<int, std::string>>& v, int client) {
    int n = 0;
    for (const auto& [c, t] : v) n += c == client;
    return n;
}

} // namespace

int main() {
    using namespace iw2::game;
    // Cleaning (0x44a8d0, 0x4fe010).
    CHECK(mpCleanString("^1Red^7 Guy\x01!") == "Red Guy!");
    CHECK(mpCleanString("a^^1b") == "a^b"); // "^^" is not a colour code: the first '^' is kept, then "^1" is dropped
    CHECK(mpCleanName("   ^0Bob") == "Bob");
    CHECK(mpCleanName("^1Red^7Guy") == "^1Red^7Guy");
    CHECK(mpCleanName("a     b") == "a   b");
    CHECK(mpCleanName("") == "UnnamedPlayer");
    CHECK(mpCleanName("^1^2") == "UnnamedPlayer");
    CHECK(mpCleanName(std::string(40, 'x')).size() == 31);
    CHECK(mpChatReceived(std::string("a\x19") + "b") == "ab");
    CHECK(mpChatReceived(std::string(200, 'y')).size() == 149);

    // Chat buffer (0x4d0e10 / 0x4c7760).
    {
        MpChatBuffer b;
        b.add("^8Player^7: ^7hi", 1000, 8, 12000);
        auto lines = b.frame(1000, 8, 12000);
        CHECK(lines.size() == 1 && lines[0].text == "^8Player^7: ^7hi" && lines[0].code == 8 && lines[0].alpha == 1.0f);
        // Fade over the last 200 ms; an expired line is dropped.
        lines = b.frame(1000 + 12000 - 100, 8, 12000);
        CHECK(lines.size() == 1 && lines[0].alpha > 0.49f && lines[0].alpha < 0.51f);
        CHECK(b.frame(1000 + 12001, 8, 12000).empty());
        // Wrap at 90 visible characters at the last space (dropped); the next line carries the last colour.
        std::string longMsg = "^5" + std::string(85, 'a') + " " + std::string(20, 'b');
        b.add(longMsg, 20000, 8, 12000);
        lines = b.frame(20000, 8, 12000);
        CHECK(lines.size() == 2);
        if (lines.size() == 2) {
            CHECK(lines[1].text == "^5" + std::string(85, 'a'));
            CHECK(lines[0].text == "^5" + std::string(20, 'b'));
        }
        // No space: the break is at 90 characters.
        b.add(std::string(100, 'c'), 21000, 8, 12000);
        lines = b.frame(21000, 8, 12000);
        CHECK(lines.size() == 4 && lines[1].text == std::string(90, 'c') && lines[0].text == "^7" + std::string(10, 'c'));
        // cg_chatHeight limits the lines; 0 clears.
        MpChatBuffer h;
        for (int i = 0; i < 5; ++i) h.add("line" + std::to_string(i), 100, 3, 12000);
        lines = h.frame(100, 3, 12000);
        CHECK(lines.size() == 3 && lines[0].text == "line4" && lines[2].text == "line2");
        h.add("x", 100, 0, 12000);
        CHECK(h.frame(100, 3, 12000).empty());
    }

    // Bindings (default_mp.cfg:54-56).
    {
        ClientInput in;
        in.loadConfig("unbindall\nbind t \"chatmodepublic\"\nbind y \"chatmodeteam\"\nbind v \"mp_QuickMessage\"\n");
        for (const char* k : {"T", "Y", "V"}) in.key(k, true), in.key(k, false);
        const auto cmds = in.takeConsoleCommands();
        CHECK(cmds.size() == 3 && cmds[0] == "chatmodepublic" && cmds[1] == "chatmodeteam" && cmds[2] == "mp_quickmessage");
    }

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    const int c0 = w.mpConnect("^1Local");
    w.mpBegin(c0);
    run(w, 2); // one connect per frame (the gametype's connect loop)
    const int c1 = w.mpConnect("Second");
    w.mpBegin(c1);
    run(w, 2);
    const int c2 = w.mpConnect("Third");
    w.mpBegin(c2);
    run(w, 2);
    CHECK(w.mpState().clients[size_t(c0)].name == "^1Local");
    w.takeClientCommands();

    // Spectators (all three before joining): the marker, no team colour; spectators hear each other.
    w.mpSay(c1, 0, "\x15" "hello");
    auto chat = takeChat(w);
    CHECK(chat.size() == 3);
    CHECK(!chat.empty() && chat[0].second == "h \x15" "\x15" "(\x14" "GAME_SPECTATOR\x15" ")Second^7: ^7\x15" "hello");
    // The local client gets the server command.
    bool local = false;
    for (const auto& c : w.takeClientCommands()) local |= c.name == "h" && c.arg.find("Second^7: ") != std::string::npos;
    CHECK(local);
    CHECK(!w.mpState().log.empty() && w.mpState().log.back() == "say;0;1;Second;\x15" "hello\n");

    // Join (dm: autoassign and a weapon); client 1 closes its server info menu first.
    w.mpAutoJoin(c0);
    w.mpMenuResponse(c1, "serverinfo_dm", "close");
    run(w, 1);

    w.mpAutoJoin(c1);
    run(w, 2);
    auto* p0 = w.mpClientEntity(c0);
    auto* p1 = w.mpClientEntity(c1);
    CHECK(p0->obj->get("sessionstate").toString() == "playing");
    CHECK(p1->obj->get("sessionstate").toString() == "playing");
    takeChat(w);

    // Team say from a dm player (team "none"): falls back to all; the name is cleaned (no "^1").
    w.mpSay(c0, 1, "\x15" "team?");
    chat = takeChat(w);
    CHECK(chat.size() == 3 && chat[0].second == "h \x15" "\x15" "Local^7: ^7\x15" "team?");
    CHECK(w.mpState().log.back().rfind("say;0;0;Local;", 0) == 0);

    // A spectator is not heard by players unless g_deadChat.
    w.mpSay(c2, 0, "\x15" "from the stands");
    chat = takeChat(w);
    CHECK(chat.size() == 1 && countFor(chat, c2) == 1);
    w.cvars["g_deadChat"] = "1";
    w.mpSay(c2, 0, "\x15" "again");
    CHECK(takeChat(w).size() == 3);
    w.cvars["g_deadChat"] = "0";

    // Team sides: put c0 / c1 on teams by their fields; team say reaches only the same team and is coloured.
    p0->obj->set("sessionteam", iw2::script::Value("axis"));
    p1->obj->set("sessionteam", iw2::script::Value("allies"));
    w.mpSay(c0, 1, "\x15" "axis only");
    chat = takeChat(w);
    CHECK(chat.size() == 1 && chat[0].first == c0 && chat[0].second == "i \x15" "\x15" "^9(\x14" "GAME_AXIS\x15" ")Local^7: ^5\x15" "axis only");
    CHECK(w.mpState().log.back().rfind("sayteam;0;0;Local;", 0) == 0);
    w.mpSay(c1, 0, "\x15" "allies");
    chat = takeChat(w);
    CHECK(!chat.empty() && chat[0].second == "h \x15" "\x15" "^8Second^7: ^7\x15" "allies");
    // Dead marker.
    p1->obj->set("sessionstate", iw2::script::Value("dead"));
    w.mpSay(c1, 0, "\x15" "ugh");
    chat = takeChat(w);
    CHECK(chat.size() == 2 && chat[0].first == c1 && chat[0].second == "h \x15" "\x15" "^8(\x14" "GAME_DEAD\x15" ")Second^7: ^7\x15" "ugh");
    CHECK(countFor(chat, c0) == 0);
    p1->obj->set("sessionstate", iw2::script::Value("playing"));

    // Quick messages (_quickmessages.gsc): dm sets level.QuickMessageToAll, so sayAll with the localized string (0x14
    // marker) reaches everybody; a second one within 2 s is ignored.
    run(w, 1);
    takeChat(w);
    w.mpMenuResponse(c0, "quickcommands", "1");
    run(w, 1);
    chat = takeChat(w);
    CHECK(chat.size() == 3 && chat[0].second == "h \x15" "\x15" "^9Local^7: ^7\x14" "QUICKMESSAGE_FOLLOW_ME");
    CHECK(p0->obj->get("headicon").toString() == "talkingicon");
    w.mpMenuResponse(c0, "quickresponses", "1");
    run(w, 1);
    CHECK(takeChat(w).empty());
    run(w, 45); // > 2 s
    w.mpMenuResponse(c0, "quickresponses", "1");
    run(w, 1);
    CHECK(takeChat(w).size() == 3);
    // A spectator's quick message is ignored.
    w.mpMenuResponse(c2, "quickstatements", "1");
    run(w, 1);
    CHECK(takeChat(w).empty());
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    CHECK(w.vm().errors().empty());

    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
