#include "Theme.h"

#include "data/Json.h"
#include "data/JsonRead.h"

#include <fstream>
#include <sstream>

namespace tb::render {

namespace {

using jsonread::Errors;

// The single source of truth for what a theme may override: JSON key → member.
// Table-driven so the loader, the unknown-key check, and the demo agree.
struct KeySlot {
    const char* key;
    RGBA Theme::* slot;
};
constexpr KeySlot kSlots[] = {
    {"bg", &Theme::bg},
    {"panel", &Theme::panel},
    {"panelHot", &Theme::panelHot},
    {"text", &Theme::text},
    {"muted", &Theme::muted},
    {"accent", &Theme::accent},
    {"good", &Theme::good},
    {"bad", &Theme::bad},
    {"line", &Theme::line},
    {"picked", &Theme::picked},
    {"pickedHot", &Theme::pickedHot},
    {"gridLine", &Theme::gridLine},
    {"floor", &Theme::floor},
    {"wall", &Theme::wall},
    {"obstacle", &Theme::obstacle},
    {"reach", &Theme::reach},
    {"castable", &Theme::castable},
    {"hover", &Theme::hover},
    {"zoneOk", &Theme::zoneOk},
    {"zoneBad", &Theme::zoneBad},
    {"statusDot", &Theme::statusDot},
    {"groundWall", &Theme::groundWall},
    {"glyphZone", &Theme::glyphZone},
    {"surfFire", &Theme::surfFire},
    {"surfWater", &Theme::surfWater},
    {"surfIce", &Theme::surfIce},
    {"surfPoison", &Theme::surfPoison},
    {"surfElectric", &Theme::surfElectric},
    {"surfHeal", &Theme::surfHeal},
    {"surfOil", &Theme::surfOil},
    {"surfSteam", &Theme::surfSteam},
    {"portal", &Theme::portal},
    {"storm", &Theme::storm},
    {"player", &Theme::player},
    {"enemy", &Theme::enemy},
    {"los", &Theme::los},
    {"textDim", &Theme::textDim},
    {"btnReady", &Theme::btnReady},
    {"btnCooldown", &Theme::btnCooldown},
    {"btnPoor", &Theme::btnPoor},
    {"btnSelected", &Theme::btnSelected},
    {"btnCdText", &Theme::btnCdText},
};

} // namespace

ThemeLoad loadThemeFromString(const std::string& text) {
    ThemeLoad out;
    Errors& e = out.errors;

    json::ParseResult pr = json::parse(text);
    if (!pr.ok) {
        e.push_back("json: " + pr.error);
        return out;
    }
    const json::Value& root = pr.value;
    if (!root.isObject()) {
        e.push_back("root: expected an object");
        return out;
    }

    jsonread::checkAllowed(root, {"schema", "name", "version", "colors", "metrics"}, "root", e);

    const int schema = jsonread::optInt(root, "schema", 1, "root", e);
    if (schema != kThemeSchemaVersion) e.push_back("root: unsupported \"schema\" (expected 1)");

    // Optional layout metrics (resizable-UI extension). Absent = built-in density.
    if (const json::Value* metrics = root.find("metrics")) {
        if (!metrics->isObject()) {
            e.push_back("metrics: expected an object");
        } else {
            jsonread::checkAllowed(*metrics, {"tileSize", "uiScale"}, "metrics", e);
            out.theme.metrics.tileSize =
                jsonread::optInt(*metrics, "tileSize", out.theme.metrics.tileSize, "metrics", e);
            out.theme.metrics.uiScale =
                jsonread::optDouble(*metrics, "uiScale", out.theme.metrics.uiScale, "metrics", e);
        }
    }
    if (const json::Value* n = root.find("name"); n && n->isString()) out.name = n->asString();
    if (const json::Value* v = root.find("version"); v && v->isString())
        out.version = v->asString();

    if (const json::Value* colors = root.find("colors")) {
        if (!colors->isObject()) {
            e.push_back("colors: expected an object of key -> \"#RRGGBB(AA)\"");
        } else {
            for (const json::Value::Member& m : colors->asObject()) {
                const KeySlot* slot = nullptr;
                for (const KeySlot& s : kSlots)
                    if (m.first == s.key) { slot = &s; break; }
                const std::string ctx = "colors." + m.first;
                if (!slot) {
                    e.push_back(ctx + ": unknown colour key");
                    continue;
                }
                if (!m.second.isString()) {
                    e.push_back(ctx + ": must be a \"#RRGGBB\" or \"#RRGGBBAA\" string");
                    continue;
                }
                parseHexColor(m.second.asString(), ctx, out.theme.*(slot->slot), e);
            }
        }
    }

    out.ok = e.empty();
    return out;
}

ThemeLoad loadThemeFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        ThemeLoad out;
        out.errors.push_back("file: cannot open '" + path + "'");
        return out;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadThemeFromString(ss.str());
}

} // namespace tb::render
