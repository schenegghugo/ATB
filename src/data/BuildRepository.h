#pragma once
//
// BuildRepository.h — Persistence seam for character builds.
//
// The gameplay core never sees a database. It depends only on this interface;
// the concrete store (in-memory, flat-file, or SQLite) is injected. Swapping to
// SQLite later means adding one implementation here — no changes to core/.
//
// Schema the SQLite implementation targets: data/schema.sql.
//
#include "../core/Build.h"

#include <optional>
#include <string>
#include <vector>

namespace tb {

class BuildRepository {
public:
    virtual ~BuildRepository() = default;

    [[nodiscard]] virtual std::optional<CharacterBuild> load(const std::string& name) const = 0;
    virtual void save(const CharacterBuild& build) = 0;
    [[nodiscard]] virtual std::vector<std::string> list() const = 0;
    virtual void remove(const std::string& name) = 0;
};

// Volatile store — handy for tests and the default frontend roster.
class InMemoryBuildRepository final : public BuildRepository {
public:
    [[nodiscard]] std::optional<CharacterBuild> load(const std::string& name) const override;
    void save(const CharacterBuild& build) override;
    [[nodiscard]] std::vector<std::string> list() const override;
    void remove(const std::string& name) override;

private:
    std::vector<CharacterBuild> builds_;
};

// One file per build under a directory, using serializeBuild/deserializeBuild.
// The closest thing to a database without a third-party dependency; proves the
// seam round-trips. Replace with SqliteBuildRepository for real persistence.
class FileBuildRepository final : public BuildRepository {
public:
    explicit FileBuildRepository(std::string directory);

    [[nodiscard]] std::optional<CharacterBuild> load(const std::string& name) const override;
    void save(const CharacterBuild& build) override;
    [[nodiscard]] std::vector<std::string> list() const override;
    void remove(const std::string& name) override;

private:
    [[nodiscard]] std::string pathFor(const std::string& name) const;
    std::string dir_;
};

} // namespace tb
