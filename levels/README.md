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
| `X` | grass ground (solid) |
| `H` | dirt / background (decoration, not solid) |
| `S` | player start (exactly one) |
| `N` | a nest, the goal (at least one; reaching any of them finishes the level) |
| `G` | gorilla (patrols back and forth) |
| `g` | red gorilla (chases you) |
| `O` | golden egg (collectible; hangs exactly where you draw it) |
| `I` | infinity egg (like a golden egg, but the total is never shown) |
| `R` | right direction block (only stops you moving left) |
| `L` | left direction block (only stops you moving right) |
| `U` | up direction block (only stops you falling; stand on it) |
| `D` | down direction block (only stops you rising) |
| `F` | fox |
| `B` | box (breaks when bumped from below) |
| `M` | mystery box (hatches an egg: red refills hearts, blue is speed, green is an extra life, or sometimes a rat) |
| `m` | magic bean box (pushes out a bean) |
| `0`-`9` | bean spot: a vine from here leads to that sublevel |

Symbols are case-sensitive: `G` and `g` are different gorillas. Anything not in
the table stops the build with an error.

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
- **Enemies, start and nest:** these drop from where you draw them onto the first
  solid tile or box below, so draw them on or above their floor. They need ground under
  them (not a pit).
- **Boxes:** a box is a full block: you and enemies can stand and walk on it,
  and the start, nests and enemies can be placed on top. Bumping it from below
  knocks off any enemy on it.
- **Box height:** a box fills exactly one cell. The bird is 2 tiles tall, so a box in
  the row 3 above the ground (two empty rows under it) can be walked under and
  bumped by jumping. Directly above the ground it is a crate you walk into.
- **Direction blocks:** one-way blocks. They stop you only when you move against
  the arrow, and you pass through the other three ways. Enemies cannot pass
  them. One that touches an `X` (beside, above or below), or touches another
  grassy direction block, is drawn grassy.
- **Dirt:** only the cells you mark with `H` are drawn, so fill in every cell
  you want. A golden or infinity egg with `H` on both sides keeps the dirt behind it. An
  `H` directly under an `X` is drawn shaded.
- **Height:** up to 32 rows. The screen shows 15 rows, so taller maps scroll up
  and down to follow the bird; shorter ones sit at the bottom.

## Magic beans, vines and sublevels

- Bump an `m` box and a bean pops out. It falls if nothing is under it.
- Walk into the bean and a vine grows up to the top of the level, through any
  blocks. Next to it, press jump to grab on, hold jump to climb, press crouch to
  climb down, and press left or right to step off.
- Touch the bean while crouched to carry it on your back. You move as normal
  while carrying it, can climb vines with it, and take it with you into other
  areas. Hold crouch for a second to set it down one block ahead.
- A bean that falls down a pit drops out of the sky again in the same column.
- Climbing off the top of the screen takes you to a sublevel.

**Sublevels** go in the same file, under the main map. A line holding only a
number starts that sublevel:

```
(the main map)
0
(sublevel 0)
1
(sublevel 1)
```

**Which sublevel a vine leads to** (the same rules in the main map and in
sublevels):

- A number on the **top line** of a map: any bean in that column.
- A number **anywhere else**: only a bean sitting in that exact cell (the empty
  cell just above the floor). It wins over a top-line number.
- Otherwise, sublevel `0`. A number with no sublevel of its own also goes to
  `0`, and with no sublevel `0` the vine leads nowhere.

**Inside a sublevel:** `S` is where you arrive (without one you drop in from the
sky). A nest is optional and completes the main level. Falling off the bottom is
safe and drops you back into the main level from the sky above the vine you left
it by, even if you went on through more sublevels. Losing
all hearts costs a life and restarts the main level. The level needs at least one
nest in its main map or a sublevel.

## What the player can do

- Jump about 5 tiles high; a 5-tile step is tight. Use ledges for anything
  taller.
- Clear a pit of about 4 tiles with a normal jump, and up to about 7 tiles with
  a running jump (double-tap left or right and keep holding). The checker warns
  about pits wider than 7.
- Stand on a nest to finish the level.

## Limits

Everything is set by your maps; there are no fixed counts for gorillas, boxes,
foxes, nests or golden eggs. More enemies on one level cost a little speed, so keep it reasonable.
