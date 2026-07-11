# Theming (ricing the UI)

Every colour the game draws with — menu chrome, build editor, battle board and
HUD — is a semantic key you can override from a small JSON file. Pick a theme
in **Settings**; it applies live and is remembered in `settings.json`.

## Quick start

1. Copy a shipped theme and edit it:

   ```sh
   cp themes/default.json themes/mine.json
   $EDITOR themes/mine.json
   ```

2. In-game: **Settings → UI THEME → mine**. Edit the file some more, then hit
   **Reload theme file** to see changes without restarting.

Themes are validated strictly (`tb_theme_demo` gates the shipped ones in CI):
a typo'd key or a bad hex string is an error with context, never a silent
fallback.

## Format

```json
{
  "schema": 1,
  "name": "My Theme",
  "version": "1.0.0",
  "colors": {
    "accent": "#fe8019",
    "bg": "#282828",
    "reach": "#4585885a"
  }
}
```

- Colours are `"#RRGGBB"` or `"#RRGGBBAA"` (lower- or upper-case hex).
- **Every key is optional** — override three colours or all thirty-four; the
  rest keep the built-in defaults. `themes/default.json` lists every key with
  its default value, so it doubles as the reference.

## Keys

Chrome (menus, screens, shared widgets): `bg`, `panel`, `panelHot`, `text`,
`muted`, `accent`, `good`, `bad`, `line`, `picked`, `pickedHot`.

Battle board + HUD: `gridLine`, `floor`, `wall`, `obstacle`, `reach`, `hover`,
`zoneOk`, `zoneBad`, `statusDot`, `groundWall`, `glyphZone`, `portal`, `storm`,
`player`, `enemy`, `los`, `textDim`, `btnReady`, `btnCooldown`, `btnPoor`,
`btnSelected`, `btnCdText`.

Tile colours (`floor`, `wall`, `obstacle`) are the *primitive* fallbacks — a
sprite pack's art or palette still wins where it provides one (see
[sprite-packs.md](sprite-packs.md)). A theme restyles the interface; a pack
restyles the art. They compose.

## Preferences

`settings.json` (created beside the app the first time you pick something in
Settings) stores the picks and is itself hand-editable:

```json
{
  "schema": 1,
  "theme": "gruvbox",
  "pack": "default"
}
```

`ATB_PACK=<dir>` still works as a dev override for the pack and beats the
saved pick at boot.
