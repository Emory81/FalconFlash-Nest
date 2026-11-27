# Flashy Golf maps

`level1.txt`, `level2.txt` and `level3.txt` are the three holes. Every build
turns them into `include/golf_levels.h`.

The first line is `par GOLD SILVER`: finishing in `GOLD` flaps or fewer earns
the gold star, `SILVER` or fewer the silver one. Then come 15 rows of equal
length, one character per 16 px tile (the screen is 20 tiles wide).

| Symbol | Meaning |
|---|---|
| `.` | sky |
| `X` | grass |
| `H` | dirt |
| `B` | box |
| `C` | cloud |
| `S` | where the golf egg starts (one) |
| `FF` | the hole, two tiles wide, set into the ground (one) |

All of `X`, `H`, `B` and `C` are solid. The egg bounces off the sides of the
map and its top; falling off the bottom puts it back where it last came to
rest, and the flaps still count.

The ball is `include/art/Flashy Golf/GolfEggUp.png`, switching to
`GolfEggDown.png` for a moment on each flap. Both are 20 x 16 with the egg in the middle 14 pixels (only the egg
hits things, not the wings); after changing
one, run `python tools/convert_builtin_sprites.py`.
