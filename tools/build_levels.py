"""Build Super Flashy Birds levels from plain text maps.

Reads levels/*.txt (natural order: level1, level2, ... level10) and writes
include/super_flashy_levels.h. See levels/README.md for the map format.

Run by hand:   python tools/build_levels.py
PlatformIO runs it automatically before every build.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEVELS_DIR = ROOT / "levels"
OUT = ROOT / "include" / "super_flashy_levels.h"

TILE = 16       # px per map cell (must match TILE in super_flashy_birds.cpp)
MAX_ROWS = 32   # tallest map allowed (the screen scrolls vertically)

SYMBOLS = {
    ".": "air",
    " ": "air",
    "X": "ground (solid, drawn as grass)",
    "H": "dirt / background (not solid)",
    "S": "player start",
    "N": "nest (goal)",
    "G": "gorilla",
    "g": "red gorilla",
    "O": "golden egg (collectible)",
    "R": "direction block, passable moving right",
    "L": "direction block, passable moving left",
    "U": "direction block, passable moving up",
    "D": "direction block, passable moving down",
    "F": "fox",
    "B": "box",
    "M": "mystery box",
}

ARROWS = "RLUD"      # order of the masks in Level::arrows
MAX_PIT = 7          # tiles a running jump clears


class LevelError(Exception):
    pass


def natural_key(path: Path):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", path.stem)]


def parse(path: Path):
    """Return (level dict, warnings list). Raises LevelError on mistakes."""
    raw = path.read_text(encoding="utf-8").splitlines()
    lines = [ln.rstrip() for ln in raw if not ln.lstrip().startswith("#")]
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    if not lines:
        raise LevelError("the map is empty")
    if len(lines) > MAX_ROWS:
        raise LevelError(f"the map is {len(lines)} rows tall; the most the game "
                         f"supports is {MAX_ROWS}")

    rows = len(lines)
    width = max(len(ln) for ln in lines)
    grid = [ln.ljust(width, ".") for ln in lines]  # grid[0] is the top row
    errors = []
    for y, ln in enumerate(grid):
        for x, ch in enumerate(ln):
            if ch not in SYMBOLS:
                errors.append(f"unknown symbol {ch!r} at line {y + 1}, column {x + 1}")
    if errors:
        raise LevelError("; ".join(errors[:5]))

    def cell(col, row):  # row 0 is the bottom row of the map
        return grid[rows - 1 - row][col]

    solid, decor, arrows = [], [], []
    for col in range(width):
        a_masks = [0] * len(ARROWS)
        s_mask = d_mask = 0
        for row in range(rows):
            ch = cell(col, row)
            if ch == "X":
                s_mask |= 1 << row
            elif ch in ARROWS:  # solid, apart from the one way through
                s_mask |= 1 << row
                a_masks[ARROWS.index(ch)] |= 1 << row
            elif ch == "H" or (ch == "O" and 0 < col < width - 1
                               and cell(col - 1, row) == "H"
                               and cell(col + 1, row) == "H"):
                d_mask |= 1 << row  # an egg inside a hill keeps the dirt behind
        solid.append(s_mask)
        decor.append(d_mask)
        arrows.append(tuple(a_masks))

    def is_solid(col, row):
        return row >= 0 and (solid[col] >> row) & 1

    def stand_row(what, col, row):
        """Row of the cell the thing ends up in: it drops onto the first
        ground below (so it can float a little in the map)."""
        while row > 0 and not is_solid(col, row - 1):
            row -= 1
        if row == 0:  # fell all the way: nothing below to stand on
            raise LevelError(f"{what} at column {col + 1} has no ground underneath")
        return row

    starts, nests, gorillas, foxes, boxes, golds = [], [], [], [], [], []
    for col in range(width):
        for row in range(rows):
            ch = cell(col, row)
            if ch == "S":
                starts.append((col, stand_row("S (start)", col, row)))
            elif ch == "N":
                nests.append((col, stand_row("N (nest)", col, row)))
            elif ch in ("G", "g"):
                gorillas.append((col, stand_row("G/g (gorilla)", col, row),
                                 1 if ch == "g" else 0))
            elif ch == "F":
                foxes.append((col, stand_row("F (fox)", col, row)))
            elif ch == "O":
                golds.append((col, row))  # stays where it is drawn
            elif ch in ("B", "M"):
                boxes.append((col * TILE, row * TILE, 1 if ch == "M" else 0))

    if len(starts) != 1:
        raise LevelError(f"needs exactly one S (player start), found {len(starts)}")
    if not nests:
        raise LevelError("needs at least one N (nest)")

    warnings = []
    col = 0
    while col < width:  # gaps wider than a running jump
        if solid[col] == 0:
            end = col
            while end + 1 < width and solid[end + 1] == 0:
                end += 1
            size = end - col + 1
            if size > MAX_PIT and col > 0 and end < width - 1:
                warnings.append(f"gap of {size} tiles at columns {col + 1}-{end + 1} "
                                f"is wider than a running jump ({MAX_PIT})")
            col = end + 1
        else:
            col += 1

    return {
        "width": width, "rows": rows, "solid": solid, "decor": decor,
        "start": starts[0], "nests": nests, "gorillas": gorillas,
        "foxes": foxes, "boxes": boxes, "golds": golds, "arrows": arrows,
    }, warnings


def c_array(values, per_line=8, indent="    ", fmt=str):
    out = []
    for i in range(0, len(values), per_line):
        out.append(indent + ", ".join(fmt(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(out)


def pairs(items):
    return ", ".join("{" + ", ".join(str(v) for v in it) + "}" for it in items)


def emit(levels):
    max_g = max([len(lv["gorillas"]) for _, lv in levels] + [1])
    max_b = max([len(lv["boxes"]) for _, lv in levels] + [1])
    max_f = max([len(lv["foxes"]) for _, lv in levels] + [1])
    max_o = max([len(lv["golds"]) for _, lv in levels] + [1])
    max_n = max(len(lv["nests"]) for _, lv in levels)
    max_rows = max(lv["rows"] for _, lv in levels)
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "// Generated by tools/build_levels.py from levels/*.txt - do not edit by hand.",
        "struct Level {",
        "  int16_t tiles;",
        "  int16_t rows;                     // map height in tiles",
        "  const uint32_t* solid;            // per column: bit r set = solid tile in row r",
        "  const uint32_t* decor;            // per column: bit r set = dirt/background tile",
        "  const int16_t (*gorillas)[3];     // {column, row, red?}",
        "  uint8_t gorillaCount;",
        "  const int16_t (*boxes)[3];        // {x px, bottom px above the map bottom, mystery?}",
        "  uint8_t boxCount;",
        "  const int16_t (*foxes)[2];        // {column, row}",
        "  uint8_t foxCount;",
        "  const int16_t (*nests)[2];        // {column, row}; any one is the goal",
        "  uint8_t nestCount;",
        "  int16_t startCol;",
        "  int16_t startRow;",
        "  const uint32_t (*arrows)[4];      // per column: direction blocks R, L, U, D",
        "  const int16_t (*golds)[2];        // golden eggs {column, row}",
        "  uint8_t goldCount;",
        "};",
        "",
        f"constexpr uint8_t LEVEL_MAX_GORILLAS = {max_g};",
        f"constexpr uint8_t LEVEL_MAX_BOXES = {max_b};",
        f"constexpr uint8_t LEVEL_MAX_FOXES = {max_f};",
        f"constexpr int16_t LEVEL_MAX_ROWS = {max_rows};",
        f"constexpr uint8_t LEVEL_MAX_NESTS = {max_n};",
        f"constexpr uint8_t LEVEL_MAX_GOLDS = {max_o};",
        "",
    ]
    entries = []
    for i, (name, lv) in enumerate(levels, 1):
        p = f"LEVEL{i}"
        hexu = lambda v: f"0x{v:X}u"
        lines.append(f"// {name}")
        lines.append(f"constexpr uint32_t {p}_SOLID[] = {{\n{c_array(lv['solid'], fmt=hexu)}\n}};")
        lines.append(f"constexpr uint32_t {p}_DECOR[] = {{\n{c_array(lv['decor'], fmt=hexu)}\n}};")
        lines.append(f"constexpr int16_t {p}_GORILLAS[][3] = {{{pairs(lv['gorillas'] or [(0, 0, 0)])}}};")
        lines.append(f"constexpr int16_t {p}_BOXES[][3] = {{{pairs(lv['boxes'] or [(0, 0, 0)])}}};")
        lines.append(f"constexpr int16_t {p}_FOXES[][2] = {{{pairs(lv['foxes'] or [(0, 0)])}}};")
        lines.append(f"constexpr int16_t {p}_NESTS[][2] = {{{pairs(lv['nests'])}}};")
        masks = ["{" + ", ".join(hexu(m) for m in a) + "}" for a in lv["arrows"]]
        lines.append(f"constexpr uint32_t {p}_ARROWS[][4] = {{\n{c_array(masks, per_line=4)}\n}};")
        lines.append(f"constexpr int16_t {p}_GOLDS[][2] = {{{pairs(lv['golds'] or [(0, 0)])}}};")
        lines.append("")
        entries.append(
            f"    {{{lv['width']}, {lv['rows']}, {p}_SOLID, {p}_DECOR, {p}_GORILLAS, "
            f"{len(lv['gorillas'])}, {p}_BOXES, {len(lv['boxes'])}, {p}_FOXES, "
            f"{len(lv['foxes'])}, {p}_NESTS, {len(lv['nests'])}, "
            f"{lv['start'][0]}, {lv['start'][1]}, {p}_ARROWS, {p}_GOLDS, "
            f"{len(lv['golds'])}}}")
    lines.append("const Level LEVELS[] = {")
    lines.append(",\n".join(entries))
    lines.append("};")
    lines.append("constexpr uint8_t LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);")
    lines.append("")
    return "\n".join(lines)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    quiet = "--quiet" in argv
    files = sorted(LEVELS_DIR.glob("*.txt"), key=natural_key)
    if not files:
        print(f"build_levels: no level files in {LEVELS_DIR}", file=sys.stderr)
        return 1
    levels, failed = [], False
    for f in files:
        try:
            lv, warnings = parse(f)
        except LevelError as err:
            print(f"build_levels: {f.name}: ERROR: {err}", file=sys.stderr)
            failed = True
            continue
        levels.append((f.name, lv))
        for w in warnings:
            print(f"build_levels: {f.name}: warning: {w}", file=sys.stderr)
        if not quiet:
            print(f"{f.name}: {lv['width']}x{lv['rows']} tiles, "
                  f"{len(lv['gorillas'])} gorillas, {len(lv['foxes'])} foxes, "
                  f"{len(lv['boxes'])} boxes, {len(lv['golds'])} golden eggs")
    if failed:
        return 1
    text = emit(levels)
    if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
        OUT.write_text(text, encoding="utf-8")
        if not quiet:
            print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
