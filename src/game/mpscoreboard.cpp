// Multiplayer scoreboard (docs/spec/mp-scoreboard.md). Implemented from the spec.
#include "game/mpscoreboard.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace iw2::game {

const std::array<SbColumnDef, 6> kScoreboardColumns{{
    {SbColumn::StatusIcon, 0.05f, nullptr, false},
    {SbColumn::Voice, 0.05f, nullptr, false},
    {SbColumn::Name, 0.47f, nullptr, false},
    {SbColumn::Score, 0.13f, "CGAME_SB_SCORE", true},
    {SbColumn::Deaths, 0.13f, "CGAME_SB_DEATHS", true},
    {SbColumn::Ping, 0.17f, "CGAME_SB_PING", true},
}};

void mpSortScoreClients(std::vector<MpScoreClient>& clients) {
    std::stable_sort(clients.begin(), clients.end(), [](const MpScoreClient& a, const MpScoreClient& b) {
        if (a.connecting != b.connecting) return b.connecting;
        if (a.connecting) return false;
        if (a.spectator != b.spectator) return b.spectator;
        if (a.spectator) return a.num < b.num;
        if (a.score != b.score) return a.score > b.score;
        return a.deaths < b.deaths;
    });
}

std::string mpScoresCommand(const std::vector<MpScoreClient>& sorted, int axisScore, int alliesScore) {
    std::string list;
    int n = 0;
    for (const MpScoreClient& c : sorted) {
        if (n >= 64) break;
        const std::string one = " " + std::to_string(c.num) + " " + std::to_string(c.score) + " " +
                                std::to_string(c.connecting ? -1 : c.ping) + " " + std::to_string(c.deaths) + " " +
                                std::to_string(c.statusIcon);
        if (list.size() + one.size() > 1024) break;
        list += one;
        ++n;
    }
    return "b " + std::to_string(n) + " " + std::to_string(axisScore) + " " + std::to_string(alliesScore) + list;
}

MpScoreData mpParseScores(const std::string& args, const std::function<int(int)>& teamOf) {
    std::istringstream in(args);
    std::vector<std::string> tok;
    for (std::string t; in >> t;) tok.push_back(t);
    auto at = [&](size_t i) { return i < tok.size() ? std::atoi(tok[i].c_str()) : 0; };
    MpScoreData d;
    const int count = std::clamp(at(0), 0, 64);
    d.teamScore[1] = at(1);
    d.teamScore[2] = at(2);
    std::array<int, 4> pingSum{};
    for (int i = 0; i < count; ++i) {
        const size_t b = 3 + size_t(i) * 5;
        MpScoreRow r;
        r.client = at(b);
        r.score = at(b + 1);
        r.ping = at(b + 2);
        r.deaths = at(b + 3);
        const int icon = at(b + 4);
        r.statusIcon = icon > 0 && icon < 9 ? icon : 0;
        if (r.client < 0 || r.client > 63) r.client = 0;
        const int t = teamOf ? teamOf(r.client) : 0;
        r.team = t >= 0 && t <= 3 ? t : 0;
        d.teamCount[size_t(r.team)]++;
        pingSum[size_t(r.team)] += r.ping;
        d.rows.push_back(r);
    }
    for (size_t t = 0; t < 4; ++t) d.teamPing[t] = d.teamCount[t] > 0 && pingSum[t] > 0 ? pingSum[t] / d.teamCount[t] : 0;
    return d;
}

namespace {

constexpr float kListX = 129.0f, kListBottom = 432.0f, kRowH = 12.0f;

struct Ctx {
    const MpScoreboardInput& in;
    MpScoreboardOutput& out;
    std::array<float, 4> white;
    bool full = false;
    int counter = 0;

