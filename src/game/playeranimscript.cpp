// Implemented from the spec in docs/spec/mp-players.md ("The player animation script", "Move type from the movement").

#include "game/playeranimscript.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace iw2::game {

namespace {

struct Tok {
    std::string text; // lower case
    int line;
};

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Words, with ',', '{', '}' and '=' as tokens of their own; `//` comments dropped.
std::vector<Tok> tokenize(std::string_view text) {
    std::vector<Tok> out;
    int line = 1;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c == ',' || c == '{' || c == '}' || c == '=') {
            out.push_back({std::string(1, c), line});
            ++i;
            continue;
        }
        const size_t s = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ',' && text[i] != '{' &&
               text[i] != '}' && text[i] != '=' && !(text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/'))
            ++i;
        out.push_back({lower(text.substr(s, i - s)), line});
    }
    return out;
}

bool isBodyPart(const std::string& t) { return t == "legs" || t == "torso" || t == "both" || t == "turret"; }

class Parser {
public:
    explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}
    bool atEnd() const { return p_ >= t_.size(); }
    const Tok& peek() const { return t_[p_]; }
    Tok next() { return t_[p_++]; }
    bool expect(const char* s, std::string* err) {
        if (atEnd() || peek().text != s) return fail(std::string("expected '") + s + "'", err);
        ++p_;
        return true;
    }
    bool fail(const std::string& m, std::string* err) const {
        if (err) *err = "line " + std::to_string(atEnd() ? (t_.empty() ? 0 : t_.back().line) : peek().line) + ": " + m;
        return false;
    }

    // `{ item* }`
    bool items(std::vector<PaItem>& out, std::string* err) {
        if (!expect("{", err)) return false;
        while (!atEnd() && peek().text != "}") {
            PaItem item;
            item.line = peek().line;
            while (!atEnd() && peek().text != "{") {
                Tok type = next();
                if (type.text == ",") continue;
                if (type.text == "default") continue;
                PaCondition c;
                c.type = type.text;
                while (!atEnd() && peek().text != "," && peek().text != "{") {
                    Tok v = next();
                    if (v.text != "and") c.values.push_back(v.text);
                }
                item.conds.push_back(std::move(c));
            }
            if (!expect("{", err)) return false;
            while (!atEnd() && peek().text != "}") {
                if (!isBodyPart(peek().text)) return fail("expected a body part, got '" + peek().text + "'", err);
                PaCommand cmd;
                const int line = peek().line;
                while (!atEnd() && peek().line == line && peek().text != "}") {
                    Tok w = next();
                    if (isBodyPart(w.text)) {
                        if (atEnd() || peek().line != line) return fail("missing animation", err);
                        PaPart part;
                        part.body = w.text == "legs" ? PaBody::Legs : w.text == "torso" ? PaBody::Torso : PaBody::Both;
                        part.anim = next().text;
                        if (w.text != "turret") cmd.parts.push_back(part);
                    } else if (w.text == "duration" || w.text == "blendtime") {
                        if (atEnd() || peek().line != line) return fail("missing number", err);
                        const int v = std::atoi(next().text.c_str());
                        (w.text == "duration" ? cmd.durationMs : cmd.blendMs) = v;
                    } else if (w.text == "turretanim") {
                        cmd.turretAnim = true;
                    } else if (w.text == "sound") {
                        if (!atEnd() && peek().line == line) cmd.sound = next().text;
                    } else {
                        return fail("unknown command word '" + w.text + "'", err);
                    }
                }
                item.commands.push_back(std::move(cmd));
            }
            if (!expect("}", err)) return false;
            out.push_back(std::move(item));
        }
        return expect("}", err);
    }

private:
    std::vector<Tok> t_;
    size_t p_ = 0;
};

} // namespace

bool PlayerAnimScript::parse(std::string_view text, std::string* error) {
    defines_.clear();
    anims_.clear();
    events_.clear();
    Parser ps(tokenize(text));
    enum { None, Defines, Anims, Events } section = None;
    while (!ps.atEnd()) {
        const std::string t = ps.peek().text;
        if (t == "defines") { ps.next(); section = Defines; continue; }
        if (t == "animations") { ps.next(); section = Anims; continue; }
        if (t == "events") { ps.next(); section = Events; continue; }
        if (section == Defines) {
            if (!ps.expect("set", error)) return false;
            if (ps.atEnd()) return ps.fail("truncated define", error);
            const std::string type = ps.next().text;
            if (ps.atEnd()) return ps.fail("truncated define", error);
            const std::string name = ps.next().text;
            if (!ps.expect("=", error)) return false;
            std::vector<std::string> values;
            while (!ps.atEnd()) {
                values.push_back(ps.next().text);
                if (ps.atEnd() || ps.peek().text != "and") break;
                ps.next();
            }
            defines_[type][name] = std::move(values);
        } else if (section == Anims) {
            if (!ps.expect("state", error)) return false;
            if (ps.atEnd()) return ps.fail("missing state name", error);
            const std::string state = ps.next().text;
            if (!ps.expect("{", error)) return false;
            while (!ps.atEnd() && ps.peek().text != "}") {
                const std::string moveType = ps.next().text;
                std::vector<PaItem> items;
                if (!ps.items(items, error)) return false;
                if (state == "combat") anims_[moveType] = std::move(items);
            }
            if (!ps.expect("}", error)) return false;
        } else if (section == Events) {
            const std::string name = ps.next().text;
            std::vector<PaItem> items;
            if (!ps.items(items, error)) return false;
            events_[name] = std::move(items);
        } else {
            return ps.fail("text outside a section", error);
        }
    }
    return true;
}

