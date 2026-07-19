//
// Lobby.cpp — see Lobby.h.
//
#include "Lobby.h"

#include "Arbiter.h"     // correspondence ranking
#include "GameServer.h"  // MatchConfig, runAdmittedMatch, contentHashOf
#include "MailboxRelay.h" // Mailbox (correspondence move log)
#include "Protocol.h"    // proto::parse / Msg / helpers
#include "core/Build.h"  // (de)serializeBuild, validateBuild
#include "data/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
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
// The shared "paired" payload (a live token, or a correspondence setup). The reply
// tags it "type", the async event tags it "kind" — otherwise identical.
json::Value livePairedObj(const std::string& token, Faction seat, bool rated) {
    json::Value o = json::Value::makeObject();
    o.set("live", true);
    o.set("token", token);
    o.set("seat", proto::factionName(seat));
    o.set("rated", rated);
    return o;
}
json::Value corrPairedObj(const std::string& game, unsigned seed, Faction seat, bool rated,
                          const CharacterBuild& bP, const CharacterBuild& bE) {
    json::Value o = json::Value::makeObject();
    o.set("live", false);
    o.set("game", game);
    o.set("seed", static_cast<int>(seed));
    o.set("seat", proto::factionName(seat));
    o.set("rated", rated);
    o.set("playerBuild", serializeBuild(bP));
    o.set("enemyBuild", serializeBuild(bE));
    return o;
}
std::string asReply(json::Value obj) {
    obj.set("type", "paired");
    return json::dump(obj, false);
}
json::Value asEvent(json::Value obj) {
    obj.set("kind", "paired");
    return obj;
}

// A ready-check payload (reply tagged "type", event tagged "kind" by the caller).
json::Value readyCheckObj(int id, Faction seat, const std::string& opponent, bool rated,
                          const MatchFormat& fmt, int seconds) {
    json::Value o = json::Value::makeObject();
    o.set("id", id);
    o.set("seat", proto::factionName(seat));
    o.set("opponent", opponent);
    o.set("rated", rated);
    o.set("format", formatToJson(fmt));
    o.set("seconds", seconds);
    return o;
}
json::Value cancelledObj(const std::string& reason) {
    json::Value o = json::Value::makeObject();
    o.set("message", reason);
    return o;
}

// The snake draft order over 2N seats (N per team), laid out [A0..A(N-1), B0..B(N-1)].
// Alternates with the reversing "double pick" (A B B A A B …) so neither side holds
// both final informed picks. Returns seat indices into that layout. e.g. 2v2 →
// [0,2,3,1] (A0,B0,B1,A1); 3v3 → [0,3,4,1,2,5] (A0,B0,B1,A1,A2,B2).
std::vector<int> snakeOrder(int n) {
    std::vector<int> order;
    int aNext = 0, bNext = 0;
    for (int i = 0; i < 2 * n; ++i) {
        const bool teamA = (((i + 1) / 2) % 2) == 0; // 0→A 1→B 2→B 3→A 4→A 5→B …
        order.push_back(teamA ? aNext++ : n + bNext++);
    }
    return order;
}
std::string simpleMsg(const char* type) {
    json::Value o = json::Value::makeObject();
    o.set("type", type);
    return json::dump(o, false);
}
std::string notReadyMsg(const std::string& reason) {
    json::Value o = json::Value::makeObject();
    o.set("type", "notReady");
    o.set("message", reason);
    return json::dump(o, false);
}

// --- server state ------------------------------------------------------------
// Seeks/challenges carry only a FORMAT now — the build is chosen at the ready check.
struct Seek {
    int id = 0;
    std::string user;
    int rating = kDefaultRating;
    MatchFormat fmt;
    bool guest = false;
    std::vector<std::string> team; // full party roster for a team seek (empty = 1v1)
};
struct Challenge {
    int id = 0;
    std::string from, to;
    int fromRating = kDefaultRating;
    MatchFormat fmt;
    bool fromGuest = false;
    std::vector<std::string> team; // challenger's full party roster (empty = 1v1)
};
// A quick-match queue slot: paired automatically once another slot with the SAME
// format sits within the (time-widening) Elo band.
struct QueueEntry {
    std::string user;
    int rating = kDefaultRating;
    MatchFormat fmt;
    std::chrono::steady_clock::time_point since;
};
bool sameFormat(const MatchFormat& a, const MatchFormat& b) {
    return a.time == b.time && a.perMoveSec == b.perMoveSec && a.mainSec == b.mainSec &&
           a.incSec == b.incSec && a.rated == b.rated && a.teamSize == b.teamSize;
}
// A party: one side of a future NvN match. leader == members.front(); joiners append
// in seat order. Polled by members (no push) — membership is re-fetched via partyInfo.
struct Party {
    int id = 0;
    std::vector<std::string> members; // leader first, then joiners
};
// A pending party invite (leader → target). Withdrawn when either party disbands or
// the target/leader disconnects.
struct PartyInvite {
    int id = 0;
    int partyId = 0;
    std::string from; // the inviting leader
    std::string to;
};
// A pairing awaiting BOTH players to submit a build + READY within the window.
struct ReadyCheck {
    int id = 0;
    MatchFormat fmt;
    std::string userP, userE;
    CharacterBuild buildP, buildE;
    bool readyP = false, readyE = false;
    std::chrono::steady_clock::time_point deadline;
};
// One champion seat in a pending NvN draft (the build/locked fields are filled by the
// pick loop in slice 3).
struct DraftSeat {
    Faction faction = Faction::Player;
    int index = 0;
    std::string user;
    CharacterBuild build;
    bool locked = false;
};
// Two full parties DRAFTING their builds (Player = seek/challenge poster's party,
// Enemy = acceptor's party). Seats laid out [Player 0..N-1, Enemy 0..N-1]; pickOrder
// is the snake reveal order. The pick loop (currentPick / deadline) is slice 3.
struct DraftCheck {
    int id = 0;
    MatchFormat fmt;
    std::vector<DraftSeat> seats;
    std::vector<int> pickOrder;
    int currentPick = 0;                            // indexes pickOrder (the seat picking now)
    bool complete = false;                          // every seat locked → the match can form
    std::chrono::steady_clock::time_point deadline; // per-pick window
};
// A confirmed LIVE pairing waiting for both players to open their match conn.
struct Pairing {
    MatchFormat fmt;
    std::string tokenP, tokenE, userP, userE, gameId;
    CharacterBuild buildP, buildE;
    Connection connP, connE;
    bool haveP = false, haveE = false, started = false;
};
// A confirmed LIVE TEAM pairing (NvN, from a completed draft): 2N seats each with a
// join token + locked build, awaiting all 2N match conns (filled in slice 5). Seats are
// laid out [Player 0..N-1, Enemy 0..N-1]; the parallel vectors index by seat.
struct TeamPairing {
    MatchFormat fmt;
    std::string gameId;
    std::vector<DraftSeat> seats;    // faction/index/user/build (all locked)
    std::vector<std::string> tokens; // one per seat
    std::vector<Connection> conns;   // filled as players join (slice 5)
    std::vector<bool> have;          // arrival flags (slice 5)
    bool started = false;
};
// A live match in progress — listed by `listGames`, watchable via `watch`. Its
// broadcast stream (welcome/applied/end) is logged in the Mailbox under the game
// id, so a spectator is just another mirror replaying that log (Phase 5.2). A
// finished game stays here (`over`) so late watchers can still drain the log.
struct LiveGame {
    std::string userP, userE;
    bool rated = false;
    bool over = false;
};
// A CORRESPONDENCE game: setup + seat map, played async over the server Mailbox.
struct CorrGame {
    MatchFormat fmt;
    unsigned seed = 0;
    std::string userP, userE;
    CharacterBuild buildP, buildE;
};

struct LobbyState {
    std::mutex mu;
    int nextId = 1;
    std::mt19937_64 rng{std::random_device{}()};
    std::vector<Seek> seeks;
    std::vector<Challenge> challenges;
    std::vector<QueueEntry> queue; // quick-match queue (widening Elo band)
    std::unordered_map<int, Party> parties;       // party id → party (team play)
    std::unordered_map<std::string, int> partyOf; // user → party id (one party per user)
    std::vector<PartyInvite> partyInvites;        // pending party invites
    std::unordered_map<int, ReadyCheck> readyChecks;                   // id → pending ready check
    std::unordered_map<int, DraftCheck> drafts;                        // id → pending NvN draft
    std::unordered_map<std::string, LiveGame> liveGames;               // game id → live match
    std::unordered_map<std::string, std::shared_ptr<Pairing>> byToken; // both tokens → pairing
    std::unordered_map<std::string, std::shared_ptr<TeamPairing>> teamByToken; // token → team pairing
    std::unordered_map<std::string, CorrGame> corrGames;               // game id → correspondence
    std::unordered_map<std::string, std::vector<json::Value>> events;  // user → async events
    Mailbox mailbox;                        // correspondence move logs (per game id)
    std::unique_ptr<Arbiter> arbiter;       // rated-correspondence ranking (null on casual servers)
    int readySeconds = 30;                  // ready-check window (from LobbyConfig)
    int draftPickSec = 60;                  // per-pick draft window (from LobbyConfig)

