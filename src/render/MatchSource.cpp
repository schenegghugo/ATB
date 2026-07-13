//
// MatchSource.cpp — LocalMatchSource: drive an in-process Battle. See
// MatchSource.h. This is the exact turn-driving logic main.cpp used to run
// inline, moved behind the seam verbatim so play is identical.
//
#include "MatchSource.h"

#include "core/AI.h" // enemyTakeOneAction, AIAction

#include <string>

namespace tb::render {

LocalMatchSource::LocalMatchSource(Battle battle) : battle_(std::move(battle)) {}

bool LocalMatchSource::awaitingLocalInput() const {
    if (battle_.phase() == Phase::Finished) return false;
    // A player inputs only for their own Champions; summons (either team) are AI
    // and objects are inert — those are driven by update().
    return battle_.controlOf(battle_.activeUnit()) == Control::Player;
}

std::optional<std::string> LocalMatchSource::submit(const net::Intent& in) {
    if (battle_.phase() == Phase::Finished) return std::nullopt;
    const EntityId me = battle_.activeUnit();
    switch (in.kind) {
        case net::Intent::Kind::Move: {
            const int moved = battle_.moveToward(me, in.target);
            return moved > 0 ? "Moved " + std::to_string(moved) + " tile(s)."
                             : std::string("Can't move there.");
        }
        case net::Intent::Kind::Cast: {
            const std::vector<Spell>& spells = battle_.unit(me).spells;
            const std::string name = in.spellIdx >= 0 &&
                                             in.spellIdx < static_cast<int>(spells.size())
                                         ? spells[in.spellIdx].name
                                         : std::string("spell");
            if (battle_.cast(me, in.spellIdx, in.target,
                             in.hasTarget2 ? std::optional<Vec2i>(in.target2) : std::nullopt))
                return "Cast " + name + "!";
            return std::string("Cast failed (check AP, range, LOS).");
        }
        case net::Intent::Kind::EndTurn:
            battle_.endTurn();
            aiTimer_ = 0.0f; // pause a beat before the next side's first action
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> LocalMatchSource::update(float dt) {
    if (battle_.phase() == Phase::Finished || awaitingLocalInput()) return std::nullopt;
    aiTimer_ += dt;
    if (aiTimer_ < kAiTick) return std::nullopt;
    aiTimer_ = 0.0f;

    const EntityId active = battle_.activeUnit();
    if (battle_.controlOf(active) == Control::Inert) {
        battle_.endTurn(); // e.g. a bomb: its fuse/ignition ticked at turn start
        return std::nullopt;
    }
    const std::string who = battle_.unit(active).name;
    const AIAction act = enemyTakeOneAction(battle_);
    if (act == AIAction::Attacked) return who + " casts a spell.";
    if (act == AIAction::Moved) return who + " moves.";
    battle_.endTurn();
    return std::nullopt;
}

} // namespace tb::render
