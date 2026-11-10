// Multiplayer scoreboard: the server's score list and the client's layout (docs/spec/mp-scoreboard.md).
// Implemented from the spec. No renderer dependency: the layout is a list of quads and texts in the 640x480 screen.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iw2::game {

// One client as the server sorts it (0x81097e6 / 0x810968e).
struct MpScoreClient {
    int num = 0;
    int score = 0, deaths = 0, ping = 0;
    int statusIcon = 0;     // index of the precached status icon (1..8), 0 none
    bool connecting = false; // client state "connected" (not yet active)
    bool spectator = false;  // sessionteam spectator
};

// The server's order: connecting clients last; then spectators after the others, among themselves by slot; else score
// descending, then deaths ascending. Equal entries keep no defined order (qsort); here the slot breaks the tie.
void mpSortScoreClients(std::vector<MpScoreClient>& clients);

// The server command (0x80fdf08): "b <count> <axis score> <allies score>" and per client " num score ping deaths icon",
// at most 64 clients and 1024 bytes of client text. A connecting client's ping is -1.
std::string mpScoresCommand(const std::vector<MpScoreClient>& sorted, int axisScore, int alliesScore);

// The client's request clock (0x4dabf0): at most one `score` every 2000 ms of the level's time. That time starts
// again on every map, so a new level (another `generation`) forgets the last request and the previous map's list.
struct MpScoreRequest {
    int64_t last = -1000000;
    int generation = -1;
    // True when a request is due now; then it counts as sent. `newLevel` is set when the level changed.
    bool due(int64_t nowMs, int levelGeneration, bool* newLevel = nullptr) {
        const bool fresh = generation != levelGeneration;
        if (newLevel) *newLevel = fresh;
        if (fresh) {
            generation = levelGeneration;
            last = -1000000;
        }
        if (nowMs <= last + 2000) return false;
        last = nowMs;
        return true;
    }
};

// The client's copy after the command (0x4d02b0).
struct MpScoreRow {
    int client = 0, score = 0, ping = 0, deaths = 0, statusIcon = 0;
    int team = 0; // 0 none, 1 axis, 2 allies, 3 spectator (from the client's info)
};
struct MpScoreData {
    std::array<int, 3> teamScore{}; // [1] axis, [2] allies
    std::vector<MpScoreRow> rows;    // server order
    std::array<int, 4> teamCount{};  // rows per team
    std::array<int, 4> teamPing{};   // average ping per team (0 when no row or no positive sum)
};

// Parses the arguments after the letter. `teamOf(client)` gives the team from the client's info (-1: the client slot
// is not in use; the row then counts as team 0). Client numbers outside 0..63 become 0.
MpScoreData mpParseScores(const std::string& args, const std::function<int(int)>& teamOf);

// Draw list of the layout. Rects with an empty material are solid colour.
struct SbRect {
    float x = 0, y = 0, w = 0, h = 0;
    std::array<float, 4> rgba{1, 1, 1, 1};
    std::string material; // "white", "black", a banner, a status icon, voice_on / voice_off, scroll arrows
};
struct SbText {
    std::string text;
    float x = 0, y = 0; // y is the baseline
    float scale = 0.3f;
    std::array<float, 4> rgba{1, 1, 1, 1};
    int style = 3;      // 3 shadowed, 0 plain
};
struct SbItem {
    bool isText = false;
    SbRect rect;
    SbText text;
};

struct MpScoreboardInput {
    const MpScoreData* data = nullptr;
    int localClient = 0;
    float alpha = 1.0f;
    std::function<std::string(int)> nameOf;          // client name
    std::function<bool(int)> clientValid;            // client slot in use
    std::function<std::string(const std::string&)> localize; // key -> text (the key itself when unknown)
    std::function<float(const std::string&, float)> textWidth; // width at a text scale, 640x480 units
    std::function<float(float)> textHeight;          // font height at a text scale
    std::function<std::string(int)> statusIconMaterial; // icon index -> material ("" none)
    std::string gametypeName;   // the localized gametype display name
    std::string mapName;        // the map display name
    std::string hostName;       // sv_hostname of the server
    std::string serverAddress;  // "ip:port", or empty for a listen server
    std::string objectiveText;  // cg_objectiveText
    std::string teamNameAxis, teamNameAllies;    // g_TeamName_* (localization keys or text)
    std::string bannerNone = "mpflag_none", bannerAxis = "mpflag_german", bannerAllies = "mpflag_american",
                bannerSpectators = "mpflag_spectator";
    std::array<float, 4> colorAxis{1, 0.5f, 0.5f, 1}, colorAllies{0.5f, 0.5f, 1, 1};
    int bannerHeight = 24;      // cg_scoreboardBannerHeight
    int scroll = 0;             // first line drawn (cg_scoreboardScrollStep moves it)
    int firstTeam = 2;          // the team block drawn first when the scores are equal (kept between frames)
};

struct MpScoreboardOutput {
    std::vector<SbItem> items;
    int scroll = 0;     // clamped to the last line
    int firstTeam = 2;
    bool scrollbar = false;
    int lines = 0;      // lines (banners and rows) counted
};

// The layout of 0x4d95a0 (frame and header) and 0x4da980 (list).
MpScoreboardOutput mpScoreboardLayout(const MpScoreboardInput& in);

// Column table 0x59e628: kind, width fraction of the list width, header key, right aligned.
enum class SbColumn { StatusIcon, Voice, Name, Score, Deaths, Ping };
struct SbColumnDef {
    SbColumn kind;
    float frac;
    const char* header; // nullptr: no header
    bool right;
};
extern const std::array<SbColumnDef, 6> kScoreboardColumns;

} // namespace iw2::game
