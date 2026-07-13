//
// Net.cpp — Intent / Snapshot (de)serialization + apply, over the public engine
// API. See Net.h. Strict, all-errors-with-context parsing like the other loaders;
// deterministic compact output (insertion-ordered json::dump) so a round-trip is
// byte-identical.
//
#include "Net.h"

#include "Json.h"
#include "JsonRead.h"
#include "SpellEnums.h" // enums::toString / fromString (EntityKind, GroundKind)
#include "SpellJson.h"  // statusToJson / parseStatus

#include <string>

namespace tb::net {
namespace {

using jsonread::Errors;

// --- tiny enum <-> string helpers (Faction/Phase have no SpellEnums table) ---
const char* factionStr(Faction f) { return f == Faction::Player ? "player" : "enemy"; }
bool factionFrom(const std::string& s, Faction& out) {
    if (s == "player") { out = Faction::Player; return true; }
    if (s == "enemy") { out = Faction::Enemy; return true; }
    return false;
}
const char* phaseStr(Phase p) {
    switch (p) {
        case Phase::PlayerTurn: return "playerTurn";
        case Phase::EnemyTurn: return "enemyTurn";
        case Phase::Finished: return "finished";
    }
    return "finished";
}
bool phaseFrom(const std::string& s, Phase& out) {
    if (s == "playerTurn") { out = Phase::PlayerTurn; return true; }
    if (s == "enemyTurn") { out = Phase::EnemyTurn; return true; }
    if (s == "finished") { out = Phase::Finished; return true; }
    return false;
}

// --- Vec2i <-> [x, y] --------------------------------------------------------
json::Value vec2Json(Vec2i p) {
    json::Value a = json::Value::makeArray();
    a.push_back(p.x);
    a.push_back(p.y);
    return a;
}
bool readVec2(const json::Value& v, const std::string& ctx, Vec2i& out, Errors& e) {
    if (!v.isArray() || v.asArray().size() != 2) {
        e.push_back(ctx + ": expected [x, y]");
        return false;
    }
    int x = 0, y = 0;
    if (!jsonread::toInt(v.asArray()[0], x) || !jsonread::toInt(v.asArray()[1], y)) {
        e.push_back(ctx + ": [x, y] must be integers");
        return false;
    }
    out = {x, y};
    return true;
}
void readVec2Field(const json::Value& obj, const char* key, const std::string& ctx, Vec2i& out,
                   Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) { e.push_back(ctx + ": missing \"" + std::string(key) + "\""); return; }
    readVec2(*v, ctx + "." + key, out, e);
}

} // namespace

// --- Intent -----------------------------------------------------------------
bool applyIntent(Battle& b, EntityId actor, const Intent& in) {
    switch (in.kind) {
        case Intent::Kind::Move: return b.moveToward(actor, in.target) > 0;
        case Intent::Kind::Cast:
            return b.cast(actor, in.spellIdx, in.target,
                          in.hasTarget2 ? std::optional<Vec2i>(in.target2) : std::nullopt,
                          in.rotation);
        case Intent::Kind::EndTurn: b.endTurn(); return true;
    }
    return false;
}

std::string serializeIntent(const Intent& in) {
    json::Value o = json::Value::makeObject();
    switch (in.kind) {
        case Intent::Kind::Move:
            o.set("kind", "move");
            o.set("target", vec2Json(in.target));
            break;
        case Intent::Kind::Cast:
            o.set("kind", "cast");
            o.set("spellIdx", in.spellIdx);
            o.set("target", vec2Json(in.target));
            if (in.hasTarget2) o.set("target2", vec2Json(in.target2));
            if (in.rotation != 0) o.set("rotation", in.rotation);
            break;
        case Intent::Kind::EndTurn:
            o.set("kind", "endTurn");
            break;
    }
    return json::dump(o, /*pretty=*/false);
}

