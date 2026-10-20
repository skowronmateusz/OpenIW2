#include "script/parser.h"

#include <cctype>
#include <vector>

namespace iw2::script {

const char* nodeKindName(NodeKind k) {
    switch (k) {
    case NodeKind::File: return "file";
    case NodeKind::Include: return "include";
    case NodeKind::UsingAnimtree: return "using_animtree";
    case NodeKind::Function: return "function";
    case NodeKind::Block: return "block";
    case NodeKind::ExprStmt: return "expr";
    case NodeKind::If: return "if";
    case NodeKind::While: return "while";
    case NodeKind::For: return "for";
    case NodeKind::Switch: return "switch";
    case NodeKind::Case: return "case";
    case NodeKind::Default: return "default";
    case NodeKind::Break: return "break";
    case NodeKind::Continue: return "continue";
    case NodeKind::Return: return "return";
    case NodeKind::Wait: return "wait";
    case NodeKind::WaitTillFrameEnd: return "waittillframeend";
    case NodeKind::Breakpoint: return "breakpoint";
    case NodeKind::DevBlock: return "dev";
    case NodeKind::Empty: return "empty";
    case NodeKind::Assign: return "assign";
    case NodeKind::PreIncDec: return "pre";
    case NodeKind::PostIncDec: return "post";
    case NodeKind::Binary: return "bin";
    case NodeKind::Unary: return "un";
    case NodeKind::Call: return "call";
    case NodeKind::FunctionRef: return "funcref";
    case NodeKind::Member: return "member";
    case NodeKind::Index: return "index";
    case NodeKind::Identifier: return "id";
    case NodeKind::Integer: return "int";
    case NodeKind::Float: return "float";
    case NodeKind::String: return "str";
    case NodeKind::LocalizedString: return "istr";
    case NodeKind::Vector: return "vec";
    case NodeKind::EmptyArray: return "array";
    case NodeKind::Undefined: return "undefined";
    case NodeKind::True: return "true";
    case NodeKind::False: return "false";
    case NodeKind::Self: return "self";
    case NodeKind::Level: return "level";
    case NodeKind::Game: return "game";
    case NodeKind::Anim: return "anim";
    case NodeKind::AnimRef: return "animref";
    case NodeKind::AnimTree: return "animtree";
    case NodeKind::Size: return "size";
    }
    return "?";
}

namespace {

enum class Tok { End, Ident, Int, Float, String, IString, AnimRef, Punct, Path };

struct Token {
    Tok type;
    std::string text;
    int line;
};

class Lexer {
public:
    Lexer(std::string_view src, std::string name) : s_(src), name_(std::move(name)) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        for (;;) {
            skip();
            if (i_ >= s_.size()) break;
            const int line = line_;
            const char c = s_[i_];
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                // Identifiers may contain path separators: maps\_utility, animscripts\shared
                size_t j = i_;
                bool path = false;
                while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' || s_[j] == '\\')) {
                    if (s_[j] == '\\') path = true;
                    ++j;
                }
                out.push_back({path ? Tok::Path : Tok::Ident, std::string(s_.substr(i_, j - i_)), line});
                i_ = j;
            } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
                size_t j = i_;
                bool isFloat = false;
                while (j < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[j])) || s_[j] == '.')) {
                    if (s_[j] == '.') isFloat = true;
                    ++j;
                }
                out.push_back({isFloat ? Tok::Float : Tok::Int, std::string(s_.substr(i_, j - i_)), line});
                i_ = j;
            } else if (c == '"' || (c == '&' && i_ + 1 < s_.size() && s_[i_ + 1] == '"')) {
                const bool loc = c == '&';
                if (loc) ++i_;
                ++i_;
                std::string v;
                while (i_ < s_.size() && s_[i_] != '"') {
                    if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                        const char e = s_[i_ + 1];
                        v += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                        i_ += 2;
                        continue;
                    }
                    if (s_[i_] == '\n') ++line_;
                    v += s_[i_++];
                }
                if (i_ >= s_.size()) throw ParseError(name_, line, "unterminated string");
                ++i_;
                out.push_back({loc ? Tok::IString : Tok::String, v, line});
            } else if (c == '%' && i_ + 1 < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[i_ + 1])) || s_[i_ + 1] == '_') &&
                       !afterOperand(out)) {
                size_t j = i_ + 1;
                while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_')) ++j;
                out.push_back({Tok::AnimRef, std::string(s_.substr(i_ + 1, j - i_ - 1)), line});
                i_ = j;
            } else {
                static const char* ops[] = {"<<=", ">>=", "::", "[[", "]]", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=",
                                            "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "/#", "#/"};
                std::string op(1, c);
                for (const char* o : ops) {
                    const size_t n = std::char_traits<char>::length(o);
                    if (s_.substr(i_, n) == o) { op = o; break; }
                }
                // "[[" is ambiguous with nested indexing like a[[b]]; the parser handles it.
                out.push_back({Tok::Punct, op, line});
                i_ += op.size();
            }
        }
        out.push_back({Tok::End, "", line_});
        return out;
    }

