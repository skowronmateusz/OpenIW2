// Multiplayer spectating and scoreboard (docs/spec/mp-spectate.md, docs/spec/mp-scoreboard.md): the server's score order
// and command, the client's parse and layout, the spectator move, and with the game files (IW2_MAIN) allowSpectateTeam,
// the follow cycle, stopping, the forced follow without free look, intermission and the scores of a DM game.

#include "game/collision.h"
#include "game/mpscoreboard.h"
#include "game/mpspectate.h"
#include "game/pmove.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

using namespace iw2::game;
using iw2::script::Value;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

const SbText* findText(const MpScoreboardOutput& o, const std::string& t) {
    for (const auto& it : o.items)
        if (it.isText && it.text.text == t) return &it.text;
    return nullptr;
}
const SbRect* findRect(const MpScoreboardOutput& o, const std::string& mat, size_t nth = 0) {
    for (const auto& it : o.items)
        if (!it.isText && it.rect.material == mat && nth-- == 0) return &it.rect;
    return nullptr;
}

} // namespace

int main() {
    // Score request clock (0x4dabf0): every 2000 ms of level time; a new level (the time starts again) asks at once.
    {
        MpScoreRequest r;
        bool fresh = false;
        CHECK(r.due(60000, 1, &fresh) && fresh);
        CHECK(!r.due(61000, 1, &fresh) && !fresh);
        CHECK(r.due(62500, 1));
        CHECK(r.due(58000, 2, &fresh) && fresh);  // the next map ends at a time below the last request
        CHECK(!r.due(59000, 2));
    }
    // Server order (0x810968e): connecting last, spectators after the players by slot, score descending, deaths ascending.
    {
        std::vector<MpScoreClient> v = {
            {0, 5, 3, 0, 0, false, false}, {1, 9, 0, 0, 0, true, false}, {2, 5, 1, 0, 0, false, false},
            {3, 1, 0, 0, 0, false, true},  {4, 7, 9, 0, 0, false, false}, {5, 2, 0, 0, 0, false, true},
        };
        mpSortScoreClients(v);
        CHECK(v[0].num == 4 && v[1].num == 2 && v[2].num == 0 && v[3].num == 3 && v[4].num == 5 && v[5].num == 1);
        const std::string cmd = mpScoresCommand(v, 3, 7);
        CHECK(cmd.rfind("b 6 3 7 4 7 0 9 0 2 5 0 1 0", 0) == 0);
        CHECK(cmd.size() > 2 && cmd.substr(cmd.size() - 11) == " 1 9 -1 0 0"); // a connecting client's ping is -1
    }
    // Client parse (0x4d02b0): team counts and the average ping per team.
    {
        const auto team = [](int c) { return c == 1 ? 1 : c == 3 ? 3 : 2; };
        const MpScoreData d = mpParseScores("4 10 20 1 3 50 0 2 2 1 30 1 3 3 0 0 0 0 7 4 70 0 9", team);
        CHECK(d.teamScore[1] == 10 && d.teamScore[2] == 20);
        CHECK(d.rows.size() == 4 && d.rows[0].client == 1 && d.rows[0].ping == 50 && d.rows[1].statusIcon == 3);
        CHECK(d.rows[3].statusIcon == 0); // 9 is outside 1..8
        CHECK(d.teamCount[1] == 1 && d.teamCount[2] == 2 && d.teamCount[3] == 1);
        CHECK(d.teamPing[2] == 50 && d.teamPing[1] == 50 && d.teamPing[3] == 0);
        CHECK(mpParseScores("200 0 0", nullptr).rows.size() == 64); // the count is capped at 64
    }
    // Layout (0x4d95a0 / 0x4da980) with a text width of 5 units per character at scale 0.22.
    {
        MpScoreData d;
        d.teamScore = {0, 5, 3};
        d.rows = {{0, 4, 40, 1, 0, 1}, {1, 2, 60, 2, 1, 2}, {2, 0, 0, 0, 0, 3}};
        d.teamCount = {0, 1, 1, 1};
        d.teamPing = {0, 40, 60, 0};
        MpScoreboardInput in;
        in.data = &d;
        in.localClient = 1;
        in.nameOf = [](int c) { return "P" + std::to_string(c); };
        in.clientValid = [](int) { return true; };
        in.localize = [](const std::string& k) {
            if (k == "CGAME_SB_PLAYER") return std::string("&&1 player");
            if (k == "CGAME_SB_PLAYERS") return std::string("&&1 players");
            if (k == "CGAME_SB_SCORE") return std::string("Score");
            if (k == "CGAME_SPECTATORS") return std::string("Spectators");
            return k;
        };
        in.textWidth = [](const std::string& t, float s) { return float(t.size()) * 5.0f * s / 0.22f; };
        in.textHeight = [](float s) { return 48.0f * s; };
        in.statusIconMaterial = [](int i) { return i == 1 ? std::string("hud_status_dead") : std::string(); };
        in.gametypeName = "Deathmatch";
        in.mapName = "mp_toujane";
        in.teamNameAxis = "Germans";
        in.teamNameAllies = "British";
        const MpScoreboardOutput o = mpScoreboardLayout(in);
        // Frame: the black backdrop and the six lines.
        const SbRect* bg = findRect(o, "white");
        CHECK(bg && bg->x == 120 && bg->y == 22 && bg->w == 400 && bg->h == 430 && bg->rgba[0] == 0 && near(bg->rgba[3], 0.8f));
        // Axis (5) ahead of allies (3): the axis banner first, at the list top 52 + 4 + 14 = 70.
        const SbRect* ax = findRect(o, "mpflag_german");
        const SbRect* al = findRect(o, "mpflag_american");
        CHECK(ax && al && near(ax->y, 70) && ax->x == 129 && ax->w == 382 && ax->h == 24);
        CHECK(al && near(al->y, 70 + 24 + 12 + 4)); // banner, one row, the 4-unit gap
        CHECK(o.firstTeam == 1);
        CHECK(findText(o, "Germans (1 player)") != nullptr);
        const SbText* sp = findText(o, "Spectators (1 player)");
        CHECK(sp != nullptr);
        // Banner score of the axis: right edge of the score column (129 + 382 * 0.70) at scale 0.32.
        const SbText* ts = findText(o, "5");
        CHECK(ts && near(ts->x + 1 * 5.0f * 0.32f / 0.22f, 129 + 382 * 0.70f, 0.05f) && near(ts->scale, 0.32f));
        // The local client's row has the white 0.2 backdrop.
        bool own = false;
        for (const auto& it : o.items)
            if (!it.isText && it.rect.material == "white" && near(it.rect.rgba[3], 0.2f) && it.rect.h == 12) own = true;
        CHECK(own);
        // Status icon 13x13 in the first column; the spectator row has no score or deaths, only the ping.
        const SbRect* icon = findRect(o, "hud_status_dead");
        CHECK(icon && icon->x == 129 && icon->w == 13);
        CHECK(findText(o, "Score") != nullptr);
        CHECK(!o.scrollbar && o.lines == 6);
    }
    // Scrolling: 40 rows overflow the list; the bar shows and the width drops to 374.
    {
        MpScoreData d;
        for (int i = 0; i < 40; ++i) d.rows.push_back({i, 0, 0, 0, 0, 0});
        d.teamCount[0] = 40;
        MpScoreboardInput in;
        in.data = &d;
        in.scroll = 100;
        const MpScoreboardOutput o = mpScoreboardLayout(in);
        CHECK(o.scrollbar && o.scroll == 40);
        const SbRect* b = findRect(o, "mpflag_none");
        CHECK(b == nullptr); // scrolled past the banner
    }
    // Spectator move (pm_type 4): flies along the view with the 2-D command scale, stops without free look.
    {
        CHECK(mpClientPmType(mpSessionStateNumber("spectator")) == 4 && mpClientPmType(mpSessionStateNumber("intermission")) == 5 &&
              mpClientPmType(mpSessionStateNumber("dead")) == 6 && mpClientPmType(mpSessionStateNumber("playing")) == 0);
        CHECK(mpSpectatorSpeed(true) == 800.0f && mpSpectatorSpeed(false) == 0.0f);
        CollisionWorld empty;
        PlayerState ps;
        ps.origin = {0, 0, 100};
        PlayerInput in;
        in.forward = 1.0f;
        in.pitch = -45.0f; // looking up
        MoveParams mp;
        mp.mpSpectator = true;
        for (int i = 0; i < 60; ++i) playerMove(empty, ps, in, 1.0f / 60.0f, mp);
        CHECK(ps.origin[0] > 100.0f && ps.origin[2] > 150.0f); // no gravity, up along the view
        const float speed = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1] + ps.velocity[2] * ps.velocity[2]);
        CHECK(speed > 500.0f && speed <= 801.0f);
        PlayerState still;
        mp.spectateSpeed = 0.0f;
        for (int i = 0; i < 30; ++i) playerMove(empty, still, in, 1.0f / 60.0f, mp);
        CHECK(still.origin[0] == 0.0f && still.origin[2] == 0.0f);
    }

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / "iw2_test_mpspectate";
    std::filesystem::create_directories(dir / "maps" / "mp");
    {
        std::ofstream f(dir / "maps" / "mp" / "spectest.gsc");
        f << "nofree()\n{\n\tself allowSpectateTeam(\"freelook\", false);\n}\n"
             "noaxis()\n{\n\tself allowSpectateTeam(\"none\", false);\n}\n"
             "all()\n{\n\tself allowSpectateTeam(\"none\", true);\n\tself allowSpectateTeam(\"freelook\", true);\n}\n"
             "bad()\n{\n\tself allowSpectateTeam(\"red\", false);\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    int c[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = w.mpConnect("P" + std::to_string(i));
        w.mpBegin(c[i]);
        run(w, 2);
    }
    w.mpAutoJoin(c[0]);
    w.mpMenuResponse(c[2], "serverinfo_dm", "close");
    run(w, 1);
    w.mpAutoJoin(c[2]);
    run(w, 3);
    CHECK(w.mpSessionState(c[0]) == 0 && w.mpSessionState(c[1]) == 2 && w.mpSessionState(c[2]) == 0);
    auto script = [&](const char* fn, int client) {
        w.vm().start("maps/mp/spectest.gsc", fn, {}, Value(w.mpClientEntity(client)->obj));
        w.vm().drainReady();
    };

    // Attack press: cycle forward from slot 0 (1 is the spectator itself) -> 2; again -> 0; ADS (0x1000) -> back to 2.
    w.mpSetButtons(c[1], 1);
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) == c[2]);
    w.mpSetButtons(c[1], 0);
    run(w, 1);
    w.mpSetButtons(c[1], 1);
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) == c[0]);
    w.mpSetButtons(c[1], 0x1000);
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) == c[2]);
    // Holding does not cycle again.
    run(w, 2);
    CHECK(w.mpFollowing(c[1]) == c[2]);
    // Button 4 changing with free look allowed: stop, placed near the target's eye.
    w.mpSetButtons(c[1], 0x1000 | 4);
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) == -1);
    {
        const auto so = w.mpClientEntity(c[1])->obj->get("origin").asVector();
        const auto to = w.mpClientEntity(c[2])->obj->get("origin").asVector();
        const float dx = so[0] - to[0], dy = so[1] - to[1];
        CHECK(std::sqrt(dx * dx + dy * dy) <= 41.0f && so[2] > to[2] + 40.0f);
    }
    w.mpSetButtons(c[1], 0);
    run(w, 1);
    // Without free look the spectator is put on the next target by itself.
    script("nofree", c[1]);
    CHECK(!w.mpSpectateAllowed(c[1], 4));
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) >= 0);
    // DM players are team "none": disallowing it leaves no target; the follow is dropped.
    script("noaxis", c[1]);
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) == -1);
    script("all", c[1]);
    CHECK(w.mpSpectateAllowed(c[1], 0) && w.mpSpectateAllowed(c[1], 4));
    const size_t errs = w.vm().errors().size();
    script("bad", c[1]);
    CHECK(w.vm().errors().size() == errs + 1);
    // A followed client's death stops its followers.
    w.mpSetButtons(c[1], 1);
    run(w, 1);
    const int target = w.mpFollowing(c[1]);
    CHECK(target >= 0);
    w.mpDebugDamage(target, -1, 1000, "MOD_RIFLE_BULLET");
    run(w, 1);
    CHECK(w.mpFollowing(c[1]) != target);

    // Scores of the game: players first, the spectator last.
    {
        const std::string cmd = w.mpScoresCommand();
        CHECK(cmd.rfind("b 3 0 0 ", 0) == 0);
        w.mpSetClientPing(c[0], 87); // a network client's server ping goes out in the scores
        const std::string withPing = w.mpScoresCommand();
        CHECK(withPing.find(" " + std::to_string(c[0]) + " ") != std::string::npos && withPing.find(" 87 ") != std::string::npos);
        w.mpSetClientPing(c[0], 0);
        const MpScoreData d = mpParseScores(cmd.substr(2), [&](int cl) { return w.mpTeamOf(w.mpClientEntity(cl)); });
        CHECK(d.rows.size() == 3 && d.rows[2].client == c[1] && d.rows[2].team == 3);
        // The dead player shows the precached dead icon (dm.gsc:79, dm.gsc:322).
        bool deadIcon = false;
        for (const auto& r : d.rows)
            if (r.client == target && r.statusIcon > 0) deadIcon = w.mpState().statusIcons[size_t(r.statusIcon - 1)] == "hud_status_dead";
        CHECK(deadIcon);
    }
    // A followed network client's view comes from its movement: command angles and view height, not the standing eye.
    {
        iw2::game::Vec3 eye, ang;
        w.mpSetView(c[1], {12.0f, 34.0f, 0.0f}, 40.0f);
        const auto o = w.mpClientEntity(c[1])->obj->get("origin").asVector();
        CHECK(w.mpClientView(c[1], eye, ang));
        CHECK(std::fabs(eye[2] - (o[2] + 40.0f)) < 1e-3f);
        CHECK(std::fabs(ang[0] - 12.0f) < 1e-3f && std::fabs(ang[1] - 34.0f) < 1e-3f);
    }
    // Intermission: endMap (dm.gsc:598) puts every client in session state intermission.
    w.vm().start("maps/mp/gametypes/dm", "endMap", {}, Value(w.vm().level()));
    w.vm().drainReady();
    run(w, 2);
    CHECK(w.mpSessionState(c[0]) == 3 && w.mpSessionState(c[1]) == 3);
    CHECK(mpClientPmType(w.mpSessionState(c[0])) == 5);
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";

    std::cout << (failures ? "FAILED\n" : "ok\n");
    return failures ? 1 : 0;
}
