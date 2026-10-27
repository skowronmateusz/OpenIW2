#include "script/vm.h"

#include "script/parser.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace iw2::script {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct CompileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

ArrayKey toKey(const Value& v) {
    ArrayKey k;
    if (v.isString()) { k.isString = true; k.s = v.asString(); }
    else if (v.isNumber()) k.i = v.asInt();
    else throw std::runtime_error("bad array index type " + v.typeName());
    return k;
}

Value keyValue(const ArrayKey& k) { return k.isString ? Value(k.s) : Value(k.i); }

// Copy-on-write access to an array value held somewhere.
ArrayPtr writableArray(Value& slot) {
    if (!slot.isDefined()) {
        auto a = std::make_shared<Array>();
        slot = Value(a);
        return a;
    }
    if (!slot.isArray()) throw std::runtime_error("cannot index " + slot.typeName());
    ArrayPtr& a = std::get<ArrayPtr>(slot.v);
    if (a.use_count() > 1) a = std::make_shared<Array>(*a);
    return a;
}

} // namespace

const Value& CallContext::arg(size_t i) const {
    static const Value undef;
    return i < args.size() ? args[i] : undef;
}

// ---------------------------------------------------------------------------------------------
// Compiler: AST -> bytecode for one file.

class Compiler {
public:
    Compiler(VM& vm, const std::string& file) : vm_(vm), file_(file) {}

    void function(const Node& fn) {
        CompiledFunction cf;
        cf.name = lower(fn.text);
        cf.file = file_;
        f_ = &cf;
        locals_.clear();
        for (const auto& p : fn.children[0]->children) slot(p->text);
        cf.paramCount = int(locals_.size());
        breaks_.clear();
        continues_.clear();
        statement(*fn.children[1]);
        emit(Op::PushUndef, 0, 0, 0);
        emit(Op::Return, 0, 0, 0);
        cf.localCount = int(locals_.size());
        const int index = vm_.files_[file_].functions.at(cf.name);
        vm_.functions_[size_t(index)] = std::move(cf);
    }

private:
    int slot(const std::string& name) {
        const std::string n = lower(name);
        auto it = locals_.find(n);
        if (it != locals_.end()) return it->second;
        const int s = int(locals_.size());
        locals_.emplace(n, s);
        return s;
    }
    size_t emit(Op op, int32_t a, int32_t b, int line, uint8_t flags = 0) {
        f_->code.push_back({op, a, b, flags, line});
        return f_->code.size() - 1;
    }
    int str(const std::string& s) {
        auto& v = f_->strings;
        auto it = std::find(v.begin(), v.end(), s);
        if (it != v.end()) return int(it - v.begin());
        v.push_back(s);
        return int(v.size() - 1);
    }
    size_t here() const { return f_->code.size(); }
    void patch(size_t at, size_t target) { f_->code[at].a = int32_t(target); }

    void statement(const Node& n) {
        switch (n.kind) {
        case NodeKind::Block:
            for (const auto& c : n.children) statement(*c);
            break;
        case NodeKind::DevBlock: // compiled only with developer_script, a bool dvar defaulting to 0 (0x42df9d; lexer 0x457820)
            break;
        case NodeKind::Empty:
        case NodeKind::Breakpoint:
            break;
        case NodeKind::ExprStmt: exprStatement(*n.children[0]); break;
        case NodeKind::If: {
            expr(*n.children[0]);
            const size_t jf = emit(Op::JumpIfFalse, 0, 0, n.line);
            statement(*n.children[1]);
            if (n.children.size() > 2) {
                const size_t j = emit(Op::Jump, 0, 0, n.line);
                patch(jf, here());
                statement(*n.children[2]);
                patch(j, here());
            } else {
                patch(jf, here());
            }
            break;
        }
        case NodeKind::While: {
            const size_t top = here();
            loopBegin();
            expr(*n.children[0]);
            const size_t jf = emit(Op::JumpIfFalse, 0, 0, n.line);
            statement(*n.children[1]);
            emit(Op::Jump, int32_t(top), 0, n.line);
            loopEnd(here(), top);
            patch(jf, here());
            break;
        }
        case NodeKind::For: {
            if (n.children[0]->kind != NodeKind::Empty) exprStatement(*n.children[0]);
            const size_t top = here();
            loopBegin();
            size_t jf = SIZE_MAX;
            if (n.children[1]->kind != NodeKind::Empty) {
                expr(*n.children[1]);
                jf = emit(Op::JumpIfFalse, 0, 0, n.line);
            }
            statement(*n.children[3]);
            const size_t cont = here();
            if (n.children[2]->kind != NodeKind::Empty) exprStatement(*n.children[2]);
            emit(Op::Jump, int32_t(top), 0, n.line);
            loopEnd(here(), cont);
            if (jf != SIZE_MAX) patch(jf, here());
            break;
        }
        case NodeKind::Switch: {
            const int tmp = slot("$switch" + std::to_string(here()));
            expr(*n.children[0]);
            emit(Op::StoreLocal, tmp, 0, n.line);
            const Node& body = *n.children[1];
            // Jump table: compare against each case in order, then default.
            std::vector<std::pair<size_t, size_t>> caseJumps; // (body child index, jump instr)
            for (size_t i = 0; i < body.children.size(); ++i) {
                const Node& c = *body.children[i];
                if (c.kind != NodeKind::Case) continue;
                emit(Op::LoadLocal, tmp, 0, c.line);
                expr(*c.children[0]);
                emit(Op::Eq, 0, 0, c.line);
                emit(Op::Not, 0, 0, c.line);
                caseJumps.push_back({i, emit(Op::JumpIfFalse, 0, 0, c.line)});
            }
            const size_t toDefault = emit(Op::Jump, 0, 0, n.line);
            breaks_.emplace_back();
            bool hasDefault = false;
            for (size_t i = 0; i < body.children.size(); ++i) {
                const Node& c = *body.children[i];
                if (c.kind == NodeKind::Case) {
                    for (auto& [idx, j] : caseJumps)
                        if (idx == i) patch(j, here());
                } else if (c.kind == NodeKind::Default) {
                    patch(toDefault, here());
                    hasDefault = true;
                } else {
                    statement(c);
                }
            }
            if (!hasDefault) patch(toDefault, here());
            for (size_t b : breaks_.back()) patch(b, here());
            breaks_.pop_back();
            break;
        }
        case NodeKind::Break:
            if (breaks_.empty()) throw CompileError("break outside loop or switch");
            breaks_.back().push_back(emit(Op::Jump, 0, 0, n.line));
            break;
        case NodeKind::Continue:
            if (continues_.empty()) throw CompileError("continue outside loop");
            continues_.back().push_back(emit(Op::Jump, 0, 0, n.line));
            break;
        case NodeKind::Return:
            if (n.children.empty()) emit(Op::PushUndef, 0, 0, n.line);
            else expr(*n.children[0]);
            emit(Op::Return, 0, 0, n.line);
            break;
        case NodeKind::Wait:
            expr(*n.children[0]);
            emit(Op::Wait, 0, 0, n.line);
            break;
        case NodeKind::WaitTillFrameEnd: emit(Op::WaitTillFrameEnd, 0, 0, n.line); break;
        default: throw CompileError(std::string("unexpected statement ") + nodeKindName(n.kind));
        }
    }

