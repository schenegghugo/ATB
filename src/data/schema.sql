-- schema.sql — Database design for the skill dictionary + character builds.
--
-- The core/ code is DB-agnostic; this is the target a SqliteBuildRepository (or
-- a server-side Postgres) hydrates from. `makeDefaultCatalog()` in Spells.cpp is
-- the seed data for the `spells` / `spell_effects` tables.
--
-- Mapping to code:
--   spells          <-> SpellDef (id, key, build_cost) + Spell stats
--   spell_effects   <-> Effect (a spell has many)
--   builds          <-> CharacterBuild (name + StatAllocation)
--   build_spells    <-> CharacterBuild::spellIds (many-to-many)

-- The dictionary of skills players choose from. -----------------------------
CREATE TABLE spells (
    id          INTEGER PRIMARY KEY,
    key         TEXT    NOT NULL UNIQUE,   -- stable slug, e.g. 'fireball'
    name        TEXT    NOT NULL,
    build_cost  INTEGER NOT NULL,          -- point-buy cost
    ap_cost     INTEGER NOT NULL,
    min_range   INTEGER NOT NULL,
    max_range   INTEGER NOT NULL,
    needs_los   INTEGER NOT NULL,          -- 0/1
    shape       TEXT    NOT NULL,          -- 'single' | 'line' | 'cross' | 'circle'
    radius      INTEGER NOT NULL DEFAULT 0
);

-- A spell applies one or more effects, in order. ----------------------------
CREATE TABLE spell_effects (
    id               INTEGER PRIMARY KEY,
    spell_id         INTEGER NOT NULL REFERENCES spells(id) ON DELETE CASCADE,
    ordinal          INTEGER NOT NULL,          -- application order
    type             TEXT    NOT NULL,          -- 'damage'|'heal'|'push'|'pull'|'apply_status'
    amount           INTEGER NOT NULL DEFAULT 0,
    status_kind      TEXT,                       -- when type='apply_status'
    status_magnitude INTEGER,
    status_turns     INTEGER
);

-- A player's classless character definition. --------------------------------
CREATE TABLE builds (
    id         INTEGER PRIMARY KEY,
    name       TEXT    NOT NULL UNIQUE,
    bonus_hp   INTEGER NOT NULL DEFAULT 0,   -- StatAllocation.hpPurchases
    bonus_ap   INTEGER NOT NULL DEFAULT 0,
    bonus_mp   INTEGER NOT NULL DEFAULT 0
);

-- Which skills a build has picked (point budget enforced in validateBuild). --
CREATE TABLE build_spells (
    build_id INTEGER NOT NULL REFERENCES builds(id) ON DELETE CASCADE,
    spell_id INTEGER NOT NULL REFERENCES spells(id),
    PRIMARY KEY (build_id, spell_id)
);

CREATE INDEX idx_spell_effects_spell ON spell_effects(spell_id);
CREATE INDEX idx_build_spells_build  ON build_spells(build_id);