Parse<Intent> parseIntent(const std::string& text) {
    Parse<Intent> r;
    json::ParseResult pr = json::parse(text);
    if (!pr.ok) { r.errors.push_back(pr.error); return r; }
    if (!pr.value.isObject()) { r.errors.push_back("intent: expected an object"); return r; }
    const json::Value& o = pr.value;

    const json::Value* k = o.find("kind");
    if (!k || !k->isString()) { r.errors.push_back("intent: missing string \"kind\""); return r; }
    const std::string& kind = k->asString();

    Errors e;
    Intent in;
    if (kind == "move") {
        in.kind = Intent::Kind::Move;
        jsonread::checkAllowed(o, {"kind", "target"}, "intent", e);
        if (const json::Value* t = o.find("target")) readVec2(*t, "intent.target", in.target, e);
        else e.push_back("intent: \"move\" requires \"target\"");
    } else if (kind == "cast") {
        in.kind = Intent::Kind::Cast;
        jsonread::checkAllowed(o, {"kind", "spellIdx", "target", "target2", "rotation"}, "intent", e);
        if (jsonread::wantInt(o, "spellIdx", "intent", in.spellIdx, e) && in.spellIdx < 0)
            e.push_back("intent: \"spellIdx\" must be >= 0");
        if (const json::Value* t = o.find("target")) readVec2(*t, "intent.target", in.target, e);
        else e.push_back("intent: \"cast\" requires \"target\"");
        if (const json::Value* t2 = o.find("target2")) {
            readVec2(*t2, "intent.target2", in.target2, e);
            in.hasTarget2 = true;
        }
        if (o.find("rotation")) jsonread::wantInt(o, "rotation", "intent", in.rotation, e);
    } else if (kind == "endTurn") {
        in.kind = Intent::Kind::EndTurn;
        jsonread::checkAllowed(o, {"kind"}, "intent", e);
    } else {
        e.push_back("intent: unknown kind \"" + kind + "\"");
    }

    if (!e.empty()) { r.errors = std::move(e); return r; }
    r.ok = true;
    r.value = in;
    return r;
}

// --- Snapshot ---------------------------------------------------------------
Snapshot snapshotOf(const Battle& b) {
    Snapshot s;
    s.phase = b.phase();
    s.finished = b.phase() == Phase::Finished;
    s.winner = b.winner();
    s.active = b.activeUnit();
    s.round = b.round();
    s.stormCenter = b.stormCenter();
    s.safeRadius = b.safeRadius();
    s.stormDamage = b.stormDamage();

    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& e = b.unit(i);
        Snapshot::Unit u;
        u.id = i;
        u.name = e.name;
        u.team = e.team;
        u.kind = e.kind;
        u.pos = e.pos;
        u.hp = e.hp;
        u.maxHp = e.maxHp;
        u.ap = e.ap;
        u.maxAp = e.maxAp;
        u.mp = e.mp;
        u.maxMp = e.maxMp;
        u.initiative = e.initiative;
        u.fuse = e.fuse;
        u.statuses = e.statuses;
        u.spellCooldowns = e.spellCooldowns;
        s.units.push_back(std::move(u));
    }
    for (const GroundEffect& g : b.groundEffects()) {
        Snapshot::Ground gg;
        gg.kind = g.kind;
        gg.owner = g.owner;
        gg.tiles = g.tiles;
        gg.remainingTurns = g.remainingTurns;
        gg.magnitude = g.magnitude;
        gg.center = g.center;
        gg.exit = g.exit;
        gg.element = g.element;
        gg.blocksLos = g.blocksLos;
        s.ground.push_back(std::move(gg));
    }
    return s;
}