    // Lobby-wide chat (4.6): a capped rolling log addressed by ABSOLUTE index
    // (base = the index of front(), so cursors stay valid across trimming), plus
    // each user's last-send stamp for the rate limit (lobby + correspondence).
    std::deque<MailEntry> lobbyChat;
    std::size_t lobbyChatBase = 0;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastChat;
    static constexpr std::size_t kLobbyChatCap = 200;

    std::string corrGamesPath; // persistence: corrGames snapshot file ("" = memory-only)

    std::string mintToken() { return "tk" + std::to_string(nextId++) + "-" + std::to_string(rng()); }
    std::string mintGameId() { return "cg" + std::to_string(nextId++) + "-" + std::to_string(rng()); }
};

// The 4.6 chat safety levers, shared by lobby and correspondence chat. Empty
// return = allowed (and the user's rate stamp is taken); else the rejection
// reason. Caller holds st.mu.
std::string chatGate(LobbyState& st, const LobbyConfig& cfg, const std::string& user,
                     const std::string& text) {
    if (text.empty()) return "empty message";
    if (static_cast<int>(text.size()) > cfg.chatMaxLen) return "message too long";
    if (std::find(cfg.chatMuted.begin(), cfg.chatMuted.end(), user) != cfg.chatMuted.end())
        return "you are muted on this server";
    const auto now = std::chrono::steady_clock::now();
    const auto it = st.lastChat.find(user);
    if (it != st.lastChat.end() &&
        std::chrono::duration<float>(now - it->second).count() < cfg.chatMinIntervalSec)
        return "sending messages too fast — slow down";
    st.lastChat[user] = now;
    return "";
}

// The side-log key for a correspondence game's chat: keeps the MOVE log (polled by
// CorrespondenceSession under the bare game id) free of non-move entries.
std::string corrChatKey(const std::string& game) { return game + "#chat"; }

// --- correspondence persistence ------------------------------------------------
// Snapshot the corrGames registry to disk (whole-file rewrite — it's small and
// changes only when a game is minted). Caller holds st.mu.
void saveCorrGames(const LobbyState& st) {
    if (st.corrGamesPath.empty()) return;
    json::Value list = json::Value::makeArray();
    for (const auto& [id, g] : st.corrGames) {
        json::Value o = json::Value::makeObject();
        o.set("id", id);
        o.set("format", formatToJson(g.fmt));
        o.set("seed", static_cast<int>(g.seed));
        o.set("userP", g.userP);
        o.set("userE", g.userE);
        o.set("buildP", serializeBuild(g.buildP));
        o.set("buildE", serializeBuild(g.buildE));
        list.push_back(std::move(o));
    }
    json::Value root = json::Value::makeObject();
    root.set("games", std::move(list));
    std::ofstream out(st.corrGamesPath, std::ios::trunc);
    out << json::dump(root, true) << '\n';
}

void loadCorrGames(LobbyState& st) {
    std::ifstream in(st.corrGamesPath);
    if (!in) return; // no snapshot yet — fresh server
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json::ParseResult pr = json::parse(text);
    if (!pr.ok || !pr.value.isObject()) return;
    const json::Value* list = pr.value.find("games");
    if (!list || !list->isArray()) return;
    for (const json::Value& o : list->asArray()) {
        const std::string id = proto::strOf(o, "id");
        const std::optional<CharacterBuild> bP = deserializeBuild(proto::strOf(o, "buildP"));
        const std::optional<CharacterBuild> bE = deserializeBuild(proto::strOf(o, "buildE"));
        if (id.empty() || !bP || !bE) continue;
        CorrGame g;
        g.fmt = formatFromJson(o.find("format"));
        g.seed = static_cast<unsigned>(proto::intOf(o, "seed"));
        g.userP = proto::strOf(o, "userP");
        g.userE = proto::strOf(o, "userE");
        g.buildP = *bP;
        g.buildE = *bE;
        st.corrGames[id] = std::move(g);
    }
}

MatchConfig makeMatchConfig(const LobbyConfig& cfg, const MatchFormat& fmt) {
    MatchConfig mc;
    mc.ruleset = fmt.rated ? cfg.rankedRules : cfg.casualRules;
    mc.catalog = cfg.catalog;
    mc.creatures = cfg.creatures;
    mc.contentHash = cfg.contentHash;
    mc.accounts = fmt.rated ? cfg.accounts : nullptr;
    return mc;
}

// Finalize a ready check into a real pairing (Player = userP, Enemy = userE), filling
// each seat's `paired` payload. A live format files two match tokens; an Unlimited
// format registers a correspondence game. Caller holds st.mu.
void makePairing(LobbyState& st, const MatchFormat& fmt, const std::string& userP,
                 const CharacterBuild& buildP, const std::string& userE,
                 const CharacterBuild& buildE, json::Value& pPaired, json::Value& ePaired) {
    if (fmt.live()) {
        auto p = std::make_shared<Pairing>();
        p->fmt = fmt;
        p->tokenP = st.mintToken();
        p->tokenE = st.mintToken();
        p->userP = userP;
        p->userE = userE;
        p->buildP = buildP;
        p->buildE = buildE;
        p->gameId = st.mintGameId(); // for the watch log + listGames
        st.byToken[p->tokenP] = p;
        st.byToken[p->tokenE] = p;
        pPaired = livePairedObj(p->tokenP, Faction::Player, fmt.rated);
        ePaired = livePairedObj(p->tokenE, Faction::Enemy, fmt.rated);
        return;
    }
    // Correspondence: mint a game + seed and hand both sides the full setup. A
    // non-zero seed keeps both mirrors' arenas identical (see runAdmittedMatch).
    const std::string game = st.mintGameId();
    const unsigned seed = static_cast<unsigned>(st.rng() % 1000000000u) + 1u;
    st.corrGames[game] = {fmt, seed, userP, userE, buildP, buildE};
    saveCorrGames(st); // persistence: a minted game survives a restart
    pPaired = corrPairedObj(game, seed, Faction::Player, fmt.rated, buildP, buildE);
    ePaired = corrPairedObj(game, seed, Faction::Enemy, fmt.rated, buildP, buildE);
}

// Open a ready check for a just-accepted seek/challenge (initiator = Player). Returns
// the acceptor's reply; enqueues the initiator's ready-check event. Caller holds mu.
std::string openReadyCheck(LobbyState& st, const MatchFormat& fmt, const std::string& initUser,
                           const std::string& accUser) {
    ReadyCheck rc;
    rc.id = st.nextId++;
    rc.fmt = fmt;
    rc.userP = initUser;
    rc.userE = accUser;
    const int secs = st.readySeconds;
    rc.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    const int id = rc.id;
    st.readyChecks[id] = std::move(rc);
    json::Value ev = readyCheckObj(id, Faction::Player, accUser, fmt.rated, fmt, secs);
    ev.set("kind", "readyCheck");
    st.events[initUser].push_back(std::move(ev));
    json::Value reply = readyCheckObj(id, Faction::Enemy, initUser, fmt.rated, fmt, secs);
    reply.set("type", "readyCheck");
    return json::dump(reply, false);
}

// Open a ready check for a QUEUE pairing (earlier-queued player = Player seat).
// Neither side sent the triggering request, so BOTH learn via poll events. Caller
// holds mu.
void openQueueReadyCheck(LobbyState& st, const MatchFormat& fmt, const QueueEntry& p,
                         const QueueEntry& e) {
    ReadyCheck rc;
    rc.id = st.nextId++;
    rc.fmt = fmt;
    rc.userP = p.user;
    rc.userE = e.user;
    const int secs = st.readySeconds;
    rc.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    const int id = rc.id;
    st.readyChecks[id] = std::move(rc);
    json::Value evP = readyCheckObj(id, Faction::Player, e.user, fmt.rated, fmt, secs);
    evP.set("kind", "readyCheck");
    st.events[p.user].push_back(std::move(evP));
    json::Value evE = readyCheckObj(id, Faction::Enemy, p.user, fmt.rated, fmt, secs);
    evE.set("kind", "readyCheck");
    st.events[e.user].push_back(std::move(evE));
}

// If `user` leads a party of EXACTLY `size`, return its roster; else nullopt with a
// reason in `why`. The gate for posting/accepting a team seek or challenge. Caller
// holds mu.
std::optional<std::vector<std::string>> fullPartyRoster(LobbyState& st, const std::string& user,
                                                        int size, std::string& why) {
    auto it = st.partyOf.find(user);
    if (it == st.partyOf.end()) { why = "you need a party for team play"; return std::nullopt; }
    const Party& p = st.parties[it->second];
    if (p.members.empty() || p.members.front() != user) {
        why = "only the party leader can do that";
        return std::nullopt;
    }
    if (static_cast<int>(p.members.size()) != size) {
        why = "your party needs exactly " + std::to_string(size) + " members for this format";
        return std::nullopt;
    }
    return p.members;
}

// A draft payload (reply tagged "type", event tagged "kind" by the caller), stamped
// with the recipient's own seat index.
json::Value draftInfoObj(const DraftCheck& dc, int mySeat) {
    json::Value o = json::Value::makeObject();
    o.set("id", dc.id);
    o.set("format", formatToJson(dc.fmt));
    o.set("mySeat", mySeat);
    json::Value seats = json::Value::makeArray();
    for (const DraftSeat& s : dc.seats) {
        json::Value so = json::Value::makeObject();
        so.set("faction", proto::factionName(s.faction));
        so.set("index", s.index);
        so.set("user", s.user);
        seats.push_back(std::move(so));
    }
    o.set("seats", std::move(seats));
    json::Value order = json::Value::makeArray();
    for (int i : dc.pickOrder) order.push_back(i);
    o.set("pickOrder", std::move(order));
    return o;
}

