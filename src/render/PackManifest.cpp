#include "PackManifest.h"

#include "data/JsonRead.h"

#include <fstream>
#include <sstream>

namespace tb::render {

namespace {

using jsonread::Errors;

// "#RRGGBB" or "#RRGGBBAA" → RGBA. Returns false (with an error) otherwise.
bool parseHexColor(const std::string& s, const std::string& ctx, RGBA& out, Errors& e) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') {
        e.push_back(ctx + ": colour must be \"#RRGGBB\" or \"#RRGGBBAA\"");
        return false;
    }
    auto nib = [&](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    auto byte = [&](size_t i, unsigned char& b) {
        int hi = 0, lo = 0;
        if (!nib(s[i], hi) || !nib(s[i + 1], lo)) return false;
        b = static_cast<unsigned char>(hi * 16 + lo);
        return true;
    };
    RGBA c;
    c.a = 255;
    if (!byte(1, c.r) || !byte(3, c.g) || !byte(5, c.b) || (s.size() == 9 && !byte(7, c.a))) {
        e.push_back(ctx + ": colour has a non-hex digit");
        return false;
    }
    out = c;
    return true;
}

void parseSprite(const json::Value& v, const std::string& key, const PackManifest& m,
                 SpriteDef& out, Errors& e) {
    const std::string ctx = "sprites." + key;
    if (!v.isObject()) { e.push_back(ctx + ": expected an object"); return; }
    jsonread::checkAllowed(v, {"atlas", "rect", "anchor", "anim", "cast"}, ctx, e);

    const json::Value* atlas = v.find("atlas");
    if (!atlas || !atlas->isString()) {
        e.push_back(ctx + ": missing/invalid required string \"atlas\"");
    } else {
        out.atlas = atlas->asString();
        if (!m.findAtlasFile(out.atlas))
            e.push_back(ctx + ": \"atlas\" refers to unknown atlas \"" + out.atlas + "\"");
    }

    const json::Value* rect = v.find("rect");
    if (!rect || !rect->isArray() || rect->asArray().size() != 4) {
        e.push_back(ctx + ": \"rect\" must be [x, y, w, h]");
    } else {
        int r[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i)
            if (!jsonread::toInt(rect->asArray()[static_cast<size_t>(i)], r[i]) || r[i] < 0)
                e.push_back(ctx + ": \"rect\" values must be non-negative integers");
        out.x = r[0]; out.y = r[1]; out.w = r[2]; out.h = r[3];
        if (out.w <= 0 || out.h <= 0) e.push_back(ctx + ": \"rect\" width/height must be > 0");
    }

    if (const json::Value* a = v.find("anchor")) {
        if (!a->isString() || (a->asString() != "center" && a->asString() != "bottom"))
            e.push_back(ctx + ": \"anchor\" must be \"center\" or \"bottom\"");
        else if (a->asString() == "bottom")
            out.anchor = SpriteDef::Anchor::Bottom;
    }
    // anim/cast tolerated but not interpreted in v1.
}

} // namespace

PackManifestLoad loadPackManifestFromString(const std::string& text) {
    PackManifestLoad result;
    Errors& e = result.errors;

    json::ParseResult pr = json::parse(text);
    if (!pr.ok) {
        e.push_back("JSON parse error: " + pr.error);
        return result;
    }
    const json::Value& root = pr.value;
    if (!root.isObject()) {
        e.push_back("root: expected an object");
        return result;
    }
    jsonread::checkAllowed(root, {"schema", "name", "version", "tileSize", "atlases", "palette",
                                  "sprites"},
                           "root", e);

    PackManifest m;
    m.schema = jsonread::optInt(root, "schema", 1, "root", e);
    if (m.schema != 1) e.push_back("root: unsupported \"schema\" (expected 1)");
    if (const json::Value* n = root.find("name"); n && n->isString()) m.name = n->asString();
    if (const json::Value* v = root.find("version"); v && v->isString()) m.version = v->asString();
    m.tileSize = jsonread::optInt(root, "tileSize", 32, "root", e);
    if (m.tileSize <= 0) e.push_back("root: \"tileSize\" must be > 0");

    if (const json::Value* at = root.find("atlases")) {
        if (!at->isObject()) e.push_back("atlases: expected an object");
        else
            for (const auto& [name, file] : at->asObject()) {
                if (!file.isString()) { e.push_back("atlases." + name + ": must be a filename string"); continue; }
                m.atlases.emplace_back(name, file.asString());
            }
    }

    if (const json::Value* pal = root.find("palette")) {
        if (!pal->isObject()) e.push_back("palette: expected an object");
        else
            for (const auto& [key, col] : pal->asObject()) {
                if (!col.isString()) { e.push_back("palette." + key + ": must be a colour string"); continue; }
                RGBA c;
                if (parseHexColor(col.asString(), "palette." + key, c, e)) m.palette[key] = c;
            }
    }

    if (const json::Value* sp = root.find("sprites")) {
        if (!sp->isObject()) e.push_back("sprites: expected an object");
        else
            for (const auto& [key, def] : sp->asObject()) {
                SpriteDef sd;
                parseSprite(def, key, m, sd, e);
                m.sprites[key] = sd;
            }
    }

    if (e.empty()) {
        result.manifest = std::move(m);
        result.ok = true;
    }
    return result;
}

PackManifestLoad loadPackManifestFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        PackManifestLoad r;
        r.errors.push_back("cannot open pack manifest: " + path);
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadPackManifestFromString(ss.str());
}

} // namespace tb::render
