// Script values (docs/spec/gsc-vm.md).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iw2::script {

struct Object;
struct Array;
struct Value;

struct FunctionRef {
    int function = -1; // index into the VM's function table
    bool operator==(const FunctionRef& o) const { return function == o.function; }
};
struct LocalizedString { std::string key; bool operator==(const LocalizedString& o) const { return key == o.key; } };
struct AnimRef { std::string name; bool operator==(const AnimRef& o) const { return name == o.name; } };
struct AnimTreeRef { std::string name; bool operator==(const AnimTreeRef& o) const { return name == o.name; } };
using Vec3 = std::array<float, 3>;
struct Undefined { bool operator==(const Undefined&) const { return true; } };

// Arrays have value semantics in scripts; they are shared copy-on-write.
using ArrayPtr = std::shared_ptr<Array>;
// Objects (level, game, entities, structs) are reference types.
using ObjectPtr = std::shared_ptr<Object>;

struct Value {
    std::variant<Undefined, int32_t, float, std::string, LocalizedString, Vec3, ObjectPtr, ArrayPtr, FunctionRef, AnimRef, AnimTreeRef> v;

    Value() = default;
    Value(int32_t i) : v(i) {}
    Value(float f) : v(f) {}
    Value(bool b) : v(int32_t(b ? 1 : 0)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(Vec3 a) : v(a) {}
    Value(ObjectPtr o) : v(std::move(o)) {}
    Value(ArrayPtr a) : v(std::move(a)) {}
    Value(FunctionRef f) : v(f) {}
    Value(LocalizedString s) : v(std::move(s)) {}
    Value(AnimRef a) : v(std::move(a)) {}
    Value(AnimTreeRef a) : v(std::move(a)) {}

    bool isDefined() const { return !std::holds_alternative<Undefined>(v); }
    bool isInt() const { return std::holds_alternative<int32_t>(v); }
    bool isFloat() const { return std::holds_alternative<float>(v); }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isString() const { return std::holds_alternative<std::string>(v); }
    bool isVector() const { return std::holds_alternative<Vec3>(v); }
    bool isObject() const { return std::holds_alternative<ObjectPtr>(v); }
    bool isArray() const { return std::holds_alternative<ArrayPtr>(v); }
    bool isFunction() const { return std::holds_alternative<FunctionRef>(v); }

    int32_t asInt() const;
    float asFloat() const;
    const std::string& asString() const;
    Vec3 asVector() const;
    const ObjectPtr& asObject() const;
    bool truthy() const;
    std::string typeName() const;
    std::string toString() const; // for printing / string concatenation
};

bool operator==(const Value& a, const Value& b);

// Array key: integer or string.
struct ArrayKey {
    bool isString = false;
    int32_t i = 0;
    std::string s;
    bool operator==(const ArrayKey& o) const { return isString == o.isString && (isString ? s == o.s : i == o.i); }
};
struct ArrayKeyHash {
    size_t operator()(const ArrayKey& k) const { return k.isString ? std::hash<std::string>()(k.s) : std::hash<int32_t>()(k.i); }
};

struct Array {
    std::vector<std::pair<ArrayKey, Value>> items; // insertion order
    std::unordered_map<ArrayKey, size_t, ArrayKeyHash> index;

    const Value* get(const ArrayKey& k) const;
    void set(const ArrayKey& k, Value v); // undefined removes the element
    size_t size() const { return items.size(); }
};

struct Object {
    std::string kind;   // "level", "game", "anim", "struct", or an entity class
    uint64_t id = 0;
    bool removed = false;
    std::unordered_map<std::string, Value> fields; // lower-case field names

    Value get(const std::string& field) const;
    void set(const std::string& field, Value v);
};

} // namespace iw2::script
