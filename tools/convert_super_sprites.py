"""Convert Super Flashy Birds PNG art to RGB565 C arrays for TFT_eSPI.

Transparent pixels (alpha < 128) become SPRITE_TRANSPARENT (magenta).
Output: include/super_flashy_sprites.h
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "include" / "art" / "super-flashy-birds"
OUT = ROOT / "include" / "super_flashy_sprites.h"

TRANSPARENT = 0xF81F
DIRT = ART / "DirtTile.png"


def grass_shadow(y: int) -> float:
    """Brightness of row y of a dirt tile that sits under grass."""
    return 0.55 if y < 5 else (0.75 if y < 9 else 1.0)
ARROW = ART / "DirectionalBlock.png"
GRASSY = ART / "GrassyDirectionalBlock.png"  # used next to grass

SPRITES = [
    ("sfbBird", ART / "FlashyBirdStandStill.png"),
    ("sfbWalk0", ART / "FlashyBirdLeftFootForward.png"),
    ("sfbWalk1", ART / "FlashyBirdLeftFootFullUp.png"),
    ("sfbWalk2", ART / "FlashyBirdRightFootForward.png"),
    ("sfbWalk3", ART / "FlashyBirdRightFootFullUp.png"),
    ("sfbWalk4", ART / "FlashyBirdLeftFootTransitionPost.png"),
    ("sfbWalk5", ART / "FlashyBirdLeftFootTransitionPost2.png"),
    ("sfbWalk6", ART / "FlashyBirdRightFootTransitionPost.png"),
    ("sfbWalk7", ART / "FlashyBirdRightFootTransitionPost2.png"),
    ("sfbDuck", ART / "FlashyBirdDucking.png"),
    ("sfbDive", ART / "FlashyBirdDive.png"),  # shown while floating
    ("sfbGorilla", ART / "Gorilla.png"),
    ("sfbRedGorilla", ART / "RedGorilla.png"),
    ("sfbDirt", DIRT),
    ("sfbDirtShadow", DIRT, grass_shadow),
    ("sfbGrass", ART / "GrassTile.png"),
    ("sfbBox", ART / "Box.png"),
    ("sfbMystery", ART / "MysteryBox.png"),
    # Used mystery box: same art, darkened (colors scaled to 40%).
    ("sfbMysteryOff", ART / "MysteryBox.png", 0.4),
    ("sfbEgg", ART / "RedEgg.png"),
    ("sfbBlueEgg", ART / "BlueEgg.png"),
    ("sfbGreenEgg", ART / "GreenEgg.png"),
    ("sfbRat", ART / "Rat.png"),
    ("sfbFox", ART / "Fox.png"),
    ("sfbNest", ART / "Nest.png"),
    ("sfbEggCracking", ART / "CrackingEgg.png"),
    ("sfbEggOpen", ART / "CrackedOpenEgg.png"),
    ("sfbGoldEgg", ART / "GoldenEgg.png"),
    ("sfbInfinityEgg", ART / "InfinityEgg.png"),
    ("sfbBean", ART / "MagicBean.png"),
    # Direction blocks: one drawing (pointing right), rotated counterclockwise.
    ("sfbArrowRight", ARROW),
    ("sfbArrowLeft", ARROW, 1.0, 180),
    ("sfbArrowUp", ARROW, 1.0, 90),
    ("sfbArrowDown", ARROW, 1.0, 270),
    ("sfbGrassyArrowRight", GRASSY),
    ("sfbGrassyArrowLeft", GRASSY, 1.0, 180),
    ("sfbGrassyArrowUp", GRASSY, 1.0, 90),
    ("sfbGrassyArrowDown", GRASSY, 1.0, 270),
]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert(path: Path, shade=1.0, angle: int = 0):
    # shade: a brightness factor, or a function of the row that returns one.
    image = Image.open(path).convert("RGBA").rotate(angle, expand=True)
    pixels = []
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = image.getpixel((x, y))
            k = shade(y) if callable(shade) else shade
            r, g, b = int(r * k), int(g * k), int(b * k)
            pixels.append(TRANSPARENT if a < 128 else rgb565(r, g, b))
            if pixels[-1] == TRANSPARENT and a >= 128:
                pixels[-1] ^= 1  # never collide with the key color
    return image.width, image.height, pixels


# The sky fills the screen and wraps sideways. It is stored as RGB332, the
# game's 8-bit frame buffer format, so rows can be copied straight in.
SKY = ART / "Sky.png"


def sky_lines() -> list[str]:
    image = Image.open(SKY).convert("RGB")
    if image.size != (320, 240):
        raise ValueError(f"{SKY.name} must be 320x240")
    pixels = []
    for y in range(image.height):
        for x in range(image.width):
            r, g, b = image.getpixel((x, y))
            pixels.append((round(r * 7 / 255) << 5) | (round(g * 7 / 255) << 2)
                          | round(b * 3 / 255))
    # Each row is run-length encoded as (length, color) byte pairs; the sky
    # is mostly flat, so this is a small fraction of the raw picture.
    runs, starts = [], []
    for y in range(image.height):
        starts.append(len(runs))
        row = pixels[y * image.width:(y + 1) * image.width]
        x = 0
        while x < len(row):
            n = 1
            while x + n < len(row) and n < 255 and row[x + n] == row[x]:
                n += 1
            runs += [n, row[x]]
            x += n
    if len(runs) > 0xFFFF:
        raise ValueError(f"{SKY.name} has too much detail to compress")
    lines = [
        f"constexpr uint16_t sfbSkyWidth = {image.width};",
        f"constexpr uint16_t sfbSkyHeight = {image.height};",
        "// Where each row starts in sfbSkyRuns.",
        "constexpr uint16_t sfbSkyRows[] PROGMEM = {",
    ]
    for i in range(0, len(starts), 12):
        lines.append("    " + ", ".join(str(v) for v in starts[i:i + 12]) + ",")
    lines += ["};", "// RGB332 runs: length, color.",
              "constexpr uint8_t sfbSkyRuns[] PROGMEM = {"]
    for i in range(0, len(runs), 16):
        lines.append("    " + ", ".join(f"0x{v:02X}" for v in runs[i:i + 16]) + ",")
    lines += ["};", ""]
    print(f"{SKY.name}: {len(runs) + 2 * len(starts)} bytes compressed")
    return lines


def main() -> None:
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "// Generated by tools/convert_super_sprites.py - do not edit by hand.",
        "constexpr uint16_t SFB_TRANSPARENT = 0xF81F;",
        "",
    ]
    for name, path, *options in SPRITES:
        w, h, pixels = convert(path, *options)
        upper = name[0].upper() + name[1:]
        lines.append(f"constexpr uint16_t {name}Width = {w};")
        lines.append(f"constexpr uint16_t {name}Height = {h};")
        lines.append(f"constexpr uint16_t {name}[] PROGMEM = {{")
        for i in range(0, len(pixels), 12):
            row = ", ".join(f"0x{p:04X}" for p in pixels[i:i + 12])
            lines.append(f"    {row},")
        lines.append("};")
        lines.append("")
    lines += sky_lines()
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