bool PlayerAnimScript::valueMatches(const std::string& type, const std::string& value, const std::string& current) const {
    if (value == current) return true;
    const auto t = defines_.find(type);
    if (t == defines_.end()) return false;
    const auto d = t->second.find(value);
    return d != t->second.end() && std::find(d->second.begin(), d->second.end(), current) != d->second.end();
}

bool PlayerAnimScript::holds(const PaItem& item, const PaContext& ctx) const {
    for (const PaCondition& c : item.conds) {
        const std::string* current = nullptr;
        if (c.type == "playeranimtype") current = &ctx.playerAnimType;
        else if (c.type == "weaponclass") current = &ctx.weaponClass;
        else if (c.type == "movetype") current = &ctx.moveType;
        else if (c.type == "weapon_position") current = &ctx.weaponPosition;
        else if (c.type == "strafing") current = &ctx.strafing;
        else if (c.type == "mounted") {
            if (!ctx.mounted) return false;
            continue;
        } else if (c.type == "firing") {
            if (!ctx.firing) return false;
            continue;
        } else {
            return false; // underhand, crouching, ...: never set for a client here
        }
        bool any = false;
        for (const std::string& v : c.values)
            if (valueMatches(c.type, v, *current)) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

const PaItem* PlayerAnimScript::animation(const std::string& moveType, const PaContext& ctx) const {
    const auto it = anims_.find(moveType);
    if (it == anims_.end()) return nullptr;
    for (const PaItem& i : it->second)
        if (holds(i, ctx)) return &i;
    return nullptr;
}

const PaItem* PlayerAnimScript::event(const std::string& name, const PaContext& ctx) const {
    const auto it = events_.find(name);
    if (it == events_.end()) return nullptr;
    for (const PaItem& i : it->second)
        if (holds(i, ctx)) return &i;
    return nullptr;
}

std::string paMantleMoveType(int mantleAnim) {
    static const char* const kNames[] = {"mantle_up_57", "mantle_up_51", "mantle_up_45", "mantle_up_39", "mantle_up_33", "mantle_up_27",
                                         "mantle_up_21", "mantle_over_high", "mantle_over_mid", "mantle_over_low"};
    return mantleAnim >= 1 && mantleAnim <= 10 ? kNames[mantleAnim - 1] : std::string();
}

bool paServerOnlyMoveType(const std::string& t) { return t == "climbup" || t == "climbdown" || t.rfind("mantle_", 0) == 0; }

std::string paMoveType(const PaMovement& m, float moveThreshold) {
    // The mantle move plays its type every step (MP 0x80de377); on a ladder in the air the footsteps step climbs.
    if (const std::string mt = paMantleMoveType(m.mantleAnim); !mt.empty()) return mt;
    if (m.climb != 0) return m.climb > 0 ? "climbup" : "climbdown";
    const bool prone = m.viewHeightTarget == 11, crouch = m.viewHeightTarget == 40;
    const bool stumbling = m.damageTimerMs > std::max(m.damageDurationMs - m.stumbleTimeMs, 0);
    if (m.xySpeed < moveThreshold) {
        if (prone) return "idleprone";
        if (crouch) return "idlecr";
        if (!stumbling) return "idle";
        // The standing idle flinches away from the side the hit came from (docs/spec/mp-players.md).
        float y = m.flinchYaw;
        if (y < 0) y += 360.0f;
        if (y >= 315.0f || y < 45.0f) return "flinch_forward";
        if (y < 135.0f) return "flinch_left";
        if (y < 225.0f) return "flinch_backward";
        return "flinch_right";
    }
    const bool back = m.forwardMove < 0;
    if (prone) return back ? "walkpronebk" : "walkprone";
    if (crouch) {
        if (stumbling) return back ? "stumble_crouch_backward" : "stumble_crouch_forward";
        return m.slow ? (back ? "walkcrbk" : "walkcr") : (back ? "runcrbk" : "runcr");
    }
    if (stumbling) return m.slow ? (back ? "stumble_walk_backward" : "stumble_walk_forward") : (back ? "stumble_backward" : "stumble_forward");
    return m.slow ? (back ? "walkbk" : "walk") : (back ? "runbk" : "run");
}

std::string paStrafing(const PaMovement& m) {
    if (m.forwardMove != 0 || m.rightMove == 0) return "not";
    return m.rightMove > 0 ? "right" : "left";
}

} // namespace iw2::game
