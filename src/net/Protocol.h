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
#include <vector>

namespace tb::net::proto {

inline std::string factionName(Faction f) { return f == Faction::Player ? "player" : "enemy"; }
inline std::optional<Faction> factionParse(const std::string& s) {
    if (s == "player") return Faction::Player;
    if (s == "enemy") return Faction::Enemy;
    return std::nullopt;
}

// --- builders (return a framed-ready JSON string) ---------------------------
inline std::string hello(const std::string& contentHash, const std::string& buildText,
                         const std::string& user = "", const std::string& pass = "",
                         const std::string& lobby = "") {
    json::Value o = json::Value::makeObject();
    o.set("type", "hello");
    o.set("content", contentHash);
    o.set("build", buildText);
    o.set("user", user);   // empty for unranked/custom; the server authenticates only if ranked
    o.set("pass", pass);
    o.set("lobby", lobby); // empty = open matchmaking; a shared code = private match (unrated)
    return json::dump(o, false);
}
inline std::string welcome(Faction seat, int seed, const std::string& playerBuild,
                           const std::string& enemyBuild, int clockSec = 0, int mainSec = 0,
                           int incSec = 0) {
    json::Value o = json::Value::makeObject();
    o.set("type", "welcome");
    o.set("seat", factionName(seat));
    o.set("seed", seed);
    o.set("playerBuild", playerBuild);
    o.set("enemyBuild", enemyBuild);
    o.set("clockSec", clockSec); // per-move idle window (0 = no clock, e.g. correspondence)
    o.set("mainSec", mainSec);   // chess bank per seat (0 = not a chess clock)
    o.set("incSec", incSec);     // chess increment per completed turn
    return json::dump(o, false);
}
// Team match setup (NvN): like welcome, but carries BOTH full rosters (arrays of
// serialized builds, by seat index) so the client mirrors the identical N-champion
// Battle, plus which champion within its faction this client pilots (controllerSeat).
inline std::string welcomeTeam(Faction seat, int controllerSeat, int seed,
                               const std::vector<std::string>& playerTeam,
                               const std::vector<std::string>& enemyTeam, int clockSec = 0,
                               int mainSec = 0, int incSec = 0) {
    json::Value o = json::Value::makeObject();
    o.set("type", "welcomeTeam");
    o.set("seat", factionName(seat));
    o.set("controllerSeat", controllerSeat);
    o.set("seed", seed);
    json::Value pt = json::Value::makeArray(), et = json::Value::makeArray();
    for (const std::string& b : playerTeam) pt.push_back(b);
    for (const std::string& b : enemyTeam) et.push_back(b);
    o.set("playerTeam", std::move(pt));
    o.set("enemyTeam", std::move(et));
    o.set("clockSec", clockSec);
    o.set("mainSec", mainSec);
    o.set("incSec", incSec);
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
// Chess-clock variant: every applied intent also carries both seats' authoritative
// remaining banks (seconds, fractional), so the mirrors' clocks never drift.
inline std::string applied(Faction seat, const Intent& in, double clockP, double clockE) {
    json::Value o = json::Value::makeObject();
    o.set("type", "applied");
    o.set("seat", factionName(seat));
    o.set("intent", serializeIntent(in));
    o.set("clockP", clockP);
    o.set("clockE", clockE);
    return json::dump(o, false);
}
// Match over. A normal end (someone died) carries no winner — the mirror derives it
// from the battle. A FORFEIT (idle clock / disconnect) has no death to infer from,
// so it carries the authoritative `winner` + a `forfeit` flag for the client.
inline std::string endMsg(std::optional<Faction> winner = std::nullopt, bool forfeit = false) {
    json::Value o = json::Value::makeObject();
    o.set("type", "end");
    if (winner) o.set("winner", factionName(*winner));
    if (forfeit) o.set("forfeit", true);
    return json::dump(o, false);
}
inline std::string intentMsg(const Intent& in) {
    json::Value o = json::Value::makeObject();
    o.set("type", "intent");
    o.set("intent", serializeIntent(in));
    return json::dump(o, false);
}
// In-match chat. A client sends {text}; the server re-broadcasts it tagged with the
// sender's seat so both mirrors show who spoke.
inline std::string chatMsg(const std::string& text, std::optional<Faction> seat = std::nullopt) {
    json::Value o = json::Value::makeObject();
    o.set("type", "chat");
    if (seat) o.set("seat", factionName(*seat));
    o.set("text", text);
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
    [[nodiscard]] double numField(const char* key, double def = 0.0) const {
        const json::Value* v = body.find(key);
        return (v && v->isNumber()) ? v->asNumber() : def;
    }
    [[nodiscard]] bool has(const char* key) const { return body.find(key) != nullptr; }
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

// Read a nested object member of a parsed Msg body (e.g. a "format" block). Returns
// nullptr if absent / not an object.
inline const json::Value* objField(const Msg& m, const char* key) {
    const json::Value* v = m.body.find(key);
    return (v && v->isObject()) ? v : nullptr;
}
inline int intOf(const json::Value& o, const char* key, int def = 0) {
    const json::Value* v = o.find(key);
    return (v && v->isNumber()) ? static_cast<int>(v->asNumber()) : def;
}
inline std::string strOf(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return (v && v->isString()) ? v->asString() : std::string();
}

} // namespace tb::net::proto