// Open a draft from two full rosters (teamA = Player, teamB = Enemy). Stamps every
// player's DraftInfo (their own mySeat); the acceptor (`replyUser`) gets it back as
// the reply, everyone else via a poll event. Returns the acceptor's reply. Caller
// holds mu.
std::string openDraftCheck(LobbyState& st, const MatchFormat& fmt,
                           const std::vector<std::string>& teamA,
                           const std::vector<std::string>& teamB, const std::string& replyUser) {
    DraftCheck dc;
    dc.id = st.nextId++;
    dc.fmt = fmt;
    const int n = static_cast<int>(teamA.size());
    for (int i = 0; i < n; ++i) dc.seats.push_back({Faction::Player, i, teamA[i], {}, false});
    for (int i = 0; i < n; ++i) dc.seats.push_back({Faction::Enemy, i, teamB[i], {}, false});
    dc.pickOrder = snakeOrder(n);
    dc.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(st.draftPickSec);
    std::string reply;
    for (int s = 0; s < static_cast<int>(dc.seats.size()); ++s) {
        json::Value info = draftInfoObj(dc, s);
        if (dc.seats[s].user == replyUser) {
            info.set("type", "draft");
            reply = json::dump(info, false);
        } else {
            info.set("kind", "draft");
            st.events[dc.seats[s].user].push_back(std::move(info));
        }
    }
    st.drafts[dc.id] = std::move(dc);
    return reply;
}

// Cancel + notify every seat of a pending draft (idempotent). Caller holds mu.
void cancelDraft(LobbyState& st, int id, const std::string& reason) {
    auto it = st.drafts.find(id);
    if (it == st.drafts.end()) return;
    for (const DraftSeat& s : it->second.seats) {
        json::Value ev = cancelledObj(reason);
        ev.set("kind", "cancelled");
        st.events[s.user].push_back(std::move(ev));
    }
    st.drafts.erase(it);
}

// Lock the current seat's build, REVEAL it (it's now in the polled state), and advance
// the snake pointer; the last lock marks the draft complete. Resets the per-pick
// deadline. Caller holds mu.
void advanceDraft(DraftCheck& dc, const CharacterBuild& build, int pickSec) {
    const int seat = dc.pickOrder[dc.currentPick];
    dc.seats[seat].build = build;
    dc.seats[seat].locked = true;
    ++dc.currentPick;
    if (dc.currentPick >= static_cast<int>(dc.pickOrder.size())) dc.complete = true;
    dc.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(pickSec);
}

// The per-player payload for a completed team draft: this seat's join token + faction +
// which champion (index within the faction) they pilot, plus BOTH rosters by seat index
// so the client can show the matchup. Tagged "kind":"paired" by the caller.
json::Value teamPairedObj(const TeamPairing& tp, int mySeat) {
    const DraftSeat& me = tp.seats[mySeat];
    json::Value o = json::Value::makeObject();
    o.set("live", true);
    o.set("token", tp.tokens[mySeat]);
    o.set("seat", proto::factionName(me.faction));
    o.set("controllerSeat", me.index);
    o.set("rated", tp.fmt.rated);
    json::Value pteam = json::Value::makeArray(), eteam = json::Value::makeArray();
    for (const DraftSeat& s : tp.seats) { // seats are in [Player…, Enemy…] index order
        if (s.faction == Faction::Player) pteam.push_back(serializeBuild(s.build));
        else eteam.push_back(serializeBuild(s.build));
    }
    o.set("playerTeam", std::move(pteam));
    o.set("enemyTeam", std::move(eteam));
    return o;
}

// Turn a COMPLETED draft into a live team pairing: mint a join token per seat, register
// the TeamPairing (slice 5 runs the 2N-conn match), hand every player their PairedInfo
// via a poll event, and drop the draft. Caller holds mu.
void finalizeDraft(LobbyState& st, int id) {
    auto it = st.drafts.find(id);
    if (it == st.drafts.end()) return;
    DraftCheck& dc = it->second;
    auto tp = std::make_shared<TeamPairing>();
    tp->fmt = dc.fmt;
    tp->gameId = st.mintGameId();
    tp->seats = dc.seats;
    const int total = static_cast<int>(dc.seats.size());
    tp->tokens.resize(static_cast<std::size_t>(total));
    tp->conns.resize(static_cast<std::size_t>(total));
    tp->have.assign(static_cast<std::size_t>(total), false);
    for (int s = 0; s < total; ++s) {
        tp->tokens[s] = st.mintToken();
        st.teamByToken[tp->tokens[s]] = tp;
    }
    for (int s = 0; s < total; ++s) {
        json::Value pi = teamPairedObj(*tp, s);
        pi.set("kind", "paired");
        st.events[dc.seats[s].user].push_back(std::move(pi));
    }
    st.drafts.erase(it);
}

// Auto-lock a minimal legal default (a bare Attack — legal under every shipped ruleset)
// for any draft whose current pick ran out of time. Lazy — called on poll/lock, like
// reapReadyChecks. Advances at most one seat per call (each pick keeps its own window);
// a pick that COMPLETES the draft finalizes it into a team pairing. Caller holds mu.
void reapDrafts(LobbyState& st) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> completed;
    for (auto& [id, dc] : st.drafts) {
        if (dc.complete || now < dc.deadline) continue;
        CharacterBuild dflt;
        dflt.name = "(auto)";
        dflt.spellIds = {spellid::Attack};
        advanceDraft(dc, dflt, st.draftPickSec);
        if (dc.complete) completed.push_back(id);
    }
    for (int id : completed) finalizeDraft(st, id);
}

// Pair queued players whose formats match and whose Elo gap fits inside the wider
// of the two time-widened bands (a long wait relaxes the match). Lazy — run on
// queue joins and polls, like reapReadyChecks. Caller holds mu.
void matchQueue(LobbyState& st, const LobbyConfig& cfg) {
    const auto now = std::chrono::steady_clock::now();
    auto bandOf = [&](const QueueEntry& q) {
        const double waited = std::chrono::duration<double>(now - q.since).count();
        return cfg.queueBandStart + static_cast<int>(cfg.queueBandPerSec * waited);
    };
    bool matched = true;
    while (matched) {
        matched = false;
        for (std::size_t i = 0; i < st.queue.size() && !matched; ++i)
            for (std::size_t j = i + 1; j < st.queue.size() && !matched; ++j) {
                const QueueEntry& a = st.queue[i];
                const QueueEntry& b = st.queue[j];
                if (!sameFormat(a.fmt, b.fmt)) continue;
                if (std::abs(a.rating - b.rating) > std::max(bandOf(a), bandOf(b))) continue;
                const QueueEntry first = a.since <= b.since ? a : b;
                const QueueEntry second = a.since <= b.since ? b : a;
                openQueueReadyCheck(st, a.fmt, first, second);
                st.queue.erase(st.queue.begin() + static_cast<std::ptrdiff_t>(j));
                st.queue.erase(st.queue.begin() + static_cast<std::ptrdiff_t>(i));
                matched = true;
            }
    }
}

// Cancel + notify both participants (idempotent). Caller holds mu.
void cancelReadyCheck(LobbyState& st, int id, const std::string& reason) {
    auto it = st.readyChecks.find(id);
    if (it == st.readyChecks.end()) return;
    for (const std::string& u : {it->second.userP, it->second.userE}) {
        json::Value ev = cancelledObj(reason);
        ev.set("kind", "cancelled");
        st.events[u].push_back(std::move(ev));
    }
    st.readyChecks.erase(it);
}

// Drop any ready checks whose window elapsed (lazy — called on ready/poll). Caller
// holds mu.
void reapReadyChecks(LobbyState& st) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired;
    for (const auto& [id, rc] : st.readyChecks)
        if (now >= rc.deadline) expired.push_back(id);
    for (int id : expired) cancelReadyCheck(st, id, "ready check timed out");
}

// Remove `user` from their party. The LEADER (members.front()) leaving DISBANDS the
// party for everyone and withdraws its pending invites; a member just drops out.
// Does NOT touch invites addressed TO `user` (those stay actionable). Caller holds mu.
void leaveParty(LobbyState& st, const std::string& user) {
    auto pit = st.partyOf.find(user);
    if (pit == st.partyOf.end()) return;
    const int pid = pit->second;
    st.partyOf.erase(pit);
    auto party = st.parties.find(pid);
    if (party == st.parties.end()) return; // dangling index — already cleaned
    auto& mem = party->second.members;
    const bool isLeader = !mem.empty() && mem.front() == user;
    if (isLeader) {
        for (const std::string& u : mem) st.partyOf.erase(u); // drop everyone (user already gone)
        st.parties.erase(party);
        st.partyInvites.erase(std::remove_if(st.partyInvites.begin(), st.partyInvites.end(),
                                             [&](const PartyInvite& i) { return i.partyId == pid; }),
                              st.partyInvites.end());
    } else {
        mem.erase(std::remove(mem.begin(), mem.end(), user), mem.end());
    }
}

