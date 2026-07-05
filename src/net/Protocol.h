#pragma once
//
// Protocol.h — The message layer for 1v1 custom matches (Phase 4.4).
//
// Each frame (see Socket.h) is one JSON object with a "type". Payloads reuse the
// §4.1 wire serializers (Intent / build text) verbatim.
//
// The client keeps a deterministic *mirror* of the match (a MatchRunner it builds
// from the setup in `welcome`), then stays in lockstep by replaying the server's
// authoritative `applied`-intent stream. So the server broadcasts the human
// intents it accepts — not full snapshots — and the mirror reproduces everything
// else (AI/summon/inert turns) identically, because the core is deterministic.
//
//   C -> S  hello    { content, build }              handshake: hash + build
//   S -> C  welcome  { seat, seed, playerBuild,      admitted + the setup needed
//                      enemyBuild }                   to build an identical mirror
//   S -> C  error    { message }                     rejected (hash / build / ...)
//   S -> C  applied  { seat, intent }                one authoritative applied intent
//   S -> C  end      { }                             match over
//   C -> S  intent   { intent }                      one action for the active unit
//
#include "core/Entity.h" // Faction
#include "data/Json.h"
#include "data/Net.h" // Intent, serialize/parse

#include <optional>
#include <string>

namespace tb::net::proto {

inline std::string factionName(Faction f) { return f == Faction::Player ? "player" : "enemy"; }
inline std::optional<Faction> factionParse(const std::string& s) {
    if (s == "player") return Faction::Player;
    if (s == "enemy") return Faction::Enemy;
    return std::nullopt;
}

// --- builders (return a framed-ready JSON string) ---------------------------
inline std::string hello(const std::string& contentHash, const std::string& buildText,
                         const std::string& user = "", const std::string& pass = "") {
    json::Value o = json::Value::makeObject();
    o.set("type", "hello");
    o.set("content", contentHash);
    o.set("build", buildText);
    o.set("user", user); // empty for unranked/custom; the server authenticates only if ranked
    o.set("pass", pass);
    return json::dump(o, false);
}
inline std::string welcome(Faction seat, int seed, const std::string& playerBuild,
                           const std::string& enemyBuild) {
    json::Value o = json::Value::makeObject();
    o.set("type", "welcome");
    o.set("seat", factionName(seat));
    o.set("seed", seed);
    o.set("playerBuild", playerBuild);
    o.set("enemyBuild", enemyBuild);
    return json::dump(o, false);
}
inline std::string error(const std::string& message) {
    json::Value o = json::Value::makeObject();
    o.set("type", "error");
    o.set("message", message);
    return json::dump(o, false);
}
inline std::string applied(Faction seat, const Intent& in) {
    json::Value o = json::Value::makeObject();
    o.set("type", "applied");
    o.set("seat", factionName(seat));
    o.set("intent", serializeIntent(in));
    return json::dump(o, false);
}
inline std::string endMsg() {
    json::Value o = json::Value::makeObject();
    o.set("type", "end");
    return json::dump(o, false);
}
inline std::string intentMsg(const Intent& in) {
    json::Value o = json::Value::makeObject();
    o.set("type", "intent");
    o.set("intent", serializeIntent(in));
    return json::dump(o, false);
}

// --- parsing ----------------------------------------------------------------
struct Msg {
    std::string type;
    json::Value body;
    [[nodiscard]] std::string field(const char* key) const {
        const json::Value* v = body.find(key);
        return (v && v->isString()) ? v->asString() : std::string();
    }
    [[nodiscard]] int intField(const char* key, int def = 0) const {
        const json::Value* v = body.find(key);
        return (v && v->isNumber()) ? static_cast<int>(v->asNumber()) : def;
    }
};

inline std::optional<Msg> parse(const std::string& text) {
    json::ParseResult pr = json::parse(text);
    if (!pr.ok || !pr.value.isObject()) return std::nullopt;
    const json::Value* t = pr.value.find("type");
    if (!t || !t->isString()) return std::nullopt;
    Msg m;
    m.type = t->asString();
    m.body = std::move(pr.value);
    return m;
}

} // namespace tb::net::proto