private:
    // "%name" is an animation reference unless it follows an operand, where '%' is modulo.
    static bool afterOperand(const std::vector<Token>& out) {
        if (out.empty()) return false;
        const Token& t = out.back();
        if (t.type == Tok::Ident) return t.text != "return" && t.text != "case" && t.text != "wait";
        if (t.type == Tok::Int || t.type == Tok::Float || t.type == Tok::String || t.type == Tok::Path || t.type == Tok::AnimRef) return true;
        return t.type == Tok::Punct && (t.text == ")" || t.text == "]" || t.text == "]]");
    }

    void skip() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n') { ++line_; ++i_; }
            else if (std::isspace(static_cast<unsigned char>(c))) ++i_;
            else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') { while (i_ < s_.size() && s_[i_] != '\n') ++i_; }
            else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                i_ += 2;
                while (i_ + 1 < s_.size() && !(s_[i_] == '*' && s_[i_ + 1] == '/')) { if (s_[i_] == '\n') ++line_; ++i_; }
                i_ += 2;
            } else break;
        }
    }

    std::string_view s_;
    std::string name_;
    size_t i_ = 0;
    int line_ = 1;
};

class Parser {
public:
    Parser(std::vector<Token> toks, std::string name) : t_(std::move(toks)), name_(std::move(name)) {}

    NodePtr file() {
        auto f = make(NodeKind::File, "");
        while (!at(Tok::End)) {
            if (isPunct("#")) {
                next();
                const Token d = expect(Tok::Ident, "directive");
                if (d.text == "include") {
                    const Token p = next();
                    if (p.type != Tok::Path && p.type != Tok::Ident) fail("expected include path");
                    f->add(std::make_unique<Node>(NodeKind::Include, p.text, p.line));
                    expectPunct(";");
                } else if (d.text == "using_animtree") {
                    expectPunct("(");
                    const Token s = expect(Tok::String, "animtree name");
                    expectPunct(")");
                    expectPunct(";");
                    f->add(std::make_unique<Node>(NodeKind::UsingAnimtree, s.text, s.line));
                } else {
                    fail("unknown directive #" + d.text);
                }
            } else if (isPunct("/#") || isPunct("#/")) {
                next(); // stray developer markers at file scope
            } else {
                f->add(function());
            }
        }
        return f;
    }

private:
    // --- token helpers
    const Token& peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    Token next() { return t_[std::min(p_++, t_.size() - 1)]; }
    bool at(Tok t) const { return peek().type == t; }
    bool isPunct(const char* s, size_t k = 0) const { return peek(k).type == Tok::Punct && peek(k).text == s; }
    bool isIdent(const char* s, size_t k = 0) const { return peek(k).type == Tok::Ident && peek(k).text == s; }
    bool acceptPunct(const char* s) { if (isPunct(s)) { ++p_; return true; } return false; }
    [[noreturn]] void fail(const std::string& msg) const { throw ParseError(name_, peek().line, msg + " (at '" + peek().text + "')"); }
    Token expect(Tok t, const char* what) { if (!at(t)) fail(std::string("expected ") + what); return next(); }
    void expectPunct(const char* s) { if (!acceptPunct(s)) fail(std::string("expected '") + s + "'"); }
    NodePtr make(NodeKind k, std::string text) { return std::make_unique<Node>(k, std::move(text), peek().line); }

    // --- declarations
    NodePtr function() {
        const Token name = expect(Tok::Ident, "function name");
        auto fn = std::make_unique<Node>(NodeKind::Function, name.text, name.line);
        expectPunct("(");
        auto params = make(NodeKind::Block, "params");
        if (!isPunct(")")) {
            do params->add(std::make_unique<Node>(NodeKind::Identifier, expect(Tok::Ident, "parameter").text, peek().line));
            while (acceptPunct(","));
        }
        expectPunct(")");
        fn->add(std::move(params));
        fn->add(block());
        return fn;
    }

