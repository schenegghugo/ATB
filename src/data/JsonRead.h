#pragma once
//
// JsonRead.h — Small inline helpers for strict JSON reading, shared by the
// catalog and creature loaders. Each records a contextual error (and leaves the
// output at a default) rather than throwing.
//
#include "Json.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace tb::jsonread {

using Errors = std::vector<std::string>;

inline bool toInt(const json::Value& v, int& out) {
    if (!v.isNumber()) return false;
    const double d = v.asNumber();
    if (d != std::floor(d)) return false; // reject 15.5
    if (d < static_cast<double>(std::numeric_limits<int>::min()) ||
        d > static_cast<double>(std::numeric_limits<int>::max()))
        return false;
    out = static_cast<int>(d);
    return true;
}

inline bool wantInt(const json::Value& obj, const char* key, const std::string& ctx, int& out,
                    Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) {
        e.push_back(ctx + ": missing required field \"" + key + "\"");
        return false;
    }
    if (!toInt(*v, out)) {
        e.push_back(ctx + ": \"" + key + "\" must be an integer");
        return false;
    }
    return true;
}

inline int optInt(const json::Value& obj, const char* key, int def, const std::string& ctx,
                  Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) return def;
    int out = def;
    if (!toInt(*v, out)) {
        e.push_back(ctx + ": \"" + key + "\" must be an integer");
        return def;
    }
    return out;
}

inline bool optBool(const json::Value& obj, const char* key, bool def, const std::string& ctx,
                    Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) return def;
    if (!v->isBool()) {
        e.push_back(ctx + ": \"" + key + "\" must be a boolean");
        return def;
    }
    return v->asBool();
}

inline void checkAllowed(const json::Value& obj, std::initializer_list<const char*> allowed,
                         const std::string& ctx, Errors& e) {
    for (const json::Value::Member& m : obj.asObject()) {
        bool ok = false;
        for (const char* a : allowed)
            if (m.first == a) {
                ok = true;
                break;
            }
        if (!ok) e.push_back(ctx + ": unknown field \"" + m.first + "\"");
    }
}

inline void forbid(const json::Value& obj, const char* key, const std::string& typeName,
                   const std::string& ctx, Errors& e) {
    if (obj.find(key))
        e.push_back(ctx + ": \"" + key + "\" is not valid for effect type \"" + typeName + "\"");
}

} // namespace tb::jsonread
