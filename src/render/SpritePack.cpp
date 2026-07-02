#include "SpritePack.h"

namespace tb::render {

bool SpritePack::load(const std::string& dir, std::vector<std::string>& errors) {
    unload();
    PackManifestLoad load = loadPackManifestFromFile(dir + "/pack.json");
    if (!load.ok) {
        errors = std::move(load.errors);
        return false;
    }
    manifest_ = std::move(load.manifest);

    // Load each atlas page. A page that fails to decode is simply skipped (its
    // sprites will miss and fall back) — a pack is never all-or-nothing.
    for (const auto& [name, file] : manifest_.atlases) {
        Texture2D tex = LoadTexture((dir + "/" + file).c_str());
        if (tex.id == 0) {
            TraceLog(LOG_WARNING, "Pack '%s': atlas '%s' failed to load — its sprites fall back.",
                     manifest_.name.c_str(), file.c_str());
            continue;
        }
        atlases_[name] = tex;
    }
    ready_ = true;
    return true;
}

void SpritePack::unload() {
    for (auto& [name, tex] : atlases_) UnloadTexture(tex);
    atlases_.clear();
    ready_ = false;
    manifest_ = PackManifest{};
}

bool SpritePack::drawSprite(const std::string& key, Rectangle dest, Color tint) const {
    return drawSprite(key, dest, /*now=*/0.0, /*castElapsed=*/-1.0, tint);
}

bool SpritePack::drawSprite(const std::string& key, Rectangle dest, double now, double castElapsed,
                            Color tint) const {
    const SpriteDef* sd = manifest_.findSprite(key);
    if (!sd) return false;
    auto it = atlases_.find(sd->atlas);
    if (it == atlases_.end()) return false; // atlas image didn't load → fall back

    // Pick the source rect: a running one-shot cast clip wins, then ambient, then
    // the static rect. `castElapsed` past the clip's duration means it finished.
    std::array<int, 4> r{sd->x, sd->y, sd->w, sd->h};
    if (castElapsed >= 0.0 && sd->cast && castElapsed < sd->cast->duration())
        r = sd->cast->frameAt(castElapsed);
    else if (sd->anim)
        r = sd->anim->frameAt(now);

    const Rectangle src{static_cast<float>(r[0]), static_cast<float>(r[1]),
                        static_cast<float>(r[2]), static_cast<float>(r[3])};
    DrawTexturePro(it->second, src, dest, Vector2{0, 0}, 0.0f, tint);
    return true;
}

Color SpritePack::paletteOr(const std::string& key, Color fallback) const {
    const RGBA* c = manifest_.findPalette(key);
    return c ? Color{c->r, c->g, c->b, c->a} : fallback;
}

} // namespace tb::render
