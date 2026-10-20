#include "script/value.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace iw2::script {

int32_t Value::asInt() const {
    if (auto p = std::get_if<int32_t>(&v)) return *p;
    if (auto p = std::get_if<float>(&v)) return int32_t(*p);
    throw std::runtime_error("type " + typeName() + " is not an int");
}

float Value::asFloat() const {
    if (auto p = std::get_if<float>(&v)) return *p;
    if (auto p = std::get_if<int32_t>(&v)) return float(*p);
    throw std::runtime_error("type " + typeName() + " is not a float");
}

const std::string& Value::asString() const {
    if (auto p = std::get_if<std::string>(&v)) return *p;
    throw std::runtime_error("type " + typeName() + " is not a string");
}

Vec3 Value::asVector() const {
    if (auto p = std::get_if<Vec3>(&v)) return *p;
    throw std::runtime_error("type " + typeName() + " is not a vector");
}

const ObjectPtr& Value::asObject() const {
    if (auto p = std::get_if<ObjectPtr>(&v)) return *p;
    throw std::runtime_error("type " + typeName() + " is not an object");
}

bool Value::truthy() const {
    if (auto p = std::get_if<int32_t>(&v)) return *p != 0;
    if (auto p = std::get_if<float>(&v)) return *p != 0.0f;
    if (!isDefined()) return false;
    if (auto p = std::get_if<std::string>(&v)) return !p->empty();
    return true;
}

std::string Value::typeName() const {
    switch (v.index()) {
    case 0: return "undefined";
    case 1: return "int";
    case 2: return "float";
    case 3: return "string";
    case 4: return "localized string";
    case 5: return "vector";
    case 6: return "object";
    case 7: return "array";
    case 8: return "function";
    case 9: return "animation";
    case 10: return "animtree";
    }
    return "?";
}

std::string Value::toString() const {
    char buf[96];
    switch (v.index()) {
    case 0: return "undefined";
    case 1: return std::to_string(std::get<int32_t>(v));
    case 2: {
        std::snprintf(buf, sizeof buf, "%g", double(std::get<float>(v)));
        return buf;
    }
    case 3: return std::get<std::string>(v);
    case 4: return "&\"" + std::get<LocalizedString>(v).key + "\"";
    case 5: {
        const auto& a = std::get<Vec3>(v);
        std::snprintf(buf, sizeof buf, "(%g, %g, %g)", double(a[0]), double(a[1]), double(a[2]));
        return buf;
    }
    case 6: return "<object " + std::get<ObjectPtr>(v)->kind + ">";
    case 7: return "<array>";
    case 8: return "<function>";
    case 9: return "%" + std::get<AnimRef>(v).name;
    case 10: return "#animtree";
    }
    return "?";
}

bool operator==(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
        return a.asFloat() == b.asFloat();
    }
    if (a.v.index() != b.v.index()) return false;
    if (a.isObject()) return a.asObject() == b.asObject();
    if (a.isArray()) return std::get<ArrayPtr>(a.v) == std::get<ArrayPtr>(b.v);
    return a.v == b.v;
}

const Value* Array::get(const ArrayKey& k) const {
    auto it = index.find(k);
    return it == index.end() ? nullptr : &items[it->second].second;
}

void Array::set(const ArrayKey& k, Value v) {
    auto it = index.find(k);
    if (!v.isDefined()) {
        if (it == index.end()) return;
        const size_t pos = it->second;
        items.erase(items.begin() + long(pos));
        index.erase(it);
        for (auto& [key, p] : index)
            if (p > pos) --p;
        return;
    }
    if (it != index.end()) {
        items[it->second].second = std::move(v);
        return;
    }
    index.emplace(k, items.size());
    items.emplace_back(k, std::move(v));
}

Value Object::get(const std::string& field) const {
    auto it = fields.find(field);
    return it == fields.end() ? Value{} : it->second;
}

void Object::set(const std::string& field, Value v) {
    if (!v.isDefined()) fields.erase(field);
    else fields[field] = std::move(v);
}

} // namespace iw2::script
