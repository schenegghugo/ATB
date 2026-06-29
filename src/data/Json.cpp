#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tb::json {

// --- Value -----------------------------------------------------------------

Value::Type Value::type() const {
    switch (data_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2: return Type::Number;
        case 3: return Type::String;
        case 4: return Type::Array;
        default: return Type::Object;
    }
}

const Value* Value::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    for (const Member& m : asObject())
        if (m.first == key) return &m.second;
    return nullptr;
}

void Value::push_back(Value v) {
    if (isArray()) asArray().push_back(std::move(v));
}

void Value::set(std::string key, Value v) {
    if (!isObject()) return;
    Object& obj = asObject();
    for (Member& m : obj)
        if (m.first == key) {
            m.second = std::move(v);
            return;
        }
    obj.emplace_back(std::move(key), std::move(v));
}

bool Value::operator==(const Value& o) const {
    if (type() != o.type()) return false;
    switch (type()) {
        case Type::Null: return true;
        case Type::Bool: return asBool() == o.asBool();
        case Type::Number: return asNumber() == o.asNumber();
        case Type::String: return asString() == o.asString();
        case Type::Array: {
            const Array& a = asArray();
            const Array& b = o.asArray();
            if (a.size() != b.size()) return false;
            for (std::size_t k = 0; k < a.size(); ++k)
                if (!(a[k] == b[k])) return false;
            return true;
        }
        case Type::Object: {
            // Order-insensitive: two objects with the same members are equal
            // regardless of key order.
            const Object& a = asObject();
            if (a.size() != o.asObject().size()) return false;
            for (const Member& m : a) {
                const Value* bv = o.find(m.first);
                if (!bv || !(*bv == m.second)) return false;
            }
            return true;
        }
    }
    return false;
}

// --- Parser ----------------------------------------------------------------

namespace {

void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const std::string& msg) {
        if (err.empty()) {
            std::size_t line = 1, col = 1;
            for (std::size_t k = 0; k < i && k < s.size(); ++k) {
                if (s[k] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            err = "line " + std::to_string(line) + " column " + std::to_string(col) + ": " + msg;
        }
        return false;
    }

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }

    bool parseHex4(unsigned& out) {
        if (i + 4 > s.size()) return fail("incomplete \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        out = v;
        return true;
    }

    // Assumes s[i] == '"'.
    bool parseString(std::string& out) {
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return fail("unterminated escape");
                char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate
                            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                                i += 2;
                                unsigned lo = 0;
                                if (!parseHex4(lo)) return false;
                                if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                return fail("expected low surrogate after high surrogate");
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("unexpected low surrogate");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return fail("invalid escape character");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value& out) {
        std::size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i < s.size() && s[i] == '0') {
            ++i;
        } else if (i < s.size() && s[i] >= '1' && s[i] <= '9') {
            while (i < s.size() && isDigit(s[i])) ++i;
        } else {
            return fail("invalid number");
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (!(i < s.size() && isDigit(s[i]))) return fail("expected digit after '.'");
            while (i < s.size() && isDigit(s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!(i < s.size() && isDigit(s[i]))) return fail("expected digit in exponent");
            while (i < s.size() && isDigit(s[i])) ++i;
        }
        std::string num = s.substr(start, i - start);
        out = Value(std::strtod(num.c_str(), nullptr));
        return true;
    }

    bool parseLiteral(const char* lit, Value v, Value& out) {
        std::size_t n = std::strlen(lit);
        if (i + n <= s.size() && s.compare(i, n, lit) == 0) {
            i += n;
            out = std::move(v);
            return true;
        }
        return fail(std::string("invalid literal, expected '") + lit + "'");
    }

    bool parseArray(Value& out) {
        ++i; // consume '['
        out = Value::makeArray();
        skipWs();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            Value elem;
            if (!parseValue(elem)) return false;
            out.push_back(std::move(elem));
            skipWs();
            if (i >= s.size()) return fail("unterminated array");
            char c = s[i++];
            if (c == ']') return true;
            if (c != ',') return fail("expected ',' or ']' in array");
        }
    }

    bool parseObject(Value& out) {
        ++i; // consume '{'
        out = Value::makeObject();
        skipWs();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return fail("expected string key in object");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected ':' after object key");
            ++i;
            Value val;
            if (!parseValue(val)) return false;
            out.set(std::move(key), std::move(val));
            skipWs();
            if (i >= s.size()) return fail("unterminated object");
            char c = s[i++];
            if (c == '}') return true;
            if (c != ',') return fail("expected ',' or '}' in object");
        }
    }

    bool parseValue(Value& out) {
        skipWs();
        if (i >= s.size()) return fail("unexpected end of input");
        char c = s[i];
        switch (c) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't': return parseLiteral("true", Value(true), out);
            case 'f': return parseLiteral("false", Value(false), out);
            case 'n': return parseLiteral("null", Value(nullptr), out);
            default:
                if (c == '-' || isDigit(c)) return parseNumber(out);
                return fail(std::string("unexpected character '") + c + "'");
        }
    }
};

} // namespace

ParseResult parse(const std::string& text) {
    Parser p(text);
    ParseResult r;
    Value v;
    if (!p.parseValue(v)) {
        r.error = p.err;
        return r;
    }
    p.skipWs();
    if (p.i != text.size()) {
        p.fail("trailing characters after JSON value");
        r.error = p.err;
        return r;
    }
    r.ok = true;
    r.value = std::move(v);
    return r;
}

// --- Serializer ------------------------------------------------------------

namespace {

void escapeTo(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c) & 0xFF);
                    out += buf;
                } else {
                    out.push_back(c); // raw UTF-8 bytes pass through
                }
        }
    }
    out.push_back('"');
}

void numberTo(std::string& out, double n) {
    // Integral values print without a fractional part (keeps the catalog tidy:
    // 15, not 15.0). Non-integral values use a round-trippable representation.
    if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15) {
        out += std::to_string(static_cast<long long>(n));
    } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", n);
        out += buf;
    }
}

void dumpTo(std::string& out, const Value& v, bool pretty, int depth) {
    auto newlineIndent = [&](int d) {
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(d) * 2, ' ');
        }
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: numberTo(out, v.asNumber()); break;
        case Value::Type::String: escapeTo(out, v.asString()); break;
        case Value::Type::Array: {
            const Value::Array& a = v.asArray();
            if (a.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t k = 0; k < a.size(); ++k) {
                if (k) out.push_back(',');
                newlineIndent(depth + 1);
                dumpTo(out, a[k], pretty, depth + 1);
            }
            newlineIndent(depth);
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            const Value::Object& o = v.asObject();
            if (o.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t k = 0; k < o.size(); ++k) {
                if (k) out.push_back(',');
                newlineIndent(depth + 1);
                escapeTo(out, o[k].first);
                out += pretty ? ": " : ":";
                dumpTo(out, o[k].second, pretty, depth + 1);
            }
            newlineIndent(depth);
            out.push_back('}');
            break;
        }
    }
}

} // namespace

std::string dump(const Value& v, bool pretty) {
    std::string out;
    dumpTo(out, v, pretty, 0);
    if (pretty) out.push_back('\n');
    return out;
}

} // namespace tb::json
