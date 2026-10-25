"""Convert FalconFlash logo PNG to RGB565 C array for TFT_eSPI."""

from pathlib import Path

from PIL import Image

SRC = Path(__file__).resolve().parents[1] / "assets" / "falcon_logo.png"
OUT = Path(__file__).resolve().parents[1] / "include" / "falcon_logo.h"

TARGET_WIDTH = 140


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def is_logo_pixel(r: int, g: int, b: int) -> bool:
    # Keep bright/white line art; drop dark navy background.
    brightness = 0.299 * r + 0.587 * g + 0.114 * b
    return brightness > 90


def main() -> None:
    if not SRC.exists():
        raise FileNotFoundError(f"Logo not found at {SRC}")

    image = Image.open(SRC).convert("RGB")
    ratio = TARGET_WIDTH / image.width
    target_height = int(image.height * ratio)
    image = image.resize((TARGET_WIDTH, target_height), Image.Resampling.LANCZOS)

    pixels: list[int] = []
    for y in range(target_height):
        for x in range(TARGET_WIDTH):
            r, g, b = image.getpixel((x, y))
            if is_logo_pixel(r, g, b):
                pixels.append(rgb_to_rgb565(255, 255, 255))
            else:
                pixels.append(rgb_to_rgb565(0, 0, 0))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"constexpr uint16_t FALCON_LOGO_WIDTH = {TARGET_WIDTH};",
        f"constexpr uint16_t FALCON_LOGO_HEIGHT = {target_height};",
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
    print(f"Wrote {OUT} ({TARGET_WIDTH}x{target_height}, {len(pixels)} pixels)")


if __name__ == "__main__":
    main()