// Drop a departed user's open seek + queue slot + challenges + pending ready
// checks + party + events.
void withdrawUser(LobbyState& st, const std::string& user) {
    std::lock_guard<std::mutex> lk(st.mu);
    auto& sk = st.seeks;
    sk.erase(std::remove_if(sk.begin(), sk.end(), [&](const Seek& s) { return s.user == user; }),
             sk.end());
    auto& q = st.queue;
    q.erase(std::remove_if(q.begin(), q.end(), [&](const QueueEntry& e) { return e.user == user; }),
            q.end());
    auto& ch = st.challenges;
    ch.erase(std::remove_if(ch.begin(), ch.end(),
                            [&](const Challenge& c) { return c.from == user || c.to == user; }),
             ch.end());
    // Cancel any ready check they were in (notifies the opponent), then drop events.
    std::vector<int> mine;
    for (const auto& [id, rc] : st.readyChecks)
        if (rc.userP == user || rc.userE == user) mine.push_back(id);
    for (int id : mine) cancelReadyCheck(st, id, "opponent disconnected");
    // Cancel any pending draft they were a seat in (notifies the other 2N-1 players).
    std::vector<int> myDrafts;
    for (const auto& [id, dc] : st.drafts)
        if (!dc.complete)
            for (const DraftSeat& s : dc.seats)
                if (s.user == user) { myDrafts.push_back(id); break; }
    for (int id : myDrafts) cancelDraft(st, id, "a player disconnected");
    // Leave their party (disbanding it if they led it), then drop invites naming them.
    leaveParty(st, user);
    st.partyInvites.erase(std::remove_if(st.partyInvites.begin(), st.partyInvites.end(),
                                         [&](const PartyInvite& i) {
                                             return i.from == user || i.to == user;
                                         }),
                          st.partyInvites.end());
    st.events.erase(user);
}

