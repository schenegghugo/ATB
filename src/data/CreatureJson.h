#pragma once
//
// CreatureJson.h — Load / save the bestiary (spawnable creature prototypes) as
// JSON. A creature is an Entity template keyed by `name` (kind, stats, innate
// spells, statuses, fuse, onDeath). Built on the same Json + SpellJson + enum
// machinery as the spell catalog. core/ is untouched.
//
#include "../core/Battle.h"

#include <string>
#include <vector>

namespace tb {

inline constexpr int kCreatureSchemaVersion = 1;

struct CreatureLoad {
    bool ok = false;
    std::vector<Entity> creatures;   // ready for Battle::setCreatures (valid when ok)
    std::string version;
    std::string sha256;              // digest of the source bytes
    std::vector<std::string> errors; // all problems, each with context
};

[[nodiscard]] CreatureLoad loadCreaturesFromString(const std::string& json);
[[nodiscard]] CreatureLoad loadCreaturesFromFile(const std::string& path);
[[nodiscard]] std::string serializeCreatures(const std::vector<Entity>& creatures,
                                             const std::string& version);

} // namespace tb
