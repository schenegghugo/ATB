#pragma once
//
// ContentPaths.h — Locate runtime content (catalog.json, packs/…) on disk.
//
// Frontend-only (uses Raylib's GetApplicationDirectory), so it lives in render/,
// not in the graphics-free data/ tier. Resolution order, first hit wins:
//   1. $ATB_DATA_DIR                  (explicit override)
//   2. <exe dir>/data                 (installed layout: data next to the binary)
//   3. <exe dir>/../data              (dev layout: build/ under the repo root)
//   4. <exe dir>/../../data
//   5. ./data                         (current working directory)
//
// Shared by the catalog loader and (Phase 2) the sprite-pack loader — solve the
// "where does content live" question once.
//
#include <optional>
#include <string>

namespace tb::render {

// The resolved runtime content directory, or "" if none of the candidates exist.
[[nodiscard]] std::string contentDir();

// Absolute path to a named content file (e.g. "catalog.json"), or nullopt if it
// isn't present in the resolved content directory.
[[nodiscard]] std::optional<std::string> findContent(const std::string& name);

// A content directory that lives BESIDE data/ (same ladder, minus the env
// override): "themes", "packs". "" if none of the candidates exist.
[[nodiscard]] std::string siblingDir(const std::string& name);

} // namespace tb::render
