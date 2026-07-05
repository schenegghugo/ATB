//
// MailboxRelay.cpp — see MailboxRelay.h.
//
#include "MailboxRelay.h"

#include "data/Json.h"

#include <thread>

namespace tb::net {
namespace {

std::string strField(const json::Value& o, const char* k) {
    const json::Value* v = o.find(k);
    return (v && v->isString()) ? v->asString() : std::string();
}
std::size_t uintField(const json::Value& o, const char* k) {
    const json::Value* v = o.find(k);
    return (v && v->isNumber() && v->asNumber() >= 0) ? static_cast<std::size_t>(v->asNumber()) : 0;
}
std::string errMsg(const std::string& why) {
    json::Value o = json::Value::makeObject();
    o.set("ok", false);
    o.set("error", why);
    return json::dump(o, false);
}

// Serve one connection's request stream until it disconnects.
void handleConn(Connection conn, Mailbox& box) {
    while (true) {
        const std::optional<std::string> raw = conn.recv();
        if (!raw) break; // client closed
        const json::ParseResult pr = json::parse(*raw);
        if (!pr.ok || !pr.value.isObject()) {
            if (!conn.send(errMsg("bad request"))) break;
            continue;
        }
        const json::Value& o = pr.value;
        const std::string op = strField(o, "op");

        if (op == "post") {
            const std::size_t len =
                box.post(strField(o, "game"), strField(o, "sender"), strField(o, "msg"));
            json::Value r = json::Value::makeObject();
            r.set("ok", true);
            r.set("len", static_cast<int>(len));
            if (!conn.send(json::dump(r, false))) break;
        } else if (op == "poll") {
            const std::string game = strField(o, "game");
            const std::size_t from = uintField(o, "from");
            const std::vector<MailEntry> entries = box.poll(game, from);
            json::Value arr = json::Value::makeArray();
            for (const MailEntry& e : entries) {
                json::Value ev = json::Value::makeObject();
                ev.set("s", e.sender);
                ev.set("m", e.msg);
                arr.push_back(ev);
            }
            json::Value r = json::Value::makeObject();
            r.set("ok", true);
            r.set("entries", arr);
            r.set("next", static_cast<int>(from + entries.size()));
            if (!conn.send(json::dump(r, false))) break;
        } else {
            if (!conn.send(errMsg("unknown op"))) break;
        }
    }
}

} // namespace

// --- Mailbox ----------------------------------------------------------------
std::size_t Mailbox::post(const std::string& game, const std::string& sender,
                          const std::string& msg) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MailEntry>& log = logs_[game];
    log.push_back({sender, msg});
    return log.size();
}

std::vector<MailEntry> Mailbox::poll(const std::string& game, std::size_t from) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = logs_.find(game);
    if (it == logs_.end() || from >= it->second.size()) return {};
    return {it->second.begin() + static_cast<std::ptrdiff_t>(from), it->second.end()};
}

std::size_t Mailbox::size(const std::string& game) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = logs_.find(game);
    return it == logs_.end() ? 0 : it->second.size();
}

// --- Relay server -----------------------------------------------------------
void serveRelay(Listener& listener, Mailbox& box, int maxConns, int readTimeoutSec) {
    std::vector<std::thread> conns;
    int accepted = 0;
    while (maxConns < 0 || accepted < maxConns) {
        std::optional<Connection> c = listener.accept();
        if (!c) break; // listener closed
        c->setReadTimeout(readTimeoutSec);
        conns.emplace_back([conn = std::move(*c), &box]() mutable { handleConn(std::move(conn), box); });
        ++accepted;
    }
    for (std::thread& t : conns)
        if (t.joinable()) t.join();
}

// --- Relay client -----------------------------------------------------------
std::optional<RelayClient> RelayClient::connect(const std::string& host, uint16_t port,
                                                int readTimeoutSec) {
    std::optional<Connection> c = Connection::connect(host, port);
    if (!c) return std::nullopt;
    c->setReadTimeout(readTimeoutSec);
    return RelayClient(std::move(*c));
}

std::optional<std::size_t> RelayClient::post(const std::string& game, const std::string& sender,
                                             const std::string& msg) {
    json::Value req = json::Value::makeObject();
    req.set("op", "post");
    req.set("game", game);
    req.set("sender", sender);
    req.set("msg", msg);
    if (!conn_.send(json::dump(req, false))) return std::nullopt;
    const std::optional<std::string> raw = conn_.recv();
    if (!raw) return std::nullopt;
    const json::ParseResult pr = json::parse(*raw);
    if (!pr.ok || !pr.value.isObject()) return std::nullopt;
    const json::Value* len = pr.value.find("len");
    if (!len || !len->isNumber()) return std::nullopt;
    return static_cast<std::size_t>(len->asNumber());
}

std::optional<RelayClient::PollResult> RelayClient::poll(const std::string& game, std::size_t from) {
    json::Value req = json::Value::makeObject();
    req.set("op", "poll");
    req.set("game", game);
    req.set("from", static_cast<int>(from));
    if (!conn_.send(json::dump(req, false))) return std::nullopt;
    const std::optional<std::string> raw = conn_.recv();
    if (!raw) return std::nullopt;
    const json::ParseResult pr = json::parse(*raw);
    if (!pr.ok || !pr.value.isObject()) return std::nullopt;

    PollResult out;
    if (const json::Value* n = pr.value.find("next"); n && n->isNumber())
        out.next = static_cast<std::size_t>(n->asNumber());
    if (const json::Value* arr = pr.value.find("entries"); arr && arr->isArray())
        for (const json::Value& e : arr->asArray()) {
            if (!e.isObject()) continue;
            const json::Value* s = e.find("s");
            const json::Value* m = e.find("m");
            out.entries.push_back({s && s->isString() ? s->asString() : "",
                                   m && m->isString() ? m->asString() : ""});
        }
    return out;
}

} // namespace tb::net