    void loopBegin() {
        breaks_.emplace_back();
        continues_.emplace_back();
    }
    void loopEnd(size_t breakTarget, size_t continueTarget) {
        for (size_t b : breaks_.back()) patch(b, breakTarget);
        for (size_t c : continues_.back()) patch(c, continueTarget);
        breaks_.pop_back();
        continues_.pop_back();
    }

    void exprStatement(const Node& n) {
        switch (n.kind) {
        case NodeKind::Assign: {
            ref(*n.children[0]);
            if (n.text == "=") {
                expr(*n.children[1]);
            } else {
                emit(Op::LoadRef, 0, 0, n.line);
                expr(*n.children[1]);
                emit(binaryOp(n.text.substr(0, n.text.size() - 1)), 0, 0, n.line);
            }
            emit(Op::Store, 0, 0, n.line);
            break;
        }
        case NodeKind::PreIncDec:
        case NodeKind::PostIncDec:
            ref(*n.children[0]);
            emit(Op::LoadRef, 0, 0, n.line);
            emit(Op::PushInt, 1, 0, n.line);
            emit(n.text == "++" ? Op::Add : Op::Sub, 0, 0, n.line);
            emit(Op::Store, 0, 0, n.line);
            break;
        case NodeKind::Call:
            call(n, true);
            break;
        default:
            expr(n);
            emit(Op::Pop, 0, 0, n.line);
        }
    }

    static Op binaryOp(const std::string& op) {
        if (op == "+") return Op::Add;
        if (op == "-") return Op::Sub;
        if (op == "*") return Op::Mul;
        if (op == "/") return Op::Div;
        if (op == "%") return Op::Mod;
        if (op == "&") return Op::BitAnd;
        if (op == "|") return Op::BitOr;
        if (op == "^") return Op::BitXor;
        if (op == "<<") return Op::Shl;
        if (op == ">>") return Op::Shr;
        if (op == "==") return Op::Eq;
        if (op == "!=") return Op::Ne;
        if (op == "<") return Op::Lt;
        if (op == ">") return Op::Gt;
        if (op == "<=") return Op::Le;
        if (op == ">=") return Op::Ge;
        throw CompileError("unknown operator " + op);
    }

    // Pushes a reference for an assignable expression onto the ref stack.
    void ref(const Node& n) {
        switch (n.kind) {
        case NodeKind::Identifier: emit(Op::RefLocal, slot(n.text), 0, n.line); break;
        case NodeKind::Member:
            expr(*n.children[0]);
            emit(Op::RefField, str(lower(n.text)), 0, n.line);
            break;
        case NodeKind::Index: {
            const NodeKind root = n.children[0]->kind;
            if (root == NodeKind::Game || root == NodeKind::Level || root == NodeKind::Anim || root == NodeKind::Self) {
                // game["key"] = v sets a field of the object.
                expr(*n.children[0]);
                expr(*n.children[1]);
                emit(Op::RefFieldDynamic, 0, 0, n.line);
                break;
            }
            ref(*n.children[0]);
            expr(*n.children[1]);
            emit(Op::RefIndex, 0, 0, n.line);
            break;
        }
        default: throw CompileError(std::string("cannot assign to ") + nodeKindName(n.kind));
        }
    }

