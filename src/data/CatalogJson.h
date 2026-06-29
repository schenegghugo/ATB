#pragma once
//
// CatalogJson.h — Load / save the spell catalog as JSON.
//
// Maps the generic json::Value tree (data/Json) to a core SpellCatalog, using the
// enum tables in SpellEnums.h, with strict validation: every problem is reported
// (not just the first), each with context (which spell, which effect, which
// field). Unknown fields are rejected so typos fail loudly instead of being
// silently dropped.
//
// core/ is untouched: this lives in data/ and only depends on core's public
// SpellCatalog / Spell types.
//
#include "../core/Spells.h"

#include <string>
#include <vector>

namespace tb {

// Structural format version. Bump only on a breaking structural change — adding
// enum string values (new shapes, status kinds, …) is backward-compatible.
inline constexpr int kCatalogSchemaVersion = 1;

struct CatalogLoad {
    bool ok = false;                  // true iff errors is empty
    SpellCatalog catalog;             // valid only when ok
    std::string version;              // author-declared content version
    std::vector<std::string> errors;  // all problems found, each with context
};

// Parse + validate a catalog document. Never throws; collects every error.
[[nodiscard]] CatalogLoad loadCatalogFromString(const std::string& json);

// Serialize a catalog to canonical, deterministic JSON (the generator's output
// and the round-trip target). `version` is written into the document.
[[nodiscard]] std::string serializeCatalog(const SpellCatalog& catalog, const std::string& version);

} // namespace tb
