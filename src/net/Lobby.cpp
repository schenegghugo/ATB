//
// Lobby.cpp — see Lobby.h.
//
#include "Lobby.h"

#include "GameServer.h" // MatchConfig, runAdmittedMatch, contentHashOf
#include "Protocol.h"   // proto::parse / Msg / helpers
#include "core/Build.h" // (de)serializeBuild, validateBuild
#include "data/Json.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tb::net {
namespace {

// --- MatchFormat <-> JSON ----------------------------------------------------
const char* timeName(MatchFormat::Time t) {
    switch (t) {
        case MatchFormat::Time::Unlimited: return "unlimited";
        case MatchFormat::Time::PerMove: return "permove";
        case MatchFormat::Time::Chess: return "chess";
    }
    return "permove";
}
json::Value formatToJson(const MatchFormat& f) {
    json::Value o = json::Value::makeObject();
    o.set("time", timeName(f.time));
    o.set("perMove", f.perMoveSec);
    o.set("main", f.mainSec);
    o.set("inc", f.incSec);
    o.set("rated", f.rated);
    o.set("team", f.teamSize);
    return o;
}
MatchFormat formatFromJson(const json::Value* o) {
    MatchFormat f;
    if (!o) return f;
    const std::string t = proto::strOf(*o, "time");
    f.time = t == "unlimited" ? MatchFormat::Time::Unlimited
             : t == "chess"   ? MatchFormat::Time::Chess
                              : MatchFormat::Time::PerMove;
    f.perMoveSec = proto::intOf(*o, "perMove", 30);
    f.mainSec = proto::intOf(*o, "main", 300);
    f.incSec = proto::intOf(*o, "inc", 5);
    const json::Value* r = o->find("rated");
    f.rated = r && r->isBool() && r->asBool();
    f.teamSize = proto::intOf(*o, "team", 1);
    return f;
}

// --- small reply builders ----------------------------------------------------
std::string ok() {
    json::Value o = json::Value::makeObject();
    o.set("type", "ok");
    return json::dump(o, false);
}
std::string err(const std::string& msg) { return proto::error(msg); }
std::string pairedMsg(const std::string& token, Faction seat, bool rated) {
    json::Value o = json::Value::makeObject();
    o.set("type", "paired");
    o.set("token", token);
    o.set("seat", proto::factionName(seat));
    o.set("rated", rated);
    return json::dump(o, false);
}
// The same object, minus the "type", enqueued as an async event.
json::Value pairedEvent(const std::string& token, Faction seat, bool rated) {
    json::Value o = json::Value::makeObject();
    o.set("kind", "paired");
    o.set("token", token);
    o.set("seat", proto::factionName(seat));
    o.set("rated", rated);
    return o;
}

// --- server state ------------------------------------------------------------
struct Seek {
    int id = 0;
    std::string user;
    int rating = kDefaultRating;
    MatchFormat fmt;
    CharacterBuild build;
    bool guest = false;
};
struct Challenge {
    int id = 0;
    std::string from, to;
    int fromRating = kDefaultRating;
    MatchFormat fmt;
    CharacterBuild fromBuild;
    bool fromGuest = false;
};
// A confirmed pairing waiting for both players to open their match conn.
struct Pairing {
    MatchFormat fmt;
    std::string tokenP, tokenE, userP, userE;
    CharacterBuild buildP, buildE;
    Connection connP, connE;
    bool haveP = false, haveE = false, started = false;
};

struct LobbyState {
    std::mutex mu;
    int nextId = 1;
    std::mt19937_64 rng{std::random_device{}()};
    std::vector<Seek> seeks;
    std::vector<Challenge> challenges;
    std::unordered_map<std::string, std::shared_ptr<Pairing>> byToken; // both tokens → pairing
    std::unordered_map<std::string, std::vector<json::Value>> events;  // user → async events

