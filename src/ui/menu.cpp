#include "ui/menu.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>

namespace iw2::ui {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Token {
    std::string text;
    bool quoted = false;
    int line = 0; // unique per physical source line, so macro expansions share the line of their use
};

using Macros = std::map<std::string, std::vector<Token>>;

bool isPunct(char c) { return c == '{' || c == '}' || c == ';' || c == ','; }

class Preprocessor {
public:
    Preprocessor(const fs::Vfs& vfs, std::vector<std::string>& errors) : vfs_(vfs), errors_(errors) {}

    void run(const std::string& src, const std::string& file, std::vector<Token>& out, int depth = 0) {
        if (depth > 16) {
            errors_.push_back(file + ": #include nesting too deep");
            return;
        }
        size_t i = 0;
        const size_t n = src.size();
        int line = ++lineId_;
        bool lineStart = true;
        auto newline = [&]() {
            line = ++lineId_;
            lineStart = true;
        };
        while (i < n) {
            const char c = src[i];
            if (c == '\n') {
                ++i;
                newline();
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && src[i + 1] == '/') {
                while (i < n && src[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                    if (src[i] == '\n') newline();
                    ++i;
                }
                i = std::min(n, i + 2);
                continue;
            }
            if (c == '#' && lineStart) {
                size_t e = src.find('\n', i);
                if (e == std::string::npos) e = n;
                directive(src.substr(i + 1, e - i - 1), file, out, depth);
                i = e;
                continue;
            }
            lineStart = false;
            Token t;
            t.line = line;
            if (c == '"') {
                ++i;
                while (i < n && src[i] != '"' && src[i] != '\n') {
                    if (src[i] == '\\' && i + 1 < n && (src[i + 1] == '"' || src[i + 1] == '\\')) ++i;
                    t.text += src[i++];
                }
                if (i < n && src[i] == '"') ++i;
                t.quoted = true;
            } else if (isPunct(c)) {
                t.text = std::string(1, c);
                ++i;
            } else {
                while (i < n && !std::isspace(static_cast<unsigned char>(src[i])) && !isPunct(src[i]) && src[i] != '"' &&
                       !(src[i] == '/' && i + 1 < n && (src[i + 1] == '/' || src[i + 1] == '*')))
                    t.text += src[i++];
            }
            if (!active()) continue;
            emit(t, out, 0);
        }
    }

private:
    bool active() const {
        return std::all_of(conds_.begin(), conds_.end(), [](bool b) { return b; });
    }

    void emit(const Token& t, std::vector<Token>& out, int depth) {
        if (!t.quoted && depth < 32) {
            if (auto it = macros_.find(t.text); it != macros_.end()) {
                for (Token m : it->second) {
                    m.line = t.line;
                    emit(m, out, depth + 1);
                }
                return;
            }
        }
        out.push_back(t);
    }

    void directive(const std::string& text, const std::string& file, std::vector<Token>& out, int depth) {
        // Lex the directive line on its own (comments removed, strings kept).
        std::vector<Token> toks;
        {
            Preprocessor sub(vfs_, errors_);
            std::vector<Token> raw;
            sub.lexOnly(text, raw);
            toks = std::move(raw);
        }
        if (toks.empty()) return;
        const std::string d = toks[0].text;
        if (d == "ifdef" || d == "ifndef") {
            const bool def = toks.size() > 1 && macros_.count(toks[1].text);
            conds_.push_back(d == "ifdef" ? def : !def);
        } else if (d == "else") {
            if (!conds_.empty()) conds_.back() = !conds_.back();
        } else if (d == "endif") {
            if (!conds_.empty()) conds_.pop_back();
        } else if (!active()) {
            return;
        } else if (d == "define" && toks.size() > 1) {
            macros_[toks[1].text] = std::vector<Token>(toks.begin() + 2, toks.end());
        } else if (d == "undef" && toks.size() > 1) {
            macros_.erase(toks[1].text);
        } else if (d == "include" && toks.size() > 1) {
            const std::string path = toks[1].text;
            const auto data = vfs_.read(path);
            if (!data) {
                errors_.push_back(file + ": cannot include " + path);
                return;
            }
            run(std::string(data->begin(), data->end()), path, out, depth + 1);
        }
    }

    void lexOnly(const std::string& text, std::vector<Token>& out) {
        // Same lexer without directives or macros.
        const Macros saved = std::move(macros_);
        macros_.clear();
        run(text, "", out);
        macros_ = saved;
    }

    const fs::Vfs& vfs_;
    std::vector<std::string>& errors_;
    Macros macros_;
    std::vector<bool> conds_;
    static inline int lineId_ = 0;
};

class Parser {
public:
    Parser(std::vector<Token> toks, std::string file, std::vector<std::string>& errors)
        : t_(std::move(toks)), file_(std::move(file)), errors_(errors) {}