    void rect(float x, float y, float w, float h, std::array<float, 4> c, const std::string& mat) {
        SbItem it;
        it.rect = {x, y, w, h, c, mat};
        out.items.push_back(it);
    }
    void text(const std::string& t, float x, float y, float s, std::array<float, 4> c, int style = 3) {
        SbItem it;
        it.isText = true;
        it.text = {t, x, y, s, c, style};
        out.items.push_back(it);
    }
    float tw(const std::string& t, float s) const { return in.textWidth ? in.textWidth(t, s) : 0.0f; }
    float th(float s) const { return in.textHeight ? in.textHeight(s) : 0.0f; }
    std::string loc(const std::string& k) const { return in.localize ? in.localize(k) : k; }

    // A line that may not be drawn (0x4d9ec0 / 0x4da210): once the list is full nothing more; lines above the scroll
    // position are counted and skipped; a line that would end below y 432 fills the list.
    bool admit(float y, float h) {
        if (full) return false;
        if (counter < in.scroll) {
            ++counter;
            return false;
        }
        if (y + h > kListBottom) {
            full = true;
            return false;
        }
        ++counter;
        return true;
    }

    std::array<float, 4> teamColor(int team) const {
        std::array<float, 4> c = team == 1 ? in.colorAxis : team == 2 ? in.colorAllies : std::array<float, 4>{1, 1, 1, 1};
        c[3] = in.alpha;
        return c;
    }

    float banner(float y, float width, int team) {
        const float bh = float(in.bannerHeight);
        if (!admit(y, bh)) return y;
        const MpScoreData& d = *in.data;
        const int count = d.teamCount[size_t(team)];
        std::string players = loc(count == 1 ? "CGAME_SB_PLAYER" : "CGAME_SB_PLAYERS");
        const auto p = players.find("&&1");
        if (p != std::string::npos) players.replace(p, 3, std::to_string(count));
        std::string label = players, mat = in.bannerNone;
        if (team == 1) {
            mat = in.bannerAxis;
            label = loc(in.teamNameAxis) + " (" + players + ")";
        } else if (team == 2) {
            mat = in.bannerAllies;
            label = loc(in.teamNameAllies) + " (" + players + ")";
        } else if (team == 3) {
            mat = in.bannerSpectators;
            label = loc("CGAME_SPECTATORS") + " (" + players + ")";
        }
        rect(kListX, y, width, bh, white, mat);
        const float base = bh * 0.84f + y;
        text(label, 133.0f, base, 0.32f, teamColor(team));
        if (team == 1 || team == 2) {
            float x = kListX;
            for (const SbColumnDef& c : kScoreboardColumns) {
                std::string v;
                if (c.kind == SbColumn::Score) v = std::to_string(d.teamScore[size_t(team)]);
                else if (c.kind == SbColumn::Ping) v = std::to_string(d.teamPing[size_t(team)]);
                if (!v.empty()) {
                    const float off = c.right ? width * c.frac - tw(v, 0.32f) : 0.0f;
                    text(v, x + off, base, 0.32f, teamColor(team));
                }
                x += width * c.frac;
            }
        }
        return y + bh;
    }

    float row(float y, const MpScoreRow& r, float width, bool odd) {
        if (!admit(y, kRowH)) return y;
        if (in.clientValid && !in.clientValid(r.client)) return y;
        if (r.client == in.localClient) rect(kListX, y, width, kRowH, {1, 1, 1, in.alpha * 0.2f}, "white");
        else if (odd) rect(kListX, y, width, kRowH, {1, 1, 1, in.alpha * 0.15f}, "black");
        float x = kListX;
        for (const SbColumnDef& c : kScoreboardColumns) {
            const float cw = width * c.frac;
            std::string v;
            switch (c.kind) {
            case SbColumn::StatusIcon: {
                const std::string m = r.statusIcon && in.statusIconMaterial ? in.statusIconMaterial(r.statusIcon) : std::string();
                if (!m.empty()) rect(x + (c.right ? cw - 13.0f : 0.0f), y, 13.0f, 13.0f, white, m);
                break;
            }
            case SbColumn::Voice: break; // TODO(spec): voice chat icons (voice_on / voice_off); no voice chat here
            case SbColumn::Name: v = in.nameOf ? in.nameOf(r.client) : std::string(); break;
            case SbColumn::Score: if (r.team != 3) v = std::to_string(r.score); break;
            case SbColumn::Deaths: if (r.team != 3) v = std::to_string(r.deaths); break;
            case SbColumn::Ping: v = std::to_string(r.ping); break;
            }
            if (!v.empty()) {
                float s = 0.22f;
                int style = 3;
                if (tw(v, s) > cw) {
                    while (tw(v, s) > cw && s > 0.0f) s -= 0.02f;
                    if (s < 0.16f) style = 0;
                }
                const float off = c.right ? cw - tw(v, s) : 0.0f;
                text(v, x + off, y + 10.08f, s, white, style);
            }
            x += cw;
        }
        return y + kRowH;
    }

