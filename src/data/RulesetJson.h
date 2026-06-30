#pragma once
//
// RulesetJson.h — Load / save the match ruleset as JSON.
//
// data/rules.json is the source of truth (hand-editable), loaded by the same
// Json + JsonRead + validation machinery as the catalog / creatures. The third
// pinned, hashable artifact (ARCHITECTURE §5/§7). core/ is untouched.
//
#include "../core/Ruleset.h"

#include <string>
#include <vector>

namespace tb {

inline constexpr int kRulesetSchemaVersion = 1;

struct RulesetLoad {
    bool ok = false;
    Ruleset ruleset;                 // valid when ok (defaults for omitted fields)
    std::string version;
    std::string sha256;              // digest of the source bytes (PvP anchor)
    std::vector<std::string> errors; // all problems, each with context
};

[[nodiscard]] RulesetLoad loadRulesetFromString(const std::string& json);
[[nodiscard]] RulesetLoad loadRulesetFromFile(const std::string& path);
[[nodiscard]] std::string serializeRuleset(const Ruleset& ruleset, const std::string& version);

} // namespace tb