    void expr(const Node& n) {
        switch (n.kind) {
        case NodeKind::Integer: emit(Op::PushInt, int32_t(std::stol(n.text)), 0, n.line); break;
        case NodeKind::Float:
            f_->floats.push_back(std::stof(n.text));
            emit(Op::PushFloat, int32_t(f_->floats.size() - 1), 0, n.line);
            break;
        case NodeKind::String: emit(Op::PushStr, str(n.text), 0, n.line); break;
        case NodeKind::LocalizedString: emit(Op::PushIStr, str(n.text), 0, n.line); break;
        case NodeKind::AnimRef: emit(Op::PushAnim, str(n.text), 0, n.line); break;
        case NodeKind::AnimTree: emit(Op::PushAnimTree, str(vm_.files_[file_].animtree), 0, n.line); break;
        case NodeKind::Undefined: emit(Op::PushUndef, 0, 0, n.line); break;
        case NodeKind::True: emit(Op::PushInt, 1, 0, n.line); break;
        case NodeKind::False: emit(Op::PushInt, 0, 0, n.line); break;
        case NodeKind::Self: emit(Op::PushSelf, 0, 0, n.line); break;
        case NodeKind::Level: emit(Op::PushLevel, 0, 0, n.line); break;
        case NodeKind::Game: emit(Op::PushGame, 0, 0, n.line); break;
        case NodeKind::Anim: emit(Op::PushAnimObj, 0, 0, n.line); break;
        case NodeKind::EmptyArray: emit(Op::PushEmptyArray, 0, 0, n.line); break;
        case NodeKind::Identifier: emit(Op::LoadLocal, slot(n.text), 0, n.line); break;
        case NodeKind::Vector:
            for (const auto& c : n.children) expr(*c);
            emit(Op::MakeVector, 0, 0, n.line);
            break;
        case NodeKind::Member:
            expr(*n.children[0]);
            emit(Op::LoadField, str(lower(n.text)), 0, n.line);
            break;
        case NodeKind::Size:
            expr(*n.children[0]);
            emit(Op::Size, 0, 0, n.line);
            break;
        case NodeKind::Index:
            expr(*n.children[0]);
            expr(*n.children[1]);
            emit(Op::LoadIndex, 0, 0, n.line);
            break;
        case NodeKind::FunctionRef: {
            bool builtin = false;
            const int f = vm_.resolve(file_, n.text, &builtin);
            if (f < 0 || builtin) throw CompileError("unknown function reference " + n.text);
            emit(Op::PushFunc, f, 0, n.line);
            break;
        }
        case NodeKind::Unary:
            expr(*n.children[0]);
            emit(n.text == "!" ? Op::Not : n.text == "-" ? Op::Neg : Op::BitNot, 0, 0, n.line);
            break;
        case NodeKind::Binary:
            if (n.text == "&&" || n.text == "||") {
                expr(*n.children[0]);
                if (n.text == "||") emit(Op::Not, 0, 0, n.line);
                const size_t j = emit(Op::JumpIfFalse, 0, 0, n.line);
                expr(*n.children[1]);
                emit(Op::ToBool, 0, 0, n.line);
                const size_t end = emit(Op::Jump, 0, 0, n.line);
                patch(j, here());
                emit(Op::PushInt, n.text == "||" ? 1 : 0, 0, n.line);
                patch(end, here());
            } else {
                expr(*n.children[0]);
                expr(*n.children[1]);
                emit(binaryOp(n.text), 0, 0, n.line);
            }
            break;
        case NodeKind::Call: call(n, false); break;
        default: throw CompileError(std::string("unexpected expression ") + nodeKindName(n.kind));
        }
    }

    // Emits a call. For statements the result is discarded.
    void call(const Node& n, bool discard) {
        const Node& object = *n.children[0];
        const bool method = object.kind != NodeKind::Empty;
        const size_t argBegin = n.isPointerCall || true ? 2 : 2;
        const size_t argc = n.children.size() - argBegin;
        const std::string name = n.isPointerCall ? std::string() : lower(n.children[1]->text);

        // Event primitives compile to dedicated instructions.
        if (!n.isPointerCall && method && !n.isThread &&
            (name == "notify" || name == "waittill" || name == "waittillmatch" || name == "endon")) {
            expr(object);
            if (name == "waittill") {
                if (argc < 1) throw CompileError("waittill needs an event");
                expr(*n.children[argBegin]);
                const int vars = int(argc - 1);
                emit(Op::WaitTill, vars, 0, n.line);
                // Received values are pushed in order; store them back to front.
                for (int i = vars - 1; i >= 0; --i) {
                    const Node& v = *n.children[argBegin + 1 + size_t(i)];
                    if (v.kind != NodeKind::Identifier) throw CompileError("waittill arguments must be variables");
                    emit(Op::StoreLocal, slot(v.text), 0, n.line);
                }
            } else if (name == "waittillmatch") {
                if (argc != 2) throw CompileError("waittillmatch needs an event and a match");
                expr(*n.children[argBegin]);
                expr(*n.children[argBegin + 1]);
                emit(Op::WaitTillMatch, 0, 0, n.line);
            } else if (name == "endon") {
                if (argc != 1) throw CompileError("endon needs one event");
                expr(*n.children[argBegin]);
                emit(Op::EndOn, 0, 0, n.line);
            } else {
                for (size_t i = argBegin; i < n.children.size(); ++i) expr(*n.children[i]);
                emit(Op::Notify, int32_t(argc), 0, n.line);
            }
            if (!discard) emit(Op::PushUndef, 0, 0, n.line);
            return;
        }

        uint8_t flags = 0;
        if (method) flags |= CallFlags::Method;
        if (n.isThread) flags |= CallFlags::Thread;
        if (method) expr(object);
        for (size_t i = argBegin; i < n.children.size(); ++i) expr(*n.children[i]);
        if (n.isPointerCall) {
            expr(*n.children[1]);
            emit(Op::CallPointer, int32_t(argc), 0, n.line, flags);
        } else {
            bool builtin = false;
            const int f = vm_.resolve(file_, n.children[1]->text, &builtin);
            if (f < 0) emit(Op::CallUnresolved, str(n.children[1]->text), int32_t(argc), n.line, flags);
            else emit(builtin ? Op::CallBuiltin : Op::CallScript, f, int32_t(argc), n.line, flags);
        }
        if (discard) emit(Op::Pop, 0, 0, n.line);
    }

    VM& vm_;
    std::string file_;
    CompiledFunction* f_ = nullptr;
    std::unordered_map<std::string, int> locals_;
    std::vector<std::vector<size_t>> breaks_, continues_;
};

// ---------------------------------------------------------------------------------------------
// VM