    // --- statements
    NodePtr block() {
        auto b = make(NodeKind::Block, "");
        expectPunct("{");
        while (!isPunct("}")) {
            if (at(Tok::End)) fail("unexpected end of file in block");
            b->add(statement());
        }
        next();
        return b;
    }

    NodePtr statement() {
        if (isPunct("{")) return block();
        if (isPunct(";")) { next(); return make(NodeKind::Empty, ""); }
        if (isPunct("/#")) {
            auto d = make(NodeKind::DevBlock, "");
            next();
            while (!isPunct("#/")) {
                if (at(Tok::End)) fail("unterminated /# block");
                d->add(statement());
            }
            next();
            return d;
        }
        if (at(Tok::Ident)) {
            const std::string& k = peek().text;
            if (k == "if") {
                auto n = make(NodeKind::If, ""); next();
                expectPunct("("); n->add(expression()); expectPunct(")");
                n->add(statement());
                if (isIdent("else")) { next(); n->add(statement()); }
                return n;
            }
            if (k == "while") {
                auto n = make(NodeKind::While, ""); next();
                expectPunct("("); n->add(expression()); expectPunct(")");
                n->add(statement());
                return n;
            }
            if (k == "for") {
                auto n = make(NodeKind::For, ""); next();
                expectPunct("(");
                n->add(isPunct(";") ? make(NodeKind::Empty, "") : simpleStatementExpr()); expectPunct(";");
                n->add(isPunct(";") ? make(NodeKind::Empty, "") : expression()); expectPunct(";");
                n->add(isPunct(")") ? make(NodeKind::Empty, "") : simpleStatementExpr()); expectPunct(")");
                n->add(statement());
                return n;
            }
            if (k == "switch") {
                auto n = make(NodeKind::Switch, ""); next();
                expectPunct("("); n->add(expression()); expectPunct(")");
                expectPunct("{");
                auto body = make(NodeKind::Block, "");
                while (!isPunct("}")) {
                    if (isIdent("case")) {
                        auto c = make(NodeKind::Case, ""); next();
                        c->add(expression()); expectPunct(":");
                        body->add(std::move(c));
                    } else if (isIdent("default")) {
                        body->add(make(NodeKind::Default, "")); next(); expectPunct(":");
                    } else {
                        if (at(Tok::End)) fail("unterminated switch");
                        body->add(statement());
                    }
                }
                next();
                n->add(std::move(body));
                return n;
            }
            if (k == "break") { auto n = make(NodeKind::Break, ""); next(); expectPunct(";"); return n; }
            if (k == "continue") { auto n = make(NodeKind::Continue, ""); next(); expectPunct(";"); return n; }
            if (k == "breakpoint") { auto n = make(NodeKind::Breakpoint, ""); next(); expectPunct(";"); return n; }
            if (k == "waittillframeend") { auto n = make(NodeKind::WaitTillFrameEnd, ""); next(); expectPunct(";"); return n; }
            if (k == "return") {
                auto n = make(NodeKind::Return, ""); next();
                if (!isPunct(";")) n->add(expression());
                expectPunct(";");
                return n;
            }
            if (k == "wait") {
                auto n = make(NodeKind::Wait, ""); next();
                n->add(expression());
                expectPunct(";");
                return n;
            }
        }
        auto e = make(NodeKind::ExprStmt, "");
        e->add(simpleStatementExpr());
        // Two shipped scripts (animscripts/traverse/stairs_*.gsc) omit ';' at the end of a line.
        // Accept that leniently; whether the original compiler does is not known.
        if (!acceptPunct(";") && !(p_ > 0 && peek().line > t_[p_ - 1].line)) fail("expected ';'");
        return e;
    }

    // Assignment, ++/--, or a call.
    NodePtr simpleStatementExpr() {
        if (isPunct("++") || isPunct("--")) {
            auto n = make(NodeKind::PreIncDec, next().text);
            n->add(postfix());
            return n;
        }
        auto lhs = expression();
        static const char* assignOps[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const char* op : assignOps)
            if (isPunct(op)) {
                auto n = make(NodeKind::Assign, next().text);
                n->add(std::move(lhs));
                n->add(expression());
                return n;
            }
        if (isPunct("++") || isPunct("--")) {
            auto n = make(NodeKind::PostIncDec, next().text);
            n->add(std::move(lhs));
            return n;
        }
        return lhs;
    }

