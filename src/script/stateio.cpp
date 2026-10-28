#include "script/stateio.h"

#include <vector>

namespace iw2::script {

namespace {
enum Tag : uint8_t { TUndef, TInt, TFloat, TString, TIString, TVec, TObject, TArray, TFunc, TAnim, TAnimTree };
}

void ObjectTable::collect(const ObjectPtr& root) {
    if (!root || objIndex_.count(root.get())) return;
    // iterative walk (deep struct / array chains would overflow a recursive one)
    std::vector<Value> stack;
    objIndex_[root.get()] = int32_t(objects_.size());
    objects_.push_back(root);
    for (const auto& [k, v] : root->fields) stack.push_back(v);
    while (!stack.empty()) {
        const Value v = std::move(stack.back());
        stack.pop_back();
        if (v.isObject()) {
            const ObjectPtr& o = v.asObject();
            if (!o || objIndex_.count(o.get())) continue;
            objIndex_[o.get()] = int32_t(objects_.size());
            objects_.push_back(o);
            for (const auto& [k, fv] : o->fields) stack.push_back(fv);
        } else if (v.isArray()) {
            const ArrayPtr& a = std::get<ArrayPtr>(v.v);
            if (!a || arrIndex_.count(a.get())) continue;
            arrIndex_[a.get()] = int32_t(arrays_.size());
            arrays_.push_back(a);
            for (const auto& [k, iv] : a->items) stack.push_back(iv);
        }
    }
}

void ObjectTable::collect(const Value& v) {
    if (v.isObject()) collect(v.asObject());
    else if (v.isArray()) {
        // wrap: an array root is walked through a temporary holder object that is not itself recorded
        const ArrayPtr& a = std::get<ArrayPtr>(v.v);
        if (!a || arrIndex_.count(a.get())) return;
        arrIndex_[a.get()] = int32_t(arrays_.size());
        arrays_.push_back(a);
        for (const auto& [k, iv] : a->items) collect(iv);
    }
}

int32_t ObjectTable::indexOf(const ObjectPtr& o) const {
    if (!o) return -1;
    auto it = objIndex_.find(o.get());
    if (it == objIndex_.end()) throw std::runtime_error("savegame: object " + std::to_string(o->id) + " (" + o->kind + ") not collected");
    return it->second;
}

int32_t ObjectTable::indexOf(const ArrayPtr& a) const {
    if (!a) return -1;
    auto it = arrIndex_.find(a.get());
    if (it == arrIndex_.end()) throw std::runtime_error("savegame: array not collected");
    return it->second;
}

void ObjectTable::writeValue(StateWriter& w, const Value& v) const {
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Undefined>) w.u8(TUndef);
            else if constexpr (std::is_same_v<T, int32_t>) { w.u8(TInt); w.i32(x); }
            else if constexpr (std::is_same_v<T, float>) { w.u8(TFloat); w.f32(x); }
            else if constexpr (std::is_same_v<T, std::string>) { w.u8(TString); w.str(x); }
            else if constexpr (std::is_same_v<T, LocalizedString>) { w.u8(TIString); w.str(x.key); }
            else if constexpr (std::is_same_v<T, Vec3>) { w.u8(TVec); w.vec3(x); }
            else if constexpr (std::is_same_v<T, ObjectPtr>) { w.u8(TObject); w.i32(indexOf(x)); }
            else if constexpr (std::is_same_v<T, ArrayPtr>) { w.u8(TArray); w.i32(indexOf(x)); }
            else if constexpr (std::is_same_v<T, FunctionRef>) {
                w.u8(TFunc);
                if (!writeFunction) throw std::runtime_error("savegame: no function writer");
                writeFunction(w, x.function);
            } else if constexpr (std::is_same_v<T, AnimRef>) { w.u8(TAnim); w.str(x.name); }
            else if constexpr (std::is_same_v<T, AnimTreeRef>) { w.u8(TAnimTree); w.str(x.name); }
        },
        v.v);
}

Value ObjectTable::readValue(StateReader& r) const {
    switch (r.u8()) {
    case TUndef: return Value();
    case TInt: return Value(r.i32());
    case TFloat: return Value(r.f32());
    case TString: return Value(r.str());
    case TIString: return Value(LocalizedString{r.str()});
    case TVec: return Value(r.vec3());
    case TObject: return Value(object(r.i32()));
    case TArray: {
        const int32_t i = r.i32();
        if (i < 0 || size_t(i) >= arrays_.size()) throw std::runtime_error("savegame: bad array index");
        return Value(arrays_[size_t(i)]);
    }
    case TFunc: {
        if (!readFunction) throw std::runtime_error("savegame: no function reader");
        return Value(FunctionRef{readFunction(r)});
    }
    case TAnim: return Value(AnimRef{r.str()});
    case TAnimTree: return Value(AnimTreeRef{r.str()});
    default: throw std::runtime_error("savegame: bad value tag");
    }
}

ObjectPtr ObjectTable::object(int32_t index) const {
    if (index < 0) return nullptr;
    if (size_t(index) >= objects_.size()) throw std::runtime_error("savegame: bad object index");
    return objects_[size_t(index)];
}

void ObjectTable::write(StateWriter& w) const {
    w.u32(uint32_t(objects_.size()));
    for (const auto& o : objects_) {
        w.str(o->kind);
        w.u64(o->id);
        w.b(o->removed);
    }
    w.u32(uint32_t(arrays_.size()));
    for (const auto& o : objects_) {
        w.u32(uint32_t(o->fields.size()));
        for (const auto& [k, v] : o->fields) {
            w.str(k);
            writeValue(w, v);
        }
    }
    for (const auto& a : arrays_) {
        w.u32(uint32_t(a->items.size()));
        for (const auto& [k, v] : a->items) {
            w.b(k.isString);
            if (k.isString) w.str(k.s);
            else w.i32(k.i);
            writeValue(w, v);
        }
    }
}

void ObjectTable::readContents(StateReader& r) {
    for (const auto& o : objects_) {
        o->fields.clear();
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) {
            std::string k = r.str();
            o->fields[k] = readValue(r);
        }
    }
    for (const auto& a : arrays_) {
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) {
            ArrayKey k;
            k.isString = r.b();
            if (k.isString) k.s = r.str();
            else k.i = r.i32();
            Value v = readValue(r);
            a->index[k] = a->items.size();
            a->items.emplace_back(std::move(k), std::move(v));
        }
    }
}

void collectLive(const std::vector<Value>& roots, std::unordered_map<uint64_t, ObjectPtr>& out) {
    ObjectTable t;
    for (const auto& v : roots) t.collect(v);
    for (const auto& o : t.objects()) out.emplace(o->id, o);
}

} // namespace iw2::script