VM::VM(SourceLoader loader) : loader_(std::move(loader)) {
    level_ = newObject("level");
    game_ = newObject("game");
    anim_ = newObject("anim");
    // isdefined (0x4ebf20): an object reference is undefined once the object is freed (object types 0x14 and ≥ 0x17,
    // e.g. a deleted entity).
    registerBuiltin("isdefined", [](CallContext& c) {
        if (c.arg(0).isObject()) return Value(!c.arg(0).asObject()->removed);
        return Value(c.arg(0).isDefined());
    });
    registerBuiltin("isstring", [](CallContext& c) { return Value(c.arg(0).isString()); });
    registerBuiltin("spawnstruct", [](CallContext& c) { return Value(c.vm.newObject("struct")); });
    registerBuiltin("int", [](CallContext& c) {
        const Value& v = c.arg(0);
        if (v.isString()) return Value(int32_t(std::atoi(v.asString().c_str())));
        return Value(v.asInt());
    });
    registerBuiltin("float", [](CallContext& c) {
        const Value& v = c.arg(0);
        if (v.isString()) return Value(float(std::atof(v.asString().c_str())));
        return Value(v.asFloat());
    });
    registerBuiltin("println", [](CallContext& c) {
        std::string s;
        for (const auto& a : c.args) s += a.toString();
        c.vm.print(s);
        return Value();
    });
    registerBuiltin("iprintln", [](CallContext& c) { c.vm.print(c.arg(0).toString()); return Value(); });
    registerBuiltin("assert", [](CallContext& c) {
        if (!c.arg(0).truthy()) throw std::runtime_error("assert failed");
        return Value();
    });
    registerBuiltin("assertex", [](CallContext& c) {
        if (!c.arg(0).truthy()) throw std::runtime_error("assert failed: " + c.arg(1).toString());
        return Value();
    });
    registerBuiltin("assertmsg", [](CallContext& c) -> Value { throw std::runtime_error("assert: " + c.arg(0).toString()); });
    registerBuiltin("gettime", [](CallContext& c) { return Value(int32_t(c.vm.time())); });
    registerBuiltin("getarraykeys", [](CallContext& c) {
        auto out = std::make_shared<Array>();
        if (c.arg(0).isArray()) {
            const auto& a = std::get<ArrayPtr>(c.arg(0).v);
            int32_t i = 0;
            for (const auto& [k, v] : a->items) {
                ArrayKey key;
                key.i = i++;
                out->set(key, keyValue(k));
            }
        }
        return Value(out);
    });
}

ObjectPtr VM::newObject(std::string kind) {
    auto o = std::make_shared<Object>();
    o->kind = std::move(kind);
    o->id = nextObjectId_++;
    return o;
}

void VM::registerBuiltin(const std::string& name, Builtin fn) {
    const std::string n = lower(name);
    auto it = builtinIndex_.find(n);
    if (it != builtinIndex_.end()) {
        builtins_[size_t(it->second)] = std::move(fn);
        return;
    }
    builtinIndex_[n] = int(builtins_.size());
    builtins_.push_back(std::move(fn));
    builtinNames_.push_back(n);
}

bool VM::hasBuiltin(const std::string& name) const { return builtinIndex_.count(lower(name)) != 0; }

std::string VM::normalizePath(const std::string& path) {
    std::string p = lower(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.size() < 4 || p.substr(p.size() - 4) != ".gsc") p += ".gsc";
    return p;
}

std::optional<int> VM::findFunction(const std::string& file, const std::string& name) const {
    auto it = files_.find(normalizePath(file));
    if (it == files_.end()) return std::nullopt;
    auto f = it->second.functions.find(lower(name));
    if (f == it->second.functions.end()) return std::nullopt;
    return f->second;
}

void VM::loadFile(const std::string& path) {
    const std::string key = normalizePath(path);
    if (files_.count(key)) return;
    const auto src = loader_(key);
    if (!src) throw std::runtime_error("script not found: " + key);
    const NodePtr tree = parseScript(*src, key);

    File& file = files_[key];
    file.path = key;
    // Declare functions first so calls within the file resolve.
    for (const auto& c : tree->children) {
        if (c->kind == NodeKind::Function) {
            const std::string fname = lower(c->text);
            if (!file.functions.count(fname)) {
                file.functions[fname] = int(functions_.size());
                functions_.emplace_back();
                functions_.back().name = fname;
                functions_.back().file = key;
            }
        } else if (c->kind == NodeKind::Include) {
            file.includes.push_back(normalizePath(c->text));
        } else if (c->kind == NodeKind::UsingAnimtree) {
            file.animtree = c->text;
        }
    }
    for (const auto& inc : file.includes) loadFile(inc);

    // #animtree means the tree of the closest preceding #using_animtree, so a file may switch trees between
    // functions (maps/_truck.gsc does).
    files_[key].animtree.clear();
    for (const auto& c : tree->children) {
        if (c->kind == NodeKind::UsingAnimtree) files_[key].animtree = c->text;
        if (c->kind != NodeKind::Function) continue;
        Compiler comp(*this, key);
        try {
            comp.function(*c);
        } catch (const std::exception& e) {
            throw std::runtime_error(key + ":" + std::to_string(c->line) + ": in " + c->text + ": " + e.what());
        }
    }
}

int VM::resolve(const std::string& fromFile, const std::string& name, bool* isBuiltin) {
    *isBuiltin = false;
    const std::string n = lower(name);
    const auto sep = n.find("::");
    if (sep != std::string::npos) {
        std::string path = n.substr(0, sep);
        const std::string fn = n.substr(sep + 2);
        if (path.empty()) {
            // "::f" names a script function visible from this file (its own or an included file's).
            bool b = false;
            const int r = resolve(fromFile, fn, &b);
            return b ? -1 : r;
        }
        const std::string key = normalizePath(path);
        if (!files_.count(key)) loadFile(key);
        auto& funcs = files_.at(key).functions;
        auto it = funcs.find(fn);
        return it == funcs.end() ? -1 : it->second;
    }
    const File& f = files_.at(fromFile);
    if (auto it = f.functions.find(n); it != f.functions.end()) return it->second;
    for (const auto& inc : f.includes) {
        const File& g = files_.at(inc);
        if (auto it = g.functions.find(n); it != g.functions.end()) return it->second;
    }
    if (auto it = builtinIndex_.find(n); it != builtinIndex_.end()) {
        *isBuiltin = true;
        return it->second;
    }
    return -1;
}

Thread& VM::newThread() {
    auto t = std::make_unique<Thread>();
    t->id = nextThreadId_++;
    threads_.push_back(std::move(t));
    return *threads_.back();
}

