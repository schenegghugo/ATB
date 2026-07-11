#include "ContentPaths.h"

#include "raylib.h"

#include <cstdlib>
#include <vector>

namespace tb::render {

std::string contentDir() {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("ATB_DATA_DIR"); env && *env)
        candidates.emplace_back(env);

    // GetApplicationDirectory() returns the executable's directory with a
    // trailing separator (works before InitWindow — it's a filesystem helper).
    const std::string app = GetApplicationDirectory();
    candidates.push_back(app + "data");
    candidates.push_back(app + "../data");
    candidates.push_back(app + "../../data");
    candidates.emplace_back("data"); // CWD fallback (running from the repo root)

    for (const std::string& dir : candidates)
        if (DirectoryExists(dir.c_str())) return dir;
    return {};
}

std::optional<std::string> findContent(const std::string& name) {
    const std::string dir = contentDir();
    if (dir.empty()) return std::nullopt;
    std::string path = dir + "/" + name;
    if (FileExists(path.c_str())) return path;
    return std::nullopt;
}

std::string siblingDir(const std::string& name) {
    const std::string app = GetApplicationDirectory();
    const std::string candidates[] = {app + name, app + "../" + name, app + "../../" + name, name};
    for (const std::string& dir : candidates)
        if (DirectoryExists(dir.c_str())) return dir;
    return {};
}

} // namespace tb::render
