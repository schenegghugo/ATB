#include "BuildEditorScreen.h"

#include "raylib.h"

#include <algorithm>

namespace tb::render {

namespace {

constexpr Color kBg{18, 20, 28, 255};
constexpr Color kPanel{30, 34, 46, 255};
constexpr Color kPanelHot{44, 50, 66, 255};
constexpr Color kPicked{40, 90, 60, 255};
constexpr Color kPickedHot{52, 116, 78, 255};
constexpr Color kText{220, 224, 235, 255};
constexpr Color kMuted{150, 156, 170, 255};
constexpr Color kAccent{230, 140, 50, 255};
constexpr Color kGood{90, 200, 130, 255};
constexpr Color kBad{210, 90, 90, 255};
constexpr Color kLine{0, 0, 0, 160};

bool hovered(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }
bool pressed(Rectangle r, Vector2 m) {
    return hovered(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void textCentered(const char* s, Rectangle r, int size, Color c) {
    int w = MeasureText(s, size);
    DrawText(s, static_cast<int>(r.x + (r.width - w) / 2),
             static_cast<int>(r.y + (r.height - size) / 2), size, c);
}

bool button(Rectangle r, const char* label, Vector2 m, Color base, bool enabled = true) {
    Color fill = !enabled ? kPanel : (hovered(r, m) ? kPanelHot : base);
    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, 1.0f, kLine);
    textCentered(label, r, 18, enabled ? kText : kMuted);
    return enabled && pressed(r, m);
}

const char* shapeName(TargetShape s) {
    switch (s) {
        case TargetShape::Single: return "single";
        case TargetShape::Line: return "line";
        case TargetShape::Cross: return "cross";
        case TargetShape::Circle: return "circle";
    }
    return "?";
}

} // namespace

BuildEditorScreen::BuildEditorScreen(const SpellCatalog& catalog, BuildRepository& repo,
                                     BuildRules rules)
    : catalog_(catalog), repo_(repo), rules_(rules) {
    if (auto seed = repo_.load("Pyromancer")) {
        player_ = *seed;
    } else {
        player_.name = "Hero";
        player_.spellIds = {spellid::Attack};
    }
    refreshSaved();
    auto it = std::find(savedNames_.begin(), savedNames_.end(), std::string("Bruiser"));
    if (it != savedNames_.end()) enemyIdx_ = static_cast<int>(it - savedNames_.begin());
}

void BuildEditorScreen::refreshSaved() {
    savedNames_ = repo_.list();
    if (enemyIdx_ >= static_cast<int>(savedNames_.size())) enemyIdx_ = 0;
}

bool BuildEditorScreen::hasSpell(int id) const {
    return std::find(player_.spellIds.begin(), player_.spellIds.end(), id) !=
           player_.spellIds.end();
}

void BuildEditorScreen::toggleSpell(int id) {
    auto it = std::find(player_.spellIds.begin(), player_.spellIds.end(), id);
    if (it != player_.spellIds.end()) player_.spellIds.erase(it);
    else player_.spellIds.push_back(id);
}

CharacterBuild BuildEditorScreen::enemyBuild() const {
    if (!savedNames_.empty()) {
        if (auto b = repo_.load(savedNames_[enemyIdx_])) return *b;
    }
    CharacterBuild fallback;
    fallback.name = "Dummy";
    fallback.spellIds = {spellid::Attack};
    return fallback;
}

BuildEditorScreen::Result BuildEditorScreen::runFrame(int screenW, int screenH) {
    const Vector2 m = GetMousePosition();
    Result result = Result::None;
    ClearBackground(kBg);

    DrawText("BUILD EDITOR", 16, 12, 24, kText);
    DrawText("Classless point-buy — spend the budget on skills + stats. No classes.", 16, 40, 16,
             kMuted);

    const BuildValidation val = validateBuild(player_, catalog_, rules_);

    // --- Name field (editable) ----------------------------------------------
    const float rightX = 384.0f;
    const float rightW = static_cast<float>(screenW) - rightX - 16.0f;
    Rectangle nameRect{rightX, 64, rightW, 32};
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) editingName_ = hovered(nameRect, m);
    if (editingName_) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && player_.name.size() < 16)
                player_.name.push_back(static_cast<char>(key));
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !player_.name.empty()) player_.name.pop_back();
        if (IsKeyPressed(KEY_ENTER)) editingName_ = false;
    }
    DrawRectangleRec(nameRect, editingName_ ? kPanelHot : kPanel);
    DrawRectangleLinesEx(nameRect, 1.0f, editingName_ ? kAccent : kLine);
    DrawText(TextFormat("Name: %s%s", player_.name.c_str(), editingName_ ? "_" : ""),
             static_cast<int>(nameRect.x) + 8, static_cast<int>(nameRect.y) + 8, 18, kText);

    // --- Catalog list (left) -------------------------------------------------
    DrawText("Skill Dictionary  (click to add / remove)", 16, 48, 16, kMuted);
    const float rowH = 46.0f;
    float y = 72.0f;
    for (const SpellDef& d : catalog_.all()) {
        Rectangle row{16, y, 352, rowH - 6};
        const bool picked = hasSpell(d.id);
        Color base = picked ? (hovered(row, m) ? kPickedHot : kPicked)
                            : (hovered(row, m) ? kPanelHot : kPanel);
        DrawRectangleRec(row, base);
        DrawRectangleLinesEx(row, 1.0f, kLine);

        // Checkbox marker.
        Rectangle box{row.x + 8, row.y + 8, 22, 22};
        DrawRectangleRec(box, picked ? kGood : kBg);
        DrawRectangleLinesEx(box, 1.0f, kLine);
        if (picked) textCentered("x", box, 18, kBg);

        DrawText(TextFormat("%s", d.spell.name.c_str()), static_cast<int>(row.x) + 40,
                 static_cast<int>(row.y) + 4, 18, kText);
        DrawText(TextFormat("%d AP  range %d-%d  %s", d.spell.apCost, d.spell.minRange,
                            d.spell.maxRange, shapeName(d.spell.shape)),
                 static_cast<int>(row.x) + 40, static_cast<int>(row.y) + 22, 13, kMuted);
        DrawText(TextFormat("%d pt", d.buildCost), static_cast<int>(row.x + row.width) - 52,
                 static_cast<int>(row.y) + 10, 18, kAccent);

        if (pressed(row, m)) toggleSpell(d.id);
        y += rowH;
    }

    // --- Stat steppers (right) ----------------------------------------------
    auto stepper = [&](float sy, const char* label, int& value, int costPer, const char* valFmt) {
        DrawText(label, static_cast<int>(rightX), static_cast<int>(sy) + 6, 18, kText);
        Rectangle minus{rightX + 150, sy, 30, 30};
        Rectangle plus{rightX + 230, sy, 30, 30};
        if (button(minus, "-", m, kPanel, value > 0)) --value;
        if (button(plus, "+", m, kPanel)) ++value;
        DrawText(TextFormat(valFmt, value), static_cast<int>(rightX) + 186,
                 static_cast<int>(sy) + 6, 18, kGood);
        DrawText(TextFormat("%d pt", costPer), static_cast<int>(rightX) + 270,
                 static_cast<int>(sy) + 8, 14, kMuted);
    };
    DrawText("Stat upgrades", static_cast<int>(rightX), 108, 16, kMuted);
    stepper(128, "+HP", player_.stats.hpPurchases, rules_.hpCost, "x%d");
    stepper(166, "+AP", player_.stats.bonusAp, rules_.apCost, "+%d");
    stepper(204, "+MP", player_.stats.bonusMp, rules_.mpCost, "+%d");

    // --- Budget + validation -------------------------------------------------
    Rectangle barBg{rightX, 252, rightW, 22};
    DrawRectangleRec(barBg, kPanel);
    float frac = val.budget ? std::clamp(static_cast<float>(val.spent) / val.budget, 0.0f, 1.0f) : 0;
    DrawRectangle(static_cast<int>(barBg.x), static_cast<int>(barBg.y),
                  static_cast<int>(barBg.width * frac), static_cast<int>(barBg.height),
                  val.ok ? kGood : kBad);
    DrawRectangleLinesEx(barBg, 1.0f, kLine);
    DrawText(TextFormat("Points: %d / %d", val.spent, val.budget), static_cast<int>(rightX),
             280, 18, val.ok ? kText : kBad);

    int ey = 306;
    for (const std::string& err : val.errors) {
        DrawText(TextFormat("- %s", err.c_str()), static_cast<int>(rightX), ey, 14, kBad);
        ey += 18;
    }

    // --- Bottom action bar ---------------------------------------------------
    const float by = static_cast<float>(screenH) - 44;
    Rectangle saveBtn{16, by, 110, 32};
    Rectangle enemyBtn{136, by, 250, 32};
    Rectangle fightBtn{static_cast<float>(screenW) - 156, by, 140, 32};

    if (button(saveBtn, "Save", m, kPanel, val.ok)) {
        repo_.save(player_);
        refreshSaved();
        statusMsg_ = "Saved '" + player_.name + "'.";
    }

    const char* enemyName = savedNames_.empty() ? "(none)" : savedNames_[enemyIdx_].c_str();
    if (button(enemyBtn, TextFormat("Enemy: %s  >", enemyName), m, kPanel,
               !savedNames_.empty())) {
        enemyIdx_ = (enemyIdx_ + 1) % static_cast<int>(savedNames_.size());
    }

    if (button(fightBtn, "Fight >", m, kAccent, val.ok)) {
        result = Result::Fight;
    }

    if (!statusMsg_.empty())
        DrawText(statusMsg_.c_str(), static_cast<int>(enemyBtn.x), static_cast<int>(by) - 20, 14,
                 kMuted);

    return result;
}

} // namespace tb::render