    std::vector<std::string> loadMenus;
    std::vector<std::unique_ptr<Menu>> menus;
    std::vector<std::string> warnings;

    void parseFile() {
        bool skipped = false;
        while (pos_ < t_.size()) {
            const Token& k = t_[pos_];
            if (k.text == "{" || k.text == "}") {
                ++pos_;
                continue;
            }
            const std::string key = lower(k.text);
            ++pos_;
            if (key == "menudef") {
                auto m = std::make_unique<Menu>();
                m->file = file_;
                parseWindow(*m, [&](const std::string& kk) {
                    if (kk != "itemdef") return false;
                    auto it = std::make_unique<Item>();
                    parseWindow(*it, [](const std::string&) { return false; });
                    decodeItem(*it);
                    m->items.push_back(std::move(it));
                    return true;
                });
                decodeMenu(*m);
                menus.push_back(std::move(m));
            } else if (key == "loadmenu") {
                for (const auto& s : block()) loadMenus.push_back(s);
            } else if (key == "assetglobaldef") {
                block();
            } else if (!skipped) {
                // Files such as ui/mission_select/mission_layout.menu are meant to be #included inside a
                // menuDef but are also listed in ui/menus.txt; the loose definitions are skipped.
                skipped = true;
                warnings.push_back(file_ + ": skipped top-level '" + k.text + "' and what follows outside menuDef");
            }
        }
    }

private:
    bool at(const char* s) const { return pos_ < t_.size() && !t_[pos_].quoted && t_[pos_].text == s; }

    // Tokens of a balanced { … } block (without the outer braces).
    std::vector<std::string> block() {
        std::vector<std::string> out;
        if (!at("{")) return out;
        ++pos_;
        int depth = 1;
        while (pos_ < t_.size()) {
            const Token& t = t_[pos_++];
            if (!t.quoted && t.text == "{") ++depth;
            if (!t.quoted && t.text == "}" && --depth == 0) break;
            out.push_back(t.text);
        }
        return out;
    }

    // Parses `{ key args… [{ block }] … }` into props. `child` handles nested definitions (itemDef).
    void parseWindow(Window& w, const std::function<bool(const std::string&)>& child) {
        if (!at("{")) {
            errors_.push_back(file_ + ": expected '{'");
            return;
        }
        ++pos_;
        while (pos_ < t_.size() && !at("}")) {
            const Token key = t_[pos_++];
            const std::string k = lower(key.text);
            if (child(k)) continue;
            std::vector<std::string> args;
            while (pos_ < t_.size() && t_[pos_].line == key.line && !at("{") && !at("}")) args.push_back(t_[pos_++].text);
            if (at("{")) {
                auto b = block();
                args.push_back("{");
                args.insert(args.end(), b.begin(), b.end());
            }
            w.props.emplace(k, std::move(args));
        }
        if (at("}")) ++pos_;
    }

    static float num(const std::vector<std::string>& a, size_t i, float def = 0) {
        return i < a.size() ? std::strtof(a[i].c_str(), nullptr) : def;
    }
    static Color color(const std::vector<std::string>& a) { return {num(a, 0), num(a, 1), num(a, 2), num(a, 3, 1)}; }
    // Splits args at the "{" marker: [before, block].
    static std::pair<std::vector<std::string>, std::vector<std::string>> split(const std::vector<std::string>& a) {
        auto it = std::find(a.begin(), a.end(), "{");
        return {std::vector<std::string>(a.begin(), it), it == a.end() ? std::vector<std::string>() : std::vector<std::string>(it + 1, a.end())};
    }

