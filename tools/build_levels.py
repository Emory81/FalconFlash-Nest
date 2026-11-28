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
MIN_WIDTH = 20  # one screen; narrower maps are padded with air

SYMBOLS = {
    ".": "air",
    " ": "air",
    "X": "ground (solid, drawn as grass)",
    "H": "dirt / background (not solid)",
    "Y": "yellow grass (one-way, crouch to drop through)",
    "S": "player start",
    "N": "nest (goal)",
    "G": "gorilla",
    "g": "red gorilla",
    "O": "golden egg (collectible)",
    "I": "infinity egg (collectible, total not shown)",
    "C": "coin (collectible)",
    "c": "green egg (extra life)",
    "R": "direction block, passable moving right",
    "L": "direction block, passable moving left",
    "U": "direction block, passable moving up",
    "D": "direction block, passable moving down",
    "F": "fox",
    "f": "glow frog",
    "B": "box",
    "M": "mystery box",
    "m": "magic bean box",
    "b": "coin brick",
    "P": "cardboard box (carry and throw)",
    "T": "tennis ball (carry and throw)",
    "h": "red egg (refills hearts)",
    "z": "blue egg (speed boost)",
    "k": "rotten egg (a rat hatches from it)",
    "A": "rat",
    **{d: f"bean spot for sublevel {d}" for d in "0123456789"},
}

ARROWS = "RLUD"      # order of the masks in Level::arrows
MAX_PIT = 7          # tiles a running jump clears


class LevelError(Exception):
    pass


def natural_key(path: Path):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", path.stem)]


def split_sections(lines):
    """[(None, main map), (number, sublevel map), ...]. A line holding only a
    digit, anywhere after the main map starts, begins that sublevel."""
    sections = [(None, [])]
    for ln in lines:
        if re.fullmatch(r"\d", ln) and any(l.strip() for l in sections[0][1]):
            number = int(ln)
            if any(n == number for n, _ in sections):
                raise LevelError(f"sublevel {number} appears twice")
            sections.append((number, []))
        else:
            sections[-1][1].append(ln)
    return sections


def parse(path: Path):
    """Return (level dict, warnings list). Raises LevelError on mistakes. The
    level's "subs" is a list of (number, level dict)."""
    raw = path.read_text(encoding="utf-8").splitlines()
    lines = [ln.rstrip() for ln in raw if not ln.lstrip().startswith("#")]
    sections = split_sections(lines)
    main_level, warnings = parse_map(sections[0][1], is_sub=False)
    subs = []
    for number, sub_lines in sections[1:]:
        try:
            lv, sub_warnings = parse_map(sub_lines, is_sub=True)
        except LevelError as err:
            raise LevelError(f"sublevel {number}: {err}") from err
        warnings += [f"sublevel {number}: {w}" for w in sub_warnings]
        subs.append((number, lv))
    main_level["subs"] = subs

    if not main_level["nests"] and not any(lv["nests"] for _, lv in subs):
        raise LevelError("needs at least one N (nest), here or in a sublevel")
    numbers = {n for n, _ in subs}
    maps = [main_level] + [lv for _, lv in subs]
    if any(b[2] == 2 for lv in maps for b in lv["boxes"]) and 0 not in numbers:
        warnings.append("has a magic bean box (m) but no sublevel 0, so most "
                        "vines will lead nowhere")
    for number in sorted({s[2] for lv in maps for s in lv["spots"]} - numbers):
        warnings.append(f"a bean spot asks for sublevel {number}, which does "
                        f"not exist yet (the vine goes to sublevel 0)")
    return main_level, warnings


def is_dirt(cell, col, row, width, rows):
    """Whether dirt is drawn in this cell: an H, or something inside a hill that
    keeps the dirt behind it. cell(col, row) gives the symbol, row 0 at the
    bottom. The level editor uses this too."""
    ch = cell(col, row)
    if ch == "H" or (ch in "OIChcz0123456789" and 0 < col < width - 1
                     and cell(col - 1, row) == "H"
                     and cell(col + 1, row) == "H"):
        return True
    sides = sum(0 <= c < width and 0 <= r < rows and cell(c, r) == "H"
                for c, r in ((col - 1, row), (col + 1, row),
                             (col, row - 1), (col, row + 1)))
    # Coins and enemies need only two sides.
    return ((ch in "SOIChcz" and sides >= 3)
            or (ch in "CGgFfPTkA" and sides >= 2))