// --- per-session request handling (server) ----------------------------------
void handleRequest(const proto::Msg& m, Connection& conn, const std::string& user, bool guest,
                   int rating, const LobbyConfig& cfg, LobbyState& st) {
    const std::string& t = m.type;

    if (t == "seek") {
        const MatchFormat fmt = formatFromJson(proto::objField(m, "format"));
        if (fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        std::vector<std::string> team; // a team seek carries the poster's full roster
        if (fmt.teamSize > 1) {
            std::string why;
            std::optional<std::vector<std::string>> roster = fullPartyRoster(st, user, fmt.teamSize, why);
            if (!roster) { conn.send(err(why)); return; }
            team = std::move(*roster);
        }
        st.seeks.erase(std::remove_if(st.seeks.begin(), st.seeks.end(),
                                      [&](const Seek& s) { return s.user == user; }),
                       st.seeks.end()); // one open seek per session
        st.seeks.push_back({st.nextId++, user, rating, fmt, guest, std::move(team)});
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
                if (!s.team.empty()) {
                    json::Value team = json::Value::makeArray();
                    for (const std::string& u : s.team) team.push_back(u);
                    o.set("team", std::move(team));
                }
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
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = std::find_if(st.seeks.begin(), st.seeks.end(),
                               [&](const Seek& s) { return s.id == id; });
        if (it == st.seeks.end()) { conn.send(err("no such seek")); return; }
        if (it->user == user) { conn.send(err("can't accept your own seek")); return; }
        if (it->fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        if (it->fmt.teamSize > 1) { // team seek → a draft between two full parties
            std::string why;
            std::optional<std::vector<std::string>> accRoster =
                fullPartyRoster(st, user, it->fmt.teamSize, why);
            if (!accRoster) { conn.send(err(why)); return; }
            // Re-validate the seeker still leads a full party (it may have broken since).
            std::string whyP;
            std::optional<std::vector<std::string>> posterRoster =
                fullPartyRoster(st, it->user, it->fmt.teamSize, whyP);
            if (!posterRoster) {
                st.seeks.erase(it);
                conn.send(err("the seeker's party is no longer ready"));
                return;
            }
            const std::string reply = openDraftCheck(st, it->fmt, *posterRoster, *accRoster, user);
            st.seeks.erase(it);
            conn.send(reply);
            return;
        }
        const std::string reply = openReadyCheck(st, it->fmt, it->user, user);
        st.seeks.erase(it);
        conn.send(reply);
        return;
    }
    if (t == "queueJoin") { // quick-match: auto-pair on the widening Elo band
        const MatchFormat fmt = formatFromJson(proto::objField(m, "format"));
        if (fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        st.queue.erase(std::remove_if(st.queue.begin(), st.queue.end(),
                                      [&](const QueueEntry& e) { return e.user == user; }),
                       st.queue.end()); // one slot per session
        st.queue.push_back({user, rating, fmt, std::chrono::steady_clock::now()});
        matchQueue(st, cfg); // may pair immediately — the ready check lands via poll
        conn.send(ok());
        return;
    }
    if (t == "queueLeave") {
        std::lock_guard<std::mutex> lk(st.mu);
        st.queue.erase(std::remove_if(st.queue.begin(), st.queue.end(),
                                      [&](const QueueEntry& e) { return e.user == user; }),
                       st.queue.end());
        conn.send(ok());
        return;
    }
    if (t == "challenge") {
        const std::string to = m.field("to");
        const MatchFormat fmt = formatFromJson(proto::objField(m, "format"));
        if (to.empty()) { conn.send(err("bad challenge")); return; }
        if (fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        std::vector<std::string> team; // a team challenge carries the challenger's roster
        if (fmt.teamSize > 1) {
            std::string why;
            std::optional<std::vector<std::string>> roster = fullPartyRoster(st, user, fmt.teamSize, why);
            if (!roster) { conn.send(err(why)); return; }
            team = std::move(*roster);
        }
        st.challenges.push_back({st.nextId++, user, to, rating, fmt, guest, std::move(team)});
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
                if (!c.team.empty()) {
                    json::Value team = json::Value::makeArray();
                    for (const std::string& u : c.team) team.push_back(u);
                    o.set("team", std::move(team));
                }
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
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = std::find_if(st.challenges.begin(), st.challenges.end(),
                               [&](const Challenge& c) { return c.id == id; });
        if (it == st.challenges.end() || it->to != user) { conn.send(err("no such challenge")); return; }
        if (it->fmt.rated && guest) { conn.send(err("rated play needs login")); return; }
        if (it->fmt.teamSize > 1) { // team challenge → a draft between two full parties
            std::string why;
            std::optional<std::vector<std::string>> accRoster =
                fullPartyRoster(st, user, it->fmt.teamSize, why);
            if (!accRoster) { conn.send(err(why)); return; }
            std::string whyC;
            std::optional<std::vector<std::string>> challengerRoster =
                fullPartyRoster(st, it->from, it->fmt.teamSize, whyC);
            if (!challengerRoster) {
                st.challenges.erase(it);
                conn.send(err("the challenger's party is no longer ready"));
                return;
            }
            const std::string reply = openDraftCheck(st, it->fmt, *challengerRoster, *accRoster, user);
            st.challenges.erase(it);
            conn.send(reply);
            return;
        }
        const std::string reply = openReadyCheck(st, it->fmt, it->from, user);
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
    // --- parties (team play) -------------------------------------------------
    if (t == "partyInvite") {
        const std::string to = m.field("to");
        if (to.empty() || to == user) { conn.send(err("bad invite")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        // If I already lead a party, keep using it; if I'm a non-leader member, refuse
        // (leadership is checked before the target so the inviter's own problem wins).
        int pid = 0;
        if (auto mine = st.partyOf.find(user); mine != st.partyOf.end()) {
            pid = mine->second;
            const Party& p = st.parties[pid];
            if (p.members.empty() || p.members.front() != user) {
                conn.send(err("only the party leader can invite"));
                return;
            }
        }
        if (st.partyOf.count(to)) { conn.send(err("that player is already in a party")); return; }
        if (pid == 0) { // auto-create my party (me = leader) on the first invite
            pid = st.nextId++;
            st.parties[pid] = Party{pid, {user}};
            st.partyOf[user] = pid;
        }
        for (const PartyInvite& i : st.partyInvites) // no duplicate pending invite
            if (i.partyId == pid && i.to == to) { conn.send(ok()); return; }
        st.partyInvites.push_back({st.nextId++, pid, user, to});
        conn.send(ok());
        return;
    }
    if (t == "partyAccept") {
        const int id = m.intField("id");
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.partyOf.count(user)) { conn.send(err("leave your current party first")); return; }
        auto it = std::find_if(st.partyInvites.begin(), st.partyInvites.end(),
                               [&](const PartyInvite& i) { return i.id == id && i.to == user; });
        if (it == st.partyInvites.end()) { conn.send(err("no such invite")); return; }
        auto party = st.parties.find(it->partyId);
        if (party == st.parties.end()) { // leader disbanded meanwhile
            st.partyInvites.erase(it);
            conn.send(err("that party no longer exists"));
            return;
        }
        party->second.members.push_back(user);
        st.partyOf[user] = it->partyId;
        // I'm in a party now — drop every invite addressed to me.
        st.partyInvites.erase(std::remove_if(st.partyInvites.begin(), st.partyInvites.end(),
                                             [&](const PartyInvite& i) { return i.to == user; }),
                              st.partyInvites.end());
        conn.send(ok());
        return;
    }
    if (t == "partyDecline") {
        const int id = m.intField("id");
        std::lock_guard<std::mutex> lk(st.mu);
        st.partyInvites.erase(
            std::remove_if(st.partyInvites.begin(), st.partyInvites.end(),
                           [&](const PartyInvite& i) { return i.id == id && i.to == user; }),
            st.partyInvites.end());
        conn.send(ok());
        return;
    }
    if (t == "partyLeave") {
        std::lock_guard<std::mutex> lk(st.mu);
        leaveParty(st, user);
        conn.send(ok());
        return;
    }
    if (t == "partyInfo") {
        json::Value o = json::Value::makeObject();
        o.set("type", "party");
        {
            std::lock_guard<std::mutex> lk(st.mu);
            auto mine = st.partyOf.find(user);
            json::Value mem = json::Value::makeArray();
            if (mine != st.partyOf.end() && st.parties.count(mine->second)) {
                const Party& p = st.parties[mine->second];
                o.set("id", p.id);
                for (const std::string& u : p.members) mem.push_back(u);
            } else {
                o.set("id", 0);
            }
            o.set("members", std::move(mem));
        }
        conn.send(json::dump(o, false));
        return;
    }
    if (t == "partyInvites") { // incoming invites addressed to me
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            for (const PartyInvite& i : st.partyInvites) {
                if (i.to != user) continue;
                json::Value o = json::Value::makeObject();
                o.set("id", i.id);
                o.set("partyId", i.partyId);
                o.set("from", i.from);
                json::Value mem = json::Value::makeArray();
                auto p = st.parties.find(i.partyId);
                if (p != st.parties.end())
                    for (const std::string& u : p->second.members) mem.push_back(u);
                o.set("members", std::move(mem));
                list.push_back(std::move(o));
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "partyinvites");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }

    // --- draft (the snake pick loop) -----------------------------------------
    if (t == "draftLock") {
        const int id = m.intField("id");
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (!b) { conn.send(err("malformed build")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        reapDrafts(st); // a timed-out pick may have auto-advanced past someone
        auto it = st.drafts.find(id);
        if (it == st.drafts.end()) { conn.send(simpleMsg("cancelled")); return; }
        DraftCheck& dc = it->second;
        if (dc.complete) { conn.send(err("draft already complete")); return; }
        const int seat = dc.pickOrder[dc.currentPick];
        if (dc.seats[seat].user != user) { conn.send(err("not your turn")); return; }
        // Validate against the format's ruleset — an illegal build is refused (re-pick).
        const Ruleset& rs = dc.fmt.rated ? cfg.rankedRules : cfg.casualRules;
        if (!validateBuild(*b, cfg.catalog, rs.economy, rs.bannedSpells).ok) {
            conn.send(notReadyMsg("that build is illegal for this format"));
            return;
        }
        advanceDraft(dc, *b, st.draftPickSec);
        const int cp = dc.currentPick; // capture before finalize erases the draft
        const bool done = dc.complete;
        json::Value reply = json::Value::makeObject();
        reply.set("type", "locked");
        reply.set("currentPick", cp);
        reply.set("complete", done);
        // The last lock finalizes the draft; everyone (this locker included) learns of the
        // resulting pairing via a poll `paired` event, so the reply is just the ack.
        if (done) finalizeDraft(st, id);
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "draftPoll") {
        const int id = m.intField("id");
        json::Value o = json::Value::makeObject();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            reapDrafts(st);
            auto it = st.drafts.find(id);
            if (it == st.drafts.end()) { conn.send(err("no such draft")); return; }
            const DraftCheck& dc = it->second;
            bool participant = false;
            for (const DraftSeat& s : dc.seats)
                if (s.user == user) { participant = true; break; }
            if (!participant) { conn.send(err("not your draft")); return; }
            o.set("type", "draftstate");
            o.set("currentPick", dc.currentPick);
            o.set("complete", dc.complete);
            const int secsLeft =
                dc.complete ? 0
                            : std::max(0, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                               dc.deadline - std::chrono::steady_clock::now())
                                                               .count()));
            o.set("secondsLeft", secsLeft);
            json::Value seats = json::Value::makeArray();
            for (const DraftSeat& s : dc.seats) {
                json::Value so = json::Value::makeObject();
                so.set("faction", proto::factionName(s.faction));
                so.set("index", s.index);
                so.set("user", s.user);
                so.set("locked", s.locked);
                if (s.locked) so.set("build", serializeBuild(s.build)); // revealed on lock
                seats.push_back(std::move(so));
            }
            o.set("seats", std::move(seats));
        }
        conn.send(json::dump(o, false));
        return;
    }

    if (t == "ready") {
        const int rcId = m.intField("id");
        const std::optional<CharacterBuild> b = deserializeBuild(m.field("build"));
        if (!b) { conn.send(err("malformed build")); return; }
        std::lock_guard<std::mutex> lk(st.mu);
        reapReadyChecks(st);
        auto it = st.readyChecks.find(rcId);
        if (it == st.readyChecks.end()) { conn.send(simpleMsg("cancelled")); return; }
        ReadyCheck& rc = it->second;
        const bool meIsPlayer = user == rc.userP;
        if (!meIsPlayer && user != rc.userE) { conn.send(err("not your ready check")); return; }
        // Validate the build against the format's ruleset — an illegal one is refused
        // so the player can re-pick (server-authoritative; ranked can't be dodged).
        const Ruleset& rs = rc.fmt.rated ? cfg.rankedRules : cfg.casualRules;
        if (!validateBuild(*b, cfg.catalog, rs.economy, rs.bannedSpells).ok) {
            conn.send(notReadyMsg("that build is illegal for this format"));
            return;
        }
        (meIsPlayer ? rc.buildP : rc.buildE) = *b;
        (meIsPlayer ? rc.readyP : rc.readyE) = true;
        if (!(rc.readyP && rc.readyE)) { conn.send(simpleMsg("waiting")); return; }

        // Both ready → finalize. This player (second to ready) gets the paired reply;
        // the other (readied first) learns via poll.
        json::Value pPaired, ePaired;
        makePairing(st, rc.fmt, rc.userP, rc.buildP, rc.userE, rc.buildE, pPaired, ePaired);
        const std::string otherUser = meIsPlayer ? rc.userE : rc.userP;
        json::Value theirs = asEvent(meIsPlayer ? ePaired : pPaired);
        st.events[otherUser].push_back(std::move(theirs));
        const std::string reply = asReply(meIsPlayer ? pPaired : ePaired);
        st.readyChecks.erase(it);
        conn.send(reply);
        return;
    }
    if (t == "cancelReady") {
        const int rcId = m.intField("id");
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.readyChecks.find(rcId);
        if (it != st.readyChecks.end() && (user == it->second.userP || user == it->second.userE))
            cancelReadyCheck(st, rcId, "opponent declined");
        conn.send(ok());
        return;
    }
    if (t == "myGames") { // my open correspondence games, for cold resume (persistence)
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            for (const auto& [id, g] : st.corrGames) {
                if (g.userP != user && g.userE != user) continue;
                const Faction seat = g.userP == user ? Faction::Player : Faction::Enemy;
                json::Value o = corrPairedObj(id, g.seed, seat, g.fmt.rated, g.buildP, g.buildE);
                o.set("opponent", seat == Faction::Player ? g.userE : g.userP);
                list.push_back(std::move(o));
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "mygames");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "chat") { // lobby-wide chat (4.6)
        const std::string text = m.field("text");
        std::lock_guard<std::mutex> lk(st.mu);
        const std::string bad = chatGate(st, cfg, user, text);
        if (!bad.empty()) { conn.send(err(bad)); return; }
        st.lobbyChat.push_back({user, text});
        while (st.lobbyChat.size() > LobbyState::kLobbyChatCap) {
            st.lobbyChat.pop_front();
            ++st.lobbyChatBase;
        }
        conn.send(ok());
        return;
    }
    if (t == "chatLog") { // poll the lobby chat from an absolute cursor (4.6)
        const std::size_t from = static_cast<std::size_t>(m.intField("from"));
        json::Value list = json::Value::makeArray();
        std::size_t next = from;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            const std::size_t start = std::max(from, st.lobbyChatBase);
            for (std::size_t i = start; i < st.lobbyChatBase + st.lobbyChat.size(); ++i) {
                const MailEntry& e = st.lobbyChat[i - st.lobbyChatBase];
                json::Value o = json::Value::makeObject();
                o.set("sender", e.sender);
                o.set("msg", e.msg);
                list.push_back(std::move(o));
            }
            next = st.lobbyChatBase + st.lobbyChat.size();
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "chatlog");
        reply.set("list", std::move(list));
        reply.set("next", static_cast<int>(next));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "corrChat") { // per-correspondence-game chat, participants only (4.6)
        const std::string game = m.field("game");
        const std::string text = m.field("text");
        {
            std::lock_guard<std::mutex> lk(st.mu);
            auto it = st.corrGames.find(game);
            if (it == st.corrGames.end() || (it->second.userP != user && it->second.userE != user)) {
                conn.send(err("no such correspondence game"));
                return;
            }
            const std::string bad = chatGate(st, cfg, user, text);
            if (!bad.empty()) { conn.send(err(bad)); return; }
        }
        st.mailbox.post(corrChatKey(game), user, text); // Mailbox is thread-safe
        conn.send(ok());
        return;
    }
    if (t == "corrChatLog") { // poll a correspondence game's chat (4.6)
        const std::string game = m.field("game");
        {
            std::lock_guard<std::mutex> lk(st.mu);
            auto it = st.corrGames.find(game);
            if (it == st.corrGames.end() || (it->second.userP != user && it->second.userE != user)) {
                conn.send(err("no such correspondence game"));
                return;
            }
        }
        const std::size_t from = static_cast<std::size_t>(m.intField("from"));
        const std::vector<MailEntry> entries = st.mailbox.poll(corrChatKey(game), from);
        json::Value list = json::Value::makeArray();
        for (const MailEntry& e : entries) {
            json::Value o = json::Value::makeObject();
            o.set("sender", e.sender);
            o.set("msg", e.msg);
            list.push_back(std::move(o));
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "corrchat");
        reply.set("list", std::move(list));
        reply.set("next", static_cast<int>(from + entries.size()));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "listGames") { // live matches in progress (watchable) — Phase 5.2
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            for (const auto& [id, g] : st.liveGames) {
                if (g.over) continue;
                json::Value o = json::Value::makeObject();
                o.set("id", id);
                o.set("userP", g.userP);
                o.set("userE", g.userE);
                o.set("rated", g.rated);
                list.push_back(std::move(o));
            }
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "games");
        reply.set("list", std::move(list));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "watch") { // poll a live game's broadcast log from a cursor — Phase 5.2
        const std::string game = m.field("game");
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (!st.liveGames.count(game)) { conn.send(err("no such live game")); return; }
        }
        const std::size_t from = static_cast<std::size_t>(m.intField("from"));
        const std::vector<MailEntry> entries = st.mailbox.poll(game, from);
        json::Value list = json::Value::makeArray();
        for (const MailEntry& e : entries) {
            json::Value o = json::Value::makeObject();
            o.set("msg", e.msg);
            list.push_back(std::move(o));
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "watchlog");
        reply.set("list", std::move(list));
        reply.set("next", static_cast<int>(from + entries.size()));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "poll") {
        json::Value list = json::Value::makeArray();
        {
            std::lock_guard<std::mutex> lk(st.mu);
            reapReadyChecks(st);
            reapDrafts(st);      // auto-lock any pick whose window elapsed
            matchQueue(st, cfg); // widening bands may have crossed since the join
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

    // --- correspondence: move relay + scoresheet submission -----------------
    if (t == "corrPost") {
        const std::string game = m.field("game");
        std::size_t len = 0;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (!st.corrGames.count(game)) { conn.send(err("no such correspondence game")); return; }
        }
        len = st.mailbox.post(game, user, m.field("msg")); // Mailbox is itself thread-safe
        json::Value reply = json::Value::makeObject();
        reply.set("type", "posted");
        reply.set("len", static_cast<int>(len));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "corrPoll") {
        const std::string game = m.field("game");
        const std::size_t from = static_cast<std::size_t>(m.intField("from"));
        const std::vector<MailEntry> entries = st.mailbox.poll(game, from);
        json::Value list = json::Value::makeArray();
        for (const MailEntry& e : entries) {
            json::Value o = json::Value::makeObject();
            o.set("sender", e.sender);
            o.set("msg", e.msg);
            list.push_back(std::move(o));
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "corrlog");
        reply.set("list", std::move(list));
        reply.set("next", static_cast<int>(from + entries.size()));
        conn.send(json::dump(reply, false));
        return;
    }
    if (t == "submit") {
        const std::string game = m.field("game");
        const std::optional<Faction> seat = proto::factionParse(m.field("seat"));
        const std::string notation = m.field("notation");
        std::string opponent;
        bool rated = false;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            auto it = st.corrGames.find(game);
            if (it == st.corrGames.end() || !seat) { conn.send(err("no such correspondence game")); return; }
            rated = it->second.fmt.rated;
            opponent = (*seat == Faction::Player) ? it->second.userE : it->second.userP;
        }
        json::Value reply = json::Value::makeObject();
        reply.set("type", "submitted");
        if (!rated || !st.arbiter) {
            reply.set("status", "casual");
        } else {
            const Arbiter::Result r = st.arbiter->submit({user, opponent, *seat, notation});
            reply.set("status", r.status == Arbiter::Status::Ranked      ? "ranked"
                                : r.status == Arbiter::Status::Pending    ? "pending"
                                                                          : "rejected");
            reply.set("winner", r.winner);
            reply.set("error", r.error);
        }
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
    hello.set("server", cfg.version); // client warns loudly if this != its own ATB_VERSION
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

// Build the server-enforced clock from a format (shared by 1v1 and team matches).
MatchClock clockFromFormat(const MatchFormat& fmt) {
    MatchClock clock;
    switch (fmt.time) {
        case MatchFormat::Time::PerMove: clock.perMoveSec = std::max(1, fmt.perMoveSec); break;
        case MatchFormat::Time::Chess:
            clock.mainSec = std::max(1, fmt.mainSec);
            clock.incSec = std::max(0, fmt.incSec);
            break;
        case MatchFormat::Time::Unlimited: break;
    }
    return clock;
}

// Once all 2N players of a team pairing have joined, run the NvN match on this thread.
// conns/builds are parallel to tp->seats (seat order [Player 0..N-1, Enemy 0..N-1]).
// Team ranking is a separate ladder (not wired here) — no Elo is recorded.
void runTeamPairing(const std::shared_ptr<TeamPairing>& tp, const LobbyConfig& cfg) {
    const MatchConfig mc = makeMatchConfig(cfg, tp->fmt);
    const MatchClock clock = clockFromFormat(tp->fmt);
    const int readTo = clock.chess() ? clock.mainSec + clock.incSec + 30
                       : clock.perMoveSec > 0 ? clock.perMoveSec + 30
                                              : 300;
    for (Connection& c : tp->conns) c.setReadTimeout(readTo);
    std::vector<CharacterBuild> builds;
    builds.reserve(tp->seats.size());
    for (const DraftSeat& s : tp->seats) builds.push_back(s.build);
    const int n = static_cast<int>(tp->seats.size()) / 2;
    runAdmittedTeamMatch(std::move(tp->conns), std::move(builds), n, mc, clock);
}

// A `joinMatch` connection: park it in its pairing; the final arrival runs the match.
// Handles both 1v1 pairings (byToken) and NvN team pairings (teamByToken).
void matchJoin(Connection conn, const proto::Msg& join, const LobbyConfig& cfg, LobbyState& st) {
    const std::string token = join.field("token");
    // Team pairing (NvN)? Park this seat's conn; the last of 2N to arrive drives it.
    {
        std::shared_ptr<TeamPairing> tp;
        bool runNow = false;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (auto it = st.teamByToken.find(token); it != st.teamByToken.end()) {
                tp = it->second;
                int seat = -1;
                for (int s = 0; s < static_cast<int>(tp->tokens.size()); ++s)
                    if (tp->tokens[s] == token) { seat = s; break; }
                if (seat >= 0 && !tp->have[static_cast<std::size_t>(seat)]) {
                    tp->conns[static_cast<std::size_t>(seat)] = std::move(conn);
                    tp->have[static_cast<std::size_t>(seat)] = true;
                }
                bool all = true;
                for (bool h : tp->have) all = all && h;
                if (all && !tp->started) {
                    tp->started = true;
                    for (const std::string& tk : tp->tokens) st.teamByToken.erase(tk);
                    runNow = true;
                }
            }
        }
        if (tp) {                    // it was a team token
            if (runNow) runTeamPairing(tp, cfg);
            return;                  // parked (or ran) — done either way
        }
    }
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
    // The server-enforced clock: a Per-move format is a fixed window per decision;
    // Chess is a true accumulating bank (main + increment, 6.3) — idling simply
    // burns the bank until the flag falls.
    const MatchClock clock = clockFromFormat(p->fmt);
    // Socket read timeout = a generous ceiling over the clock (the deadline loop in
    // runAdmittedMatch does the real enforcement; this only catches a wedged link).
    const int readTo = clock.chess() ? clock.mainSec + clock.incSec + 30
                       : clock.perMoveSec > 0 ? clock.perMoveSec + 30
                                              : 300;
    p->connP.setReadTimeout(readTo);
    p->connE.setReadTimeout(readTo);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.liveGames[p->gameId] = {p->userP, p->userE, p->fmt.rated, /*over=*/false};
    }
    // Log the match's broadcast stream under its game id so watchers can mirror it
    // (Phase 5.2). Mailbox is thread-safe; this runs on the match's own thread.
    const std::function<void(const std::string&)> spectate =
        [&st, gid = p->gameId](const std::string& msg) { st.mailbox.post(gid, "match", msg); };
    const ServeResult r = runAdmittedMatch(std::move(p->connP), std::move(p->connE), p->buildP,
                                           p->buildE, mc, clock, spectate);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.liveGames.find(p->gameId);
        if (it != st.liveGames.end()) it->second.over = true; // delisted; log stays drainable
    }
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
    if (cfg.rngSeed != 0) st.rng.seed(cfg.rngSeed);
    // Rated correspondence games rank through an embedded arbiter pinned to the
    // ranked ruleset (double-submit + re-simulation; CR.4). Casual servers skip it.
    if (cfg.accounts)
        st.arbiter = std::make_unique<Arbiter>(*cfg.accounts, cfg.rankedRules, cfg.catalog, cfg.creatures);
    st.readySeconds = cfg.readyCheckSec;
    st.draftPickSec = cfg.draftPickSec;
    // Persistence: reload the correspondence state (game registry + move logs) so
    // open games survive the restart; every future change lands on disk too.
    if (!cfg.persistDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.persistDir, ec);
        st.corrGamesPath = cfg.persistDir + "/corrgames.json";
        loadCorrGames(st);
        st.mailbox.openJournal(cfg.persistDir + "/mailbox.jsonl");
    }
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
    s->serverVersion_ = m->field("server"); // "" from a server predating the version field
    s->acct_.user = m->field("user");
    s->acct_.rating = m->intField("rating", kDefaultRating);
    s->acct_.wins = m->intField("wins");
    s->acct_.losses = m->intField("losses");
    return s;
}