    void decodeWindow(Window& w) {
        for (const auto& [k, a] : w.props) {
            if (k == "name" && !a.empty()) w.name = a[0];
            else if (k == "group" && !a.empty()) w.group = a[0];
            else if (k == "rect") {
                w.rect = {num(a, 0), num(a, 1), num(a, 2), num(a, 3), int(num(a, 4)), int(num(a, 5))};
            } else if (k == "origin") {
                w.originX = num(a, 0);
                w.originY = num(a, 1);
            } else if (k == "style") w.style = int(num(a, 0));
            else if (k == "border") w.border = int(num(a, 0));
            else if (k == "bordersize") w.borderSize = num(a, 0);
            else if (k == "visible") w.visible = num(a, 0) != 0;
            else if (k == "decoration") w.decoration = true;
            else if (k == "ownerdraw") w.ownerDraw = int(num(a, 0));
            else if (k == "textsavegame") w.textSaveGame = true;
            else if (k == "forecolor") w.foreColor = color(a);
            else if (k == "backcolor") w.backColor = color(a);
            else if (k == "bordercolor") w.borderColor = color(a);
            else if (k == "outlinecolor") w.outlineColor = color(a);
            else if (k == "background" && !a.empty()) w.background = a[0];
        }
        // exe 0x4d1dd0: `origin x y` stores rectClient + origin into the live rect (w/h/align copied unchanged).
        if (w.props.count("origin")) {
            w.rect.x += w.originX;
            w.rect.y += w.originY;
        }
    }

    void decodeItem(Item& it) {
        decodeWindow(it);
        for (const auto& [k, a] : it.props) {
            const auto [args, blk] = split(a);
            if (k == "type") it.type = int(num(a, 0));
            else if (k == "text" && !a.empty()) it.text = a[0];
            else if (k == "textscale") it.textScale = num(a, 0);
            else if (k == "textstyle") it.textStyle = int(num(a, 0));
            else if (k == "textalign") it.textAlign = int(num(a, 0));
            else if (k == "textalignx") it.textAlignX = num(a, 0);
            else if (k == "textaligny") it.textAlignY = num(a, 0);
            else if (k == "textfont") it.textFont = int(num(a, 0));
            else if (k == "autowrapped") it.autoWrapped = true;
            else if (k == "dvar" && !a.empty()) it.dvar = a[0];
            else if (k == "dvartest" && !a.empty()) it.dvarTest = a[0];
            else if (k == "dvarfloat" && args.size() >= 4) {
                it.dvar = args[0];
                it.hasRange = true;
                it.dvarDef = num(args, 1);
                it.dvarMin = num(args, 2);
                it.dvarMax = num(args, 3);
            } else if (k == "dvarfloatlist") {
                const auto v = strings(blk);
                for (size_t i = 0; i + 1 < v.size(); i += 2) it.floatList.emplace_back(v[i], std::strtof(v[i + 1].c_str(), nullptr));
            } else if (k == "dvarstrlist") {
                const auto v = strings(blk);
                for (size_t i = 0; i + 1 < v.size(); i += 2) it.strList.emplace_back(v[i], v[i + 1]);
            } else if (k == "dvarenumlist" && !a.empty()) it.enumList = a[0];
            else if (k == "focusdvar") it.focusDvar = strings(blk);
            else if (k == "showdvar") it.showDvar = strings(blk);
            else if (k == "hidedvar") it.hideDvar = strings(blk);
            else if (k == "enabledvar") it.enableDvar = strings(blk);
            else if (k == "disabledvar") it.disableDvar = strings(blk);
            else if (k == "action") it.action = parseScript(blk);
            else if (k == "mouseenter") it.mouseEnter = parseScript(blk);
            else if (k == "mouseexit") it.mouseExit = parseScript(blk);
            else if (k == "onfocus") it.onFocus = parseScript(blk);
            else if (k == "leavefocus") it.leaveFocus = parseScript(blk);
            else if (k == "accept") it.accept = parseScript(blk);
            else if (k == "elementwidth") it.elementWidth = num(a, 0);
            else if (k == "elementheight") it.elementHeight = num(a, 0);
            else if (k == "notselectable") it.notSelectable = true;
            else if (k == "elementtype") it.elementType = int(num(a, 0));
            else if (k == "feeder") it.feeder = int(num(a, 0));
            else if (k == "doubleclick") it.doubleClick = parseScript(blk);
            else if (k == "maxchars") it.maxChars = int(num(a, 0));
            else if (k == "maxpaintchars") it.maxPaintChars = int(num(a, 0));
            else if (k == "columns") {
                const int n = int(num(a, 0));
                for (int c = 0; c < n && size_t(1 + c * 3 + 2) < a.size(); ++c)
                    it.columns.push_back({int(num(a, 1 + c * 3)), int(num(a, 2 + c * 3)), int(num(a, 3 + c * 3))});
            }
            else if ((k == "execkey" || k == "execkeyint") && !args.empty()) it.execKeys.emplace_back(args[0], parseScript(blk));
        }
    }

