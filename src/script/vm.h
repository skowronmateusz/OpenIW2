// GSC compiler and virtual machine with cooperative threads (docs/spec/gsc-vm.md).
#pragma once

#include "script/ast.h"
#include "script/stateio.h"
#include "script/value.h"

#include <chrono>
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
    size_t refBase = 0; // the thread's ref stack size when the call began
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
    uint64_t waitSeq = 0;     // filing order of the wait (0x46a2e9 links the thread at the head of its frame's list)
    Value result;             // value of the first function's return; a thread call yields it if the thread ends at once
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
    // Sets the script clock before the frame's threads run (the engine's entity loop runs woken threads first).
    void setTime(int64_t nowMs) { nowMs_ = nowMs; }

    // Wakes the threads waiting for `event` on `object`. They are queued, not run: the exe's VM_Notify (0x46ca60)
    // only moves each woken thread to the end of the current frame's run list (0x46c130), which Scr_RunFrame
    // (0x46f5c0 -> 0x46de90 -> 0x46d560) runs, re-reading its head until it is empty. So they run after the notifying
    // thread yields, at the next runFrame()/drainReady(); `thread f()` still runs f at once.
    void notify(const ObjectPtr& object, const std::string& event, std::vector<Value> args = {});
    // Frees an entity's script object (Scr_FreeEntityNum 0x4670b0, from G_FreeEntity 0x4d7c30): the object reads as
    // undefined at once; the threads registered on its notify list (waittill and endon) end when the freed-object list is
    // processed (0x467150 -> 0x46d1f0), which the exe does after a script run returns to the engine.
    void freeObject(const ObjectPtr& object);
    void processFreedObjects();
    // Runs the queued woken threads (and those woken while running them) until none is left.
    void drainReady();

    ObjectPtr level() const { return level_; }
    ObjectPtr game() const { return game_; }
    ObjectPtr anim() const { return anim_; }
    ObjectPtr newObject(std::string kind);

    size_t liveThreadCount() const;
    bool isThreadAlive(uint64_t id) const;
    // Debug: "file:line function (state event)" for each live thread's innermost frame.
    std::vector<std::string> describeThreads() const;
    std::string describeThread(uint64_t id) const; // debug: wait site of one thread (empty if gone)
    std::vector<std::string> describeThreadsOf(const ObjectPtr& self) const; // debug: wait sites of the threads whose innermost self is `self`
    // Debug (iw2game --autoplay): every thread suspended in waittill / waittillmatch (the object and event it waits for) or in a
    // timed wait (its innermost self, event ""), with the innermost wait site ("file:line function"). Read-only.
    struct Waiter { ObjectPtr object; std::string event; std::string site; };
    std::vector<Waiter> waiters() const;
    // Debug hook: called at the start of every notify (object, event), before endons and waiters run. Empty by default.
    std::function<void(const ObjectPtr&, const std::string&)> notifyHook;
    // Debug: with n > 0 the "potential infinite loop" guard kills a script after n backward jumps without suspending instead
    // of after 2500 ms of wall time (0x46aae9, the exe behaviour), so headless runs are deterministic (iw2game sets it).
    void setLoopGuardIterations(size_t n) { loopGuardIterations_ = n; }
    // Debug: " < file:line function" for each frame of a thread, outermost first.
    std::string backtrace(const Thread& t) const;
    const std::vector<std::string>& errors() const { return errors_; }
    std::function<void(const std::string&)> print = [](const std::string&) {};
    ObjectPtr traceSelf; // debug: log built-in calls made with this self

    // Calls a function value synchronously (no suspension allowed); used by builtins.
    const CompiledFunction* function(int index) const { return &functions_[size_t(index)]; }
    std::optional<int> findFunction(const std::string& file, const std::string& name) const;

    // Savegame (src/script/vmstate.cpp, docs/spec/savegame.md section 5). Only between frames (no thread running).
    // collectState adds every object / array the VM holds (level, game, anim, thread frames, stacks, refs, waits) to the table;
    // saveState / loadState write / read the threads, the woken queue, time and the id counters. Functions are stored by
    // file + name and re-resolved (loading the file if needed), so the load order of script files does not matter.
    void collectState(ObjectTable& t) const;
    void saveState(StateWriter& w, const ObjectTable& t) const;
    void loadState(StateReader& r, const ObjectTable& t);
    void writeFunctionName(StateWriter& w, int function) const;
    int readFunctionName(StateReader& r);
    uint64_t nextObjectId() const { return nextObjectId_; }
    // Creates an object with a given id (savegame load of an object the fresh world does not have).
    ObjectPtr newObjectWithId(std::string kind, uint64_t id);

private:
    std::vector<ObjectPtr> freed_; // 0x9bef38 list
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
    std::chrono::steady_clock::time_point runStart_ = std::chrono::steady_clock::now(); // 0x9ff01c: start of the running script
    size_t loopGuardIterations_ = 0; // debug: > 0 replaces the 2500 ms wall-clock loop guard by a backward-jump count (deterministic)
    size_t backwardJumps_ = 0;       // backward jumps of the running script since its run started
    std::unordered_map<std::string, int> builtinIndex_;
    std::vector<Builtin> builtins_;
    std::vector<std::string> builtinNames_;
    std::deque<std::unique_ptr<Thread>> threads_;
    uint64_t nextThreadId_ = 1;
    uint64_t nextObjectId_ = 1;
    int64_t nowMs_ = 0;
    int runDepth_ = 0;
    uint64_t waitSeq_ = 0;
    std::deque<Thread*> ready_; // woken by notify, not yet run
    bool draining_ = false;
    ObjectPtr level_, game_, anim_;
    std::vector<std::string> errors_;
};

} // namespace iw2::script