    float rows(float y, int team, float width) {
        bool odd = false;
        for (const MpScoreRow& r : in.data->rows) {
            if (in.clientValid && !in.clientValid(r.client)) continue;
            if (r.team != team) continue;
            y = row(y, r, width, odd);
            odd = !odd;
        }
        return y;
    }

    // Objective text (0x4d95f0): wrapped at 374 units with scale 0.24, 12 units a line from y 56, then a separator line.
    float objective(float y) {
        if (in.objectiveText.empty()) return y;
        y += 4.0f;
        std::vector<std::string> lines;
        std::string cur, word;
        auto flushWord = [&]() {
            if (word.empty()) return;
            const std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && tw(cand, 0.24f) > 374.0f) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = cand;
            }
            word.clear();
        };
        const std::string& t = in.objectiveText;
        for (size_t i = 0; i < t.size(); ++i) {
            const bool nl = t[i] == '\n' || (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == 'n');
            if (nl) {
                flushWord();
                lines.push_back(cur);
                cur.clear();
                if (t[i] == '\\') ++i;
            } else if (t[i] == ' ') {
                flushWord();
            } else {
                word += t[i];
            }
        }
        flushWord();
        lines.push_back(cur);
        for (const std::string& l : lines) {
            text(l, kListX, y + 10.08f, 0.24f, white);
            y += 12.0f;
        }
        rect(125.0f, y + 2.0f, 390.0f, 1.0f, {1, 1, 1, in.alpha * 0.1f}, "white");
        return y + 3.0f;
    }
};

} // namespace