    std::string mintToken() { return "tk" + std::to_string(nextId++) + "-" + std::to_string(rng()); }
};

MatchConfig makeMatchConfig(const LobbyConfig& cfg, const MatchFormat& fmt) {
    MatchConfig mc;
    mc.ruleset = fmt.rated ? cfg.rankedRules : cfg.casualRules;
    mc.catalog = cfg.catalog;
    mc.creatures = cfg.creatures;
    mc.contentHash = cfg.contentHash;
    mc.accounts = fmt.rated ? cfg.accounts : nullptr;
    return mc;
}

// Reject a pairing whose format we can't honour. Empty string = ok.
std::string pairingError(const LobbyConfig& cfg, const MatchFormat& fmt, const CharacterBuild& a,
                         const CharacterBuild& b, bool aGuest, bool bGuest) {
    if (!fmt.live()) return "unlimited (correspondence) play is not available yet";
    if (fmt.rated && (!cfg.accounts || aGuest || bGuest))
        return "rated play needs both players logged in";
    const Ruleset& rs = fmt.rated ? cfg.rankedRules : cfg.casualRules;
    if (!validateBuild(a, cfg.catalog, rs.economy, rs.bannedSpells).ok)
        return "your build is illegal for this format";
    if (!validateBuild(b, cfg.catalog, rs.economy, rs.bannedSpells).ok)
        return "opponent's build is illegal for this format";
    return "";
}

// Create a pairing (initiator = Player, acceptor = Enemy), file the tokens, and
// enqueue the async `paired` event for the initiator. Returns the acceptor's reply.
// Caller holds st.mu.
std::string pairUp(LobbyState& st, const MatchFormat& fmt, const std::string& initUser,
                   const CharacterBuild& initBuild, const std::string& accUser,
                   const CharacterBuild& accBuild) {
    auto p = std::make_shared<Pairing>();
    p->fmt = fmt;
    p->tokenP = st.mintToken();
    p->tokenE = st.mintToken();
    p->userP = initUser;
    p->userE = accUser;
    p->buildP = initBuild;
    p->buildE = accBuild;
    st.byToken[p->tokenP] = p;
    st.byToken[p->tokenE] = p;
    st.events[initUser].push_back(pairedEvent(p->tokenP, Faction::Player, fmt.rated));
    return pairedMsg(p->tokenE, Faction::Enemy, fmt.rated);
}

// Drop a departed user's open seek + challenges + queued events.
void withdrawUser(LobbyState& st, const std::string& user) {
    std::lock_guard<std::mutex> lk(st.mu);
    auto& sk = st.seeks;
    sk.erase(std::remove_if(sk.begin(), sk.end(), [&](const Seek& s) { return s.user == user; }),
             sk.end());
    auto& ch = st.challenges;
    ch.erase(std::remove_if(ch.begin(), ch.end(),
                            [&](const Challenge& c) { return c.from == user || c.to == user; }),
             ch.end());
    st.events.erase(user);
}

// --- per-session request handling (server) ----------------------------------
void handleRequest(const proto::Msg& m, Connection& conn, const std::string& user, bool guest,
                   int rating, const LobbyConfig& cfg, LobbyState& st) {
    const std::string& t = m.type;

    if (t == "seek") {
        const MatchFormat fmt = formatFromJson(proto::objField(m, "format"));
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (!b) { conn.send(err("malformed build")); return; }
        if (fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        if (!fmt.live()) { conn.send(err("unlimited (correspondence) not available yet")); return; }
        const Ruleset& rs = fmt.rated ? cfg.rankedRules : cfg.casualRules;
        if (!validateBuild(*b, cfg.catalog, rs.economy, rs.bannedSpells).ok) {
            conn.send(err("build illegal for this format"));
            return;
        }
        std::lock_guard<std::mutex> lk(st.mu);
        st.seeks.erase(std::remove_if(st.seeks.begin(), st.seeks.end(),
                                      [&](const Seek& s) { return s.user == user; }),
                       st.seeks.end()); // one open seek per session
        st.seeks.push_back({st.nextId++, user, rating, fmt, *b, guest});
        conn.send(ok());
        return;
    }
    if (t == "cancelSeek") {
        std::lock_guard<std::mutex> lk(st.mu);
        st.seeks.erase(std::remove_if(st.seeks.begin(), st.seeks.end(),
                                      [&](const Seek& s) { return s.user == user; }),
                       st.seeks.end());
        conn.send(ok());
        return;
    }
    if (t == "listSeeks") {
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            for (const Seek& s : st.seeks) {
                json::Value o = json::Value::makeObject();
                o.set("id", s.id);
                o.set("user", s.user);
                o.set("rating", s.rating);
                o.set("format", formatToJson(s.fmt));
                list.push_back(std::move(o));
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "seeks");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "acceptSeek") {
        const int id = m.intField("id");
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (!b) { conn.send(err("malformed build")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = std::find_if(st.seeks.begin(), st.seeks.end(),
                               [&](const Seek& s) { return s.id == id; });
        if (it == st.seeks.end()) { conn.send(err("no such seek")); return; }
        if (it->user == user) { conn.send(err("can't accept your own seek")); return; }
        const std::string e = pairingError(cfg, it->fmt, it->build, *b, it->guest, guest);
        if (!e.empty()) { conn.send(err(e)); return; }
        const std::string reply = pairUp(st, it->fmt, it->user, it->build, user, *b);
        st.seeks.erase(it);
        conn.send(reply);
        return;
    }
    if (t == "challenge") {
        const std::string to = m.field("to");
        const MatchFormat fmt = formatFromJson(proto::objField(m, "format"));
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (to.empty() || !b) { conn.send(err("bad challenge")); return; }
        if (fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        if (!fmt.live()) { conn.send(err("unlimited (correspondence) not available yet")); return; }
        const Ruleset& rs = fmt.rated ? cfg.rankedRules : cfg.casualRules;
        if (!validateBuild(*b, cfg.catalog, rs.economy, rs.bannedSpells).ok) {
            conn.send(err("build illegal for this format"));
            return;
        }
        std::lock_guard<std::mutex> lk(st.mu);
        st.challenges.push_back({st.nextId++, user, to, rating, fmt, *b, guest});
        conn.send(ok());
        return;
    }
    if (t == "listChallenges") {
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            for (const Challenge& c : st.challenges) {
                if (c.to != user) continue;
                json::Value o = json::Value::makeObject();
                o.set("id", c.id);
                o.set("from", c.from);
                o.set("rating", c.fromRating);
                o.set("format", formatToJson(c.fmt));
                list.push_back(std::move(o));
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "challenges");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "acceptChallenge") {
        const int id = m.intField("id");
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (!b) { conn.send(err("malformed build")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = std::find_if(st.challenges.begin(), st.challenges.end(),
                               [&](const Challenge& c) { return c.id == id; });
        if (it == st.challenges.end() || it->to != user) { conn.send(err("no such challenge")); return; }
        const std::string e = pairingError(cfg, it->fmt, it->fromBuild, *b, it->fromGuest, guest);
        if (!e.empty()) { conn.send(err(e)); return; }
        const std::string reply = pairUp(st, it->fmt, it->from, it->fromBuild, user, *b);
        st.challenges.erase(it);
        conn.send(reply);
        return;
    }
    if (t == "decline") {
        const int id = m.intField("id");
        std::lock_guard<std::mutex> lk(st.mu);
        st.challenges.erase(std::remove_if(st.challenges.begin(), st.challenges.end(),
                                           [&](const Challenge& c) {
                                               return c.id == id && c.to == user;
                                           }),
                            st.challenges.end());
        conn.send(ok());
        return;
    }
    if (t == "poll") {
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            auto it = st.events.find(user);
            if (it != st.events.end()) {
                for (json::Value& ev : it->second) list.push_back(std::move(ev));
                it->second.clear();
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "events");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }
    conn.send(err("unknown request: " + t));
}

// A `login` connection: authenticate, then loop over lobby requests.
void sessionLoop(Connection conn, const proto::Msg& login, const LobbyConfig& cfg, LobbyState& st) {
    if (login.field("content") != cfg.contentHash) {
        conn.send(err("content hash mismatch"));
        return;
    }
    const std::string reqUser = login.field("user");
    const bool guest = reqUser.empty();
    AccountView acct;
    std::string user;
    if (guest) {
        std::lock_guard<std::mutex> lk(st.mu);
        user = "guest#" + std::to_string(st.nextId++);
    } else {
        if (!cfg.accounts) { conn.send(err("this server has no accounts (guest/casual only)")); return; }
        const AuthResult au = cfg.accounts->authenticate(reqUser, login.field("pass"));
        if (!au.ok) { conn.send(err("login: " + au.error)); return; }
        acct = au.account;
        user = reqUser;
    }

    // A lobby session idles between requests (a human browsing) — don't let the
    // accept-time read timeout evict it. Match conns keep the clock timeout.
    conn.setReadTimeout(0);

    json::Value hello = json::Value::makeObject();
    hello.set("type", "session");
    hello.set("user", user);
    hello.set("rating", acct.rating);
    hello.set("wins", acct.wins);
    hello.set("losses", acct.losses);
    conn.send(json::dump(hello, false));

    while (true) {
        const std::optional<std::string> raw = conn.recv();
        if (!raw) break; // clean disconnect / timeout
        const std::optional<proto::Msg> m = proto::parse(*raw);
        if (!m) { conn.send(err("unparseable request")); continue; }
        handleRequest(*m, conn, user, guest, acct.rating, cfg, st);
    }
    withdrawUser(st, user);
}

// A `joinMatch` connection: park it in its pairing; the second arrival runs the match.
void matchJoin(Connection conn, const proto::Msg& join, const LobbyConfig& cfg, LobbyState& st) {
    const std::string token = join.field("token");
    std::shared_ptr<Pairing> p;
    bool runNow = false;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.byToken.find(token);
        if (it == st.byToken.end()) { conn.send(err("unknown or expired match token")); return; }
        p = it->second;
        if (token == p->tokenP) { p->connP = std::move(conn); p->haveP = true; }
        else { p->connE = std::move(conn); p->haveE = true; }
        if (p->haveP && p->haveE && !p->started) {
            p->started = true;
            st.byToken.erase(p->tokenP);
            st.byToken.erase(p->tokenE);
            runNow = true;
        }
    }
    if (!runNow) return; // parked — the other side will drive the match

    const MatchConfig mc = makeMatchConfig(cfg, p->fmt);
    const int clock = p->fmt.time == MatchFormat::Time::PerMove ? p->fmt.perMoveSec : p->fmt.mainSec;
    p->connP.setReadTimeout(clock);
    p->connE.setReadTimeout(clock);
    const ServeResult r =
        runAdmittedMatch(std::move(p->connP), std::move(p->connE), p->buildP, p->buildE, mc);
    if (p->fmt.rated && cfg.accounts && r.ok && r.winner) {
        const bool playerWon = *r.winner == Faction::Player;
        cfg.accounts->recordResult(playerWon ? p->userP : p->userE,
                                   playerWon ? p->userE : p->userP);
    }
}

void handleConnection(Connection conn, const LobbyConfig& cfg, LobbyState& st) {
    const std::optional<std::string> raw = conn.recv();
    if (!raw) return;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) { conn.send(err("expected login or joinMatch")); return; }
    if (m->type == "login") sessionLoop(std::move(conn), *m, cfg, st);
    else if (m->type == "joinMatch") matchJoin(std::move(conn), *m, cfg, st);
    else conn.send(err("expected login or joinMatch, got: " + m->type));
}

} // namespace

void serveLobby(Listener& listener, const LobbyConfig& cfg, int maxConns, int readTimeoutSec) {
    LobbyState st;
    std::vector<std::thread> threads;
    int accepted = 0;
    while (maxConns < 0 || accepted < maxConns) {
        std::optional<Connection> conn = listener.accept();
        if (!conn) break; // listener closed
        conn->setReadTimeout(readTimeoutSec);
        ++accepted;
        std::thread t(handleConnection, std::move(*conn), std::cref(cfg), std::ref(st));
        if (maxConns < 0) t.detach();
        else threads.push_back(std::move(t));
    }
    for (std::thread& t : threads)
        if (t.joinable()) t.join();
}

// --- client -----------------------------------------------------------------

std::optional<std::string> LobbySession::rpc(const std::string& request) {
    if (!conn_.send(request)) return std::nullopt;
    return conn_.recv();
}

std::unique_ptr<LobbySession> LobbySession::connect(const std::string& host, uint16_t port,
                                                    const std::string& contentHash,
                                                    const std::string& user, const std::string& pass,
                                                    std::string* error, int readTimeoutSec) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<LobbySession> {
        if (error) *error = e;
        return nullptr;
    };
    std::optional<Connection> conn = Connection::connect(host, port);
    if (!conn) return fail("connect failed");
    conn->setReadTimeout(readTimeoutSec);

    json::Value o = json::Value::makeObject();
    o.set("type", "login");
    o.set("content", contentHash);
    o.set("user", user);
    o.set("pass", pass);
    if (!conn->send(json::dump(o, false))) return fail("sending login failed");

    const std::optional<std::string> raw = conn->recv();
    if (!raw) return fail("no server reply to login");
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) return fail("unparseable server reply");
    if (m->type == "error") return fail(m->field("message"));
    if (m->type != "session") return fail("unexpected reply: " + m->type);

    std::unique_ptr<LobbySession> s(new LobbySession(std::move(*conn)));
    s->guest_ = user.empty();
    s->acct_.user = m->field("user");
    s->acct_.rating = m->intField("rating", kDefaultRating);
    s->acct_.wins = m->intField("wins");
    s->acct_.losses = m->intField("losses");
    return s;
}

namespace {
// Parse a `paired` object/message into PairedInfo.
std::optional<PairedInfo> parsePaired(const proto::Msg& m, std::string* error) {
    if (m.type == "error") {
        if (error) *error = m.field("message");
        return std::nullopt;
    }
    PairedInfo pi;
    pi.token = m.field("token");
    const std::optional<Faction> seat = proto::factionParse(m.field("seat"));
    if (pi.token.empty() || !seat) {
        if (error) *error = "malformed pairing reply";
        return std::nullopt;
    }
    pi.seat = *seat;
    const json::Value* r = m.body.find("rated");
    pi.rated = r && r->isBool() && r->asBool();
    return pi;
}
} // namespace

bool LobbySession::seek(const MatchFormat& fmt, const CharacterBuild& build, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "seek");
    o.set("format", formatToJson(fmt));
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

bool LobbySession::cancelSeek() {
    json::Value o = json::Value::makeObject();
    o.set("type", "cancelSeek");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

std::optional<std::vector<SeekInfo>> LobbySession::listSeeks() {
    json::Value o = json::Value::makeObject();
    o.set("type", "listSeeks");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "seeks") return std::nullopt;
    std::vector<SeekInfo> out;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray()) {
            SeekInfo s;
            s.id = proto::intOf(e, "id");
            s.user = proto::strOf(e, "user");
            s.rating = proto::intOf(e, "rating", kDefaultRating);
            s.format = formatFromJson(e.find("format"));
            out.push_back(std::move(s));
        }
    return out;
}

std::optional<PairedInfo> LobbySession::acceptSeek(int seekId, const CharacterBuild& build,
                                                   std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptSeek");
    o.set("id", seekId);
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) { if (error) *error = "bad reply"; return std::nullopt; }
    return parsePaired(*m, error);
}