std::string serializeSnapshot(const Snapshot& s) {
    json::Value o = json::Value::makeObject();
    o.set("phase", phaseStr(s.phase));
    o.set("finished", s.finished);
    o.set("winner", s.winner ? json::Value(factionStr(*s.winner)) : json::Value(nullptr));
    o.set("active", static_cast<int>(s.active));
    o.set("round", s.round);

    json::Value storm = json::Value::makeObject();
    storm.set("center", vec2Json(s.stormCenter));
    storm.set("safeRadius", s.safeRadius);
    storm.set("damage", s.stormDamage);
    o.set("storm", storm);

    json::Value units = json::Value::makeArray();
    for (const Snapshot::Unit& u : s.units) {
        json::Value uo = json::Value::makeObject();
        uo.set("id", static_cast<int>(u.id));
        uo.set("name", u.name);
        uo.set("team", factionStr(u.team));
        uo.set("kind", std::string(enums::toString(enums::kEntityKinds, u.kind)));
        uo.set("pos", vec2Json(u.pos));
        uo.set("hp", u.hp);
        uo.set("maxHp", u.maxHp);
        uo.set("ap", u.ap);
        uo.set("maxAp", u.maxAp);
        uo.set("mp", u.mp);
        uo.set("maxMp", u.maxMp);
        uo.set("initiative", u.initiative);
        uo.set("fuse", u.fuse);
        json::Value st = json::Value::makeArray();
        for (const StatusEffect& se : u.statuses) st.push_back(spelljson::statusToJson(se));
        uo.set("statuses", st);
        json::Value cd = json::Value::makeArray();
        for (int c : u.spellCooldowns) cd.push_back(c);
        uo.set("cooldowns", cd);
        units.push_back(uo);
    }
    o.set("units", units);

    json::Value ground = json::Value::makeArray();
    for (const Snapshot::Ground& g : s.ground) {
        json::Value go = json::Value::makeObject();
        go.set("kind", std::string(enums::toString(enums::kGroundKinds, g.kind)));
        go.set("owner", factionStr(g.owner));
        json::Value tiles = json::Value::makeArray();
        for (Vec2i t : g.tiles) tiles.push_back(vec2Json(t));
        go.set("tiles", tiles);
        go.set("remainingTurns", g.remainingTurns);
        go.set("magnitude", g.magnitude);
        go.set("center", vec2Json(g.center));
        go.set("exit", vec2Json(g.exit));
        // Omit neutral/false defaults so pre-elemental snapshots round-trip identically.
        if (g.element != Element::None)
            go.set("element", std::string(enums::toString(enums::kElements, g.element)));
        if (g.blocksLos) go.set("blocksLos", true);
        ground.push_back(go);
    }
    o.set("ground", ground);

    return json::dump(o, /*pretty=*/false);
}