namespace {
// Parse a `paired` object (a reply body or an async event) into PairedInfo. Handles
// both live (token) and correspondence (game + setup) pairings.
std::optional<PairedInfo> pairedFromObj(const json::Value& o, std::string* error) {
    PairedInfo pi;
    const std::optional<Faction> seat = proto::factionParse(proto::strOf(o, "seat"));
    if (!seat) {
        if (error) *error = "malformed pairing";
        return std::nullopt;
    }
    pi.seat = *seat;
    const json::Value* live = o.find("live");
    pi.live = !(live && live->isBool()) || live->asBool(); // default live
    const json::Value* r = o.find("rated");
    pi.rated = r && r->isBool() && r->asBool();
    if (pi.live) {
        pi.token = proto::strOf(o, "token");
        if (pi.token.empty()) { if (error) *error = "missing match token"; return std::nullopt; }
        // Team pairing (NvN): controller seat + both rosters (absent/empty for 1v1).
        pi.controllerSeat = proto::intOf(o, "controllerSeat", 0);
        auto readTeam = [&](const char* key, std::vector<CharacterBuild>& out) {
            if (const json::Value* arr = o.find(key); arr && arr->isArray())
                for (const json::Value& b : arr->asArray())
                    if (b.isString())
                        if (std::optional<CharacterBuild> cb = deserializeBuild(b.asString()))
                            out.push_back(*cb);
        };
        readTeam("playerTeam", pi.playerTeam);
        readTeam("enemyTeam", pi.enemyTeam);
    } else {
        pi.game = proto::strOf(o, "game");
        pi.seed = static_cast<unsigned>(proto::intOf(o, "seed"));
        const std::optional<CharacterBuild> p = deserializeBuild(proto::strOf(o, "playerBuild"));
        const std::optional<CharacterBuild> e = deserializeBuild(proto::strOf(o, "enemyBuild"));
        if (pi.game.empty() || !p || !e) {
            if (error) *error = "malformed correspondence setup";
            return std::nullopt;
        }
        pi.player = *p;
        pi.enemy = *e;
        pi.opponent = proto::strOf(o, "opponent");
    }
    return pi;
}
// Parse a `readyCheck` object (reply body or event) into ReadyCheckInfo.
std::optional<ReadyCheckInfo> readyCheckFromObj(const json::Value& o) {
    ReadyCheckInfo rc;
    rc.id = proto::intOf(o, "id");
    rc.opponent = proto::strOf(o, "opponent");
    const std::optional<Faction> seat = proto::factionParse(proto::strOf(o, "seat"));
    if (rc.id == 0 || !seat) return std::nullopt;
    rc.seat = *seat;
    const json::Value* r = o.find("rated");
    rc.rated = r && r->isBool() && r->asBool();
    rc.format = formatFromJson(o.find("format"));
    rc.seconds = proto::intOf(o, "seconds", 30);
    return rc;
}
// Pull a JSON string array (of usernames) into a vector.
std::vector<std::string> membersFrom(const json::Value* arr) {
    std::vector<std::string> out;
    if (arr && arr->isArray())
        for (const json::Value& e : arr->asArray())
            if (e.isString()) out.push_back(e.asString());
    return out;
}
// Parse a `draft` object (reply body or event) into DraftInfo.
std::optional<DraftInfo> draftInfoFromObj(const json::Value& o) {
    DraftInfo d;
    d.id = proto::intOf(o, "id");
    if (d.id == 0) return std::nullopt;
    d.format = formatFromJson(o.find("format"));
    d.mySeat = proto::intOf(o, "mySeat", -1);
    if (const json::Value* seats = o.find("seats"); seats && seats->isArray())
        for (const json::Value& s : seats->asArray()) {
            DraftSeatInfo si;
            si.faction = proto::factionParse(proto::strOf(s, "faction")).value_or(Faction::Player);
            si.index = proto::intOf(s, "index");
            si.user = proto::strOf(s, "user");
            d.seats.push_back(std::move(si));
        }
    if (const json::Value* order = o.find("pickOrder"); order && order->isArray())
        for (const json::Value& i : order->asArray())
            if (i.isNumber()) d.pickOrder.push_back(static_cast<int>(i.asNumber()));
    return d;
}
} // namespace

