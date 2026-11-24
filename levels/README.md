# Super Flashy Birds levels

Each level is a plain text file in this folder: `level1.txt`, `level2.txt`, ...
The game plays them in order (natural order, so `level10.txt` comes after
`level9.txt`) and loops back to level 1 after the last one.

**To add a level:** copy an existing file, edit it, save it as the next number,
and build/upload as usual. PlatformIO converts the text files automatically
before every build. To check a map without flashing, run:

```bash
python tools/build_levels.py
```

It prints a summary, plus an error (build stops) or a warning (build continues)
if something looks wrong.

## The map

One character is one tile (16 px, the same size as a box). The top line is the highest row and the
bottom line is the ground row. Lines starting with `#` are comments. Short lines
are padded with air, so you don't need trailing dots.

| Symbol | Meaning |
| --- | --- |
| `.` or space | air |
| `X` | ground |
| `H` | background hill (decoration, not solid) |
| `S` | player start (exactly one) |
| `N` | a nest, the goal (at least one; reaching any of them finishes the level) |
| `G` | gorilla (patrols back and forth) |
| `g` | red gorilla (chases you) |
| `O` | golden egg (collectible; hangs exactly where you draw it) |
| `R` `L` `U` `D` | direction block: you can only pass through it moving right, left, up or down |
| `F` | fox |
| `B` | box (breaks when bumped from below) |
| `M` | mystery box (hatches an egg: powerup, or sometimes a rat) |

```
...BMB..........
..S...XXX.g...GN
XXXXXXXXXXXX..XX
```

## How the symbols are read

- **Ground:** every `X` is a solid grass tile, wherever it is on the map; the
  game draws exactly what you draw. A column with no `X` is a gap you fall
  through. Use `H` under the grass to draw dirt (it is only decoration, so
  you can't stand on it; the grass row is the floor).
- **Enemies, start and nest:** these only use the *column* they are in. They are
  placed on the ground of that column, so you can put them in any row. They need
  ground under them (not a pit).
- **Boxes:** a box fills exactly one cell. The bird is 2 tiles tall, so a box in
  the row 3 above the ground (two empty rows under it) can be walked under and
  bumped by jumping. Directly above the ground it is a crate you walk into.
- **Direction blocks:** every side is solid except the way in that matches the
  arrow (`U` is a platform you can jump up through). Enemies cannot pass them.
- **Hill:** you only need to mark the top of the hill in each column. Every
  cell below it (above the ground) is filled in.
- **Height:** up to 32 rows. The screen shows 15 rows, so taller maps scroll up
  and down to follow the bird; shorter ones sit at the bottom.

## What the player can do

- Jump about 5 tiles high; a 5-tile step is tight. Use ledges for anything
  taller (the checker warns about walls taller than 4 with no ledge near them).
- Clear a pit of about 4 tiles with a normal jump, and up to about 7 tiles with
  a running jump (double-tap left or right and keep holding). Wider pits get a
  warning.
- Stand on a nest to finish the level.

## Limits

Everything is set by your maps; there are no fixed counts for gorillas, boxes or
foxes. More enemies on one level cost a little speed, so keep it reasonable.
