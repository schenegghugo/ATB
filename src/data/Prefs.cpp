#include "Prefs.h"

#include "Json.h"
#include "JsonRead.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tb {

namespace {
using jsonread::Errors;

void optName(const json::Value& root, const char* key, std::string& out, Errors& e) {
    const json::Value* v = root.find(key);
    if (!v) return;
    if (!v->isString()) {
        e.push_back(std::string("root: \"") + key + "\" must be a string");
        return;
    }
    out = v->asString();
}
} // namespace

PrefsLoad loadPrefsFromString(const std::string& text) {
    PrefsLoad out;
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

    jsonread::checkAllowed(
        root, {"schema", "theme", "pack", "uiScale", "clockHeight", "chatFraction", "onlineChecked"},
        "root", e);
    const int schema = jsonread::optInt(root, "schema", 1, "root", e);
    if (schema != kPrefsSchemaVersion) e.push_back("root: unsupported \"schema\" (expected 1)");
    optName(root, "theme", out.prefs.theme, e);
    optName(root, "pack", out.prefs.pack, e);
    out.prefs.uiScale = static_cast<float>(jsonread::optDouble(root, "uiScale", 1.0, "root", e));
    out.prefs.clockHeight = jsonread::optInt(root, "clockHeight", 68, "root", e);
    out.prefs.chatFraction =
        static_cast<float>(jsonread::optDouble(root, "chatFraction", 0.5, "root", e));
    out.prefs.onlineChecked = jsonread::optBool(root, "onlineChecked", false, "root", e);

    out.ok = e.empty();
    return out;
}

PrefsLoad loadPrefsFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        PrefsLoad out; // a fresh install: no file, defaults apply
        out.ok = true;
        out.absent = true;
        return out;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadPrefsFromString(ss.str());
}

std::string serializePrefs(const Prefs& prefs) {
    json::Value root = json::Value::makeObject();
    root.set("schema", kPrefsSchemaVersion);
    root.set("theme", prefs.theme);
    root.set("pack", prefs.pack);
    root.set("uiScale", static_cast<double>(prefs.uiScale));
    root.set("clockHeight", prefs.clockHeight);
    root.set("chatFraction", static_cast<double>(prefs.chatFraction));
    root.set("onlineChecked", prefs.onlineChecked);
    return json::dump(root) + "\n";
}

bool savePrefsToFile(const Prefs& prefs, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << serializePrefs(prefs);
    return static_cast<bool>(f);
}

std::vector<std::string> listThemes(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::filesystem::path& p = entry.path();
        if (p.extension() == ".json") names.push_back(p.stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> listPacks(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (std::filesystem::exists(entry.path() / "pack.json", ec))
            names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace tb
