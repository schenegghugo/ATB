//
// Replay.cpp — see Replay.h.
//
#include "Replay.h"

#include "MatchRunner.h"
#include "core/Battle.h"
#include "core/Match.h"
#include "data/Base64.h"
#include "data/CatalogJson.h"
#include "data/RulesetJson.h"
#include "data/Sha256.h"

#include <cstdlib>
#include <sstream>

namespace tb::replay {
namespace {

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

} // namespace

// --- terse intent token <-> Intent (shared by the notation + the CR.6 wire) ---
std::string intentToken(const net::Intent& in) {
    switch (in.kind) {
        case net::Intent::Kind::Move:
            return "m" + std::to_string(in.target.x) + "," + std::to_string(in.target.y);
        case net::Intent::Kind::Cast: {
            std::string s = "c" + std::to_string(in.spellIdx) + "@" +
                            std::to_string(in.target.x) + "," + std::to_string(in.target.y);
            // A player-placed portal exit rides after a '>' (whitespace-free token).
            if (in.hasTarget2)
                s += ">" + std::to_string(in.target2.x) + "," + std::to_string(in.target2.y);
            return s;
        }
        case net::Intent::Kind::EndTurn:
            return ".";
    }
    return ".";
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
        const auto gt = t.find('>', at); // optional ">x2,y2" portal exit
        Vec2i entry;
        if (!parseXY(t.substr(at + 1, gt == std::string::npos ? std::string::npos : gt - at - 1),
                     entry))
            return false;
        if (gt == std::string::npos) {
            out = net::Intent::cast(static_cast<int>(slot), entry);
            return true;
        }
        Vec2i exit;
        if (!parseXY(t.substr(gt + 1), exit)) return false;
        out = net::Intent::castTo(static_cast<int>(slot), entry, exit);
        return true;
    }
    return false;
}

std::string catalogHash(const SpellCatalog& catalog) {
    return sha256Hex(serializeCatalog(catalog, "match"));
}

std::string rulesetHash(const Ruleset& ruleset) {
    return sha256Hex(serializeRuleset(ruleset, "match"));
}

std::string makeCommitment(const std::string& choice, const std::string& nonce) {
    return sha256Hex(choice + ":" + nonce);
}

std::string serializeRecord(const GameRecord& rec) {
    std::ostringstream os;
    os << "ATB" << rec.version << ' ' << rec.catalogHash << ' ' << rec.rulesetHash << ' '
       << rec.seed << ' ' << base64::encode(serializeBuild(rec.player)) << ' '
       << base64::encode(serializeBuild(rec.enemy));
    for (const net::Intent& in : rec.intents) os << ' ' << intentToken(in);
    // Decoy commitments trail the moves, one '#' token per decoy cast in order.
    for (const DecoyCommit& c : rec.commits) os << " #" << c.commit << ':' << c.choice << ':' << c.nonce;
    return os.str();
}

RecordParse parseRecord(const std::string& text) {
    RecordParse r;
    std::istringstream is(text);
    std::vector<std::string> tok;
    for (std::string t; is >> t;) tok.push_back(t);
    if (tok.size() < 6) { r.error = "truncated record"; return r; }

    if (tok[0].rfind("ATB", 0) != 0) { r.error = "not an ATB record"; return r; }
    r.record.version = std::atoi(tok[0].c_str() + 3);
    r.record.catalogHash = tok[1];
    r.record.rulesetHash = tok[2];
    r.record.seed = static_cast<unsigned>(std::strtoul(tok[3].c_str(), nullptr, 10));

    const std::optional<std::string> pb = base64::decode(tok[4]);
    const std::optional<std::string> eb = base64::decode(tok[5]);
    if (!pb || !eb) { r.error = "malformed build payload"; return r; }
    const std::optional<CharacterBuild> p = deserializeBuild(*pb);
    const std::optional<CharacterBuild> e = deserializeBuild(*eb);
    if (!p || !e) { r.error = "unparseable build"; return r; }
    r.record.player = *p;
    r.record.enemy = *e;

    for (std::size_t i = 6; i < tok.size(); ++i) {
        if (tok[i].size() > 1 && tok[i][0] == '#') { // a decoy commitment token
            const std::string body = tok[i].substr(1);
            const auto c1 = body.find(':');
            const auto c2 = c1 == std::string::npos ? std::string::npos : body.find(':', c1 + 1);
            if (c2 == std::string::npos) { r.error = "bad commitment token: " + tok[i]; return r; }
            DecoyCommit dc{body.substr(0, c1), body.substr(c1 + 1, c2 - c1 - 1), body.substr(c2 + 1)};
            if (dc.commit.empty() || (dc.choice != "a" && dc.choice != "b") || dc.nonce.empty()) {
                r.error = "bad commitment token: " + tok[i];
                return r;
            }
            r.record.commits.push_back(std::move(dc));
            continue;
        }
        net::Intent in;
        if (!parseIntentToken(tok[i], in)) { r.error = "bad intent token: " + tok[i]; return r; }
        r.record.intents.push_back(in);
    }
    r.ok = true;
    return r;
}

