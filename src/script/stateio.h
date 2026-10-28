// Binary state stream used by the openiw2 savegame body (docs/spec/savegame.md section 5). Script objects and arrays are written
// once in a table and referenced by index everywhere else, so shared references (objects are reference types, arrays are shared
// copy-on-write) keep their identity across a save and a load.
#pragma once

#include "script/value.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::script {

class StateWriter {
public:
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void b(bool v) { u8(v ? 1 : 0); }
    void i32(int32_t v) { raw(&v, 4); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i64(int64_t v) { raw(&v, 8); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void vec3(const Vec3& v) { for (float f : v) f32(f); }
    void str(const std::string& s) { u32(uint32_t(s.size())); raw(s.data(), s.size()); }
    void raw(const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); bytes.insert(bytes.end(), c, c + n); }
};

class StateReader {
public:
    StateReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    uint8_t u8() { need(1); return *p_++; }
    bool b() { return u8() != 0; }
    int32_t i32() { int32_t v; raw(&v, 4); return v; }
    uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
    int64_t i64() { int64_t v; raw(&v, 8); return v; }
    uint64_t u64() { uint64_t v; raw(&v, 8); return v; }
    float f32() { float v; raw(&v, 4); return v; }
    Vec3 vec3() { Vec3 v; for (float& f : v) f = f32(); return v; }
    std::string str() { const uint32_t n = u32(); need(n); std::string s(reinterpret_cast<const char*>(p_), n); p_ += n; return s; }
    void raw(void* out, size_t n) { need(n); std::memcpy(out, p_, n); p_ += n; }
    bool atEnd() const { return p_ == end_; }
    uint32_t version = ~0u; // format version of the stream (set by the reader of a versioned file; ~0 = current)

private:
    void need(size_t n) const { if (size_t(end_ - p_) < n) throw std::runtime_error("savegame: truncated state"); }
    const uint8_t* p_;
    const uint8_t* end_;
};

// Object / array table. Save: collect() every root (recursively follows fields and array items), then write the table; values are
// then written as table indices. Load: the table is read first (objects are matched to live ones by id or created), then filled.
class ObjectTable {
public:
    // Function values are written by name (file + function), not by the VM's load-order index.
    std::function<void(StateWriter&, int)> writeFunction;
    std::function<int(StateReader&)> readFunction;

    // --- save side ---
    void collect(const Value& v);
    void collect(const ObjectPtr& o);
    int32_t indexOf(const ObjectPtr& o) const; // -1 for null; throws if the object was not collected
    int32_t indexOf(const ArrayPtr& a) const;
    void writeValue(StateWriter& w, const Value& v) const;
    // Writes the table: object headers (kind, id, removed), array count, then the fields and items.
    void write(StateWriter& w) const;

    // --- load side ---
    // `live` = objects of the freshly started world by id (reused so engine pointers to them stay valid); `create` makes a new object.
    template <class Create>
    void read(StateReader& r, const std::unordered_map<uint64_t, ObjectPtr>& live, Create create);
    ObjectPtr object(int32_t index) const;
    Value readValue(StateReader& r) const;

    const std::vector<ObjectPtr>& objects() const { return objects_; }

private:
    void readContents(StateReader& r);
    std::vector<ObjectPtr> objects_;
    std::vector<ArrayPtr> arrays_;
    std::unordered_map<const Object*, int32_t> objIndex_;
    std::unordered_map<const Array*, int32_t> arrIndex_;
};

// Collects every object reachable from a value list (used to index the live objects of a fresh world by id).
void collectLive(const std::vector<Value>& roots, std::unordered_map<uint64_t, ObjectPtr>& out);

template <class Create>
void ObjectTable::read(StateReader& r, const std::unordered_map<uint64_t, ObjectPtr>& live, Create create) {
    const uint32_t nObj = r.u32();
    objects_.clear();
    objects_.reserve(nObj);
    for (uint32_t i = 0; i < nObj; ++i) {
        std::string kind = r.str();
        const uint64_t id = r.u64();
        const bool removed = r.b();
        auto it = live.find(id);
        ObjectPtr o = it != live.end() && it->second->kind == kind ? it->second : create(kind, id);
        o->removed = removed;
        objects_.push_back(o);
    }
    const uint32_t nArr = r.u32();
    arrays_.clear();
    for (uint32_t i = 0; i < nArr; ++i) arrays_.push_back(std::make_shared<Array>());
    readContents(r);
}

} // namespace iw2::script