bool LobbySession::challenge(const std::string& toUser, const MatchFormat& fmt,
                             const CharacterBuild& build, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "challenge");
    o.set("to", toUser);
    o.set("format", formatToJson(fmt));
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

std::optional<std::vector<ChallengeInfo>> LobbySession::listChallenges() {
    json::Value o = json::Value::makeObject();
    o.set("type", "listChallenges");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "challenges") return std::nullopt;
    std::vector<ChallengeInfo> out;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray()) {
            ChallengeInfo c;
            c.id = proto::intOf(e, "id");
            c.from = proto::strOf(e, "from");
            c.fromRating = proto::intOf(e, "rating", kDefaultRating);
            c.format = formatFromJson(e.find("format"));
            out.push_back(std::move(c));
        }
    return out;
}

std::optional<PairedInfo> LobbySession::acceptChallenge(int id, const CharacterBuild& build,
                                                        std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptChallenge");
    o.set("id", id);
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) { if (error) *error = "bad reply"; return std::nullopt; }
    return parsePaired(*m, error);
}

bool LobbySession::declineChallenge(int id) {
    json::Value o = json::Value::makeObject();
    o.set("type", "decline");
    o.set("id", id);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

std::optional<PairedInfo> LobbySession::poll() {
    json::Value o = json::Value::makeObject();
    o.set("type", "poll");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "events") return std::nullopt;
    const json::Value* list = m->body.find("list");
    if (!list || !list->isArray()) return std::nullopt;
    for (const json::Value& e : list->asArray()) {
        if (proto::strOf(e, "kind") != "paired") continue;
        PairedInfo pi;
        pi.token = proto::strOf(e, "token");
        const std::optional<Faction> seat = proto::factionParse(proto::strOf(e, "seat"));
        if (pi.token.empty() || !seat) continue;
        pi.seat = *seat;
        const json::Value* r = e.find("rated");
        pi.rated = r && r->isBool() && r->asBool();
        return pi;
    }
    return std::nullopt;
}

} // namespace tb::net
