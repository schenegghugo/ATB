//
// Replay.cpp — see Replay.h.
//
#include "Replay.h"

#include "MatchRunner.h"
#include "core/Battle.h"
#include "core/Match.h"
#include "data/Base64.h"
#include "data/CatalogJson.h"
#include "data/Sha256.h"

#include <cstdlib>
#include <sstream>

namespace tb::replay {
namespace {

// --- terse intent token <-> Intent -----------------------------------------
std::string intentToken(const net::Intent& in) {
    switch (in.kind) {
        case net::Intent::Kind::Move:
            return "m" + std::to_string(in.target.x) + "," + std::to_string(in.target.y);
        case net::Intent::Kind::Cast:
            return "c" + std::to_string(in.spellIdx) + "@" + std::to_string(in.target.x) + "," +
                   std::to_string(in.target.y);
        case net::Intent::Kind::EndTurn:
            return ".";
    }
    return ".";
}

// Parse "x,y" -> Vec2i (x may be negative). Returns false on malformed.
bool parseXY(const std::string& s, Vec2i& out) {
    const auto comma = s.find(',');
    if (comma == std::string::npos) return false;
    char* end = nullptr;
    const long x = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() + comma) return false;
    const std::string ys = s.substr(comma + 1);
    const long y = std::strtol(ys.c_str(), &end, 10);
    if (ys.empty() || *end != '\0') return false;
    out = {static_cast<int>(x), static_cast<int>(y)};
    return true;
}

bool parseIntentToken(const std::string& t, net::Intent& out) {
    if (t == ".") { out = net::Intent::endTurn(); return true; }
    if (t.size() >= 2 && t[0] == 'm') {
        Vec2i tgt;
        if (!parseXY(t.substr(1), tgt)) return false;
        out = net::Intent::move(tgt);
        return true;
    }
    if (t.size() >= 4 && t[0] == 'c') {
        const auto at = t.find('@');
        if (at == std::string::npos) return false;
        char* end = nullptr;
        const long slot = std::strtol(t.c_str() + 1, &end, 10);
        if (end != t.c_str() + at) return false;
        Vec2i tgt;
        if (!parseXY(t.substr(at + 1), tgt)) return false;
        out = net::Intent::cast(static_cast<int>(slot), tgt);
        return true;
    }
    return false;
}

} // namespace

std::string catalogHash(const SpellCatalog& catalog) {
    return sha256Hex(serializeCatalog(catalog, "match"));
}

std::string serializeRecord(const GameRecord& rec) {
    std::ostringstream os;
    os << "ATB" << rec.version << ' ' << rec.catalogHash << ' ' << rec.seed << ' '
       << base64::encode(serializeBuild(rec.player)) << ' '
       << base64::encode(serializeBuild(rec.enemy));
    for (const net::Intent& in : rec.intents) os << ' ' << intentToken(in);
    return os.str();
}

RecordParse parseRecord(const std::string& text) {
    RecordParse r;
    std::istringstream is(text);
    std::vector<std::string> tok;
    for (std::string t; is >> t;) tok.push_back(t);
    if (tok.size() < 5) { r.error = "truncated record"; return r; }

    if (tok[0].rfind("ATB", 0) != 0) { r.error = "not an ATB record"; return r; }
    r.record.version = std::atoi(tok[0].c_str() + 3);
    r.record.catalogHash = tok[1];
    r.record.seed = static_cast<unsigned>(std::strtoul(tok[2].c_str(), nullptr, 10));

    const std::optional<std::string> pb = base64::decode(tok[3]);
    const std::optional<std::string> eb = base64::decode(tok[4]);
    if (!pb || !eb) { r.error = "malformed build payload"; return r; }
    const std::optional<CharacterBuild> p = deserializeBuild(*pb);
    const std::optional<CharacterBuild> e = deserializeBuild(*eb);
    if (!p || !e) { r.error = "unparseable build"; return r; }
    r.record.player = *p;
    r.record.enemy = *e;

    for (std::size_t i = 5; i < tok.size(); ++i) {
        net::Intent in;
        if (!parseIntentToken(tok[i], in)) { r.error = "bad intent token: " + tok[i]; return r; }
        r.record.intents.push_back(in);
    }
    r.ok = true;
    return r;
}

VerifyResult verify(const GameRecord& rec, const Ruleset& ruleset, const SpellCatalog& catalog,
                    const std::vector<Entity>& creatures) {
    VerifyResult res;

    if (rec.catalogHash != catalogHash(catalog)) { res.error = "catalog hash mismatch"; return res; }
    if (!validateBuild(rec.player, catalog, ruleset.economy, ruleset.bannedSpells).ok) {
        res.error = "illegal player build";
        return res;
    }
    if (!validateBuild(rec.enemy, catalog, ruleset.economy, ruleset.bannedSpells).ok) {
        res.error = "illegal enemy build";
        return res;
    }

    Battle battle = buildMatch(ruleset, {rec.player}, {rec.enemy}, catalog, rec.seed, creatures);
    net::MatchRunner runner(std::move(battle), net::Seat::Human, net::Seat::Human);

    // Apply the human intents in order; the runner supplies the seat + drives any
    // AI/summon/inert turns between them. Illegal intents are refused (no-op), so
    // they can't fabricate an outcome — the replayed winner is authoritative.
    for (const net::Intent& in : rec.intents) {
        if (runner.finished()) break;
        const std::optional<Faction> seat = runner.awaitingSeat();
        if (!seat) break;
        runner.submit(*seat, in);
    }

    if (!runner.finished()) { res.error = "record does not reach a conclusion"; return res; }
    res.ok = true;
    res.winner = runner.battle().winner();
    res.finalSnapshot = net::serializeSnapshot(net::snapshotOf(runner.battle()));
    return res;
}

} // namespace tb::replay