VerifyResult verify(const GameRecord& rec, const Ruleset& ruleset, const SpellCatalog& catalog,
                    const std::vector<Entity>& creatures, bool requireCommitments) {
    VerifyResult res;

    if (rec.catalogHash != catalogHash(catalog)) { res.error = "catalog hash mismatch"; return res; }
    if (rec.rulesetHash != rulesetHash(ruleset)) { res.error = "ruleset hash mismatch"; return res; }
    if (!validateBuild(rec.player, catalog, ruleset.economy, ruleset.bannedSpells).ok) {
        res.error = "illegal player build";
        return res;
    }
    if (!validateBuild(rec.enemy, catalog, ruleset.economy, ruleset.bannedSpells).ok) {
        res.error = "illegal enemy build";
        return res;
    }

    // Every listed commitment must hash-verify, resolved or not.
    for (std::size_t i = 0; i < rec.commits.size(); ++i)
        if (makeCommitment(rec.commits[i].choice, rec.commits[i].nonce) != rec.commits[i].commit) {
            res.error = "commitment " + std::to_string(i) + " fails its hash";
            return res;
        }

    Battle battle = buildMatch(ruleset, {rec.player}, {rec.enemy}, catalog, rec.seed, creatures);
    net::MatchRunner runner(std::move(battle), net::Seat::Human, net::Seat::Human);

    // Cloak-pair bookkeeping: the Nth decoy cast is bound to the Nth commitment.
    // We diff live pairs across each intent — a new pair binds the next
    // commitment; a vanished pair was revealed either by the cast we just noted
    // (the acting member is the real one) or by expiry (the original, "a").
    constexpr std::size_t kUntracked = static_cast<std::size_t>(-1);
    struct OpenPair {
        EntityId a, b;
        std::size_t commitIdx;
    };
    std::vector<OpenPair> open;
    std::size_t decoyCasts = 0;

    // Apply the human intents in order; the runner supplies the seat + drives any
    // AI/summon/inert turns between them. Illegal intents are refused (no-op), so
    // they can't fabricate an outcome — the replayed winner is authoritative.
    for (const net::Intent& in : rec.intents) {
        if (runner.finished()) break;
        const std::optional<Faction> seat = runner.awaitingSeat();
        if (!seat) break;

        // If this cast comes from a cloaked member, note which member acts — a
        // successful cast reveals it as the real one.
        std::optional<std::size_t> actingPair;
        char actingMember = 'a';
        if (in.kind == net::Intent::Kind::Cast) {
            const EntityId actor = runner.battle().activeUnit();
            for (std::size_t i = 0; i < open.size(); ++i)
                if (open[i].a == actor || open[i].b == actor) {
                    actingPair = i;
                    actingMember = open[i].a == actor ? 'a' : 'b';
                }
        }

        runner.submit(*seat, in);
        const std::vector<CloakPair>& live = runner.battle().cloakPairs();
        auto isLive = [&](const OpenPair& p) {
            for (const CloakPair& lp : live)
                if (lp.a == p.a && lp.b == p.b) return true;
            return false;
        };

        // Vanished pairs: check the reveal against the bound commitment.
        std::vector<OpenPair> stillOpen;
        for (std::size_t i = 0; i < open.size(); ++i) {
            if (isLive(open[i])) { stillOpen.push_back(open[i]); continue; }
            const char actual = (actingPair && *actingPair == i) ? actingMember : 'a';
            if (open[i].commitIdx != kUntracked &&
                rec.commits[open[i].commitIdx].choice != std::string(1, actual)) {
                res.error = "decoy reveal contradicts its commitment (committed \"" +
                            rec.commits[open[i].commitIdx].choice + "\", revealed \"" +
                            std::string(1, actual) + "\")";
                return res;
            }
        }
        open = std::move(stillOpen);

        // New pairs (cloaks_ is append-only, so creation order is preserved).
        for (const CloakPair& lp : live) {
            bool known = false;
            for (const OpenPair& p : open)
                if (p.a == lp.a && p.b == lp.b) { known = true; break; }
            if (known) continue;
            if (decoyCasts >= rec.commits.size()) {
                if (requireCommitments) {
                    res.error = "decoy cast " + std::to_string(decoyCasts) + " has no commitment";
                    return res;
                }
                open.push_back({lp.a, lp.b, kUntracked});
            } else {
                open.push_back({lp.a, lp.b, decoyCasts});
            }
            ++decoyCasts;
        }
    }

    if (decoyCasts < rec.commits.size()) {
        res.error = "more commitments than decoy casts";
        return res;
    }
    if (!runner.finished()) { res.error = "record does not reach a conclusion"; return res; }
    // Pairs still cloaked at the end never resolved — their choice is exempt
    // (the hidden bit never influenced the game); their hashes were checked above.
    res.ok = true;
    res.winner = runner.battle().winner();
    res.finalSnapshot = net::serializeSnapshot(net::snapshotOf(runner.battle()));
    return res;
}

} // namespace tb::replay
