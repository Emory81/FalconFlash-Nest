"""Convert FalconFlash logo PNG to RGB565 C array for TFT_eSPI."""

from pathlib import Path

from PIL import Image

SRC = Path(__file__).resolve().parents[1] / "assets" / "falcon_logo_small.png"
OUT = Path(__file__).resolve().parents[1] / "include" / "display" / "falcon_logo.h"

# The source is drawn at its final on-screen size, so it is not resized.


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main() -> None:
    if not SRC.exists():
        raise FileNotFoundError(f"Logo not found at {SRC}")

    # Blend over the black splash screen using the alpha channel, which keeps
    # the grey shades and smooth edges.
    image = Image.open(SRC).convert("RGBA")
    width, height = image.size

    pixels: list[int] = []
    for y in range(height):
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            pixels.append(rgb_to_rgb565(r * a // 255, g * a // 255, b * a // 255))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"constexpr uint16_t FALCON_LOGO_WIDTH = {width};",
        f"constexpr uint16_t FALCON_LOGO_HEIGHT = {height};",
        "",
        "constexpr uint16_t falconLogo[] PROGMEM = {",
    ]

    for index, value in enumerate(pixels):
        suffix = "," if index < len(pixels) - 1 else ""
        if index % 12 == 0:
            lines.append("  " + f"0x{value:04X}{suffix}")
        else:
            lines[-1] = lines[-1].rstrip(",") + f", 0x{value:04X}{suffix}"

    lines.append("};")
    lines.append("")
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {OUT} ({width}x{height}, {len(pixels)} pixels)")


if __name__ == "__main__":
    main()
