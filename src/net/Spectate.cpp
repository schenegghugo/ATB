//
// Spectate.cpp — see Spectate.h.
//
#include "Spectate.h"

#include "Protocol.h"
#include "core/Build.h" // deserializeBuild
#include "core/Match.h" // buildMatch
#include "data/Net.h"   // Intent, parseIntent

namespace tb::net {

SpectatorMirror::SpectatorMirror(Ruleset ruleset, SpellCatalog catalog,
                                 std::vector<Entity> creatures)
    : ruleset_(std::move(ruleset)), catalog_(std::move(catalog)),
      creatures_(std::move(creatures)) {}

bool SpectatorMirror::feed(const std::string& msg) {
    const std::optional<proto::Msg> m = proto::parse(msg);
    if (!m) return false;
    if (m->type == "welcome") {
        const int seed = m->intField("seed", 0);
        const std::optional<CharacterBuild> pB = deserializeBuild(m->field("playerBuild"));
        const std::optional<CharacterBuild> eB = deserializeBuild(m->field("enemyBuild"));
        if (!pB || !eB) return false;
        Battle battle =
            buildMatch(ruleset_, {*pB}, {*eB}, catalog_, static_cast<unsigned>(seed), creatures_);
        runner_ = std::make_unique<MatchRunner>(std::move(battle), Seat::Human, Seat::Human);
        return true;
    }
    if (!runner_) return false;
    if (m->type == "applied") {
        const std::optional<Faction> seat = proto::factionParse(m->field("seat"));
        const Parse<Intent> in = parseIntent(m->field("intent"));
        if (!seat || !in.ok) return false;
        runner_->submit(*seat, in.value); // authoritative replay
        return true;
    }
    if (m->type == "end") {
        ended_ = true;
        if (const json::Value* f = m->body.find("forfeit"); f && f->isBool() && f->asBool())
            forfeitWinner_ = proto::factionParse(m->field("winner"));
        return true;
    }
    return false;
}

} // namespace tb::net
