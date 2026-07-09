//
// MirrorSession.cpp — see MirrorSession.h.
//
#include "MirrorSession.h"

#include "Protocol.h"
#include "core/Match.h" // buildMatch

#include <algorithm>

namespace tb::net {

std::string MirrorSession::proto_intent(const Intent& in) { return proto::intentMsg(in); }
std::string MirrorSession::proto_chat(const std::string& text) { return proto::chatMsg(text); }

std::unique_ptr<MirrorSession>
MirrorSession::fromWelcome(Connection conn, const Ruleset& ruleset, const SpellCatalog& catalog,
                           const std::vector<Entity>& creatures, std::string* error) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<MirrorSession> {
        if (error) *error = e;
        return nullptr;
    };
    const std::optional<std::string> first = conn.recv();
    if (!first) return fail("no server reply to handshake");
    const std::optional<proto::Msg> m = proto::parse(*first);
    if (!m) return fail("unparseable server reply");
    if (m->type == "error") return fail(m->field("message"));
    if (m->type != "welcome") return fail("unexpected reply: " + m->type);

    const std::optional<Faction> seat = proto::factionParse(m->field("seat"));
    if (!seat) return fail("bad seat in welcome");
    const int seed = m->intField("seed", 0);
    const std::optional<CharacterBuild> pB = deserializeBuild(m->field("playerBuild"));
    const std::optional<CharacterBuild> eB = deserializeBuild(m->field("enemyBuild"));
    if (!pB || !eB) return fail("bad setup builds in welcome");

    // Build the identical initial Battle the server built, and mirror its runner.
    Battle battle =
        buildMatch(ruleset, {*pB}, {*eB}, catalog, static_cast<unsigned>(seed), creatures);
    MatchRunner runner(std::move(battle), Seat::Human, Seat::Human);
    std::unique_ptr<MirrorSession> s(
        new MirrorSession(std::move(conn), std::move(runner), *seat, m->intField("clockSec", 0)));
    s->mainSec_ = m->intField("mainSec", 0);
    s->incSec_ = m->intField("incSec", 0);
    s->bankP_ = s->bankE_ = static_cast<float>(s->mainSec_);
    s->bankStamp_ = std::chrono::steady_clock::now();
    return s;
}

float MirrorSession::bankSeconds(Faction f) const {
    float v = f == Faction::Player ? bankP_ : bankE_;
    // Only the seat currently deciding burns time; the server refreshes both banks
    // on every applied intent, so this local tick never drifts far.
    if (!ended_ && !runner_.finished() && runner_.awaitingSeat() == f)
        v -= std::chrono::duration<float>(std::chrono::steady_clock::now() - bankStamp_).count();
    return std::max(0.0f, v);
}

std::unique_ptr<MirrorSession>
MirrorSession::connect(const std::string& host, uint16_t port, const std::string& contentHash,
                       const CharacterBuild& build, const Ruleset& ruleset,
                       const SpellCatalog& catalog, const std::vector<Entity>& creatures,
                       std::string* error, int readTimeoutSec, const std::string& user,
                       const std::string& pass, const std::string& lobby) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<MirrorSession> {
        if (error) *error = e;
        return nullptr;
    };
    std::optional<Connection> conn = Connection::connect(host, port);
    if (!conn) return fail("connect failed");
    conn->setReadTimeout(readTimeoutSec);
    if (!conn->send(proto::hello(contentHash, serializeBuild(build), user, pass, lobby)))
        return fail("sending hello failed");
    return fromWelcome(std::move(*conn), ruleset, catalog, creatures, error);
}

std::unique_ptr<MirrorSession>
MirrorSession::joinToken(const std::string& host, uint16_t port, const std::string& token,
                         const Ruleset& ruleset, const SpellCatalog& catalog,
                         const std::vector<Entity>& creatures, std::string* error,
                         int readTimeoutSec) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<MirrorSession> {
        if (error) *error = e;
        return nullptr;
    };
    std::optional<Connection> conn = Connection::connect(host, port);
    if (!conn) return fail("connect failed");
    conn->setReadTimeout(readTimeoutSec);
    json::Value o = json::Value::makeObject();
    o.set("type", "joinMatch");
    o.set("token", token);
    if (!conn->send(json::dump(o, false))) return fail("sending join token failed");
    return fromWelcome(std::move(*conn), ruleset, catalog, creatures, error);
}

bool MirrorSession::pump(int timeoutMs) {
    if (ended_) return false;
    bool first = true;
    while (conn_.waitReadable(first ? timeoutMs : 0)) {
        first = false;
        const std::optional<std::string> raw = conn_.recv();
        if (!raw) { ended_ = true; return false; }
        const std::optional<proto::Msg> m = proto::parse(*raw);
        if (!m) continue;
        if (m->type == "end") {
            ended_ = true;
            if (const json::Value* f = m->body.find("forfeit"); f && f->isBool() && f->asBool())
                forfeitWinner_ = proto::factionParse(m->field("winner"));
            return false;
        }
        if (m->type == "applied") {
            const std::optional<Faction> seat = proto::factionParse(m->field("seat"));
            const Parse<Intent> in = parseIntent(m->field("intent"));
            if (seat && in.ok) runner_.submit(*seat, in.value); // authoritative replay
            if (m->has("clockP")) { // chess mode: adopt the authoritative banks
                bankP_ = static_cast<float>(m->numField("clockP"));
                bankE_ = static_cast<float>(m->numField("clockE"));
                bankStamp_ = std::chrono::steady_clock::now();
            }
        }
        if (m->type == "chat") {
            const std::optional<Faction> seat = proto::factionParse(m->field("seat"));
            if (seat) chat_.push_back({*seat, m->field("text")});
        }
    }
    return !ended_;
}

} // namespace tb::net
