//
// Correspondence.cpp — see Correspondence.h.
//
#include "Correspondence.h"

#include "core/Match.h" // buildMatch

#include <sstream>

namespace tb::net {

CorrespondenceSession::CorrespondenceSession(std::unique_ptr<MoveChannel> channel, std::string game,
                                             CorrespondenceSetup setup, Faction mySeat,
                                             std::string user)
    : channel_(std::move(channel)), game_(std::move(game)), mySeat_(mySeat), user_(std::move(user)),
      runner_(buildMatch(setup.ruleset, {setup.player}, {setup.enemy}, setup.catalog, setup.seed,
                         setup.creatures),
              Seat::Human, Seat::Human),
      rng_(std::random_device{}()) {
    rec_.catalogHash = replay::catalogHash(setup.catalog);
    rec_.rulesetHash = replay::rulesetHash(setup.ruleset);
    rec_.seed = setup.seed;
    rec_.player = setup.player;
    rec_.enemy = setup.enemy;
}

bool CorrespondenceSession::isDecoyCast(EntityId actor, int slot) const {
    const Entity& u = runner_.battle().unit(actor);
    if (slot < 0 || slot >= static_cast<int>(u.spells.size())) return false;
    for (const Effect& e : u.spells[slot].effects)
        if (e.type == Effect::Type::Decoy) return true;
    return false;
}

std::string CorrespondenceSession::mintNonce() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(16, '0');
    for (char& ch : s) ch = kHex[rng_() & 0xF];
    return s;
}

bool CorrespondenceSession::submitLocal(const Intent& in, std::optional<char> decoyChoice,
                                        std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    if (!awaitingMe()) return fail("not your turn");

    const EntityId actor = runner_.battle().activeUnit();
    const bool decoy = in.kind == Intent::Kind::Cast && isDecoyCast(actor, in.spellIdx);
    if (decoy) {
        if (!decoyChoice) return fail("a decoy cast needs a commitment choice ('a' or 'b')");
        if (*decoyChoice != 'a' && *decoyChoice != 'b') return fail("decoy choice must be 'a' or 'b'");
    }

    if (!runner_.submit(mySeat_, in)) return fail("illegal intent");
    rec_.intents.push_back(in);

    std::string msg = replay::intentToken(in);
    // A decoy cast spawns exactly one cloak pair — mint the commitment and ship its
    // hash (only) with the move, pinning the hidden choice before the opponent acts.
    const std::size_t now = runner_.battle().cloakPairs().size();
    if (decoy && now > seenCloaks_) {
        const std::string choice(1, *decoyChoice);
        const std::string nonce = mintNonce();
        const std::string commit = replay::makeCommitment(choice, nonce);
        myCommits_.push_back(rec_.commits.size());
        rec_.commits.push_back({commit, choice, nonce});
        seenCloaks_ = now;
        msg += " C" + commit;
    }
    (void)channel_->post(game_, user_, msg);
    return true;
}

bool CorrespondenceSession::sync() {
    if (finished()) return false; // reveals (post-finish) are finalize()'s job
    const std::optional<ChannelPoll> res = channel_->poll(game_, pollCursor_);
    if (!res) return false;

    bool applied = false;
    std::size_t i = 0;
    for (; i < res->entries.size(); ++i) {
        const MailEntry& e = res->entries[i];
        if (e.sender == user_) continue;          // my own move — already applied locally
        if (e.msg.rfind("R ", 0) == 0) continue;  // a reveal arriving early — ignore

        // "<token>" or "<token> C<commit>".
        std::string token = e.msg, wireCommit;
        const std::size_t sp = e.msg.find(' ');
        if (sp != std::string::npos) {
            token = e.msg.substr(0, sp);
            const std::string rest = e.msg.substr(sp + 1);
            if (rest.size() > 1 && rest[0] == 'C') wireCommit = rest.substr(1);
        }
        Intent in;
        if (!replay::parseIntentToken(token, in)) continue; // ignore unparseable

        const std::optional<Faction> seat = runner_.awaitingSeat();
        if (!seat || *seat == mySeat_) break; // not the opponent's turn yet — leave for next sync
        if (!runner_.submit(*seat, in)) continue; // illegal (shouldn't happen honestly) — skip
        rec_.intents.push_back(in);

        // A cloak pair the opponent just cast: record it with the hash off the wire;
        // the choice+nonce are revealed in finalize().
        const std::size_t now = runner_.battle().cloakPairs().size();
        if (now > seenCloaks_) {
            rec_.commits.push_back({wireCommit, "", ""});
            seenCloaks_ = now;
        }
        applied = true;
    }
    pollCursor_ += i; // advance only over entries we actually consumed
    return applied;
}

bool CorrespondenceSession::finalize() {
    if (!finished()) return false;
    if (rec_.commits.empty()) return true; // no decoys — nothing to reveal

    if (!revealsPosted_) {
        for (const std::size_t idx : myCommits_) {
            const replay::DecoyCommit& c = rec_.commits[idx];
            (void)channel_->post(game_, user_,
                              "R " + std::to_string(idx) + ' ' + c.choice + ' ' + c.nonce);
        }
        revealsPosted_ = true;
    }

    const std::optional<ChannelPoll> res = channel_->poll(game_, pollCursor_);
    if (res) {
        for (const MailEntry& e : res->entries) {
            if (e.sender == user_ || e.msg.rfind("R ", 0) != 0) continue;
            std::istringstream is(e.msg);
            std::string tag, choice, nonce;
            std::size_t idx = 0;
            is >> tag >> idx >> choice >> nonce;
            if (idx < rec_.commits.size()) {
                rec_.commits[idx].choice = choice;
                rec_.commits[idx].nonce = nonce;
            }
        }
        pollCursor_ = res->next;
    }

    for (const replay::DecoyCommit& c : rec_.commits)
        if (c.choice.empty()) return false; // still waiting on an opponent reveal
    return true;
}

} // namespace tb::net
