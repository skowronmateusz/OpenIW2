// Hint strings (sethintstring, docs/spec/turrets.md): the script arguments are joined into one marker-delimited text
// (0x4eb9b0) that is registered in a table of 32 distinct strings (configstrings 0x4cd + index, 0x4f0da0).
#pragma once

#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace iw2::game {

struct HintArg {
    bool localized = false; // a localized string reference (value type 3) rather than a plain string
    std::string text;
};

constexpr int kHintStringSlots = 32;       // 0x815dba..0x815dfa, 2 bytes per slot
constexpr size_t kHintStringMax = 1024;    // 0x400 passed by 0x4f0ec0

// Joins the arguments like 0x4eb9b0. Markers: 0x14 separates a localized reference from the text before it, 0x15 introduces
// a run of non-localized text (the first argument and any non-localized one after a localized one). Bytes outside 0x1f..0x7f
// are replaced by '.'. Throws std::runtime_error with the exe's message on a script error. `warn` receives the
// "non-localized strings may not contain letters" warning (printed when loc_warningsAsErrors is off); with locWarnings
// (dvar loc_warnings) such text gets the "UNLOCALIZED: " prefix.
inline std::string buildHintString(const std::vector<HintArg>& args, const std::string& what, bool locWarnings, bool warningsAsErrors,
                                   std::string* warn = nullptr, size_t maxLen = kHintStringMax) {
    auto tooLong = [&] { return what + " is too long. Max length is " + std::to_string(maxLen); };
    std::string out;
    bool introduce = true;
    for (const HintArg& a : args) {
        const std::string& s = a.text;
        if (a.localized) {
            // 0x4eb800: only for references longer than one character.
            if (s.size() > 1)
                for (unsigned char ch : s)
                    if (!std::isalnum(ch) && ch != '_')
                        throw std::runtime_error("Illegal localized string reference: " + s + " must contain only alpha-numeric characters and underscores");
            if (out.size() + s.size() + 1 >= maxLen) throw std::runtime_error(tooLong());
            if (!out.empty()) out.push_back('\x14');
            introduce = true;
        } else {
            // 0x4eb860: an escape byte before the first letter is an error; the first letter ends the scan.
            bool letters = false;
            for (unsigned char ch : s) {
                if (ch == 0x14 || ch == 0x15 || ch == 0x16) throw std::runtime_error("bad escape character (" + std::to_string(int(ch)) + ") present in string");
                if (std::isalpha(ch)) { letters = true; break; }
            }
            if (letters) {
                if (warningsAsErrors) throw std::runtime_error("non-localized " + what + " strings are not allowed to have letters in them: \"" + s + "\"");
                if (warn) *warn = "^3WARNING: Non-localized " + what + " string is not allowed to have letters in it. Must be changed over to a localized string: \"" + s + "\"\n";
            }
            if (out.size() + s.size() + 1 >= maxLen) throw std::runtime_error(tooLong());
            if (introduce) out.push_back('\x15');
            introduce = false;
            if (letters && locWarnings) { // 0x4eb910
                if (out.size() + s.size() + 14 >= maxLen) throw std::runtime_error(tooLong());
                out += "UNLOCALIZED: ";
            }
        }
        for (unsigned char ch : s) out.push_back(ch >= 0x1f && ch <= 0x7f ? char(ch) : '.');
    }
    return out;
}

// The 32 hint-string configstrings (0x4f0da0).
class HintStringTable {
public:
    // The slot of `text`, registering it in the first free slot; -1 when all 32 hold other strings.
    int intern(const std::string& text) {
        for (int i = 0; i < kHintStringSlots; ++i) {
            if (slots_[i].empty()) { slots_[i] = text; return i; }
            if (slots_[i] == text) return i;
        }
        return -1;
    }
    const std::string& at(int i) const { static const std::string none; return i >= 0 && i < kHintStringSlots ? slots_[i] : none; }

private:
    std::array<std::string, kHintStringSlots> slots_;
};

// The text a hint string shows (reader 0x496e80): the 0x14 / 0x15 delimited pieces joined, localized pieces looked up with
// `lookup` (null result keeps the reference). TODO(spec): the "&&N" -> 0x16 parameter rewrite of the reader and the exact
// localization of the 0x15 pieces are not decoded; "&&1" is left in the text for the caller.
template <class Lookup>
std::string hintStringText(const std::string& buf, Lookup lookup) {
    std::string out, piece;
    bool localized = true;
    auto flush = [&] {
        if (piece.empty()) return;
        if (localized) { const std::string* t = lookup(piece); out += t ? *t : piece; }
        else out += piece;
        piece.clear();
    };
    for (char ch : buf) {
        if (ch == '\x14') { flush(); localized = true; }
        else if (ch == '\x15') { flush(); localized = false; }
        else piece.push_back(ch);
    }
    flush();
    return out;
}

} // namespace iw2::game
