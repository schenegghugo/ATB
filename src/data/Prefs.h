#pragma once
//
// Prefs.h — Player preferences (settings.json) + theme/pack discovery.
//
// The persisted half of the Settings screen: which UI theme and sprite pack
// the player picked. Stored beside the app as hand-editable JSON, absent file
// = defaults (a fresh install has no settings.json). Raylib-free, so it
// round-trips headlessly like every other data/ module.
//
#include <string>
#include <vector>

namespace tb {

inline constexpr int kPrefsSchemaVersion = 1;

struct Prefs {
    std::string theme;    // themes/<theme>.json ("" = built-in defaults)
    std::string pack;     // packs/<pack>/pack.json ("" = primitives, no pack)

    // Battle-screen layout the player drags into shape (grips in-match) and that
    // persists here. uiScale sizes the board (drag its corner); clockHeight and
    // chatFraction split the right-hand column (drag the dividers).
    float uiScale = 1.0f;      // board tile-size multiplier
    int clockHeight = 68;      // clock strip height, px
    float chatFraction = 0.5f; // chat's share of the column below the clock (rest = log)
};

struct PrefsLoad {
    bool ok = false;
    Prefs prefs;                     // valid when ok (defaults for omitted fields)
    bool absent = false;             // file didn't exist — ok, defaults apply
    std::vector<std::string> errors; // all problems, each with context
};

[[nodiscard]] PrefsLoad loadPrefsFromString(const std::string& json);
[[nodiscard]] PrefsLoad loadPrefsFromFile(const std::string& path);
[[nodiscard]] std::string serializePrefs(const Prefs& prefs);
// Serialize + write. Returns false if the file can't be written.
bool savePrefsToFile(const Prefs& prefs, const std::string& path);

// Discovery for the Settings pickers. Names, sorted, not paths:
//   listThemes("themes") → the *.json basenames;
//   listPacks("packs")   → subdirectories containing a pack.json.
// A missing directory is just an empty list.
[[nodiscard]] std::vector<std::string> listThemes(const std::string& dir);
[[nodiscard]] std::vector<std::string> listPacks(const std::string& dir);

} // namespace tb
