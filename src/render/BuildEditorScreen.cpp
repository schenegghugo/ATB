#include "BuildEditorScreen.h"

#include "Ui.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

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

// --- Spell hover popup (build editor) ---------------------------------------
std::string withSign(int v) { return (v >= 0 ? "+" : "") + std::to_string(v); }

const char* elementName(Element el) {
    switch (el) {
        case Element::Fire: return "Fire";
        case Element::Water: return "Water";
        case Element::Ice: return "Ice";
        case Element::Poison: return "Poison";
        case Element::Electric: return "Electric";
        case Element::Heal: return "Healing";
        case Element::Oil: return "Oil";
        case Element::Steam: return "Steam";
        case Element::None: break;
    }
    return "elemental";
}

// A plain-language one-liner for how a spell actually plays — the "why", which
// the mechanical effect list below can't convey (e.g. Storm's water+lightning
// combo). Keyed by the catalog slug; blank for spells (or mods) without one, so
// the popup simply omits it. Presentation-only — never touches the sim or hash.
std::string spellBlurb(const std::string& key) {
    if (key == "attack") return "Reliable short-range strike. Cheap, no cooldown.";
    if (key == "fireball") return "Ranged blast that hits everything in a small circle.";
    if (key == "poison") return "Hits on impact, then bleeds the target for more over several turns.";
    if (key == "knockback")
        return "Light hit that shoves the target back - slam them into walls or off their tile.";
    if (key == "harpoon") return "Yanks the target toward you, then hits - drag foes out of cover.";
    if (key == "bulwark") return "Shields an ally, soaking the next chunk of incoming damage.";
    if (key == "mend") return "Restores health to an ally.";
    if (key == "shelter") return "Raises a wall line that blocks both movement and line of sight.";
    if (key == "invisible") return "Cloaks an ally - can't be directly targeted for a couple of turns.";
    if (key == "portal")
        return "Two tiles: whoever stands on or steps onto the entry - units AND bombs - is "
               "teleported to the exit.";
    if (key == "glyph") return "Marks tiles that shove away anyone who steps onto them.";
    if (key == "rewind") return "Tags a unit; after a couple of turns it snaps back to where it was.";
    if (key == "bomb")
        return "Lobs a bomb that blows up on its 2nd turn (radius-1). Push, pull, or PORTAL it onto "
               "enemies.";
    if (key == "blocker") return "Summons a sturdy AI blocker (max 2 summons per team).";
    if (key == "healer") return "Summons an AI healer that mends your team (max 2 summons per team).";
    if (key == "brute") return "Summons an aggressive AI attacker (max 2 summons per team).";
    if (key == "blind") return "Slashes the target's spell range for a few turns.";
    if (key == "surge") return "Big AP boost now - but the unit crashes and loses AP a couple of turns later.";
    if (key == "flux") return "Shifts movement points: speeds an ally, or slows a foe.";
    if (key == "decoy") return "Swaps in an identical twin - enemies can't tell which is real until one acts.";
    if (key == "storm")
        return "Soaks a circle in water, then a stormcloud bursts and electrifies it: Water + "
               "Electric = heavy shock across the whole pool.";
    if (key == "blizzard") return "Ice cone: damages, freezes (roots) foes, and leaves an icy surface.";
    if (key == "ignite") return "Sets tiles ablaze - Fire burns anyone standing in it.";
    if (key == "puddle") return "Douses tiles with water - sets up Electric (shock) and Fire (steam) combos.";
    if (key == "electrify")
        return "Charges tiles with electricity - shocks anyone on them, and goes live on water.";
    return "";
}

// Greedy word-wrap to a maximum pixel width at font size `size`.
std::vector<std::string> wrapText(const std::string& text, int size, int maxW) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && MeasureText(cand.c_str(), size) > maxW) {
            out.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flush();
        else word.push_back(c);
    }
    flush();
    if (!line.empty()) out.push_back(line);
    return out;
}

