// The on-screen message queue: rows added by print / gm / gmb live for con_gamemessagetime (5 s) or con_boldgamemessagetime (8 s),
// then expire; cheat-toggle keys become localisation references, free text stays as it is.
#include "game/screenmessages.h"
#include "game/textmessage.h"
#include "util/codepage.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    using namespace iw2::game;
    constexpr uint64_t s = 1000000000ull;
    std::vector<ScreenMessage> q;
    addScreenMessage(q, printMessageText("GAME_GODMODE_ON"), false, 10 * s);
    addScreenMessage(q, printMessageText("fullbright ON"), false, 12 * s);
    addScreenMessage(q, "bold", true, 12 * s);
    CHECK(q.size() == 3);
    CHECK(q[0].text == "\x14GAME_GODMODE_ON");
    CHECK(q[1].text == "fullbright ON"); // not a key: unchanged
    CHECK(q[0].endNs == 15 * s && q[2].endNs == 20 * s);
    expireScreenMessages(q, 14 * s);
    CHECK(q.size() == 3);
    expireScreenMessages(q, 15 * s + 1); // the first row is gone, the second (until 17 s) stays
    CHECK(q.size() == 2 && q[0].text == "fullbright ON");
    expireScreenMessages(q, 17 * s + 1);
    CHECK(q.size() == 1 && q[0].bold);
    expireScreenMessages(q, 20 * s + 1);
    CHECK(q.empty());

    // MP game-message window anchor: above the compass, bottom-relative.
    CHECK(mpGameMessageWindowY(-140, 160, 1) == -128);
    CHECK(mpGameMessageWindowY(-140, 160, 1.5f) == -208);

    // Log lines: a server print is localized like the message window does it, then written as UTF-8.
    const auto find = [](const std::string& k) -> const std::string* {
        static const std::string connected = "&&1 po\xb3\xb9" "czony";  // Windows-1250 bytes
        return k == "MP_CONNECTED" ? &connected : nullptr;
    };
    const std::string printed = localizeTextMessage(printMessageText("MP_CONNECTED\x15player"), find);
    CHECK(printed == "player po\xb3\xb9" "czony");
    CHECK(iw2::util::codePageToUtf8(printed, iw2::util::codePageForLanguage("polish")) == "player po\xc5\x82\xc4\x85" "czony");
    CHECK(iw2::util::codePageForLanguage("English") == 1252 && iw2::util::codePageForLanguage("russian") == 1251);
    CHECK(iw2::util::codePageToUtf8("\x80", 1252) == "\xe2\x82\xac");  // euro sign
    return failures ? 1 : 0;
}
