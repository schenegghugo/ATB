#!/usr/bin/env python3
"""
gen_pack.py — generate a presentation pack (atlas.png + pack.json) for Tactical
Battler, with ZERO third-party dependencies (pure-Python PNG encoder).

It draws a small, clean, flat sprite set into one RGBA atlas and emits a manifest
whose rects are derived from where each sprite was actually placed, so the two
can never drift. Output: packs/default/{atlas.png,pack.json}.

    python3 tools/gen_pack.py            # writes packs/default/
    python3 tools/gen_pack.py mypack     # writes packs/mypack/

Keys match what the renderer looks up (tiles.*, units.player/enemy, spells.<key>).
See packs/README.md for the format.
"""
import json
import math
import os
import struct
import sys
import zlib

CELL = 48   # authoring size per sprite (scaled down in-game: tiles 36, icons ~28)
COLS = 6    # atlas columns

# ---- pure-Python RGBA canvas + PNG encoder ---------------------------------
class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h * 4)  # transparent

    def set(self, x, y, c):
        x, y = int(x), int(y)
        if not (0 <= x < self.w and 0 <= y < self.h):
            return
        i = (y * self.w + x) * 4
        a = c[3]
        if a >= 255:
            self.px[i:i + 4] = bytes((c[0], c[1], c[2], 255))
        elif a > 0:
            f = a / 255.0
            for k in range(3):
                self.px[i + k] = int(c[k] * f + self.px[i + k] * (1 - f))
            self.px[i + 3] = max(self.px[i + 3], a)

    def png_bytes(self):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)  # filter: none
            raw += self.px[y * self.w * 4:(y + 1) * self.w * 4]
        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))
        ihdr = struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


class Pen:
    """Draws in cell-local coords (0..CELL) into `cv` at offset (ox, oy)."""
    def __init__(self, cv, ox, oy):
        self.cv, self.ox, self.oy = cv, ox, oy

    def p(self, x, y, c): self.cv.set(self.ox + x, self.oy + y, c)

    def rect(self, x0, y0, x1, y1, c):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.p(x, y, c)

    def disc(self, cx, cy, r, c):
        for y in range(int(cy - r), int(cy + r) + 1):
            for x in range(int(cx - r), int(cx + r) + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.p(x, y, c)

    def ring(self, cx, cy, r, t, c):
        for y in range(int(cy - r), int(cy + r) + 1):
            for x in range(int(cx - r), int(cx + r) + 1):
                d = (x - cx) ** 2 + (y - cy) ** 2
                if (r - t) ** 2 <= d <= r * r:
                    self.p(x, y, c)

    def line(self, x0, y0, x1, y1, c, w=2):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / n
            self.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w / 2.0, c)

    def poly(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            for i in range(len(pts)):
                ax, ay = pts[i]
                bx, by = pts[(i + 1) % len(pts)]
                if (ay <= y < by) or (by <= y < ay):
                    xs.append(ax + (y - ay) / (by - ay) * (bx - ax))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.ceil(xs[i])), int(math.floor(xs[i + 1])) + 1):
                    self.p(x, y, c)


# ---- palette ---------------------------------------------------------------
WHITE = (238, 240, 245, 255)
DARK = (20, 22, 30, 255)
OUT = (14, 16, 22, 255)  # sprite outline

def token(pen, col, outline=OUT):  # a board-game-piece unit
    pen.disc(24, 26, 15, outline)
    pen.disc(24, 26, 13, col)
    pen.disc(19, 21, 4, (255, 255, 255, 60))  # highlight

def badge(pen, col):  # spell-icon chip
    pen.disc(24, 24, 20, OUT)
    pen.disc(24, 24, 18, col)

# ---- sprite drawers (each fills one CELL) ----------------------------------
def t_floor(p):
    p.rect(0, 0, CELL, CELL, (40, 44, 58, 255)); p.rect(2, 2, CELL - 2, CELL - 2, (46, 51, 66, 255))

def t_wall(p):
    p.rect(0, 0, CELL, CELL, (58, 64, 82, 255)); p.rect(0, 0, CELL, 6, (78, 86, 104, 255))
    p.rect(0, CELL - 6, CELL, CELL, (40, 44, 58, 255))

def t_obstacle(p):
    t_floor(p); p.poly([(10, 34), (18, 14), (30, 14), (38, 34)], (122, 92, 58, 255))
    p.poly([(18, 14), (30, 14), (34, 22), (14, 22)], (150, 116, 74, 255))

def u_player(p): token(p, (58, 168, 108, 255))
def u_enemy(p): token(p, (200, 82, 82, 255))

def s_attack(p, col):
    badge(p, col); p.line(15, 33, 33, 15, WHITE, 4); p.line(28, 12, 34, 18, WHITE, 3)
def s_fireball(p, col):
    badge(p, col); p.disc(24, 26, 8, WHITE); p.poly([(24, 8), (30, 20), (18, 20)], WHITE)
def s_poison(p, col):
    badge(p, col); p.disc(24, 28, 7, WHITE); p.poly([(24, 12), (31, 28), (17, 28)], WHITE)
def s_knockback(p, col):
    badge(p, col)
    for dx in (-6, 2):
        p.line(16 + dx, 24, 26 + dx, 24, WHITE, 3); p.poly(
            [(26 + dx, 18), (34 + dx, 24), (26 + dx, 30)], WHITE)
