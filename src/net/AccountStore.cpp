//
// AccountStore.cpp — see AccountStore.h.
//
#include "AccountStore.h"

#include "data/Json.h"
#include "data/Password.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tb::net {

AccountStore::AccountStore(std::string path) : path_(std::move(path)) { loadFrom(path_); }

void AccountStore::loadFrom(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return; // absent → fresh store
    std::stringstream ss;
    ss << f.rdbuf();
    json::ParseResult pr = json::parse(ss.str());
    if (!pr.ok || !pr.value.isObject()) {
        std::fprintf(stderr, "AccountStore: '%s' is not valid JSON — starting empty.\n", path.c_str());
        return;
    }
    const json::Value* players = pr.value.find("players");
    if (!players || !players->isArray()) return;
    for (const json::Value& p : players->asArray()) {
        if (!p.isObject()) continue;
        const json::Value* u = p.find("user");
        const json::Value* h = p.find("hash");
        if (!u || !u->isString() || !h || !h->isString()) continue;
        Record r;
        r.passwordHash = h->asString();
        if (const json::Value* v = p.find("rating"); v && v->isNumber()) r.rating = static_cast<int>(v->asNumber());
        if (const json::Value* v = p.find("wins"); v && v->isNumber()) r.wins = static_cast<int>(v->asNumber());
        if (const json::Value* v = p.find("losses"); v && v->isNumber()) r.losses = static_cast<int>(v->asNumber());
        accounts_[u->asString()] = std::move(r);
    }
}

void AccountStore::saveLocked() const {
    json::Value root = json::Value::makeObject();
    root.set("schema", 1);
    json::Value players = json::Value::makeArray();
    for (const auto& [user, r] : accounts_) {
        json::Value p = json::Value::makeObject();
        p.set("user", user);
        p.set("hash", r.passwordHash);
        p.set("rating", r.rating);
        p.set("wins", r.wins);
        p.set("losses", r.losses);
        players.push_back(p);
    }
    root.set("players", players);

    if (std::ofstream out(path_); out) out << json::dump(root, /*pretty=*/true);
}

AuthResult AccountStore::authenticate(const std::string& user, const std::string& password) {
    AuthResult res;
    if (user.empty() || password.empty()) { res.error = "username and password required"; return res; }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        // Auto-register: first use of a name claims it with this password.
        Record r;
        r.passwordHash = hashPassword(password);
        it = accounts_.emplace(user, std::move(r)).first;
        saveLocked();
        res.ok = true;
        res.created = true;
    } else if (!verifyPassword(password, it->second.passwordHash)) {
        res.error = "incorrect password";
        return res;
    } else {
        res.ok = true;
    }
    res.account = {user, it->second.rating, it->second.wins, it->second.losses};
    return res;
}

void AccountStore::recordResult(const std::string& winner, const std::string& loser) {
    if (winner == loser) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto w = accounts_.find(winner);
    auto l = accounts_.find(loser);
    if (w == accounts_.end() || l == accounts_.end()) return;

    // Elo, K = 32, zero-sum: the winner gains exactly what the loser drops.
    const double expWin = 1.0 / (1.0 + std::pow(10.0, (l->second.rating - w->second.rating) / 400.0));
    const int delta = static_cast<int>(std::lround(32.0 * (1.0 - expWin)));
    w->second.rating += delta;
    l->second.rating -= delta;
    ++w->second.wins;
    ++l->second.losses;
    saveLocked();
}

int AccountStore::ratingOf(const std::string& user) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = accounts_.find(user);
    return it == accounts_.end() ? kDefaultRating : it->second.rating;
}

std::optional<AccountView> AccountStore::get(const std::string& user) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = accounts_.find(user);
    if (it == accounts_.end()) return std::nullopt;
    return AccountView{user, it->second.rating, it->second.wins, it->second.losses};
}

std::size_t AccountStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return accounts_.size();
}

} // namespace tb::net
