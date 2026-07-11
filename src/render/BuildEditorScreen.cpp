#include "BuildEditorScreen.h"

#include "Ui.h"

#include "raylib.h"

#include <algorithm>

namespace tb::render {

namespace {

// Palette comes from the shared, theme-driven ui:: globals (this file predates
// Ui.h and used to keep its own copies).
using ui::kBg;
using ui::kPanel;
using ui::kPanelHot;
using ui::kPicked;
using ui::kPickedHot;
using ui::kText;
using ui::kMuted;
using ui::kAccent;
using ui::kGood;
using ui::kBad;
using ui::kLine;

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
                                     Ruleset ruleset)
    : catalog_(catalog), repo_(repo), ruleset_(std::move(ruleset)) {
    const int n = ruleset_.teamSize > 0 ? ruleset_.teamSize : 1;
    playerTeam_.resize(static_cast<std::size_t>(n));
    if (auto seed = repo_.load("Pyromancer")) {
        playerTeam_[0] = *seed;
    } else {
        playerTeam_[0].name = "Hero";
        playerTeam_[0].spellIds = {spellid::Attack};
    }
    for (int i = 1; i < n; ++i) {
        playerTeam_[i].name = "Hero " + std::to_string(i + 1);
        playerTeam_[i].spellIds = {spellid::Attack};
    }
    refreshSaved();
    int bruiser = 0;
    for (int i = 0; i < static_cast<int>(savedNames_.size()); ++i)
        if (savedNames_[i] == "Bruiser") bruiser = i;
    enemyPicks_.assign(static_cast<std::size_t>(n), bruiser);
}

void BuildEditorScreen::refreshSaved() {
    savedNames_ = repo_.list();
    for (int& pick : enemyPicks_)
        if (pick >= static_cast<int>(savedNames_.size())) pick = 0;
}

bool BuildEditorScreen::hasSpell(int id) const {
    const auto& ids = cur().spellIds;
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void BuildEditorScreen::toggleSpell(int id) {
    auto& ids = cur().spellIds;
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end()) ids.erase(it);
    else ids.push_back(id);
}

std::vector<CharacterBuild> BuildEditorScreen::enemyTeam() const {
    std::vector<CharacterBuild> out;
    for (int idx : enemyPicks_) {
        if (idx >= 0 && idx < static_cast<int>(savedNames_.size())) {
            if (auto b = repo_.load(savedNames_[idx])) {
                out.push_back(*b);
                continue;
            }
        }
        CharacterBuild dummy;
        dummy.name = "Dummy";
        dummy.spellIds = {spellid::Attack};
        out.push_back(dummy);
    }
    if (out.empty()) {
        CharacterBuild dummy;
        dummy.name = "Dummy";
        dummy.spellIds = {spellid::Attack};
        out.push_back(dummy);
    }
    return out;
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

BuildEditorScreen::Result BuildEditorScreen::runFrame(int screenW, int screenH, Mode mode) {
    const Vector2 m = GetMousePosition();
    Result result = Result::None;
    ClearBackground(kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);
    const float pad = 16.0f;
    const float rightW = 300.0f;            // fixed right column (stats + budget)
    const float rightX = W - rightW - pad;  // everything left of this is the grid

    const char* header = mode == Mode::Local    ? "LOCAL MATCH — your team"
                         : mode == Mode::Online ? "PLAY ONLINE — your team"
                                                : "BUILD EDITOR";
    DrawText(header, 16, 10, 22, kText);

    const BuildValidation val = validateBuild(cur(), catalog_, ruleset_.economy, ruleset_.bannedSpells);
    auto isBanned = [&](const std::string& key) {
        return std::find(ruleset_.bannedSpells.begin(), ruleset_.bannedSpells.end(), key) !=
               ruleset_.bannedSpells.end();
    };

    // --- Name field (top-right) edits the current slot ----------------------
    Rectangle nameRect{rightX, 10, rightW, 30};
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) editingName_ = hovered(nameRect, m);
    if (editingName_) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && cur().name.size() < 16)
                cur().name.push_back(static_cast<char>(key));
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !cur().name.empty()) cur().name.pop_back();
        if (IsKeyPressed(KEY_ENTER)) editingName_ = false;
    }
    DrawRectangleRec(nameRect, editingName_ ? kPanelHot : kPanel);
    DrawRectangleLinesEx(nameRect, 1.0f, editingName_ ? kAccent : kLine);
    DrawText(TextFormat("Name: %s%s", cur().name.c_str(), editingName_ ? "_" : ""),
             static_cast<int>(nameRect.x) + 8, static_cast<int>(nameRect.y) + 8, 16, kText);

    // --- Player team slot tabs (author one champion at a time) --------------
    DrawText("Your team:", 16, 44, 14, kMuted);
    {
        float tx = 104.0f;
        for (int i = 0; i < static_cast<int>(playerTeam_.size()); ++i) {
            const bool slotOk =
                validateBuild(playerTeam_[i], catalog_, ruleset_.economy, ruleset_.bannedSpells).ok;
            Rectangle tab{tx, 38, 34, 26};
            const Color base = (i == playerSlot_) ? kAccent : (slotOk ? kPanel : kBad);
            if (button(tab, TextFormat("%d", i + 1), m, base)) {
                playerSlot_ = i;
                editingName_ = false;
            }
            tx += 40.0f;
        }
    }

    // --- Category filter chips ----------------------------------------------
    static const char* kCats[] = {"All", "Damage", "Effects", "Support", "Summon"};
    float chipX = 16.0f;
    const float chipY = 74.0f;
    for (int i = 0; i < 5; ++i) {
        const float cw = static_cast<float>(MeasureText(kCats[i], 16)) + 22;
        Rectangle chip{chipX, chipY, cw, 28};
        if (button(chip, kCats[i], m, filter_ == i ? kAccent : kPanel)) filter_ = i;
        chipX += cw + 8;
    }

    // --- Spell card grid -----------------------------------------------------
    const float gx0 = 16.0f, gy0 = 112.0f;
    const float gridW = rightX - pad - gx0;
    const float cardW = 172.0f, cardH = 80.0f, gap = 10.0f;
    const int cols = std::max(1, static_cast<int>((gridW + gap) / (cardW + gap)));
    int shown = 0;
    for (const SpellDef& d : catalog_.all()) {
        if (!matchesFilter(d)) continue;
        const int col = shown % cols, gridRow = shown / cols;
        Rectangle card{gx0 + col * (cardW + gap), gy0 + gridRow * (cardH + gap), cardW, cardH};
        const bool banned = isBanned(d.key);
        const bool picked = hasSpell(d.id);
        Color base = banned ? kBg
                            : picked ? (hovered(card, m) ? kPickedHot : kPicked)
                                     : (hovered(card, m) ? kPanelHot : kPanel);
        DrawRectangleRec(card, base);
        DrawRectangleLinesEx(card, picked ? 2.0f : 1.0f, picked ? kGood : kLine);

        const int cx = static_cast<int>(card.x) + 8;
        DrawText(d.spell.name.c_str(), cx, static_cast<int>(card.y) + 6, 18, banned ? kMuted : kText);
        DrawText(TextFormat("%d", d.buildCost), static_cast<int>(card.x + card.width) - 22,
                 static_cast<int>(card.y) + 6, 18, banned ? kMuted : kAccent);
        DrawText(TextFormat("%d AP   rng %d-%d   %s", d.spell.apCost, d.spell.minRange,
                            d.spell.maxRange, shapeName(d.spell.shape)),
                 cx, static_cast<int>(card.y) + 30, 12, kMuted);
        if (banned) {
            DrawText("BANNED", cx, static_cast<int>(card.y) + 52, 12, kBad);
        } else {
            std::string tagline; // first few tags (secondary categorisation)
            for (std::size_t i = 0; i < d.tags.size() && i < 3; ++i) {
                if (i) tagline += " ";
                tagline += d.tags[i];
            }
            DrawText(tagline.c_str(), cx, static_cast<int>(card.y) + 52, 12, kAccent);
        }

        if (!banned && pressed(card, m)) toggleSpell(d.id);
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
    stepper(sy, "+HP", cur().stats.hpPurchases, ruleset_.economy.hpCost, "x%d"); sy += 38;
    stepper(sy, "+AP", cur().stats.bonusAp, ruleset_.economy.apCost, "+%d"); sy += 38;
    stepper(sy, "+MP", cur().stats.bonusMp, ruleset_.economy.mpCost, "+%d"); sy += 38;
    stepper(sy, "+INIT", cur().stats.bonusInitiative, ruleset_.economy.initCost, "+%d"); sy += 46;

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

    // --- Enemy team pickers (one slot per teamSize) -------------------------
    DrawText("Enemy team:", 16, static_cast<int>(H) - 78, 14, kMuted);
    {
        float ex = 110.0f;
        for (int i = 0; i < static_cast<int>(enemyPicks_.size()); ++i) {
            const char* nm = savedNames_.empty() ? "(none)" : savedNames_[enemyPicks_[i]].c_str();
            Rectangle pk{ex, H - 82, 150, 26};
            if (button(pk, TextFormat("%d: %s >", i + 1, nm), m, kPanel, !savedNames_.empty()))
                enemyPicks_[i] = (enemyPicks_[i] + 1) % static_cast<int>(savedNames_.size());
            ex += 158.0f;
        }
    }

    // --- Bottom action bar ---------------------------------------------------
    bool teamValid = true;
    for (const CharacterBuild& b : playerTeam_)
        if (!validateBuild(b, catalog_, ruleset_.economy, ruleset_.bannedSpells).ok) teamValid = false;

    const float by = H - 44;
    Rectangle saveBtn{16, by, 130, 32};
    Rectangle menuBtn{154, by, 110, 32};

    if (button(saveBtn, "Save Slot", m, kPanel, val.ok)) {
        repo_.save(cur());
        refreshSaved();
        statusMsg_ = "Saved '" + cur().name + "'.";
    }
    if (button(menuBtn, "< Menu", m, kPanel)) result = Result::Menu;

    // The launch button reflects the entered mode; Edit mode has none.
    if (mode == Mode::Local) {
        if (button({W - 156, by, 140, 32}, "Fight >", m, kAccent, teamValid)) result = Result::Fight;
    } else if (mode == Mode::Online) {
        if (button({W - 176, by, 160, 32}, "Play Online >", m, kAccent, teamValid))
            result = Result::PlayOnline;
    }

    if (!statusMsg_.empty())
        DrawText(statusMsg_.c_str(), 160, static_cast<int>(by) + 8, 14, kMuted);

    return result;
}

} // namespace tb::render