bool LobbySession::seek(const MatchFormat& fmt, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "seek");
    o.set("format", formatToJson(fmt));
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
            s.team = membersFrom(e.find("team"));
            out.push_back(std::move(s));
        }
    return out;
}

std::optional<ReadyCheckInfo> LobbySession::acceptSeek(int seekId, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptSeek");
    o.set("id", seekId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "readyCheck") {
        if (error) *error = m ? m->field("message") : "bad reply";
        return std::nullopt;
    }
    return readyCheckFromObj(m->body);
}

std::optional<DraftInfo> LobbySession::acceptSeekTeam(int seekId, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptSeek");
    o.set("id", seekId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "draft") {
        if (error) *error = m ? m->field("message") : "bad reply";
        return std::nullopt;
    }
    return draftInfoFromObj(m->body);
}

bool LobbySession::queueJoin(const MatchFormat& fmt, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "queueJoin");
    o.set("format", formatToJson(fmt));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

bool LobbySession::queueLeave() {
    json::Value o = json::Value::makeObject();
    o.set("type", "queueLeave");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

bool LobbySession::challenge(const std::string& toUser, const MatchFormat& fmt, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "challenge");
    o.set("to", toUser);
    o.set("format", formatToJson(fmt));
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
            c.team = membersFrom(e.find("team"));
            out.push_back(std::move(c));
        }
    return out;
}

std::optional<ReadyCheckInfo> LobbySession::acceptChallenge(int id, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptChallenge");
    o.set("id", id);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "readyCheck") {
        if (error) *error = m ? m->field("message") : "bad reply";
        return std::nullopt;
    }
    return readyCheckFromObj(m->body);
}

std::optional<DraftInfo> LobbySession::acceptChallengeTeam(int id, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "acceptChallenge");
    o.set("id", id);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return std::nullopt; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "draft") {
        if (error) *error = m ? m->field("message") : "bad reply";
        return std::nullopt;
    }
    return draftInfoFromObj(m->body);
}