    // --- expressions (precedence climbing)
    NodePtr expression() { return ternaryFree(0); }

    static int precedence(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return -1;
    }

    NodePtr ternaryFree(int minPrec) {
        auto lhs = unary();
        for (;;) {
            if (!at(Tok::Punct)) break;
            const int prec = precedence(peek().text);
            if (prec < 0 || prec < minPrec) break;
            auto n = make(NodeKind::Binary, next().text);
            n->add(std::move(lhs));
            n->add(ternaryFree(prec + 1));
            lhs = std::move(n);
        }
        return lhs;
    }

    NodePtr unary() {
        if (isPunct("!") || isPunct("-") || isPunct("~")) {
            auto n = make(NodeKind::Unary, next().text);
            n->add(unary());
            return n;
        }
        return postfix();
    }

    NodePtr postfix() {
        auto e = primary();
        for (;;) {
            if (isPunct(".")) {
                next();
                const Token f = expect(Tok::Ident, "field name");
                auto n = std::make_unique<Node>(f.text == "size" ? NodeKind::Size : NodeKind::Member, f.text, f.line);
                n->add(std::move(e));
                e = std::move(n);
            } else if (isPunct("[") || (isPunct("[[") && !looksLikePointerCall())) {
                // "[[" here is "[" followed by "[" of a nested index; split it.
                if (isPunct("[[")) splitDoubleBracket();
                next();
                auto n = make(NodeKind::Index, "");
                n->add(std::move(e));
                n->add(expression());
                if (isPunct("]]")) splitCloseBracket();
                expectPunct("]");
                e = std::move(n);
            } else if (startsCall()) {
                // "object [thread] func(...)" form: e is the object.
                e = call(std::move(e));
            } else {
                break;
            }
        }
        return e;
    }

    bool looksLikePointerCall() const {
        // [[ expr ]] ( ... ): count brackets one character at a time.
        int depth = 0;
        for (size_t k = p_; k < t_.size(); ++k) {
            const Token& t = t_[k];
            if (t.type == Tok::Punct) {
                if (t.text == "[[") depth += 2;
                else if (t.text == "[") depth += 1;
                else if (t.text == "]]") depth -= 2;
                else if (t.text == "]") depth -= 1;
                else if (t.text == ";") return false;
                if (depth <= 0 && (t.text == "]]" || t.text == "]"))
                    return depth == 0 && k + 1 < t_.size() && t_[k + 1].type == Tok::Punct && t_[k + 1].text == "(";
            } else if (t.type == Tok::End) return false;
        }
        return false;
    }

    // Closes a pointer call: "]]" or two "]" tokens (after a split).
    void expectCloseDouble() {
        if (acceptPunct("]]")) return;
        expectPunct("]");
        expectPunct("]");
    }
    void splitDoubleBracket() {
        Token& t = t_[p_];
        t.text = "[";
        t_.insert(t_.begin() + long(p_) + 1, Token{Tok::Punct, "[", t.line});
    }
    void splitCloseBracket() {
        Token& t = t_[p_];
        t.text = "]";
        t_.insert(t_.begin() + long(p_) + 1, Token{Tok::Punct, "]", t.line});
    }

    // After an object expression: "thread f(", "f(", "path::f(", "::f(", "[[fp]](".
    bool startsCall() const {
        size_t k = 0;
        if (isIdent("thread", k)) ++k;
        if (peek(k).type == Tok::Punct && peek(k).text == "[[") return true;
        if (peek(k).type == Tok::Punct && peek(k).text == "::") return peek(k + 1).type == Tok::Ident && peek(k + 2).text == "(";
        if (peek(k).type == Tok::Ident || peek(k).type == Tok::Path) {
            if (peek(k + 1).type == Tok::Punct && peek(k + 1).text == "(") return true;
            if (peek(k + 1).type == Tok::Punct && peek(k + 1).text == "::") return true;
        }
        return false;
    }

    NodePtr call(NodePtr object) {
        auto n = make(NodeKind::Call, "");
        if (isIdent("thread")) { next(); n->isThread = true; }
        n->add(object ? std::move(object) : make(NodeKind::Empty, ""));
        if (isPunct("[[")) {
            next();
            n->isPointerCall = true;
            n->add(expression());
            expectCloseDouble();
        } else {
            std::string name;
            if (isPunct("::")) { next(); name = "::" + expect(Tok::Ident, "function").text; }
            else {
                name = next().text;
                if (acceptPunct("::")) name += "::" + expect(Tok::Ident, "function").text;
            }
            n->add(std::make_unique<Node>(NodeKind::FunctionRef, name, n->line));
        }
        expectPunct("(");
        if (!isPunct(")")) {
            do n->add(expression());
            while (acceptPunct(","));
        }
        expectPunct(")");
        return n;
    }

