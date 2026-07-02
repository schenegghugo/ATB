#pragma once
//
// SpritePack.h — the raylib half of a presentation pack (§6). Owns the atlas
// textures and resolves a semantic key to either a blitted sprite or a palette
// colour, with a miss simply meaning "the caller draws its primitive." Loading a
// pack needs a GL context, so this is built only in the GUI target; the manifest
// (PackManifest) is parsed separately and headlessly.
//
#include "PackManifest.h"

#include "raylib.h"

#include <string>
#include <unordered_map>

namespace tb::render {

class SpritePack {
public:
    // Loads <dir>/pack.json and its atlas PNGs (relative to <dir>). Returns false
    // and fills `errors` on a bad/absent manifest; a manifest whose atlas images
    // fail to load still "loads" — those sprites just miss and fall back.
    bool load(const std::string& dir, std::vector<std::string>& errors);
    void unload(); // must be called while the GL context is still alive

    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::string& name() const { return manifest_.name; }

    // Blit the sprite for `key` into `dest` (source scaled to fill it), returning
    // true if it drew. False = no such sprite, or its atlas image didn't load →
    // the caller should paint its primitive instead. Always the static frame.
    bool drawSprite(const std::string& key, Rectangle dest, Color tint = WHITE) const;

    // Frame-aware draw (§2.4). Picks the source sub-rect by clip state:
    //   - if `castElapsed >= 0` and the sprite has a `cast` clip still running,
    //     play that one-shot at that offset;
    //   - else if the sprite has an ambient `anim` clip, loop it at time `now`;
    //   - else the static rect.
    // Same true/false contract as drawSprite (false → caller paints its primitive).
    bool drawSprite(const std::string& key, Rectangle dest, double now, double castElapsed,
                    Color tint = WHITE) const;

    // Palette colour for `key`, or `fallback` if the pack doesn't override it.
    [[nodiscard]] Color paletteOr(const std::string& key, Color fallback) const;

private:
    bool ready_ = false;
    PackManifest manifest_;
    std::unordered_map<std::string, Texture2D> atlases_; // only successfully-loaded pages
};

} // namespace tb::render
