// GSC compiler and virtual machine with cooperative threads (docs/spec/gsc-vm.md).
#pragma once

#include "script/ast.h"
#include "script/value.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::script {

class VM;
struct Thread;

struct CallContext {
    VM& vm;
    Thread& thread;
    Value self;               // undefined for plain calls
    std::span<Value> args;    // in source order

    const Value& arg(size_t i) const;
    size_t argc() const { return args.size(); }
};

using Builtin = std::function<Value(CallContext&)>;

enum class Op : uint8_t {
    PushUndef, PushInt, PushFloat, PushStr, PushIStr, PushAnim, PushAnimTree, PushFunc, PushSelf, PushLevel,
    PushGame, PushAnimObj, PushEmptyArray, MakeVector, LoadLocal, StoreLocal, LoadField, LoadIndex, Size,
    RefLocal, RefField, RefFieldDynamic, RefIndex, Store, LoadRef, Pop, Dup,
    Add, Sub, Mul, Div, Mod, BitAnd, BitOr, BitXor, Shl, Shr, Eq, Ne, Lt, Gt, Le, Ge, Not, Neg, BitNot, ToBool,
    Jump, JumpIfFalse, CallScript, CallBuiltin, CallPointer, CallUnresolved, Return, Wait, WaitTillFrameEnd,
    Notify, WaitTill, WaitTillMatch, EndOn, Nop
};

struct Instr {
    Op op;
    int32_t a = 0; // operand (slot, constant index, jump target, function index, argument count)
    int32_t b = 0; // second operand (argument count for calls)
    uint8_t flags = 0;
    int32_t line = 0;
};

namespace CallFlags {
constexpr uint8_t Method = 1, Thread = 2;
}

struct CompiledFunction {
    std::string name;
    std::string file;
    int paramCount = 0;
    int localCount = 0;
    std::vector<Instr> code;
    std::vector<std::string> strings;
    std::vector<float> floats;
};

struct Frame {
    int function = -1;
    size_t pc = 0;
    std::vector<Value> locals;
    Value self;
    size_t stackBase = 0;
};

struct Ref {
    enum Kind { Local, Field, Index } kind = Local;
    int slot = 0;
    ObjectPtr object;
    std::string field;
    int parent = -1; // index in the thread's ref stack for Index refs
    Value key;
};

enum class ThreadState { Ready, Waiting, WaitingNotify, WaitingFrameEnd, Done };

struct Thread {
    uint64_t id = 0;
    ThreadState state = ThreadState::Ready;
    std::vector<Frame> frames;
    std::vector<Value> stack;
    std::vector<Ref> refs;
    int64_t wakeTimeMs = 0;
    // waittill / waittillmatch
    ObjectPtr waitObject;
    std::string waitEvent;
    std::optional<std::string> waitMatch;
    int waitVarCount = 0;
    std::vector<Value> received;
    // endon registrations
    struct EndOn { std::weak_ptr<Object> object; std::string event; size_t frame; };
    std::vector<EndOn> endons;  // frame: index of the function call that registered it
    bool running = false;
    int pendingUnwind = -1;     // an endon of frame k fired while this thread was running
};

class VM {
public:
    using SourceLoader = std::function<std::optional<std::string>(const std::string& path)>;

    explicit VM(SourceLoader loader);

    // Loads (or returns the already loaded) script file, e.g. "maps\\_utility" or "maps/_utility.gsc".
    // Throws on parse errors.
    void loadFile(const std::string& path);

    void registerBuiltin(const std::string& name, Builtin fn);
    bool hasBuiltin(const std::string& name) const;

    // Starts a thread running file::function with arguments; it runs until it first suspends.
    uint64_t start(const std::string& file, const std::string& function, std::vector<Value> args = {}, Value self = {});
    // Starts a thread running a function value (e.g. the argument of animcustom).
    uint64_t startFunction(const FunctionRef& function, std::vector<Value> args = {}, Value self = {});

    // Advances script time and runs every thread that is due. Call once per server frame.
    void runFrame(int64_t nowMs);
    int64_t time() const { return nowMs_; }

    void notify(const ObjectPtr& object, const std::string& event, std::vector<Value> args = {});

    ObjectPtr level() const { return level_; }
    ObjectPtr game() const { return game_; }
    ObjectPtr anim() const { return anim_; }
    ObjectPtr newObject(std::string kind);

    size_t liveThreadCount() const;
    bool isThreadAlive(uint64_t id) const;
    // Debug: "file:line function (state event)" for each live thread's innermost frame.
    std::vector<std::string> describeThreads() const;
    const std::vector<std::string>& errors() const { return errors_; }
    std::function<void(const std::string&)> print = [](const std::string&) {};
    ObjectPtr traceSelf; // debug: log built-in calls made with this self

    // Calls a function value synchronously (no suspension allowed); used by builtins.
    const CompiledFunction* function(int index) const { return &functions_[size_t(index)]; }
    std::optional<int> findFunction(const std::string& file, const std::string& name) const;

private:
    struct File {
        std::string path;
        std::unordered_map<std::string, int> functions;
        std::vector<std::string> includes;
        std::string animtree;
    };

    friend class Compiler;

    static std::string normalizePath(const std::string& path);
    int resolve(const std::string& fromFile, const std::string& name, bool* isBuiltin);

    Thread& newThread();
    void enterFunction(Thread& t, int function, std::vector<Value> args, Value self);
    void run(Thread& t);
    void step(Thread& t, bool& suspended);
    void fail(Thread& t, const std::string& msg);
    void finishNotifyWait(Thread& t, const std::vector<Value>& args);
    void unwind(Thread& t, size_t frame);
    Value readRef(Thread& t, int refIndex);
    void writeRef(Thread& t, int refIndex, Value v);
    Value binary(Op op, const Value& a, const Value& b);

    SourceLoader loader_;
    std::unordered_map<std::string, File> files_;
    std::vector<CompiledFunction> functions_;
    std::unordered_map<std::string, int> builtinIndex_;
    std::vector<Builtin> builtins_;
    std::vector<std::string> builtinNames_;
    std::deque<std::unique_ptr<Thread>> threads_;
    uint64_t nextThreadId_ = 1;
    uint64_t nextObjectId_ = 1;
    int64_t nowMs_ = 0;
    int runDepth_ = 0;
    ObjectPtr level_, game_, anim_;
    std::vector<std::string> errors_;
};

} // namespace iw2::script
