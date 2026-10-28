// Savegame state of the script VM (docs/spec/savegame.md section 5). openiw2's own format: the original writes its VM with
// 0x461810 / 0x461b50 as raw images of its variable and thread tables (TODO(spec): not decoded), which have no counterpart here.
#include "script/vm.h"

#include <stdexcept>

namespace iw2::script {

void VM::writeFunctionName(StateWriter& w, int function) const {
    if (function < 0 || size_t(function) >= functions_.size()) {
        w.str("");
        w.str("");
        return;
    }
    const CompiledFunction& f = functions_[size_t(function)];
    w.str(f.file);
    w.str(f.name);
}

int VM::readFunctionName(StateReader& r) {
    const std::string file = r.str();
    const std::string name = r.str();
    if (file.empty()) return -1;
    loadFile(file);
    const auto f = findFunction(file, name);
    if (!f) throw std::runtime_error("savegame: function " + file + "::" + name + " not found");
    return *f;
}

ObjectPtr VM::newObjectWithId(std::string kind, uint64_t id) {
    auto o = std::make_shared<Object>();
    o->kind = std::move(kind);
    o->id = id;
    if (id >= nextObjectId_) nextObjectId_ = id + 1;
    return o;
}

void VM::collectState(ObjectTable& t) const {
    t.collect(level_);
    t.collect(game_);
    t.collect(anim_);
    for (const auto& tp : threads_) {
        const Thread& th = *tp;
        if (th.state == ThreadState::Done) continue;
        for (const Frame& fr : th.frames) {
            for (const Value& v : fr.locals) t.collect(v);
            t.collect(fr.self);
        }
        for (const Value& v : th.stack) t.collect(v);
        for (const Ref& rf : th.refs) {
            t.collect(rf.object);
            t.collect(rf.key);
        }
        t.collect(th.result);
        t.collect(th.waitObject);
        for (const Value& v : th.received) t.collect(v);
        for (const auto& e : th.endons)
            if (auto o = e.object.lock()) t.collect(o);
    }
}

void VM::saveState(StateWriter& w, const ObjectTable& t) const {
    if (runDepth_ != 0) throw std::runtime_error("savegame: the VM is running a thread");
    w.i64(nowMs_);
    w.u64(nextThreadId_);
    w.u64(nextObjectId_);
    w.i32(t.indexOf(level_));
    w.i32(t.indexOf(game_));
    w.i32(t.indexOf(anim_));
    uint32_t live = 0;
    for (const auto& tp : threads_) live += tp->state != ThreadState::Done;
    w.u32(live);
    for (const auto& tp : threads_) {
        const Thread& th = *tp;
        if (th.state == ThreadState::Done) continue;
        w.u64(th.id);
        w.u8(uint8_t(th.state));
        w.u32(uint32_t(th.frames.size()));
        for (const Frame& fr : th.frames) {
            writeFunctionName(w, fr.function);
            w.u64(fr.pc);
            w.u32(uint32_t(fr.locals.size()));
            for (const Value& v : fr.locals) t.writeValue(w, v);
            t.writeValue(w, fr.self);
            w.u64(fr.stackBase);
            w.u64(fr.refBase);
        }
        w.u32(uint32_t(th.stack.size()));
        for (const Value& v : th.stack) t.writeValue(w, v);
        w.u32(uint32_t(th.refs.size()));
        for (const Ref& rf : th.refs) {
            w.u8(uint8_t(rf.kind));
            w.i32(rf.slot);
            w.i32(t.indexOf(rf.object));
            w.str(rf.field);
            w.i32(rf.parent);
            t.writeValue(w, rf.key);
        }
        w.i64(th.wakeTimeMs);
        t.writeValue(w, th.result);
        w.i32(t.indexOf(th.waitObject));
        w.str(th.waitEvent);
        w.b(th.waitMatch.has_value());
        if (th.waitMatch) w.str(*th.waitMatch);
        w.i32(th.waitVarCount);
        w.u32(uint32_t(th.received.size()));
        for (const Value& v : th.received) t.writeValue(w, v);
        uint32_t ne = 0;
        for (const auto& e : th.endons) ne += !e.object.expired();
        w.u32(ne);
        for (const auto& e : th.endons) {
            auto o = e.object.lock();
            if (!o) continue;
            w.i32(t.indexOf(o));
            w.str(e.event);
            w.u64(e.frame);
        }
        w.i32(th.pendingUnwind);
    }
    uint32_t nr = 0;
    for (const Thread* th : ready_) nr += th->state != ThreadState::Done;
    w.u32(nr);
    for (const Thread* th : ready_)
        if (th->state != ThreadState::Done) w.u64(th->id);
}

void VM::loadState(StateReader& r, const ObjectTable& t) {
    if (runDepth_ != 0) throw std::runtime_error("savegame: the VM is running a thread");
    nowMs_ = r.i64();
    nextThreadId_ = r.u64();
    const uint64_t nextObj = r.u64();
    if (nextObj > nextObjectId_) nextObjectId_ = nextObj;
    level_ = t.object(r.i32());
    game_ = t.object(r.i32());
    anim_ = t.object(r.i32());
    threads_.clear();
    ready_.clear();
    const uint32_t n = r.u32();
    for (uint32_t i = 0; i < n; ++i) {
        auto th = std::make_unique<Thread>();
        th->id = r.u64();
        th->state = ThreadState(r.u8());
        const uint32_t nf = r.u32();
        for (uint32_t k = 0; k < nf; ++k) {
            Frame fr;
            fr.function = readFunctionName(r);
            fr.pc = size_t(r.u64());
            const uint32_t nl = r.u32();
            for (uint32_t j = 0; j < nl; ++j) fr.locals.push_back(t.readValue(r));
            fr.self = t.readValue(r);
            fr.stackBase = size_t(r.u64());
            fr.refBase = size_t(r.u64());
            if (fr.function < 0 || fr.pc > functions_[size_t(fr.function)].code.size())
                throw std::runtime_error("savegame: thread frame does not match the script code");
            th->frames.push_back(std::move(fr));
        }
        const uint32_t ns = r.u32();
        for (uint32_t j = 0; j < ns; ++j) th->stack.push_back(t.readValue(r));
        const uint32_t nrf = r.u32();
        for (uint32_t j = 0; j < nrf; ++j) {
            Ref rf;
            rf.kind = Ref::Kind(r.u8());
            rf.slot = r.i32();
            rf.object = t.object(r.i32());
            rf.field = r.str();
            rf.parent = r.i32();
            rf.key = t.readValue(r);
            th->refs.push_back(std::move(rf));
        }
        th->wakeTimeMs = r.i64();
        th->result = t.readValue(r);
        th->waitObject = t.object(r.i32());
        th->waitEvent = r.str();
        if (r.b()) th->waitMatch = r.str();
        th->waitVarCount = r.i32();
        const uint32_t nv = r.u32();
        for (uint32_t j = 0; j < nv; ++j) th->received.push_back(t.readValue(r));
        const uint32_t ne = r.u32();
        for (uint32_t j = 0; j < ne; ++j) {
            Thread::EndOn e;
            e.object = t.object(r.i32());
            e.event = r.str();
            e.frame = size_t(r.u64());
            th->endons.push_back(std::move(e));
        }
        th->pendingUnwind = r.i32();
        threads_.push_back(std::move(th));
    }
    const uint32_t nr = r.u32();
    for (uint32_t i = 0; i < nr; ++i) {
        const uint64_t id = r.u64();
        for (const auto& tp : threads_)
            if (tp->id == id) { ready_.push_back(tp.get()); break; }
    }
}

} // namespace iw2::script
