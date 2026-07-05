#pragma once
//
// BuildEditorScreen.h — Raylib UI for authoring classless point-buy builds.
//
// Pure presentation/input over the headless core: it edits a CharacterBuild
// against the SpellCatalog (the skill dictionary), validates live, and saves
// through the BuildRepository seam. Knows nothing about combat.
//
// Immediate-mode: runFrame() handles input AND draws in one pass (must sit
// between BeginDrawing()/EndDrawing()).
//
#include "../core/Build.h"
#include "../core/Ruleset.h"
#include "../core/Spells.h"
#include "../data/BuildRepository.h"

#include <string>
#include <vector>

namespace tb::render {

class BuildEditorScreen {
public:
    enum class Result { None, Fight, PlayOnline, Menu };

    // How the editor was entered (from the mode-first menu): its primary action
    // reflects the mode — Local shows "Fight", Online shows "Play Online", Edit is
    // pure authoring (Save + Menu, no launch button).
    enum class Mode { Local, Online, Edit };

    // `ruleset` supplies the economy (validation/budget) and teamSize (how many
    // builds per side the editor authors).
    BuildEditorScreen(const SpellCatalog& catalog, BuildRepository& repo, Ruleset ruleset);

    // One frame of input + drawing. Returns Fight / PlayOnline when the user
    // launches, or Menu when they go back.
    Result runFrame(int screenW, int screenH, Mode mode = Mode::Edit);

    // The authored player team and the picked enemy team (each sized to teamSize).
    [[nodiscard]] const std::vector<CharacterBuild>& playerTeam() const { return playerTeam_; }
    [[nodiscard]] std::vector<CharacterBuild> enemyTeam() const;

private:
    [[nodiscard]] CharacterBuild& cur() { return playerTeam_[playerSlot_]; }
    [[nodiscard]] const CharacterBuild& cur() const { return playerTeam_[playerSlot_]; }
    [[nodiscard]] bool hasSpell(int id) const;
    [[nodiscard]] bool matchesFilter(const SpellDef& d) const;
    void toggleSpell(int id);
    void refreshSaved();

    const SpellCatalog& catalog_;
    BuildRepository& repo_;
    Ruleset ruleset_;

    std::vector<CharacterBuild> playerTeam_; // one per slot (size = teamSize)
    int playerSlot_ = 0;                     // which player slot is being edited
    std::vector<int> enemyPicks_;            // index into savedNames_ per enemy slot
    std::vector<std::string> savedNames_;
    bool editingName_ = false;
    int filter_ = 0; // category filter: 0=All 1=Damage 2=Effects 3=Support 4=Summon
    std::string statusMsg_;
};

} // namespace tb::render
