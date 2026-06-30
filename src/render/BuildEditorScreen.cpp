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

bool BuildEditorScreen::matchesFilter(const SpellDef& d) const {
    auto has = [&](const char* t) {
        return std::find(d.tags.begin(), d.tags.end(), std::string(t)) != d.tags.end();
    };
    switch (filter_) {
        case 1: return has("damage");
        case 2: return has("buff") || has("debuff") || has("dot");
        case 3: return has("support");
        case 4: return has("summon");
        default: return true; // All
    }
}

BuildEditorScreen::Result BuildEditorScreen::runFrame(int screenW, int screenH) {
    const Vector2 m = GetMousePosition();
    Result result = Result::None;
    ClearBackground(kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);
    const float pad = 16.0f;
    const float rightW = 300.0f;            // fixed right column (stats + budget)
    const float rightX = W - rightW - pad;  // everything left of this is the grid

    DrawText("BUILD EDITOR", 16, 10, 22, kText);
    DrawText("Classless point-buy — filter the dictionary, click cards to add / remove.", 16, 36,
             14, kMuted);

    const BuildValidation val = validateBuild(player_, catalog_, rules_);

    // --- Name field (top-right) ---------------------------------------------
    Rectangle nameRect{rightX, 10, rightW, 30};
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
             static_cast<int>(nameRect.x) + 8, static_cast<int>(nameRect.y) + 8, 16, kText);

    // --- Category filter chips ----------------------------------------------
    static const char* kCats[] = {"All", "Damage", "Effects", "Support", "Summon"};
    float chipX = 16.0f;
    const float chipY = 62.0f;
    for (int i = 0; i < 5; ++i) {
        const float cw = static_cast<float>(MeasureText(kCats[i], 16)) + 22;
        Rectangle chip{chipX, chipY, cw, 28};
        if (button(chip, kCats[i], m, filter_ == i ? kAccent : kPanel)) filter_ = i;
        chipX += cw + 8;
    }

    // --- Spell card grid -----------------------------------------------------
    const float gx0 = 16.0f, gy0 = 100.0f;
    const float gridW = rightX - pad - gx0;
    const float cardW = 172.0f, cardH = 80.0f, gap = 10.0f;
    const int cols = std::max(1, static_cast<int>((gridW + gap) / (cardW + gap)));
    int shown = 0;
    for (const SpellDef& d : catalog_.all()) {
        if (!matchesFilter(d)) continue;
        const int col = shown % cols, gridRow = shown / cols;
        Rectangle card{gx0 + col * (cardW + gap), gy0 + gridRow * (cardH + gap), cardW, cardH};
        const bool picked = hasSpell(d.id);
        Color base = picked ? (hovered(card, m) ? kPickedHot : kPicked)
                            : (hovered(card, m) ? kPanelHot : kPanel);
        DrawRectangleRec(card, base);
        DrawRectangleLinesEx(card, picked ? 2.0f : 1.0f, picked ? kGood : kLine);

        const int cx = static_cast<int>(card.x) + 8;
        DrawText(d.spell.name.c_str(), cx, static_cast<int>(card.y) + 6, 18, kText);
        DrawText(TextFormat("%d", d.buildCost), static_cast<int>(card.x + card.width) - 22,
                 static_cast<int>(card.y) + 6, 18, kAccent);
        DrawText(TextFormat("%d AP   rng %d-%d   %s", d.spell.apCost, d.spell.minRange,
                            d.spell.maxRange, shapeName(d.spell.shape)),
                 cx, static_cast<int>(card.y) + 30, 12, kMuted);
        // First few tags (the secondary categorisation, visible per card).
        std::string tagline;
        for (std::size_t i = 0; i < d.tags.size() && i < 3; ++i) {
            if (i) tagline += " ";
            tagline += d.tags[i];
        }
        DrawText(tagline.c_str(), cx, static_cast<int>(card.y) + 52, 12, kAccent);

        if (pressed(card, m)) toggleSpell(d.id);
        ++shown;
    }
    if (shown == 0)
        DrawText("(no spells match this filter)", static_cast<int>(gx0), static_cast<int>(gy0), 16,
                 kMuted);

    // --- Right column: stat steppers ----------------------------------------
    auto stepper = [&](float sy, const char* label, int& value, int costPer, const char* valFmt) {
        DrawText(label, static_cast<int>(rightX), static_cast<int>(sy) + 6, 18, kText);
        Rectangle minus{rightX + rightW - 132, sy, 30, 30};
        Rectangle plus{rightX + rightW - 52, sy, 30, 30};
        if (button(minus, "-", m, kPanel, value > 0)) --value;
        if (button(plus, "+", m, kPanel)) ++value;
        DrawText(TextFormat(valFmt, value), static_cast<int>(rightX + rightW) - 96,
                 static_cast<int>(sy) + 6, 18, kGood);
        DrawText(TextFormat("%dpt", costPer), static_cast<int>(rightX + rightW) - 18 - 28,
                 static_cast<int>(sy) + 8, 13, kMuted);
    };
    float sy = 56.0f;
    DrawText("Stat upgrades", static_cast<int>(rightX), static_cast<int>(sy), 16, kMuted);
    sy += 24;
    stepper(sy, "+HP", player_.stats.hpPurchases, rules_.hpCost, "x%d"); sy += 38;
    stepper(sy, "+AP", player_.stats.bonusAp, rules_.apCost, "+%d"); sy += 38;
    stepper(sy, "+MP", player_.stats.bonusMp, rules_.mpCost, "+%d"); sy += 38;
    stepper(sy, "+INIT", player_.stats.bonusInitiative, rules_.initCost, "+%d"); sy += 46;

    // --- Budget + validation -------------------------------------------------
    Rectangle barBg{rightX, sy, rightW, 22};
    DrawRectangleRec(barBg, kPanel);
    const float frac =
        val.budget ? std::clamp(static_cast<float>(val.spent) / val.budget, 0.0f, 1.0f) : 0.0f;
    DrawRectangle(static_cast<int>(barBg.x), static_cast<int>(barBg.y),
                  static_cast<int>(barBg.width * frac), static_cast<int>(barBg.height),
                  val.ok ? kGood : kBad);
    DrawRectangleLinesEx(barBg, 1.0f, kLine);
    DrawText(TextFormat("Points: %d / %d", val.spent, val.budget), static_cast<int>(rightX),
             static_cast<int>(sy) + 28, 18, val.ok ? kText : kBad);

    int ey = static_cast<int>(sy) + 54;
    for (const std::string& err : val.errors) {
        DrawText(TextFormat("- %s", err.c_str()), static_cast<int>(rightX), ey, 13, kBad);
        ey += 17;
    }

    // --- Bottom action bar ---------------------------------------------------
    const float by = H - 44;
    Rectangle saveBtn{16, by, 110, 32};
    Rectangle enemyBtn{136, by, 250, 32};
    Rectangle fightBtn{W - 156, by, 140, 32};

    if (button(saveBtn, "Save", m, kPanel, val.ok)) {
        repo_.save(player_);
        refreshSaved();
        statusMsg_ = "Saved '" + player_.name + "'.";
    }
    const char* enemyName = savedNames_.empty() ? "(none)" : savedNames_[enemyIdx_].c_str();
    if (button(enemyBtn, TextFormat("Enemy: %s  >", enemyName), m, kPanel, !savedNames_.empty()))
        enemyIdx_ = (enemyIdx_ + 1) % static_cast<int>(savedNames_.size());
    if (button(fightBtn, "Fight >", m, kAccent, val.ok)) result = Result::Fight;

    if (!statusMsg_.empty())
        DrawText(statusMsg_.c_str(), static_cast<int>(enemyBtn.x), static_cast<int>(by) - 20, 14,
                 kMuted);

    return result;
}

} // namespace tb::render