def s_harpoon(p, col):
    badge(p, col); p.line(14, 24, 32, 24, WHITE, 3); p.poly([(30, 18), (36, 24), (30, 30)], WHITE)
    p.line(30, 24, 26, 20, WHITE, 2); p.line(30, 24, 26, 28, WHITE, 2)
def s_bulwark(p, col):
    badge(p, col); p.poly([(24, 12), (35, 17), (35, 27), (24, 36), (13, 27), (13, 17)], WHITE)
    p.poly([(24, 16), (31, 19), (31, 26), (24, 32), (17, 26), (17, 19)], col)
def s_mend(p, col):
    badge(p, col); p.rect(21, 13, 27, 35, WHITE); p.rect(13, 21, 35, 27, WHITE)
def s_shelter(p, col):
    badge(p, col); p.rect(13, 14, 19, 34, WHITE); p.rect(29, 14, 35, 34, WHITE); p.rect(13, 14, 35, 19, WHITE)
def s_invisible(p, col):
    badge(p, col); p.ring(24, 24, 11, 3, WHITE); p.disc(24, 24, 4, WHITE); p.line(14, 34, 34, 14, DARK, 3)
def s_portal(p, col):
    badge(p, col); p.ring(24, 24, 12, 3, WHITE); p.ring(24, 24, 6, 3, WHITE)
def s_glyph(p, col):
    badge(p, col)
    for a in range(0, 360, 60):
        r = math.radians(a); p.line(24, 24, 24 + 13 * math.cos(r), 24 + 13 * math.sin(r), WHITE, 2)
def s_rewind(p, col):
    badge(p, col); p.ring(24, 24, 11, 3, WHITE); p.rect(24, 6, 34, 22, col)
    p.poly([(24, 8), (24, 20), (16, 14)], WHITE)
def s_bomb(p, col):
    badge(p, col); p.disc(23, 28, 9, WHITE); p.line(27, 20, 32, 12, (60, 60, 60, 255), 2)
    p.disc(32, 11, 2, (240, 180, 60, 255))
def s_blocker(p, col):
    badge(p, col); p.rect(14, 14, 34, 20, WHITE); p.rect(14, 28, 34, 34, WHITE); p.rect(21, 14, 27, 34, WHITE)
def s_healer(p, col):
    badge(p, col); p.ring(24, 24, 12, 3, WHITE); p.rect(22, 16, 26, 32, WHITE); p.rect(16, 22, 32, 26, WHITE)
def s_brute(p, col):
    badge(p, col); p.line(16, 32, 30, 18, WHITE, 4); p.disc(31, 15, 6, WHITE)

# key -> (drawer, optional category colour for spell badges)
DMG = (196, 86, 46, 255); SUP = (58, 132, 176, 255); SUM = (128, 96, 190, 255)
SPRITES = [
    ("tiles.floor", t_floor, None), ("tiles.wall", t_wall, None), ("tiles.obstacle", t_obstacle, None),
    ("units.player", u_player, None), ("units.enemy", u_enemy, None),
    ("spells.attack", s_attack, DMG), ("spells.fireball", s_fireball, DMG),
    ("spells.poison", s_poison, (106, 160, 58, 255)), ("spells.knockback", s_knockback, DMG),
    ("spells.harpoon", s_harpoon, DMG), ("spells.bulwark", s_bulwark, SUP),
    ("spells.mend", s_mend, (58, 160, 106, 255)), ("spells.shelter", s_shelter, SUP),
    ("spells.invisible", s_invisible, SUM), ("spells.portal", s_portal, (64, 176, 192, 255)),
    ("spells.glyph", s_glyph, SUM), ("spells.rewind", s_rewind, SUP),
    ("spells.bomb", s_bomb, DMG), ("spells.blocker", s_blocker, SUM),
    ("spells.healer", s_healer, (58, 160, 122, 255)), ("spells.brute", s_brute, (160, 80, 48, 255)),
]

def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "default"
    out = os.path.join(os.path.dirname(__file__), "..", "packs", name)
    os.makedirs(out, exist_ok=True)

    rows = (len(SPRITES) + COLS - 1) // COLS
    atlas = Canvas(COLS * CELL, rows * CELL)
    sprites = {}
    for i, (key, fn, col) in enumerate(SPRITES):
        ox, oy = (i % COLS) * CELL, (i // COLS) * CELL
        pen = Pen(atlas, ox, oy)
        fn(pen, col) if col is not None else fn(pen)
        sprites[key] = {"atlas": "main", "rect": [ox, oy, CELL, CELL]}

    with open(os.path.join(out, "atlas.png"), "wb") as f:
        f.write(atlas.png_bytes())

    manifest = {
        "schema": 1,
        "name": f"Default ({name})",
        "version": "1.0.0",
        "tileSize": CELL,
        "atlases": {"main": "atlas.png"},
        "palette": {"floor": "#2c303e", "wall": "#3a4052", "obstacle": "#5a4a34"},
        "sprites": sprites,
    }
    with open(os.path.join(out, "pack.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"wrote {out}/atlas.png ({atlas.w}x{atlas.h}) + pack.json "
          f"({len(sprites)} sprites)")

if __name__ == "__main__":
    main()
