#pragma once
//
// ReadyCheckScreen.h — the per-match ready check (Phase 4.5 slice 6.2, GUI).
//
// After a pairing, BOTH players get this screen: confirm/edit the build and click
// READY within the window, or Decline. When both ready the server pairs them
// (→ Matched); a timeout or either player's decline cancels (→ Cancelled). Reached
// from the lobby (you accepted, or someone accepted yours via poll).
//
#include "net/Lobby.h"

#include <string>

namespace tb::render {

class ReadyCheckScreen {
public:
    enum class Result { None, Matched, Cancelled, EditBuild };

    // Enter with a fresh ready check (resets the countdown + state).
    void begin(const net::ReadyCheckInfo& rc);

    // `myBuild` is the build you'll ready with (edit it via EditBuild). Returns
    // Matched (pairing() valid), Cancelled (→ lobby), or EditBuild (→ editor).
    Result runFrame(int screenW, int screenH, net::LobbySession& session,
                    const CharacterBuild& myBuild);

    [[nodiscard]] const net::PairedInfo& pairing() const { return paired_; }
    void setStatus(std::string s) { status_ = std::move(s); }

private:
    net::ReadyCheckInfo rc_;
    net::PairedInfo paired_;
    float remaining_ = 30.0f; // local countdown (the server enforces its own)
    bool readied_ = false;    // we clicked READY, waiting on the opponent
    float pollTimer_ = 0.0f;
    std::string status_;
};

} // namespace tb::render
