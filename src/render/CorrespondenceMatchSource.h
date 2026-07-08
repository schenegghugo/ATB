#pragma once
//
// CorrespondenceMatchSource.h — Play a correspondence (Unlimited) game through the
// MatchSource seam (Phase 4.5 slice 5 / CR.6, GUI).
//
// Wraps a net::CorrespondenceSession: the local player's Intents are applied +
// posted (submit); update() pulls the opponent's moves each frame (sync) and, once
// the game finishes, exchanges the decoy-reveals and submits the scoresheet to the
// lobby for ranking — all behind the same render path as a live/local match. When
// it is the opponent's turn the HUD shows a "waiting…" status (a correspondence
// game may be resumed later; the server holds the log).
//
#include "MatchSource.h"
#include "net/Correspondence.h"
#include "net/Lobby.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace tb::render {

class CorrespondenceMatchSource final : public MatchSource {
public:
    CorrespondenceMatchSource(std::unique_ptr<net::CorrespondenceSession> session,
                              net::LobbySession* lobby, std::string game, Faction seat, bool rated)
        : session_(std::move(session)), lobby_(lobby), game_(std::move(game)), seat_(seat),
          rated_(rated) {}

    [[nodiscard]] const Battle& battle() const override { return session_->battle(); }
    [[nodiscard]] bool awaitingLocalInput() const override { return session_->awaitingMe(); }

    std::optional<std::string> submit(const net::Intent& in) override {
        std::string err;
        // Pass 'a' unconditionally: submitLocal ignores it unless this is a decoy
        // cast, where it means "stay the original". Choosing the twin needs a UI
        // prompt — a follow-up; default keeps the original.
        if (!session_->submitLocal(in, 'a', &err)) return "Illegal move: " + err;
        return std::nullopt;
    }

    std::optional<std::string> update(float) override {
        session_->sync();
        if (!session_->finished())
            return session_->awaitingMe() ? std::nullopt
                                          : std::optional<std::string>("Waiting for your opponent…");
        // Finished: reconcile the decoy reveals, then submit the scoresheet once.
        if (!submitted_) {
            if (!session_->finalize()) return std::optional<std::string>("Finishing — exchanging results…");
            const net::SubmitResult r =
                lobby_ ? lobby_->submitScore(game_, seat_, session_->notation()) : net::SubmitResult{};
            submitted_ = true;
            resultMsg_ = describe(r);
        }
        return resultMsg_;
    }

    [[nodiscard]] bool finished() const { return session_->finished(); }
    [[nodiscard]] bool matchOver() const override { return session_->finished(); }
    [[nodiscard]] Faction localSeat() const override { return seat_; }

private:
    static std::string describe(const net::SubmitResult& r) {
        using S = net::SubmitResult::Status;
        switch (r.status) {
            case S::Ranked:
                return r.winner.empty() ? "Game over — draw (ranked)."
                                        : ("Game over — " + r.winner + " wins (ranked).");
            case S::Pending: return "Game over — submitted; waiting for your opponent to submit.";
            case S::Casual: return "Game over (casual).";
            case S::Rejected: return "Game over — result rejected: " + r.error;
        }
        return "Game over.";
    }

    std::unique_ptr<net::CorrespondenceSession> session_;
    net::LobbySession* lobby_;
    std::string game_;
    Faction seat_;
    bool rated_;
    bool submitted_ = false;
    std::string resultMsg_;
};

} // namespace tb::render