MpScoreboardOutput mpScoreboardLayout(const MpScoreboardInput& in) {
    MpScoreboardOutput out;
    out.scroll = in.scroll;
    out.firstTeam = in.firstTeam;
    if (!in.data) return out;
    Ctx c{in, out, {1, 1, 1, in.alpha}};
    const MpScoreData& d = *in.data;

    // Frame (0x4d8ea0, 0x4d8f10 with the rectangles of 0x4d8e10).
    c.rect(120, 22, 400, 430, {0, 0, 0, in.alpha * 0.8f}, "white");
    const float lines[6][4] = {{123, 25, 394, 2}, {123, 447, 394, 2}, {123, 27, 2, 420},
                               {515, 27, 2, 420}, {125, 51, 390, 1}, {125, 432, 390, 1}};
    for (const auto& l : lines) c.rect(l[0], l[1], l[2], l[3], {1, 1, 1, in.alpha * 0.1f}, "white");

    // Header: the gametype at 0.41 on the left (0x4d8fa0); the map name right-aligned at 511, shrunk in steps of 0.025
    // (not below 0.075) until both fit in 386 units (0x4d90b0).
    c.text(in.gametypeName, kListX, 51.0f - (24.0f - c.th(0.41f)) * 0.5f, 0.41f, c.white);
    float s = 0.41f;
    while (c.tw(in.mapName, s) + 4.0f + c.tw(in.gametypeName, s) > 386.0f) {
        s -= 0.025f;
        if (!(s > 0.075f)) break;
    }
    c.text(in.mapName, 511.0f - (c.tw(in.mapName, s) + 4.0f), 51.0f - (24.0f - c.th(s)) * 0.5f, s, c.white);
    // Footer (0x4d9360): host name on the left, the server address (or "Listen Server") on the right, from 0.2 down in
    // steps of 0.01 until both fit in 386 units.
    const std::string addr = in.serverAddress.empty() ? c.loc("CGAME_LISTENSERVER") : in.serverAddress;
    s = 0.2f;
    while (c.tw(addr, s) + 4.0f + c.tw(in.hostName, s) > 386.0f) {
        s -= 0.01f;
        if (!(s > 0.075f)) break;
    }
    const float fy = 447.0f - (14.0f - c.th(s)) * 0.5f;
    c.text(in.hostName, kListX, fy, s, c.white);
    c.text(addr, 511.0f - (c.tw(addr, s) + 4.0f), fy, s, c.white);

    // List (0x4da980).
    float y = c.objective(52.0f);
    float total = 10.0f;
    int totalLines = 0;
    const float bh = float(in.bannerHeight);
    if (d.teamCount[1] || d.teamCount[2]) {
        total += bh * 2.0f + 8.0f;
        totalLines = 2;
    }
    if (d.teamCount[0]) total += bh + 4.0f, ++totalLines;
    if (d.teamCount[3]) total += bh + 4.0f, ++totalLines;
    total += kRowH * float(d.rows.size());
    totalLines += int(d.rows.size());
    out.lines = totalLines;
    float width = 382.0f;
    if (total > kListBottom - y) {
        width = 374.0f;
        out.scrollbar = true;
    }
    // Column headers (0x4d9c10): scale 0.3, baseline +11.76, 14 units.
    y += 4.0f;
    {
        float x = kListX;
        for (const SbColumnDef& col : kScoreboardColumns) {
            if (col.header) {
                const std::string h = c.loc(col.header);
                const float off = col.right ? width * col.frac - c.tw(h, 0.3f) : 0.0f;
                c.text(h, x + off, y + 11.76f, 0.3f, c.white);
            }
            x += width * col.frac;
        }
    }
    y += 14.0f;
    const float listTop = y;
    if (d.teamCount[1] || d.teamCount[2]) {
        int first = d.teamScore[2] < d.teamScore[1] ? 1 : d.teamScore[2] > d.teamScore[1] ? 2 : in.firstTeam;
        out.firstTeam = first;
        y = c.rows(c.banner(y, width, first), first, width);
        const int second = first == 1 ? 2 : 1;
        y = c.rows(c.banner(y + 4.0f, width, second), second, width);
        y += 4.0f;
    }
    if (d.teamCount[0]) y = c.rows(c.banner(y, width, 0), 0, width) + 4.0f;
    if (d.teamCount[3]) c.rows(c.banner(y, width, 3), 3, width);

    // Scroll bar (0x4da770) at x 505.
    if (out.scrollbar) {
        float h = kListBottom - listTop - 1.0f;
        c.rect(505, listTop, 8, h, {1, 1, 1, in.alpha * 0.5f}, "black");
        float ty = listTop + 1.0f;
        h -= 2.0f;
        if (in.scroll != 0 && totalLines != 0) ty += float(in.scroll) / float(totalLines) * h;
        if (c.counter - in.scroll > 1 && totalLines > 1) h = float(c.counter - in.scroll) / float(totalLines) * h;
        c.rect(506, ty, 6, h, {1, 1, 1, in.alpha * 0.25f}, "white");
        if (in.scroll > 0) {
            c.rect(521, listTop, 16, 16, c.white, "hudscoreboardscroll_uparrow");
            c.rect(521, listTop + 18.0f, 16, 16, c.white, "hudscoreboardscroll_upkey");
        }
        if (c.counter < totalLines) {
            c.rect(521, 415, 16, 16, c.white, "hudscoreboardscroll_downarrow");
            c.rect(521, 397, 16, 16, c.white, "hudscoreboardscroll_downkey");
        }
    }
    if (totalLines - 1 < out.scroll) out.scroll = std::max(0, totalLines - 1);
    return out;
}

} // namespace iw2::game