void VM::enterFunction(Thread& t, int function, std::vector<Value> args, Value self) {
    const CompiledFunction& f = functions_[size_t(function)];
    if (f.code.empty()) throw std::runtime_error("function " + f.name + " has no body");
    Frame fr;
    fr.function = function;
    fr.locals.resize(size_t(std::max(f.localCount, int(args.size()))));
    for (size_t i = 0; i < args.size() && i < size_t(f.paramCount); ++i) fr.locals[i] = std::move(args[i]);
    fr.self = std::move(self);
    fr.stackBase = t.stack.size();
    fr.refBase = t.refs.size();
    t.frames.push_back(std::move(fr));
}

uint64_t VM::start(const std::string& file, const std::string& function, std::vector<Value> args, Value self) {
    loadFile(file);
    const auto f = findFunction(file, function);
    if (!f) throw std::runtime_error("no function " + function + " in " + file);
    Thread& t = newThread();
    const uint64_t id = t.id;
    enterFunction(t, *f, std::move(args), std::move(self));
    run(t);
    return id;
}

uint64_t VM::startFunction(const FunctionRef& function, std::vector<Value> args, Value self) {
    if (function.function < 0 || size_t(function.function) >= functions_.size()) throw std::runtime_error("bad function reference");
    Thread& t = newThread();
    const uint64_t id = t.id;
    enterFunction(t, function.function, std::move(args), std::move(self));
    run(t);
    return id;
}

std::string VM::backtrace(const Thread& t) const {
    std::string out;
    for (const Frame& fr : t.frames) {
        const CompiledFunction& f = functions_[size_t(fr.function)];
        out += " < " + f.file + ":" + std::to_string(fr.pc > 0 && fr.pc <= f.code.size() ? f.code[fr.pc - 1].line : 0) + " " + f.name;
    }
    return out;
}

void VM::fail(Thread& t, const std::string& msg) {
    std::string where;
    if (!t.frames.empty()) {
        const Frame& fr = t.frames.back();
        const CompiledFunction& f = functions_[size_t(fr.function)];
        const int line = fr.pc > 0 && fr.pc <= f.code.size() ? f.code[fr.pc - 1].line : 0;
        where = f.file + ":" + std::to_string(line) + " (" + f.name + "): ";
    }
    errors_.push_back(where + msg);
    print("script runtime error: " + where + msg);
    if (std::getenv("IW2_SCRIPT_BACKTRACE")) print("  backtrace:" + backtrace(t)); // debug
    t.state = ThreadState::Done;
}

void VM::run(Thread& t) {
    if (t.state == ThreadState::Done) return;
    t.state = ThreadState::Ready;
    ++runDepth_;
    if (runDepth_ > 64) {
        --runDepth_;
        fail(t, "script call depth exceeded");
        return;
    }
    bool suspended = false;
    const bool wasRunning = t.running;
    t.running = true;
    while (!suspended && t.state != ThreadState::Done) {
        try {
            step(t, suspended);
        } catch (const std::exception& e) {
            fail(t, e.what());
        }
        if (t.pendingUnwind >= 0 && t.state != ThreadState::Done) {
            unwind(t, size_t(t.pendingUnwind));
            suspended = false;
        }
        t.pendingUnwind = -1;
    }
    t.running = wasRunning;
    --runDepth_;
}

Value VM::readRef(Thread& t, int refIndex) {
    Ref& r = t.refs[size_t(refIndex)];
    switch (r.kind) {
    case Ref::Local: return t.frames.back().locals[size_t(r.slot)];
    case Ref::Field: return r.object->get(r.field);
    case Ref::Index: {
        const Value parent = readRef(t, r.parent);
        if (parent.isObject()) return parent.asObject()->get(lower(r.key.toString()));
        if (!parent.isArray()) return Value();
        const Value* v = std::get<ArrayPtr>(parent.v)->get(toKey(r.key));
        return v ? *v : Value();
    }
    }
    return Value();
}

void VM::writeRef(Thread& t, int refIndex, Value v) {
    Ref& r = t.refs[size_t(refIndex)];
    switch (r.kind) {
    case Ref::Local: t.frames.back().locals[size_t(r.slot)] = std::move(v); return;
    case Ref::Field: r.object->set(r.field, std::move(v)); return;
    case Ref::Index: {
        Value parent = readRef(t, r.parent);
        if (parent.isObject()) {
            parent.asObject()->set(lower(r.key.toString()), std::move(v));
            return;
        }
        ArrayPtr arr = writableArray(parent);
        arr->set(toKey(r.key), std::move(v));
        writeRef(t, r.parent, std::move(parent));
        return;
    }
    }
}

Value VM::binary(Op op, const Value& a, const Value& b) {
    auto bothInt = [&] { return a.isInt() && b.isInt(); };
    switch (op) {
    case Op::Add:
        if (a.isString() || b.isString()) return Value(a.toString() + b.toString());
        if (a.isVector() && b.isVector()) {
            const Vec3 x = a.asVector(), y = b.asVector();
            return Value(Vec3{x[0] + y[0], x[1] + y[1], x[2] + y[2]});
        }
        if (bothInt()) return Value(a.asInt() + b.asInt());
        return Value(a.asFloat() + b.asFloat());
    case Op::Sub:
        if (a.isVector() && b.isVector()) {
            const Vec3 x = a.asVector(), y = b.asVector();
            return Value(Vec3{x[0] - y[0], x[1] - y[1], x[2] - y[2]});
        }
        if (bothInt()) return Value(a.asInt() - b.asInt());
        return Value(a.asFloat() - b.asFloat());
    case Op::Mul:
        if (a.isVector() && b.isNumber()) { auto x = a.asVector(); float s = b.asFloat(); return Value(Vec3{x[0] * s, x[1] * s, x[2] * s}); }
        if (b.isVector() && a.isNumber()) { auto x = b.asVector(); float s = a.asFloat(); return Value(Vec3{x[0] * s, x[1] * s, x[2] * s}); }
        if (a.isVector() && b.isVector()) { auto x = a.asVector(), y = b.asVector(); return Value(Vec3{x[0] * y[0], x[1] * y[1], x[2] * y[2]}); }
        if (bothInt()) return Value(a.asInt() * b.asInt());
        return Value(a.asFloat() * b.asFloat());
    case Op::Div: {
        if (a.isVector() && b.isNumber()) { auto x = a.asVector(); float s = b.asFloat(); return Value(Vec3{x[0] / s, x[1] / s, x[2] / s}); }
        const float d = b.asFloat();
        if (d == 0.0f) throw std::runtime_error("divide by zero");
        return Value(a.asFloat() / d);
    }
    case Op::Mod: {
        const int32_t d = b.asInt();
        if (d == 0) throw std::runtime_error("divide by zero");
        return Value(a.asInt() % d);
    }
    case Op::BitAnd: return Value(a.asInt() & b.asInt());
    case Op::BitOr: return Value(a.asInt() | b.asInt());
    case Op::BitXor: return Value(a.asInt() ^ b.asInt());
    case Op::Shl: return Value(a.asInt() << b.asInt());
    case Op::Shr: return Value(a.asInt() >> b.asInt());
    case Op::Eq: return Value(a == b);
    case Op::Ne: return Value(!(a == b));
    case Op::Lt: return Value(a.asFloat() < b.asFloat());
    case Op::Gt: return Value(a.asFloat() > b.asFloat());
    case Op::Le: return Value(a.asFloat() <= b.asFloat());
    case Op::Ge: return Value(a.asFloat() >= b.asFloat());
    default: throw std::runtime_error("bad binary op");
    }
}

