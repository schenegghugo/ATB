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
#include "../core/Spells.h"
#include "../data/BuildRepository.h"

#include <string>
#include <vector>

namespace tb::render {

class BuildEditorScreen {
public:
    enum class Result { None, Fight };

    BuildEditorScreen(const SpellCatalog& catalog, BuildRepository& repo, BuildRules rules);

    // One frame of input + drawing. Returns Fight when the user launches a match.
    Result runFrame(int screenW, int screenH);

    [[nodiscard]] const CharacterBuild& playerBuild() const { return player_; }
    [[nodiscard]] CharacterBuild enemyBuild() const; // loaded from the selected preset

private:
    [[nodiscard]] bool hasSpell(int id) const;
    [[nodiscard]] bool matchesFilter(const SpellDef& d) const;
    void toggleSpell(int id);
    void refreshSaved();

    const SpellCatalog& catalog_;
    BuildRepository& repo_;
    BuildRules rules_;

    CharacterBuild player_;
    std::vector<std::string> savedNames_;
    int enemyIdx_ = 0;
    bool editingName_ = false;
    int filter_ = 0; // category filter: 0=All 1=Damage 2=Effects 3=Support 4=Summon
    std::string statusMsg_;
};

} // namespace tb::render
