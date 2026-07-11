//
// prefs_demo.cpp — Test for player preferences (data/Prefs): settings.json
// round-trip, the absent-file policy, and theme/pack discovery scans. Headless.
//
#include "data/Prefs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (cond) std::printf("  [PASS] %s\n", msg);                                                \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                      \
    } while (0)

int main() {
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "tb_prefs_demo";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    const std::string file = (tmp / "settings.json").string();

    std::printf("Round-trip: save then load\n");
    {
        Prefs p;
        p.theme = "gruvbox";
        p.pack = "default";
        CHECK(savePrefsToFile(p, file), "saves");
        const PrefsLoad r = loadPrefsFromFile(file);
        CHECK(r.ok && !r.absent, "loads");
        CHECK(r.prefs.theme == "gruvbox" && r.prefs.pack == "default", "fields round-trip");
    }

    std::printf("Absent file = defaults, not an error (fresh install)\n");
    {
        const PrefsLoad r = loadPrefsFromFile((tmp / "nope.json").string());
        CHECK(r.ok && r.absent, "absent file is ok");
        CHECK(r.prefs.theme.empty() && r.prefs.pack.empty(), "defaults are empty picks");
    }

    std::printf("Malformed prefs are rejected with context\n");
    {
        auto rejects = [](const std::string& text, const std::string& needle) {
            const PrefsLoad r = loadPrefsFromString(text);
            if (r.ok) return false;
            for (const std::string& e : r.errors)
                if (e.find(needle) != std::string::npos) return true;
            return false;
        };
        CHECK(rejects("{", "json:"), "parse errors are reported");
        CHECK(rejects(R"({ "schema": 9 })", "unsupported \"schema\""), "wrong schema");
        CHECK(rejects(R"({ "them": "x" })", "unknown field"), "unknown field (typo)");
        CHECK(rejects(R"({ "theme": 3 })", "must be a string"), "non-string pick");
    }

    std::printf("Discovery scans\n");
    {
        std::filesystem::create_directories(tmp / "themes");
        std::ofstream((tmp / "themes/b.json").string()) << "{}";
        std::ofstream((tmp / "themes/a.json").string()) << "{}";
        std::ofstream((tmp / "themes/readme.txt").string()) << "not a theme";
        const auto themes = listThemes((tmp / "themes").string());
        CHECK(themes.size() == 2 && themes[0] == "a" && themes[1] == "b",
              "themes: *.json stems, sorted, non-json ignored");

        std::filesystem::create_directories(tmp / "packs/good");
        std::filesystem::create_directories(tmp / "packs/empty");
        std::ofstream((tmp / "packs/good/pack.json").string()) << "{}";
        const auto packs = listPacks((tmp / "packs").string());
        CHECK(packs.size() == 1 && packs[0] == "good",
              "packs: only directories holding a pack.json");

        CHECK(listThemes((tmp / "missing").string()).empty(), "missing dir = empty list");
    }

    std::filesystem::remove_all(tmp);

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nAll prefs checks passed.\n");
    return 0;
}