def parse_map(lines, is_sub):
    """One map: the main level or a sublevel."""
    lines = list(lines)
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
    width = max(MIN_WIDTH, max(len(ln) for ln in lines))
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

    solid, decor, arrows, yellow = [], [], [], []
    for col in range(width):
        a_masks = [0] * len(ARROWS)
        s_mask = d_mask = y_mask = 0
        for row in range(rows):
            ch = cell(col, row)
            if ch == "X":
                s_mask |= 1 << row
            elif ch in ARROWS:  # solid, apart from the one way through
                s_mask |= 1 << row
                a_masks[ARROWS.index(ch)] |= 1 << row
            elif ch == "Y":  # collides like an up block
                s_mask |= 1 << row
                a_masks[ARROWS.index("U")] |= 1 << row
                y_mask |= 1 << row
            elif is_dirt(cell, col, row, width, rows):
                d_mask |= 1 << row  # keeps the dirt behind when inside a hill
        solid.append(s_mask)
        decor.append(d_mask)
        arrows.append(tuple(a_masks))
        yellow.append(y_mask)

    def is_solid(col, row):
        return row >= 0 and (solid[col] >> row) & 1

    def holds(col, row):  # ground, or a brick or mystery box
        return row >= 0 and (is_solid(col, row) or cell(col, row) in "BMmb")

    def stand_row(what, col, row):
        """Row of the cell the thing ends up in: it drops onto the first
        ground below (so it can float a little in the map)."""
        while row > 0 and not holds(col, row - 1):
            row -= 1
        if row == 0:  # fell all the way: nothing below to stand on
            raise LevelError(f"{what} at column {col + 1} has no ground underneath")
        return row

    starts, nests, gorillas, foxes, boxes, golds = [], [], [], [], [], []
    spots, infinities, coins, greens, items = [], [], [], [], []
    frogs = []
    cardboard = set()  # cells a cardboard box ends up in
    loose = []         # eggs and rats placed in the map
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
            elif ch == "f":
                frogs.append((col, stand_row("f (glow frog)", col, row)))
            elif ch == "O":
                golds.append((col, row))  # stays where it is drawn
            elif ch == "I":
                infinities.append((col, row))
            elif ch == "C":
                coins.append((col, row))
            elif ch == "c":
                greens.append((col, row))
            elif ch == "h":
                loose.append((col, row, 0))  # stays where it is drawn
            elif ch == "z":
                loose.append((col, row, 1))
            elif ch == "k":
                loose.append((col, stand_row("k (rotten egg)", col, row), 2))
            elif ch == "A":
                loose.append((col, stand_row("A (rat)", col, row), 3))
            elif ch in ("P", "T"):
                # These also come to rest on a cardboard box, so they stack.
                # Rows are read bottom up, so the one below is already placed.
                at = row
                while (at > 0 and not holds(col, at - 1)
                       and (col, at - 1) not in cardboard):
                    at -= 1
                if at == 0:
                    raise LevelError(f"{ch} (box or ball) at column {col + 1} "
                                     "has no ground underneath")
                if ch == "P":
                    cardboard.add((col, at))
                items.append((col, at, 1 if ch == "T" else 0))
            elif ch in ("B", "M", "m", "b"):
                kind = {"B": 0, "M": 1, "m": 2, "b": 3}[ch]
                boxes.append((col * TILE, row * TILE, kind))
            elif ch.isdigit():
                # on the top line it marks the whole column (row -1)
                spots.append((col, -1 if row == rows - 1 else row, int(ch)))

    if is_sub and not starts:  # no S: drop in from the sky over the first ground
        starts.append((next((c for c in range(width)
                             if any(holds(c, r) for r in range(rows))), 0), -1))
    if len(starts) != 1:
        raise LevelError(f"needs exactly one S (player start), found {len(starts)}")

    warnings = []
    col = width if is_sub else 0  # falling out of a sublevel is safe
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
        "yellow": yellow,
        "spots": spots, "infinities": infinities,
        "coins": coins, "greens": greens, "items": items,
        "loose": loose, "frogs": frogs,
    }, warnings


def grassy_masks(solid, arrows, yellow):
    """Per column: the direction blocks touching grass, or touching another
    grassy direction block (sides, above and below)."""
    cells = {(col, row) for col, masks in enumerate(arrows)
             for row in range(MAX_ROWS) if any((m >> row) & 1 for m in masks)
             and not (yellow[col] >> row) & 1}  # yellow grass counts as grass
    grass = {(col, row) for col, mask in enumerate(solid)
             for row in range(MAX_ROWS) if (mask >> row) & 1} - cells
    grassy, frontier = set(), list(grass)
    while frontier:
        col, row = frontier.pop()
        for near in ((col - 1, row), (col + 1, row), (col, row - 1), (col, row + 1)):
            if near in cells and near not in grassy:
                grassy.add(near)
                frontier.append(near)
    out = [0] * len(solid)
    for col, row in grassy:
        out[col] |= 1 << row
    return out