void VM::step(Thread& t, bool& suspended) {
    Frame& fr = t.frames.back();
    const CompiledFunction& f = functions_[size_t(fr.function)];
    const Instr& in = f.code[fr.pc++];
    auto pop = [&]() {
        if (t.stack.size() <= fr.stackBase) throw std::runtime_error("stack underflow");
        Value v = std::move(t.stack.back());
        t.stack.pop_back();
        return v;
    };
    auto push = [&](Value v) { t.stack.push_back(std::move(v)); };

    switch (in.op) {
    case Op::Nop: break;
    case Op::PushUndef: push(Value()); break;
    case Op::PushInt: push(Value(int32_t(in.a))); break;
    case Op::PushFloat: push(Value(f.floats[size_t(in.a)])); break;
    case Op::PushStr: push(Value(f.strings[size_t(in.a)])); break;
    case Op::PushIStr: push(Value(LocalizedString{f.strings[size_t(in.a)]})); break;
    case Op::PushAnim: push(Value(AnimRef{f.strings[size_t(in.a)]})); break;
    case Op::PushAnimTree: push(Value(AnimTreeRef{f.strings[size_t(in.a)]})); break;
    case Op::PushFunc: push(Value(FunctionRef{in.a})); break;
    case Op::PushSelf: push(fr.self); break;
    case Op::PushLevel: push(Value(level_)); break;
    case Op::PushGame: push(Value(game_)); break;
    case Op::PushAnimObj: push(Value(anim_)); break;
    case Op::PushEmptyArray: push(Value(std::make_shared<Array>())); break;
    case Op::MakeVector: {
        const float z = pop().asFloat(), y = pop().asFloat(), x = pop().asFloat();
        push(Value(Vec3{x, y, z}));
        break;
    }
    case Op::LoadLocal: push(fr.locals[size_t(in.a)]); break;
    case Op::StoreLocal: fr.locals[size_t(in.a)] = pop(); break;
    case Op::LoadField: {
        const Value obj = pop();
        if (obj.isObject()) push(obj.asObject()->get(f.strings[size_t(in.a)]));
        else if (obj.isVector() && f.strings[size_t(in.a)].size() == 1) {
            // Not standard GSC, kept out: vectors are indexed with [0..2].
            throw std::runtime_error("field " + f.strings[size_t(in.a)] + " of a vector");
        } else if (!obj.isDefined()) throw std::runtime_error("field " + f.strings[size_t(in.a)] + " of undefined");
        else throw std::runtime_error("field " + f.strings[size_t(in.a)] + " of " + obj.typeName());
        break;
    }
    case Op::LoadIndex: {
        const Value key = pop();
        const Value c = pop();
        if (c.isArray()) {
            const Value* v = std::get<ArrayPtr>(c.v)->get(toKey(key));
            push(v ? *v : Value());
        } else if (c.isVector()) {
            const int i = key.asInt();
            if (i < 0 || i > 2) throw std::runtime_error("vector index out of range");
            push(Value(c.asVector()[size_t(i)]));
        } else if (c.isString()) {
            const int i = key.asInt();
            const std::string& s = c.asString();
            if (i < 0 || size_t(i) >= s.size()) throw std::runtime_error("string index out of range");
            push(Value(std::string(1, s[size_t(i)])));
        } else if (c.isObject()) {
            push(c.asObject()->get(lower(key.toString())));
        } else if (!c.isDefined()) {
            throw std::runtime_error("index of undefined");
        } else {
            throw std::runtime_error("cannot index " + c.typeName());
        }
        break;
    }
    case Op::Size: {
        const Value c = pop();
        if (c.isArray()) push(Value(int32_t(std::get<ArrayPtr>(c.v)->size())));
        else if (c.isString()) push(Value(int32_t(c.asString().size())));
        else if (!c.isDefined()) throw std::runtime_error("size of undefined");
        else throw std::runtime_error("size of " + c.typeName());
        break;
    }
    case Op::RefLocal: {
        Ref r;
        r.kind = Ref::Local;
        r.slot = in.a;
        t.refs.push_back(std::move(r));
        break;
    }
    case Op::RefField: {
        const Value obj = pop();
        if (!obj.isObject()) throw std::runtime_error("cannot set field " + f.strings[size_t(in.a)] + " on " + obj.typeName());
        Ref r;
        r.kind = Ref::Field;
        r.object = obj.asObject();
        r.field = f.strings[size_t(in.a)];
        t.refs.push_back(std::move(r));
        break;
    }
    case Op::RefFieldDynamic: {
        const Value key = pop();
        const Value obj = pop();
        if (!obj.isObject()) throw std::runtime_error("cannot index " + obj.typeName());
        Ref r;
        r.kind = Ref::Field;
        r.object = obj.asObject();
        r.field = lower(key.toString());
        t.refs.push_back(std::move(r));
        break;
    }
    case Op::RefIndex: {
        Ref r;
        r.kind = Ref::Index;
        r.key = pop();
        r.parent = int(t.refs.size()) - 1;
        t.refs.push_back(std::move(r));
        break;
    }
    case Op::LoadRef: push(readRef(t, int(t.refs.size()) - 1)); break;
    case Op::Store: {
        Value v = pop();
        writeRef(t, int(t.refs.size()) - 1, std::move(v));
        // Pop the whole reference chain.
        int i = int(t.refs.size()) - 1;
        while (i >= 0 && t.refs[size_t(i)].kind == Ref::Index) i = t.refs[size_t(i)].parent;
        t.refs.resize(size_t(std::max(i, 0)));
        break;
    }
    case Op::Pop: pop(); break;
    case Op::Dup: push(t.stack.back()); break;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Shl: case Op::Shr: case Op::Eq: case Op::Ne: case Op::Lt: case Op::Gt: case Op::Le:
    case Op::Ge: {
        const Value b = pop();
        const Value a = pop();
        push(binary(in.op, a, b));
        break;
    }
    case Op::Not: push(Value(!pop().truthy())); break;
    case Op::ToBool: push(Value(pop().truthy())); break;
    case Op::Neg: {
        const Value a = pop();
        if (a.isVector()) { auto x = a.asVector(); push(Value(Vec3{-x[0], -x[1], -x[2]})); }
        else if (a.isInt()) push(Value(-a.asInt()));
        else push(Value(-a.asFloat()));
        break;
    }
    case Op::BitNot: push(Value(~pop().asInt())); break;
    case Op::Jump: fr.pc = size_t(in.a); break;
    case Op::JumpIfFalse:
        if (!pop().truthy()) fr.pc = size_t(in.a);
        break;
    case Op::CallScript:
    case Op::CallBuiltin:
    case Op::CallPointer:
    case Op::CallUnresolved: {
        int target = in.a;
        bool builtin = in.op == Op::CallBuiltin;
        size_t argc = size_t(in.op == Op::CallPointer ? in.a : in.b);
        if (in.op == Op::CallPointer) {
            const Value fp = pop();
            if (!fp.isFunction()) throw std::runtime_error("call through " + fp.typeName() + ", not a function");
            target = std::get<FunctionRef>(fp.v).function;
        }
        if (in.op == Op::CallUnresolved) throw std::runtime_error("unknown function " + f.strings[size_t(in.a)]);
        std::vector<Value> args(argc);
        for (size_t i = argc; i-- > 0;) args[i] = pop();
        // A call without an object keeps the caller's self (inferred: character scripts are called as
        // character\x::main() from aitype main() and set fields on self).
        Value self = fr.self;
        if (in.flags & CallFlags::Method) self = pop();
        if (builtin) {
            if (!(in.flags & CallFlags::Method)) self = Value();
            if (traceSelf && self.isObject() && self.asObject() == traceSelf) {
                std::string line = "[trace " + std::to_string(nowMs_) + "] " + builtinNames_[size_t(target)] + "(";
                for (size_t i = 0; i < args.size(); ++i) line += (i ? ", " : "") + args[i].toString();
                print(line + ") at " + f.file + ":" + std::to_string(in.line));
            }
            CallContext ctx{*this, t, self, args};
            push(builtins_[size_t(target)](ctx));
        } else if (in.flags & CallFlags::Thread) {
            if (traceSelf && self.isObject() && self.asObject() == traceSelf)
                print("[trace " + std::to_string(nowMs_) + "] thread " + functions_[size_t(target)].name);
            Thread& nt = newThread();
            enterFunction(nt, target, std::move(args), std::move(self));
            run(nt);
            // A thread that ends before it first suspends gives the call its return value (0x4690c0: the new
            // thread's end/return writes the slot above the caller's stack top).
            push(nt.state == ThreadState::Done ? nt.result : Value());
        } else {
            if (traceSelf && self.isObject() && self.asObject() == traceSelf)
                print("[trace " + std::to_string(nowMs_) + "] call " + functions_[size_t(target)].file + "::" + functions_[size_t(target)].name);
            enterFunction(t, target, std::move(args), std::move(self));
        }
        break;
    }
    case Op::Return: {
        Value v = pop();
        if (traceSelf && fr.self.isObject() && fr.self.asObject() == traceSelf)
            print("[trace " + std::to_string(nowMs_) + "] return " + f.name + " -> " + v.toString());
        t.stack.resize(fr.stackBase);
        t.frames.pop_back();
        // The returning call's endons end with it (*inferred*, docs/spec/gsc-vm.md).
        std::erase_if(t.endons, [&](const Thread::EndOn& e) { return e.frame >= t.frames.size(); });
        if (t.frames.empty()) {
            t.result = std::move(v);
            t.state = ThreadState::Done;
            suspended = true;
        } else {
            push(std::move(v));
        }
        break;
    }
    case Op::Wait: {
        const float s = pop().asFloat();
        if (s < 0) throw std::runtime_error("negative wait");
        t.state = ThreadState::Waiting;
        t.wakeTimeMs = nowMs_ + std::max<int64_t>(1, int64_t(std::lround(s * 1000.0f)));
        suspended = true;
        break;
    }
    case Op::WaitTillFrameEnd:
        t.state = ThreadState::WaitingFrameEnd;
        suspended = true;
        break;
    case Op::WaitTill:
    case Op::WaitTillMatch: {
        std::optional<std::string> match;
        if (in.op == Op::WaitTillMatch) match = pop().toString();
        const Value ev = pop();
        const Value obj = pop();
        if (!obj.isObject()) throw std::runtime_error("waittill on " + obj.typeName());
        t.state = ThreadState::WaitingNotify;
        t.waitObject = obj.asObject();
        t.waitEvent = ev.toString();
        t.waitMatch = match;
        t.waitVarCount = in.op == Op::WaitTill ? in.a : 0;
        suspended = true;
        break;
    }
    case Op::EndOn: {
        const Value ev = pop();
        const Value obj = pop();
        if (!obj.isObject()) throw std::runtime_error("endon on " + obj.typeName());
        t.endons.push_back({obj.asObject(), ev.toString(), t.frames.size() - 1});
        break;
    }
    case Op::Notify: {
        std::vector<Value> args(size_t(in.a > 0 ? in.a - 1 : 0));
        for (size_t i = args.size(); i-- > 0;) args[i] = pop();
        const Value ev = pop();
        const Value obj = pop();
        if (!obj.isObject()) throw std::runtime_error("notify on " + obj.typeName());
        notify(obj.asObject(), ev.toString(), std::move(args));
        break;
    }
    }
}

