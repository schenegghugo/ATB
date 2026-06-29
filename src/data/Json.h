#pragma once
//
// Json.h — Minimal, dependency-free JSON reader/writer.
//
// A small generic JSON layer for the data/ tier: parse text into a Value tree,
// inspect it, and dump it back out. Hand-rolled on purpose (no third-party dep)
// and schema-agnostic — the catalog loader (CatalogJson) and the sprite-pack
// atlas manifest loader both build on top of it.
//
// Objects preserve insertion order, so dump() is deterministic — which keeps the
// generated catalog file stable and its sha256 reproducible.
//
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tb::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>; // insertion-ordered

    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool b) : data_(b) {}
    Value(double n) : data_(n) {}
    Value(int n) : data_(static_cast<double>(n)) {}
    Value(long long n) : data_(static_cast<double>(n)) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}

    static Value makeArray() {
        Value v;
        v.data_ = Array{};
        return v;
    }
    static Value makeObject() {
        Value v;
        v.data_ = Object{};
        return v;
    }

    [[nodiscard]] Type type() const;
    [[nodiscard]] bool isNull() const { return type() == Type::Null; }
    [[nodiscard]] bool isBool() const { return type() == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type() == Type::Number; }
    [[nodiscard]] bool isString() const { return type() == Type::String; }
    [[nodiscard]] bool isArray() const { return type() == Type::Array; }
    [[nodiscard]] bool isObject() const { return type() == Type::Object; }

    // Typed accessors. Check the type first (isX()); calling the wrong one throws
    // std::bad_variant_access — that's a programmer error, not malformed data.
    [[nodiscard]] bool asBool() const { return std::get<bool>(data_); }
    [[nodiscard]] double asNumber() const { return std::get<double>(data_); }
    [[nodiscard]] const std::string& asString() const { return std::get<std::string>(data_); }
    [[nodiscard]] const Array& asArray() const { return std::get<Array>(data_); }
    [[nodiscard]] const Object& asObject() const { return std::get<Object>(data_); }
    [[nodiscard]] Array& asArray() { return std::get<Array>(data_); }
    [[nodiscard]] Object& asObject() { return std::get<Object>(data_); }

    // Object lookup: nullptr if this isn't an object or the key is absent.
    [[nodiscard]] const Value* find(const std::string& key) const;
    [[nodiscard]] bool contains(const std::string& key) const { return find(key) != nullptr; }

    // Builders (no-op if the wrong type — call makeArray/makeObject first).
    void push_back(Value v);            // append to an array
    void set(std::string key, Value v); // set/replace an object member

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error; // human-readable; includes "line L column C" on failure
};

// Parse a complete JSON document. Trailing non-whitespace is an error (strict).
[[nodiscard]] ParseResult parse(const std::string& text);

// Serialize. pretty = 2-space indented + newlines; otherwise compact.
[[nodiscard]] std::string dump(const Value& v, bool pretty = true);

} // namespace tb::json