def c_array(values, per_line=8, indent="    ", fmt=str):
    out = []
    for i in range(0, len(values), per_line):
        out.append(indent + ", ".join(fmt(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(out)


def pairs(items):
    return ", ".join("{" + ", ".join(str(v) for v in it) + "}" for it in items)


def emit_map(lines, p, lv):
    """Append the arrays of one map, named with the prefix p."""
    hexu = lambda v: f"0x{v:X}u"
    lines.append(f"constexpr uint32_t {p}_SOLID[] = {{\n{c_array(lv['solid'], fmt=hexu)}\n}};")
    lines.append(f"constexpr uint32_t {p}_DECOR[] = {{\n{c_array(lv['decor'], fmt=hexu)}\n}};")
    lines.append(f"constexpr int16_t {p}_GORILLAS[][3] = {{{pairs(lv['gorillas'] or [(0, 0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_BOXES[][3] = {{{pairs(lv['boxes'] or [(0, 0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_FOXES[][2] = {{{pairs(lv['foxes'] or [(0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_NESTS[][2] = {{{pairs(lv['nests'] or [(0, 0)])}}};")
    masks = ["{" + ", ".join(hexu(m) for m in a) + "}" for a in lv["arrows"]]
    lines.append(f"constexpr uint32_t {p}_ARROWS[][4] = {{\n{c_array(masks, per_line=4)}\n}};")
    lines.append(f"constexpr int16_t {p}_GOLDS[][2] = {{{pairs(lv['golds'] or [(0, 0)])}}};")
    grassy = grassy_masks(lv["solid"], lv["arrows"], lv["yellow"])
    lines.append(f"constexpr uint32_t {p}_GRASSY[] = {{\n{c_array(grassy, fmt=hexu)}\n}};")
    lines.append(f"constexpr int16_t {p}_SPOTS[][3] = {{{pairs(lv['spots'] or [(0, 0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_INFINITIES[][2] = {{{pairs(lv['infinities'] or [(0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_COINS[][2] = {{{pairs(lv['coins'] or [(0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_GREENS[][2] = {{{pairs(lv['greens'] or [(0, 0)])}}};")
    lines.append(f"constexpr uint32_t {p}_YELLOW[] = {{\n{c_array(lv['yellow'], fmt=hexu)}\n}};")
    lines.append(f"constexpr int16_t {p}_ITEMS[][3] = {{{pairs(lv['items'] or [(0, 0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_LOOSE[][3] = {{{pairs(lv['loose'] or [(0, 0, 0)])}}};")
    lines.append(f"constexpr int16_t {p}_FROGS[][2] = {{{pairs(lv['frogs'] or [(0, 0)])}}};")
    lines.append("")


def initializer(p, lv, subs="nullptr, nullptr, 0"):
    return (f"{{{lv['width']}, {lv['rows']}, {p}_SOLID, {p}_DECOR, {p}_GORILLAS, "
            f"{len(lv['gorillas'])}, {p}_BOXES, {len(lv['boxes'])}, {p}_FOXES, "
            f"{len(lv['foxes'])}, {p}_NESTS, {len(lv['nests'])}, "
            f"{lv['start'][0]}, {lv['start'][1]}, {p}_ARROWS, {p}_GOLDS, "
            f"{len(lv['golds'])}, {p}_GRASSY, {p}_SPOTS, {len(lv['spots'])}, "
            f"{subs}, {p}_INFINITIES, {len(lv['infinities'])}, "
            f"{p}_COINS, {len(lv['coins'])}, {p}_GREENS, {len(lv['greens'])}, "
            f"{p}_YELLOW, {p}_ITEMS, {len(lv['items'])}, "
            f"{p}_LOOSE, {len(lv['loose'])}, "
            f"{p}_FROGS, {len(lv['frogs'])}}}")


def emit(levels):
    maps = [lv for _, lv in levels]
    maps += [sub for lv in list(maps) for _, sub in lv["subs"]]

    def most(key, floor=1):
        return max([len(lv[key]) for lv in maps] + [floor])

    specials = max([sum(b[2] != 0 for b in lv["boxes"]) + len(lv["loose"])
                    for lv in maps] + [1])

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
        "  const int16_t (*boxes)[3];        // {x px, bottom px above the map bottom,",
        "                                    //  0 brick / 1 mystery / 2 magic bean",
        "                                    //  / 3 coin brick}",
        "  uint16_t boxCount;",
        "  const int16_t (*foxes)[2];        // {column, row}",
        "  uint8_t foxCount;",
        "  const int16_t (*nests)[2];        // {column, row}; any one is the goal",
        "  uint8_t nestCount;",
        "  int16_t startCol;",
        "  int16_t startRow;                 // -1: fall in from the sky",
        "  const uint32_t (*arrows)[4];      // per column: direction blocks R, L, U, D",
        "  const int16_t (*golds)[2];        // golden eggs {column, row}",
        "  uint8_t goldCount;",
        "  const uint32_t* grassy;           // per column: grassy direction blocks",
        "  const int16_t (*spots)[3];        // {column, row or -1 = any, sublevel}",
        "  uint8_t spotCount;",
        "  const Level* subs;                // sublevels, reached by vine",
        "  const int16_t* subNumbers;",
        "  uint8_t subCount;",
        "  const int16_t (*infinities)[2];   // infinity eggs {column, row}",
        "  uint8_t infinityCount;",
        "  const int16_t (*coins)[2];        // {column, row}",
        "  uint16_t coinCount;",
        "  const int16_t (*greens)[2];       // green eggs {column, row}",
        "  uint8_t greenCount;",
        "  const uint32_t* yellow;           // per column: yellow grass tiles",
        "  const int16_t (*items)[3];        // {column, row, 0 box / 1 tennis ball}",
        "  uint8_t itemCount;",
        "  const int16_t (*loose)[3];        // {column, row, 0 red egg / 1 blue",
        "                                    //  / 2 rotten egg / 3 rat}",
        "  uint16_t looseCount;",
        "  const int16_t (*frogs)[2];        // glow frogs {column, row}",
        "  uint8_t frogCount;",
        "};",
        "",
        f"constexpr uint8_t LEVEL_MAX_GORILLAS = {most('gorillas')};",
        f"constexpr uint16_t LEVEL_MAX_BOXES = {most('boxes')};",
        "// Most in one map: boxes that give something out (not plain bricks),",
        "// plus placed eggs and rats.",
        f"constexpr uint16_t LEVEL_MAX_SPECIALS = {specials};",
        f"constexpr uint8_t LEVEL_MAX_FOXES = {most('foxes')};",
        f"constexpr uint8_t LEVEL_MAX_FROGS = {most('frogs')};",
        f"constexpr int16_t LEVEL_MAX_ROWS = {max(lv['rows'] for lv in maps)};",
        f"constexpr uint8_t LEVEL_MAX_NESTS = {most('nests')};",
        f"constexpr uint8_t LEVEL_MAX_GOLDS = {most('golds')};",
        f"constexpr uint8_t LEVEL_MAX_INFINITIES = {most('infinities')};",
        f"constexpr uint16_t LEVEL_MAX_COINS = {most('coins')};",
        f"constexpr uint8_t LEVEL_MAX_GREENS = {most('greens')};",
        f"constexpr uint8_t LEVEL_MAX_ITEMS = {most('items')};",
        "",
    ]
    entries = []
    for i, (name, lv) in enumerate(levels, 1):
        p = f"LEVEL{i}"
        lines.append(f"// {name}")
        emit_map(lines, p, lv)
        subs = "nullptr, nullptr, 0"
        if lv["subs"]:
            for number, sub_level in lv["subs"]:
                lines.append(f"// {name}, sublevel {number}")
                emit_map(lines, f"{p}_SUB{number}", sub_level)
            inits = ",\n".join("    " + initializer(f"{p}_SUB{n}", s)
                               for n, s in lv["subs"])
            lines.append(f"constexpr Level {p}_SUBS[] = {{\n{inits}\n}};")
            numbers = ", ".join(str(n) for n, _ in lv["subs"])
            lines.append(f"constexpr int16_t {p}_SUBNUMBERS[] = {{{numbers}}};")
            lines.append("")
            subs = f"{p}_SUBS, {p}_SUBNUMBERS, {len(lv['subs'])}"
        entries.append("    " + initializer(p, lv, subs))
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
                  f"{len(lv['boxes'])} boxes, {len(lv['golds'])} golden eggs, "
                  f"sublevels {[n for n, _ in lv['subs']]}")
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