void VM::finishNotifyWait(Thread& t, const std::vector<Value>& args) {
    for (int i = 0; i < t.waitVarCount; ++i) t.stack.push_back(size_t(i) < args.size() ? args[size_t(i)] : Value());
    t.waitObject.reset();
    t.waitMatch.reset();
    t.state = ThreadState::Ready;
}

// Ends call `frame` and the calls it made; the caller continues with undefined as the call's result.
void VM::unwind(Thread& t, size_t frame) {
    t.stack.resize(t.frames[frame].stackBase);
    // Drop the references the ended calls were building (e.g. `x = f();` inside them); left behind, the caller's
    // next Store would write through them into its own, smaller frame.
    t.refs.resize(t.frames[frame].refBase);
    t.frames.resize(frame);
    std::erase_if(t.endons, [&](const Thread::EndOn& e) { return e.frame >= frame; });
    t.waitObject.reset();
    t.waitMatch.reset();
    t.stack.push_back(Value());
    t.state = ThreadState::Ready;
}

void VM::notify(const ObjectPtr& object, const std::string& event, std::vector<Value> args) {
    // endon: end threads first (inferred ordering). An endon registered by a called function ends that call (and
    // the calls it made); the caller continues as if it returned undefined (*inferred*, docs/spec/gsc-vm.md).
    std::vector<Thread*> resumed;
    for (size_t i = 0; i < threads_.size(); ++i) {
        Thread& th = *threads_[i];
        if (th.state == ThreadState::Done) continue;
        size_t frame = SIZE_MAX;
        for (const auto& e : th.endons)
            if (e.event == event && e.frame < frame && e.object.lock() == object) frame = e.frame;
        if (frame == SIZE_MAX) continue;
        if (frame == 0 || frame >= th.frames.size()) {
            th.state = ThreadState::Done;
        } else if (th.running) {
            if (th.pendingUnwind < 0 || int(frame) < th.pendingUnwind) th.pendingUnwind = int(frame);
        } else {
            unwind(th, frame);
            resumed.push_back(&th);
        }
    }
    for (Thread* th : resumed) run(*th);
    // Wake waiters registered before this notify; run them in registration order.
    std::vector<Thread*> woken;
    for (auto& th : threads_) {
        if (th->state != ThreadState::WaitingNotify || th->waitObject != object || th->waitEvent != event) continue;
        if (th->waitMatch && (args.empty() || args[0].toString() != *th->waitMatch)) continue;
        finishNotifyWait(*th, args);
        woken.push_back(th.get());
    }
    for (Thread* th : woken) ready_.push_back(th);
}