    void decodeMenu(Menu& m) {
        decodeWindow(m);
        for (const auto& [k, a] : m.props) {
            const auto [args, blk] = split(a);
            if (k == "fullscreen") m.fullScreen = num(a, 0) != 0;
            else if (k == "popup") m.popup = true;
            else if (k == "focuscolor") m.focusColor = color(a);
            else if (k == "soundloop" && !a.empty()) m.soundLoop = a[0];
            else if (k == "blurworld") m.blurWorld = num(a, 0);
            else if (k == "onopen") m.onOpen = parseScript(blk);
            else if (k == "onclose") m.onClose = parseScript(blk);
            else if (k == "onesc") m.onEsc = parseScript(blk);
            else if ((k == "execkey" || k == "execkeyint") && !args.empty()) m.execKeys.emplace_back(args[0], parseScript(blk));
        }
    }

    static std::vector<std::string> strings(const std::vector<std::string>& blk) {
        std::vector<std::string> out;
        for (const auto& s : blk)
            if (s != ";" && s != ",") out.push_back(s);
        return out;
    }

    std::vector<Token> t_;
    size_t pos_ = 0;
    std::string file_;
    std::vector<std::string>& errors_;
};

} // namespace

const std::vector<std::string>* Window::prop(const std::string& key) const {
    auto it = props.find(lower(key));
    return it == props.end() ? nullptr : &it->second;
}

Script parseScript(const std::vector<std::string>& tokens) {
    Script out;
    Command cur;
    for (const auto& t : tokens) {
        if (t == ";") {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(t);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

void MenuLibrary::parseSource(const std::string& source, const std::string& file) {
    std::vector<Token> toks;
    Preprocessor(vfs_, errors_).run(source, file, toks);
    Parser p(std::move(toks), file, errors_);
    p.parseFile();
    warnings_.insert(warnings_.end(), p.warnings.begin(), p.warnings.end());
    for (auto& m : p.menus) {
        std::erase_if(menus_, [&](const auto& old) { return lower(old->name) == lower(m->name); });
        menus_.push_back(std::move(m));
    }
    for (const auto& path : p.loadMenus) loadFile(path);
}

bool MenuLibrary::loadFile(const std::string& path) {
    const auto data = vfs_.read(path);
    if (!data) {
        errors_.push_back("missing menu file " + path);
        return false;
    }
    parseSource(std::string(data->begin(), data->end()), path);
    return true;
}

bool MenuLibrary::loadList(const std::string& path) { return loadFile(path); }

Menu* MenuLibrary::find(const std::string& name) {
    const std::string n = lower(name);
    for (auto& m : menus_)
        if (lower(m->name) == n) return m.get();
    return nullptr;
}

} // namespace iw2::ui