    NodePtr primary() {
        const Token& t = peek();
        switch (t.type) {
        case Tok::Int: return std::make_unique<Node>(NodeKind::Integer, next().text, t.line);
        case Tok::Float: return std::make_unique<Node>(NodeKind::Float, next().text, t.line);
        case Tok::String: return std::make_unique<Node>(NodeKind::String, next().text, t.line);
        case Tok::IString: return std::make_unique<Node>(NodeKind::LocalizedString, next().text, t.line);
        case Tok::AnimRef: return std::make_unique<Node>(NodeKind::AnimRef, next().text, t.line);
        case Tok::Path:
            if (peek(1).text == "::" && peek(3).text != "(") { // function reference maps\x::f
                const std::string path = next().text;
                next();
                return std::make_unique<Node>(NodeKind::FunctionRef, path + "::" + expect(Tok::Ident, "function").text, t.line);
            }
            if (startsCall()) return call(nullptr);
            fail("unexpected path");
        case Tok::Ident: {
            if (startsCall() && !(peek(1).text == "::" && peek(3).text != "(")) return call(nullptr);
            if (peek(1).text == "::" && peek(3).text != "(") {
                const std::string path = next().text;
                next();
                return std::make_unique<Node>(NodeKind::FunctionRef, path + "::" + expect(Tok::Ident, "function").text, t.line);
            }
            const std::string w = next().text;
            if (w == "undefined") return std::make_unique<Node>(NodeKind::Undefined, w, t.line);
            if (w == "true") return std::make_unique<Node>(NodeKind::True, w, t.line);
            if (w == "false") return std::make_unique<Node>(NodeKind::False, w, t.line);
            if (w == "self") return std::make_unique<Node>(NodeKind::Self, w, t.line);
            if (w == "level") return std::make_unique<Node>(NodeKind::Level, w, t.line);
            if (w == "game") return std::make_unique<Node>(NodeKind::Game, w, t.line);
            if (w == "anim") return std::make_unique<Node>(NodeKind::Anim, w, t.line);
            return std::make_unique<Node>(NodeKind::Identifier, w, t.line);
        }
        case Tok::Punct:
            if (t.text == "::") {
                if (peek(2).text == "(") return call(nullptr);
                next();
                return std::make_unique<Node>(NodeKind::FunctionRef, "::" + expect(Tok::Ident, "function").text, t.line);
            }
            if (t.text == "[[") return call(nullptr);
            if (t.text == "#") {
                next();
                const Token a = expect(Tok::Ident, "animtree");
                if (a.text != "animtree") fail("expected #animtree");
                return std::make_unique<Node>(NodeKind::AnimTree, "", t.line);
            }
            if (t.text == "[") {
                next();
                expectPunct("]");
                return std::make_unique<Node>(NodeKind::EmptyArray, "", t.line);
            }
            if (t.text == "(") {
                next();
                auto first = expression();
                if (acceptPunct(",")) {
                    auto v = std::make_unique<Node>(NodeKind::Vector, "", t.line);
                    v->add(std::move(first));
                    v->add(expression());
                    expectPunct(",");
                    v->add(expression());
                    expectPunct(")");
                    return v;
                }
                expectPunct(")");
                return first;
            }
            break;
        default: break;
        }
        fail("unexpected token in expression");
    }

    std::vector<Token> t_;
    std::string name_;
    size_t p_ = 0;
};

void dump(const Node& n, std::string& out) {
    out += '(';
    out += nodeKindName(n.kind);
    if (n.isThread) out += ":thread";
    if (n.isPointerCall) out += ":ptr";
    if (!n.text.empty()) { out += ' '; out += n.text; }
    for (const auto& c : n.children) { out += ' '; dump(*c, out); }
    out += ')';
}

} // namespace

NodePtr parseScript(std::string_view source, const std::string& name) {
    Lexer lex(source, name);
    Parser p(lex.run(), name);
    return p.file();
}

std::string dumpTree(const Node& n) {
    std::string out;
    dump(n, out);
    return out;
}

} // namespace iw2::script