void VM::drainReady() {
    if (draining_) return; // a nested call: the outer drain picks up what was queued
    draining_ = true;
    while (!ready_.empty()) {
        Thread* th = ready_.front();
        ready_.pop_front();
        if (th->state == ThreadState::Ready && !th->running) run(*th);
    }
    draining_ = false;
}

void VM::runFrame(int64_t nowMs) {
    nowMs_ = nowMs;
    // Threads due this frame, in creation order.
    std::vector<Thread*> due;
    for (auto& th : threads_)
        if (th->state == ThreadState::Waiting && th->wakeTimeMs <= nowMs_) due.push_back(th.get());
    for (Thread* th : due)
        if (th->state == ThreadState::Waiting) run(*th);
    drainReady(); // the notify-woken threads follow the timed ones in the current frame's list (0x46d560)
    // waittillframeend threads run after everything else this frame.
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<Thread*> fe;
        for (auto& th : threads_)
            if (th->state == ThreadState::WaitingFrameEnd) fe.push_back(th.get());
        if (fe.empty()) break;
        for (Thread* th : fe) run(*th);
        drainReady(); // TODO(spec): where 0x467150 runs what the frame-end threads notify
    }
    // Drop finished threads.
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(), [](const auto& th) { return th->state == ThreadState::Done; }),
                   threads_.end());
}

std::vector<std::string> VM::describeThreads() const {
    std::vector<std::string> out;
    for (const auto& th : threads_) {
        if (th->state == ThreadState::Done || th->frames.empty()) continue;
        const Frame& fr = th->frames.back();
        const CompiledFunction& f = functions_[size_t(fr.function)];
        const int line = fr.pc > 0 && fr.pc <= f.code.size() ? f.code[fr.pc - 1].line : 0;
        std::string st = th->state == ThreadState::Waiting ? "wait" : th->state == ThreadState::WaitingNotify ? "waittill " + th->waitEvent
                         : th->state == ThreadState::WaitingFrameEnd ? "frameend" : "ready";
        out.push_back(f.file + ":" + std::to_string(line) + " " + f.name + " (" + st + ")");
    }
    return out;
}

bool VM::isThreadAlive(uint64_t id) const {
    for (const auto& th : threads_)
        if (th->id == id) return th->state != ThreadState::Done;
    return false;
}

size_t VM::liveThreadCount() const {
    size_t n = 0;
    for (const auto& th : threads_) n += th->state != ThreadState::Done;
    return n;
}

} // namespace iw2::script