std::string statusText(const StatusEffect& s, bool polarized) {
    const std::string t = " for " + std::to_string(s.remainingTurns) + " turns";
    switch (s.kind) {
        case StatusEffect::Kind::DamageOverTime:
            return "Damage over time: " + std::to_string(s.magnitude) + "/turn" + t;
        case StatusEffect::Kind::Shield:
            return "Shield: absorbs " + std::to_string(s.magnitude) + t;
        case StatusEffect::Kind::ApBuff:
            return withSign(s.magnitude) + " AP" + t + (s.delay > 0 ? " (delayed)" : "");
        case StatusEffect::Kind::MpBuff:
            return polarized ? "MP: " + withSign(s.magnitude) + " ally / " + withSign(-s.magnitude) +
                                   " foe" + t
                             : withSign(s.magnitude) + " MP" + t;
        case StatusEffect::Kind::Invisible: return "Invisible" + t;
        case StatusEffect::Kind::Rewind: return "Rewind: snaps back after" + t.substr(4);
        case StatusEffect::Kind::RangeDebuff:
            return "Blind: -" + std::to_string(s.magnitude) + "% range" + t;
    }
    return "a status";
}

std::string describeEffect(const Effect& fx) {
    switch (fx.type) {
        case Effect::Type::Damage: return "Deals " + std::to_string(fx.amount) + " damage";
        case Effect::Type::Heal: return "Heals " + std::to_string(fx.amount) + " HP";
        case Effect::Type::Push: return "Pushes the target " + std::to_string(fx.amount) + " tiles";
        case Effect::Type::Pull: return "Pulls the target " + std::to_string(fx.amount) + " tiles";
        case Effect::Type::ApplyStatus: return statusText(fx.status, fx.polarized);
        case Effect::Type::Spawn: {
            const char* g = fx.ground.kind == GroundKind::Wall    ? "wall line"
                            : fx.ground.kind == GroundKind::Glyph ? "repel glyph"
                                                                  : "portal";
            return "Conjures a " + std::string(g) + " (" + std::to_string(fx.ground.duration) +
                   " turns)";
        }
        case Effect::Type::Summon: return "Summons a " + fx.creature;
        case Effect::Type::Decoy: return "Creates a decoy for " + std::to_string(fx.amount) + " turns";
        case Effect::Type::PaintSurface:
            return "Paints a " + std::string(elementName(fx.element)) + " surface (" +
                   std::to_string(fx.amount) + " turns)";
    }
    return "";
}

// Total up-front damage a spell deals (sums its Damage effects; DoT is shown
// separately in the effect list). 0 = the spell deals no direct damage.
int directDamage(const Spell& sp) {
    int dmg = 0;
    for (const Effect& fx : sp.effects)
        if (fx.type == Effect::Type::Damage) dmg += fx.amount;
    return dmg;
}

