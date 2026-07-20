#pragma once
//
// CorrespondenceMatchSource.h — Play a correspondence (Unlimited) game through the
// MatchSource seam (Phase 4.5 slice 5 / CR.6, GUI).
//
// Wraps a net::CorrespondenceSession: the local player's Intents are applied +
// posted (submit); update() pulls the opponent's moves each frame (sync) and, once
// the game finishes, exchanges the decoy-reveals and submits the scoresheet to the
// lobby for ranking — all behind the same render path as a live/local match. When
// it is the opponent's turn the HUD shows a "waiting..." status (a correspondence
// game may be resumed later; the server holds the log).
//
#include "MatchSource.h"
#include "net/Correspondence.h"
#include "net/Lobby.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace tb::render {

class CorrespondenceMatchSource final : public MatchSource {
public:
    // `myUser` is this client's lobby username, used to colour the chat transcript
    // (a sender that isn't me is the opponent). `secretsPath`, if set, is where my
    // decoy commitment secrets are persisted after every local move (the log holds
    // only the hashes), so a cold-resumed client can still reveal at finalize.
    CorrespondenceMatchSource(std::unique_ptr<net::CorrespondenceSession> session,
                              net::LobbySession* lobby, std::string game, Faction seat, bool rated,
                              std::string myUser = "", std::string secretsPath = "")
        : session_(std::move(session)), lobby_(lobby), game_(std::move(game)), seat_(seat),
          rated_(rated), myUser_(std::move(myUser)), secretsPath_(std::move(secretsPath)) {}

    [[nodiscard]] const Battle& battle() const override { return session_->battle(); }
    [[nodiscard]] bool awaitingLocalInput() const override { return session_->awaitingMe(); }

    std::optional<std::string> submit(const net::Intent& in) override {
        // Decoy casts don't come through here — needsDecoyChoice() routes them to
        // submitWithChoice() after the UI prompt. ('a' is inert on non-decoys.)
        return submitWithChoice(in, 'a');
    }

    [[nodiscard]] bool needsDecoyChoice(const net::Intent& in) const override {
        return session_->wouldCastDecoy(in);
    }
    std::optional<std::string> submitWithChoice(const net::Intent& in, char choice) override {
        std::string err;
        if (!session_->submitLocal(in, choice, &err)) return "Illegal move: " + err;
        persistSecrets();
        return std::nullopt;
    }

    std::optional<std::string> update(float dt) override {
        // Correspondence chat (4.6) rides this lobby session in a side log; poll it
        // on a throttle like the moves (a blocking RPC — keep it off every frame).
        chatTimer_ += dt;
        if (lobby_ && chatTimer_ >= kChatPollSec) {
            chatTimer_ = 0.0f;
            if (const std::optional<net::ChannelPoll> cp = lobby_->corrChatPoll(game_, chatCursor_)) {
                for (const net::MailEntry& e : cp->entries)
                    chat_.push_back({e.sender == myUser_ ? seat_ : opposing(seat_), e.msg});
                chatCursor_ = cp->next;
            }
        }
        session_->sync();
        if (!session_->finished())
            return session_->awaitingMe() ? std::nullopt
                                          : std::optional<std::string>("Waiting for your opponent...");
        // Finished: reconcile the decoy reveals, then submit the scoresheet once.
        if (!submitted_) {
            if (!session_->finalize()) return std::optional<std::string>("Finishing - exchanging results...");
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

    [[nodiscard]] bool chatEnabled() const override { return lobby_ != nullptr; }
    [[nodiscard]] const std::vector<net::ChatLine>& chatLog() const override { return chat_; }
    void sendChat(const std::string& text) override {
        if (lobby_) lobby_->corrChatSend(game_, text);
    }

private:
    void persistSecrets() const {
        if (secretsPath_.empty()) return;
        const std::vector<net::DecoySecret> s = session_->mySecrets();
        if (s.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(secretsPath_).parent_path(), ec);
        std::ofstream out(secretsPath_, std::ios::trunc);
        out << net::serializeDecoySecrets(s);
    }

    static std::string describe(const net::SubmitResult& r) {
        using S = net::SubmitResult::Status;
        switch (r.status) {
            case S::Ranked:
                return r.winner.empty() ? "Game over - draw (ranked)."
                                        : ("Game over - " + r.winner + " wins (ranked).");
            case S::Pending: return "Game over - submitted; waiting for your opponent to submit.";
            case S::Casual: return "Game over (casual).";
            case S::Rejected: return "Game over - result rejected: " + r.error;
        }
        return "Game over.";
    }

    static constexpr float kChatPollSec = 1.0f;
    std::unique_ptr<net::CorrespondenceSession> session_;
    net::LobbySession* lobby_;
    std::string game_;
    Faction seat_;
    bool rated_;
    std::string myUser_;
    std::string secretsPath_;
    bool submitted_ = false;
    std::string resultMsg_;
    std::vector<net::ChatLine> chat_;
    std::size_t chatCursor_ = 0;
    float chatTimer_ = 1.0f; // poll immediately on the first frame
};

} // namespace tb::render