bool LobbySession::declineChallenge(int id) {
    json::Value o = json::Value::makeObject();
    o.set("type", "decline");
    o.set("id", id);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

// --- parties (team play) -----------------------------------------------------
bool LobbySession::partyInvite(const std::string& toUser, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyInvite");
    o.set("to", toUser);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

bool LobbySession::partyAccept(int inviteId, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyAccept");
    o.set("id", inviteId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

bool LobbySession::partyDecline(int inviteId) {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyDecline");
    o.set("id", inviteId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

bool LobbySession::partyLeave() {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyLeave");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

std::optional<PartyInfo> LobbySession::partyInfo() {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyInfo");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "party") return std::nullopt;
    PartyInfo pi;
    pi.id = proto::intOf(m->body, "id");
    pi.members = membersFrom(m->body.find("members"));
    if (!pi.members.empty()) pi.leader = pi.members.front();
    return pi;
}

std::optional<std::vector<PartyInviteInfo>> LobbySession::listPartyInvites() {
    json::Value o = json::Value::makeObject();
    o.set("type", "partyInvites");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "partyinvites") return std::nullopt;
    std::vector<PartyInviteInfo> out;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray()) {
            PartyInviteInfo i;
            i.id = proto::intOf(e, "id");
            i.partyId = proto::intOf(e, "partyId");
            i.from = proto::strOf(e, "from");
            i.members = membersFrom(e.find("members"));
            out.push_back(std::move(i));
        }
    return out;
}

std::optional<DraftState> LobbySession::draftPoll(int draftId) {
    json::Value o = json::Value::makeObject();
    o.set("type", "draftPoll");
    o.set("id", draftId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "draftstate") return std::nullopt;
    DraftState ds;
    ds.currentPick = proto::intOf(m->body, "currentPick");
    ds.secondsLeft = proto::intOf(m->body, "secondsLeft");
    if (const json::Value* comp = m->body.find("complete"); comp && comp->isBool())
        ds.complete = comp->asBool();
    if (const json::Value* seats = m->body.find("seats"); seats && seats->isArray())
        for (const json::Value& s : seats->asArray()) {
            DraftSeatState ss;
            ss.faction = proto::factionParse(proto::strOf(s, "faction")).value_or(Faction::Player);
            ss.index = proto::intOf(s, "index");
            ss.user = proto::strOf(s, "user");
            if (const json::Value* lk = s.find("locked"); lk && lk->isBool()) ss.locked = lk->asBool();
            if (ss.locked)
                if (std::optional<CharacterBuild> b = deserializeBuild(proto::strOf(s, "build")))
                    ss.build = *b;
            ds.seats.push_back(std::move(ss));
        }
    return ds;
}

DraftLockResult LobbySession::draftLock(int draftId, const CharacterBuild& build, std::string* error) {
    DraftLockResult res;
    json::Value o = json::Value::makeObject();
    o.set("type", "draftLock");
    o.set("id", draftId);
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { res.error = "link failed"; if (error) *error = res.error; return res; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) { res.error = "bad reply"; if (error) *error = res.error; return res; }
    if (m->type == "locked") {
        res.currentPick = m->intField("currentPick");
        const json::Value* comp = m->body.find("complete");
        res.status = (comp && comp->isBool() && comp->asBool()) ? DraftLockResult::Status::Complete
                                                                : DraftLockResult::Status::Locked;
        return res;
    }
    if (m->type == "cancelled") { res.status = DraftLockResult::Status::Cancelled; return res; }
    // notReady (illegal build) or error ("not your turn" / other): surface the reason.
    res.error = m->field("message");
    if (error) *error = res.error;
    res.status = m->type == "notReady" ? DraftLockResult::Status::Rejected
                 : res.error.find("your turn") != std::string::npos
                     ? DraftLockResult::Status::NotYourTurn
                     : DraftLockResult::Status::Cancelled;
    return res;
}

ReadyResult LobbySession::ready(int readyCheckId, const CharacterBuild& build, std::string* error) {
    ReadyResult res;
    json::Value o = json::Value::makeObject();
    o.set("type", "ready");
    o.set("id", readyCheckId);
    o.set("build", serializeBuild(build));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { res.error = "link failed"; if (error) *error = res.error; return res; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m) { res.error = "bad reply"; if (error) *error = res.error; return res; }
    if (m->type == "waiting") { res.status = ReadyResult::Status::Waiting; return res; }
    if (m->type == "cancelled") { res.status = ReadyResult::Status::Cancelled; return res; }
    if (m->type == "notReady" || m->type == "error") {
        res.status = ReadyResult::Status::Rejected;
        res.error = m->field("message");
        if (error) *error = res.error;
        return res;
    }
    if (m->type == "paired") {
        if (std::optional<PairedInfo> pi = pairedFromObj(m->body, error)) {
            res.status = ReadyResult::Status::Matched;
            res.paired = *pi;
        }
        return res;
    }
    res.error = "unexpected reply: " + m->type;
    if (error) *error = res.error;
    return res;
}

bool LobbySession::cancelReady(int readyCheckId) {
    json::Value o = json::Value::makeObject();
    o.set("type", "cancelReady");
    o.set("id", readyCheckId);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    const std::optional<proto::Msg> m = raw ? proto::parse(*raw) : std::nullopt;
    return m && m->type == "ok";
}

LobbyEvent LobbySession::poll() {
    LobbyEvent ev;
    json::Value o = json::Value::makeObject();
    o.set("type", "poll");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return ev;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "events") return ev;
    const json::Value* list = m->body.find("list");
    if (!list || !list->isArray()) return ev;
    // Return the first actionable event; a ready check / cancel outranks nothing else
    // typically in flight at once, but paired wins if somehow both are queued.
    for (const json::Value& e : list->asArray()) {
        const std::string kind = proto::strOf(e, "kind");
        if (kind == "paired") {
            if (std::optional<PairedInfo> pi = pairedFromObj(e, nullptr)) {
                ev.kind = LobbyEvent::Kind::Paired;
                ev.paired = *pi;
                return ev;
            }
        } else if (kind == "readyCheck" && ev.kind == LobbyEvent::Kind::None) {
            if (std::optional<ReadyCheckInfo> rc = readyCheckFromObj(e)) {
                ev.kind = LobbyEvent::Kind::ReadyCheck;
                ev.readyCheck = *rc;
            }
        } else if (kind == "draft" && ev.kind == LobbyEvent::Kind::None) {
            if (std::optional<DraftInfo> d = draftInfoFromObj(e)) {
                ev.kind = LobbyEvent::Kind::Draft;
                ev.draft = *d;
            }
        } else if (kind == "cancelled" && ev.kind == LobbyEvent::Kind::None) {
            ev.kind = LobbyEvent::Kind::Cancelled;
            ev.message = proto::strOf(e, "message");
        }
    }
    return ev;
}

std::optional<std::size_t> LobbySession::corrPost(const std::string& game, const std::string& msg) {
    json::Value o = json::Value::makeObject();
    o.set("type", "corrPost");
    o.set("game", game);
    o.set("msg", msg);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "posted") return std::nullopt;
    return static_cast<std::size_t>(m->intField("len"));
}

std::optional<ChannelPoll> LobbySession::corrPoll(const std::string& game, std::size_t from) {
    json::Value o = json::Value::makeObject();
    o.set("type", "corrPoll");
    o.set("game", game);
    o.set("from", static_cast<int>(from));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "corrlog") return std::nullopt;
    ChannelPoll cp;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray())
            cp.entries.push_back({proto::strOf(e, "sender"), proto::strOf(e, "msg")});
    cp.next = static_cast<std::size_t>(m->intField("next"));
    return cp;
}

std::optional<std::vector<PairedInfo>> LobbySession::myCorrGames() {
    json::Value o = json::Value::makeObject();
    o.set("type", "myGames");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "mygames") return std::nullopt;
    std::vector<PairedInfo> out;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray())
            if (std::optional<PairedInfo> pi = pairedFromObj(e, nullptr)) out.push_back(*pi);
    return out;
}

bool LobbySession::chatSend(const std::string& text, std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "chat");
    o.set("text", text);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

std::optional<ChannelPoll> LobbySession::chatPoll(std::size_t from) {
    json::Value o = json::Value::makeObject();
    o.set("type", "chatLog");
    o.set("from", static_cast<int>(from));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "chatlog") return std::nullopt;
    ChannelPoll cp;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray())
            cp.entries.push_back({proto::strOf(e, "sender"), proto::strOf(e, "msg")});
    cp.next = static_cast<std::size_t>(m->intField("next"));
    return cp;
}

bool LobbySession::corrChatSend(const std::string& game, const std::string& text,
                                std::string* error) {
    json::Value o = json::Value::makeObject();
    o.set("type", "corrChat");
    o.set("game", game);
    o.set("text", text);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { if (error) *error = "link failed"; return false; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (m && m->type == "ok") return true;
    if (error) *error = m ? m->field("message") : "bad reply";
    return false;
}

std::optional<ChannelPoll> LobbySession::corrChatPoll(const std::string& game, std::size_t from) {
    json::Value o = json::Value::makeObject();
    o.set("type", "corrChatLog");
    o.set("game", game);
    o.set("from", static_cast<int>(from));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "corrchat") return std::nullopt;
    ChannelPoll cp;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray())
            cp.entries.push_back({proto::strOf(e, "sender"), proto::strOf(e, "msg")});
    cp.next = static_cast<std::size_t>(m->intField("next"));
    return cp;
}

std::optional<std::vector<LiveGameInfo>> LobbySession::listGames() {
    json::Value o = json::Value::makeObject();
    o.set("type", "listGames");
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "games") return std::nullopt;
    std::vector<LiveGameInfo> out;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray()) {
            LiveGameInfo g;
            g.id = proto::strOf(e, "id");
            g.userP = proto::strOf(e, "userP");
            g.userE = proto::strOf(e, "userE");
            const json::Value* r = e.find("rated");
            g.rated = r && r->isBool() && r->asBool();
            out.push_back(std::move(g));
        }
    return out;
}

std::optional<ChannelPoll> LobbySession::watchPoll(const std::string& game, std::size_t from) {
    json::Value o = json::Value::makeObject();
    o.set("type", "watch");
    o.set("game", game);
    o.set("from", static_cast<int>(from));
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) return std::nullopt;
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "watchlog") return std::nullopt;
    ChannelPoll cp;
    const json::Value* list = m->body.find("list");
    if (list && list->isArray())
        for (const json::Value& e : list->asArray())
            cp.entries.push_back({"match", proto::strOf(e, "msg")});
    cp.next = static_cast<std::size_t>(m->intField("next"));
    return cp;
}

SubmitResult LobbySession::submitScore(const std::string& game, Faction seat,
                                       const std::string& notation) {
    SubmitResult res;
    json::Value o = json::Value::makeObject();
    o.set("type", "submit");
    o.set("game", game);
    o.set("seat", proto::factionName(seat));
    o.set("notation", notation);
    const std::optional<std::string> raw = rpc(json::dump(o, false));
    if (!raw) { res.error = "link failed"; return res; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "submitted") {
        res.error = m ? m->field("message") : "bad reply";
        return res;
    }
    const std::string s = m->field("status");
    res.status = s == "ranked"    ? SubmitResult::Status::Ranked
                 : s == "pending" ? SubmitResult::Status::Pending
                 : s == "casual"  ? SubmitResult::Status::Casual
                                  : SubmitResult::Status::Rejected;
    res.winner = m->field("winner");
    res.error = m->field("error");
    return res;
}

} // namespace tb::net