// A floating info card for the hovered spell, anchored beside `card` and clamped
// to the screen. Drawn last in the frame so it sits above every other widget.
void drawSpellPopup(const SpellDef& d, Rectangle card, int screenW, int screenH) {
    const Spell& sp = d.spell;
    const int titleSize = 18, lineSize = 13, pad = 12, lineH = 18, blurbW = 340;

    // The plain-language "how it works" line, word-wrapped to a comfortable width.
    const std::vector<std::string> blurb = wrapText(spellBlurb(d.key), lineSize, blurbW);

    // A damage headline for spells that deal direct hits (so damage reads at a glance).
    std::string headline;
    if (const int dmg = directDamage(sp); dmg > 0)
        headline = std::to_string(dmg) + " damage" +
                   (sp.shape != TargetShape::Single ? " (to each in the area)" : "");

    std::vector<std::string> lines;
    lines.push_back("Build cost: " + std::to_string(d.buildCost) + " pts     AP cost: " +
                    std::to_string(sp.apCost));
    std::string rng = "Range " + std::to_string(sp.minRange) + "-" + std::to_string(sp.maxRange) +
                      "     Shape " + shapeName(sp.shape);
    if (sp.radius > 0) rng += " (r" + std::to_string(sp.radius) + ")";
    lines.push_back(rng);
    lines.push_back(std::string(sp.needsLineOfSight ? "Needs line of sight" : "Ignores line of sight") +
                    "     Cooldown " +
                    (sp.cooldown > 0 ? std::to_string(sp.cooldown) + " turns" : std::string("none")));
    lines.push_back("");
    for (const Effect& fx : sp.effects) {
        const std::string s = describeEffect(fx);
        if (!s.empty()) lines.push_back("- " + s);
    }

    int wNeed = MeasureText(sp.name.c_str(), titleSize);
    if (!headline.empty()) wNeed = std::max(wNeed, MeasureText(headline.c_str(), lineSize));
    for (const std::string& s : blurb) wNeed = std::max(wNeed, MeasureText(s.c_str(), lineSize));
    for (const std::string& s : lines) wNeed = std::max(wNeed, MeasureText(s.c_str(), lineSize));
    const int popW = wNeed + pad * 2;
    const int blurbBlock = static_cast<int>(blurb.size()) * lineH + (blurb.empty() ? 0 : 6);
    const int headBlock = headline.empty() ? 0 : lineH + 2;
    const int popH = pad * 2 + titleSize + 8 + headBlock + blurbBlock +
                     static_cast<int>(lines.size()) * lineH;

    int px = static_cast<int>(card.x + card.width + 8); // prefer to the card's right
    int py = static_cast<int>(card.y);
    if (px + popW > screenW - 8) px = static_cast<int>(card.x) - popW - 8; // flip left if clipped
    px = std::clamp(px, 8, std::max(8, screenW - popW - 8));
    py = std::clamp(py, 8, std::max(8, screenH - popH - 8));

    DrawRectangle(px, py, popW, popH, Color{16, 18, 26, 246});
    DrawRectangleLines(px, py, popW, popH, kAccent);
    DrawText(sp.name.c_str(), px + pad, py + pad, titleSize, kText);
    int ly = py + pad + titleSize + 8;
    if (!headline.empty()) {
        DrawText(headline.c_str(), px + pad, ly, lineSize, kBad); // damage stands out
        ly += lineH + 2;
    }
    for (const std::string& s : blurb) {
        DrawText(s.c_str(), px + pad, ly, lineSize, kText);
        ly += lineH;
    }
    if (!blurb.empty()) ly += 6;
    for (const std::string& s : lines) {
        DrawText(s.c_str(), px + pad, ly, lineSize, s.rfind("- ", 0) == 0 ? kGood : kMuted);
        ly += lineH;
    }
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
                         : mode == Mode::Draft  ? "DRAFT — author your champion"
                                                : "BUILD EDITOR";
    DrawText(header, 16, 10, 22, kText);

    // Draft header: the per-pick countdown (ALWAYS visible while authoring — this is
    // the fix for "the clock isn't shown in the editor") + a scout line of revealed foes.
    if (mode == Mode::Draft) {
        const int secs = static_cast<int>(std::max(0.0f, draftSecondsLeft_) + 0.999f);
        const std::string t =
            (draftPickLabel_.empty() ? "" : draftPickLabel_ + "  ·  ") + std::to_string(secs) + "s";
        const int tw = MeasureText(t.c_str(), 22);
        DrawText(t.c_str(), static_cast<int>(W / 2) - tw / 2, 10, 22, secs <= 5 ? kBad : kAccent);
        if (!draftScoutLine_.empty()) {
            const int sw = MeasureText(draftScoutLine_.c_str(), 13);
            DrawText(draftScoutLine_.c_str(), static_cast<int>(W / 2) - sw / 2, 34, 13, kMuted);
        }
    }

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
    const SpellDef* hoveredDef = nullptr; // for the info popup (drawn last, on top)
    Rectangle hoveredRect{};
    for (const SpellDef& d : catalog_.all()) {
        if (!matchesFilter(d)) continue;
        const int col = shown % cols, gridRow = shown / cols;
        Rectangle card{gx0 + col * (cardW + gap), gy0 + gridRow * (cardH + gap), cardW, cardH};
        if (hovered(card, m)) { hoveredDef = &d; hoveredRect = card; }
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
    // Local/Edit only: online (Online/Draft) picks your opponent server-side, so the
    // pickers would be meaningless noise — the DraftScreen shows the real revealed foes.
    if (mode == Mode::Local || mode == Mode::Edit) {
        DrawText("Enemy team:", 16, static_cast<int>(H) - 78, 14, kMuted);
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
    Rectangle saveBtn{16, by, 96, 32};
    Rectangle loadBtn{118, by, 92, 32};
    Rectangle exportBtn{216, by, 104, 32};
    Rectangle importBtn{326, by, 104, 32};
    Rectangle menuBtn{436, by, 84, 32};
    Rectangle resetBtn{526, by, 84, 32};

    if (button(saveBtn, "Save", m, kPanel, val.ok)) {
        repo_.save(cur());
        refreshSaved();
        statusMsg_ = "Saved '" + cur().name + "' to your build book.";
    }
    // Load pulls the next build from the local build book into this slot; click
    // again to cycle through them.
    if (button(loadBtn, "Load >", m, kPanel, !savedNames_.empty())) {
        loadIdx_ %= static_cast<int>(savedNames_.size());
        if (auto b = repo_.load(savedNames_[loadIdx_])) {
            cur() = *b;
            editingName_ = false;
            statusMsg_ = "Loaded '" + savedNames_[loadIdx_] + "' (" + std::to_string(loadIdx_ + 1) +
                         "/" + std::to_string(savedNames_.size()) + ") - click Load for the next.";
            loadIdx_ = (loadIdx_ + 1) % static_cast<int>(savedNames_.size());
        }
    }
    // Export/Import a single build as a shareable code via the clipboard — the
    // low-friction way to swap "build books" with a friend (paste into chat).
    if (button(exportBtn, "Copy code", m, kPanel)) {
        SetClipboardText(serializeBuild(cur()).c_str());
        statusMsg_ = "Copied '" + cur().name + "' to the clipboard - share the code with a friend.";
    }
    if (button(importBtn, "Paste code", m, kPanel)) {
        const char* clip = GetClipboardText();
        if (std::optional<CharacterBuild> b = clip ? deserializeBuild(clip) : std::nullopt) {
            cur() = *b;
            editingName_ = false;
            statusMsg_ = "Imported build '" + cur().name + "' from the clipboard.";
        } else {
            statusMsg_ = "Clipboard doesn't hold a valid build code.";
        }
    }
    if (button(menuBtn, "< Menu", m, kPanel)) result = Result::Menu;
    // Reset: hand every spent point back at once (clears spells + stat upgrades,
    // keeps the name). Disabled on an already-empty build.
    if (button(resetBtn, "Reset", m, kPanel, val.spent > 0)) {
        cur().spellIds.clear();
        cur().stats = {};
        statusMsg_ = "Build reset — all points refunded.";
    }

    // The launch button reflects the entered mode; Edit mode has none.
    if (mode == Mode::Local) {
        if (button({W - 156, by, 140, 32}, "Fight >", m, kAccent, teamValid)) result = Result::Fight;
    } else if (mode == Mode::Online) {
        if (button({W - 176, by, 160, 32}, "Play Online >", m, kAccent, teamValid))
            result = Result::PlayOnline;
    } else if (mode == Mode::Draft) {
        if (button({W - 176, by, 160, 32}, "LOCK IN >", m, kAccent, teamValid))
            result = Result::Lock;
    }

    if (!statusMsg_.empty())
        DrawText(statusMsg_.c_str(), 392, static_cast<int>(by) + 8, 14, kMuted);

    // Hovered-spell info popup, drawn last so it floats above every widget.
    if (hoveredDef) drawSpellPopup(*hoveredDef, hoveredRect, screenW, screenH);

    return result;
}

} // namespace tb::render