Parse<Snapshot> parseSnapshot(const std::string& text) {
    Parse<Snapshot> r;
    json::ParseResult pr = json::parse(text);
    if (!pr.ok) { r.errors.push_back(pr.error); return r; }
    if (!pr.value.isObject()) { r.errors.push_back("snapshot: expected an object"); return r; }
    const json::Value& o = pr.value;

    Errors e;
    Snapshot s;

    if (const json::Value* v = o.find("phase"); v && v->isString()) {
        if (!phaseFrom(v->asString(), s.phase)) e.push_back("snapshot: unknown phase \"" + v->asString() + "\"");
    } else {
        e.push_back("snapshot: missing string \"phase\"");
    }
    s.finished = jsonread::optBool(o, "finished", false, "snapshot", e);
    if (const json::Value* v = o.find("winner")) {
        if (v->isNull()) s.winner = std::nullopt;
        else if (v->isString()) {
            Faction f{};
            if (factionFrom(v->asString(), f)) s.winner = f;
            else e.push_back("snapshot: unknown winner \"" + v->asString() + "\"");
        } else {
            e.push_back("snapshot: \"winner\" must be a string or null");
        }
    }
    if (int a = 0; jsonread::wantInt(o, "active", "snapshot", a, e)) s.active = static_cast<EntityId>(a);
    s.round = jsonread::optInt(o, "round", 0, "snapshot", e);

    if (const json::Value* st = o.find("storm"); st && st->isObject()) {
        readVec2Field(*st, "center", "snapshot.storm", s.stormCenter, e);
        s.safeRadius = jsonread::optInt(*st, "safeRadius", 0, "snapshot.storm", e);
        s.stormDamage = jsonread::optInt(*st, "damage", 0, "snapshot.storm", e);
    } else {
        e.push_back("snapshot: missing object \"storm\"");
    }

    if (const json::Value* us = o.find("units"); us && us->isArray()) {
        const json::Value::Array& arr = us->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const std::string ctx = "snapshot.units[" + std::to_string(i) + "]";
            if (!arr[i].isObject()) { e.push_back(ctx + ": expected an object"); continue; }
            const json::Value& uv = arr[i];
            Snapshot::Unit u;
            if (int id = 0; jsonread::wantInt(uv, "id", ctx, id, e)) u.id = static_cast<EntityId>(id);
            if (const json::Value* nv = uv.find("name"); nv && nv->isString()) u.name = nv->asString();
            else e.push_back(ctx + ": missing string \"name\"");
            if (const json::Value* tv = uv.find("team"); tv && tv->isString()) {
                if (!factionFrom(tv->asString(), u.team)) e.push_back(ctx + ": unknown team");
            } else e.push_back(ctx + ": missing \"team\"");
            if (const json::Value* kv = uv.find("kind"); kv && kv->isString()) {
                if (auto k = enums::fromString(enums::kEntityKinds, kv->asString())) u.kind = *k;
                else e.push_back(ctx + ": unknown kind \"" + kv->asString() + "\"");
            } else e.push_back(ctx + ": missing \"kind\"");
            readVec2Field(uv, "pos", ctx, u.pos, e);
            u.hp = jsonread::optInt(uv, "hp", 0, ctx, e);
            u.maxHp = jsonread::optInt(uv, "maxHp", 0, ctx, e);
            u.ap = jsonread::optInt(uv, "ap", 0, ctx, e);
            u.maxAp = jsonread::optInt(uv, "maxAp", 0, ctx, e);
            u.mp = jsonread::optInt(uv, "mp", 0, ctx, e);
            u.maxMp = jsonread::optInt(uv, "maxMp", 0, ctx, e);
            u.initiative = jsonread::optInt(uv, "initiative", 0, ctx, e);
            u.fuse = jsonread::optInt(uv, "fuse", 0, ctx, e);
            if (const json::Value* sv = uv.find("statuses"); sv && sv->isArray()) {
                const json::Value::Array& sa = sv->asArray();
                for (std::size_t j = 0; j < sa.size(); ++j) {
                    StatusEffect se;
                    spelljson::parseStatus(sa[j], ctx + ".statuses[" + std::to_string(j) + "]", se, e);
                    u.statuses.push_back(se);
                }
            }
            if (const json::Value* cv = uv.find("cooldowns"); cv && cv->isArray()) {
                for (const json::Value& c : cv->asArray()) {
                    int ci = 0;
                    if (jsonread::toInt(c, ci)) u.spellCooldowns.push_back(ci);
                    else e.push_back(ctx + ": cooldown must be an integer");
                }
            }
            s.units.push_back(std::move(u));
        }
    } else {
        e.push_back("snapshot: missing array \"units\"");
    }

    if (const json::Value* gs = o.find("ground"); gs && gs->isArray()) {
        const json::Value::Array& arr = gs->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const std::string ctx = "snapshot.ground[" + std::to_string(i) + "]";
            if (!arr[i].isObject()) { e.push_back(ctx + ": expected an object"); continue; }
            const json::Value& gv = arr[i];
            Snapshot::Ground g;
            if (const json::Value* kv = gv.find("kind"); kv && kv->isString()) {
                if (auto k = enums::fromString(enums::kGroundKinds, kv->asString())) g.kind = *k;
                else e.push_back(ctx + ": unknown kind \"" + kv->asString() + "\"");
            } else e.push_back(ctx + ": missing \"kind\"");
            if (const json::Value* ov = gv.find("owner"); ov && ov->isString()) {
                if (!factionFrom(ov->asString(), g.owner)) e.push_back(ctx + ": unknown owner");
            } else e.push_back(ctx + ": missing \"owner\"");
            if (const json::Value* tv = gv.find("tiles"); tv && tv->isArray()) {
                const json::Value::Array& ta = tv->asArray();
                for (std::size_t j = 0; j < ta.size(); ++j) {
                    Vec2i t{};
                    if (readVec2(ta[j], ctx + ".tiles[" + std::to_string(j) + "]", t, e))
                        g.tiles.push_back(t);
                }
            } else e.push_back(ctx + ": missing array \"tiles\"");
            g.remainingTurns = jsonread::optInt(gv, "remainingTurns", 0, ctx, e);
            g.magnitude = jsonread::optInt(gv, "magnitude", 0, ctx, e);
            readVec2Field(gv, "center", ctx, g.center, e);
            readVec2Field(gv, "exit", ctx, g.exit, e);
            if (const json::Value* elv = gv.find("element"); elv && elv->isString()) {
                if (auto el = enums::fromString(enums::kElements, elv->asString())) g.element = *el;
                else e.push_back(ctx + ": unknown element \"" + elv->asString() + "\"");
            }
            if (const json::Value* bv = gv.find("blocksLos"); bv && bv->isBool())
                g.blocksLos = bv->asBool();
            s.ground.push_back(std::move(g));
        }
    } else {
        e.push_back("snapshot: missing array \"ground\"");
    }

    if (!e.empty()) { r.errors = std::move(e); return r; }
    r.ok = true;
    r.value = std::move(s);
    return r;
}

} // namespace tb::net
