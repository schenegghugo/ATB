#include "BuildRepository.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tb {

// ---------------------------------------------------------------------------
// In-memory
// ---------------------------------------------------------------------------
std::optional<CharacterBuild> InMemoryBuildRepository::load(const std::string& name) const {
    for (const CharacterBuild& b : builds_)
        if (b.name == name) return b;
    return std::nullopt;
}

void InMemoryBuildRepository::save(const CharacterBuild& build) {
    for (CharacterBuild& b : builds_) {
        if (b.name == build.name) { b = build; return; } // upsert
    }
    builds_.push_back(build);
}

std::vector<std::string> InMemoryBuildRepository::list() const {
    std::vector<std::string> names;
    names.reserve(builds_.size());
    for (const CharacterBuild& b : builds_) names.push_back(b.name);
    return names;
}

void InMemoryBuildRepository::remove(const std::string& name) {
    builds_.erase(std::remove_if(builds_.begin(), builds_.end(),
                                 [&](const CharacterBuild& b) { return b.name == name; }),
                  builds_.end());
}

// ---------------------------------------------------------------------------
// Flat-file
// ---------------------------------------------------------------------------
namespace fs = std::filesystem;

FileBuildRepository::FileBuildRepository(std::string directory) : dir_(std::move(directory)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

std::string FileBuildRepository::pathFor(const std::string& name) const {
    return (fs::path(dir_) / (name + ".build")).string();
}

std::optional<CharacterBuild> FileBuildRepository::load(const std::string& name) const {
    std::ifstream in(pathFor(name));
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return deserializeBuild(ss.str());
}

void FileBuildRepository::save(const CharacterBuild& build) {
    std::ofstream out(pathFor(build.name), std::ios::trunc);
    out << serializeBuild(build);
}

std::vector<std::string> FileBuildRepository::list() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() == ".build")
            names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void FileBuildRepository::remove(const std::string& name) {
    std::error_code ec;
    fs::remove(pathFor(name), ec);
}

} // namespace tb
