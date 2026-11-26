"""Generate raw game assets and a device-readable manifest.

The ESP32 renders RGB565 sprites and an RGB332 Flappy background directly.
Keeping those bytes in files lets the firmware fetch graphics from the
repository without compiling the image payload into the OTA application.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "include" / "art"
SUPER_ART = ART / "super-flashy-birds"
OUT = ROOT / "game_assets"

TRANSPARENT = 0xF81F
SCREEN_WIDTH = 320
SCREEN_HEIGHT = 240


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def cover_resize(image: Image.Image, width: int, height: int) -> Image.Image:
    scale = max(width / image.width, height / image.height)
    resized = image.resize(
        (max(width, round(image.width * scale)),
         max(height, round(image.height * scale))),
        Image.Resampling.LANCZOS,
    )
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def convert_rgb565(path: Path, shade: float = 1.0) -> tuple[int, int, bytes]:
    image = Image.open(path).convert("RGBA")
    pixels = bytearray()
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, alpha = image.getpixel((x, y))
            if alpha < 128:
                color = TRANSPARENT
            else:
                color = rgb565(int(r * shade), int(g * shade), int(b * shade))
                # A visible pixel must never become the transparency key.
                if color == TRANSPARENT:
                    color ^= 1
            pixels.extend(struct.pack("<H", color))
    return image.width, image.height, bytes(pixels)


def compose_background(sky_path: Path) -> Image.Image:
    """The Frey building has a transparent sky area: lay a sky behind it."""
    building = Image.open(ART / "FreyDrawingBackground.png").convert("RGBA")
    canvas = Image.new("RGBA", building.size)
    canvas.alpha_composite(Image.open(sky_path).convert("RGBA"))
    canvas.alpha_composite(building)
    # Stray gaps neither layer covers take the color of the pixel above.
    for y in range(canvas.height):
        for x in range(canvas.width):
            if canvas.getpixel((x, y))[3] < 255:
                canvas.putpixel((x, y), canvas.getpixel((x, max(0, y - 1))))
    return canvas


def convert_background(sky_path: Path) -> tuple[int, int, bytes]:
    image = cover_resize(compose_background(sky_path).convert("RGB"),
                         SCREEN_WIDTH, SCREEN_HEIGHT)
    buf = [[list(map(float, image.getpixel((x, y))))
            for x in range(image.width)] for y in range(image.height)]
    levels = (7, 7, 3)
    dither = len(set(image.getdata())) > 256
    pixels = bytearray()
    for y in range(image.height):
        for x in range(image.width):
            old = [max(0.0, min(255.0, value)) for value in buf[y][x]]
            quantized = [
                round(old[index] / 255 * levels[index])
                for index in range(3)
            ]
            pixels.append((quantized[0] << 5) |
                          (quantized[1] << 2) |
                          quantized[2])
            if not dither:
                continue
            error = [
                old[index] - quantized[index] * 255 / levels[index]
                for index in range(3)
            ]
            for dx, dy, weight in ((1, 0, 7), (-1, 1, 3),
                                   (0, 1, 5), (1, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < image.width and ny < image.height:
                    for index in range(3):
                        buf[ny][nx][index] += error[index] * weight / 16
    return image.width, image.height, bytes(pixels)


ASSETS: list[tuple[str, Path, str, float]] = [
    ("flappy/bird_closed.rgb565", ART / "FlashyBirdClosedFlapBig.png",
     "rgb565", 1.0),
    ("flappy/bird_back.rgb565", ART / "FlashyBirdWingsBackBig.png",
     "rgb565", 1.0),
    ("flappy/bird_transition.rgb565",
     ART / "FlashyBirdTransitionFlapBig.png", "rgb565", 1.0),
    ("flappy/bird_open.rgb565", ART / "FlashyBirdOpenFlapBig.png",
     "rgb565", 1.0),
    ("flappy/pipe_body.rgb565", ART / "PipeBodyRed.png", "rgb565", 1.0),
    ("flappy/pipe_cap.rgb565", ART / "PipeCapRed.png", "rgb565", 1.0),
    # For backgrounds the source is the sky that goes behind the building.
    ("flappy/background_day.rgb332", ART / "DaySky.png", "rgb332", 1.0),
    ("flappy/background_night.rgb332", ART / "NightSky.png", "rgb332", 1.0),
    ("super/bird.rgb565", SUPER_ART / "FlashyBirdStandStill.png",
     "rgb565", 1.0),
    ("super/walk0.rgb565", SUPER_ART / "FlashyBirdLeftFootForward.png",
     "rgb565", 1.0),
    ("super/walk1.rgb565", SUPER_ART / "FlashyBirdLeftFootFullUp.png",
     "rgb565", 1.0),
    ("super/walk2.rgb565", SUPER_ART / "FlashyBirdRightFootForward.png",
     "rgb565", 1.0),
    ("super/walk3.rgb565", SUPER_ART / "FlashyBirdRightFootFullUp.png",
     "rgb565", 1.0),
    ("super/walk4.rgb565",
     SUPER_ART / "FlashyBirdLeftFootTransitionPost.png", "rgb565", 1.0),
    ("super/walk5.rgb565",
     SUPER_ART / "FlashyBirdLeftFootTransitionPost2.png", "rgb565", 1.0),
    ("super/walk6.rgb565",
     SUPER_ART / "FlashyBirdRightFootTransitionPost.png", "rgb565", 1.0),
    ("super/walk7.rgb565",
     SUPER_ART / "FlashyBirdRightFootTransitionPost2.png", "rgb565", 1.0),
    ("super/duck.rgb565", SUPER_ART / "FlashyBirdDucking.png",
     "rgb565", 1.0),
    ("super/gorilla.rgb565", SUPER_ART / "Gorilla.png", "rgb565", 1.0),
    ("super/red_gorilla.rgb565", SUPER_ART / "RedGorilla.png",
     "rgb565", 1.0),
    ("super/dirt.rgb565", SUPER_ART / "DirtTile.png", "rgb565", 1.0),
    ("super/grass.rgb565", SUPER_ART / "GrassTile.png", "rgb565", 1.0),
    ("super/box.rgb565", SUPER_ART / "Box.png", "rgb565", 1.0),
    ("super/mystery.rgb565", SUPER_ART / "MysteryBox.png", "rgb565", 1.0),
    ("super/mystery_off.rgb565", SUPER_ART / "MysteryBox.png",
     "rgb565", 0.4),
    ("super/egg.rgb565", SUPER_ART / "RedEgg.png", "rgb565", 1.0),
    ("super/blue_egg.rgb565", SUPER_ART / "BlueEgg.png", "rgb565", 1.0),
    ("super/rat.rgb565", SUPER_ART / "Rat.png", "rgb565", 1.0),
    ("super/fox.rgb565", SUPER_ART / "Fox.png", "rgb565", 1.0),
    ("super/nest.rgb565", SUPER_ART / "Nest.png", "rgb565", 1.0),
    ("super/egg_cracking.rgb565", SUPER_ART / "CrackingEgg.png",
     "rgb565", 1.0),
    ("super/egg_open.rgb565", SUPER_ART / "CrackedOpenEgg.png",
     "rgb565", 1.0),
]


def main() -> None:
    OUT.mkdir(exist_ok=True)
    manifest: list[str] = [
        "# FalconFlash raw game assets",
        "# path|width|height|format|size|sha256",
        "manifest_version|1",
    ]
    total = 0

    # The super/ files are left exactly as they are. Super Flashy Birds now
    # builds its pictures into the firmware, and devices still running older
    # firmware reject files whose sizes have changed.
    previous = {}
    if (OUT / "manifest.txt").exists():
        for line in (OUT / "manifest.txt").read_text(encoding="utf-8").splitlines():
            previous[line.split("|")[0]] = line

    for relative, source, format_name, shade in ASSETS:
        if relative.startswith("super/") and relative in previous:
            manifest.append(previous[relative])
            total += (OUT / relative).stat().st_size
            continue
        if format_name == "rgb332":
            width, height, data = convert_background(source)
        else:
            width, height, data = convert_rgb565(source, shade)
        output = OUT / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        manifest.append(
            f"{relative}|{width}|{height}|{format_name}|{len(data)}|{digest}"
        )
        total += len(data)
        print(f"{relative}: {width}x{height}, {len(data)} bytes")

    (OUT / "manifest.txt").write_text("\n".join(manifest) + "\n",
                                      encoding="utf-8")
    print(f"Wrote {OUT / 'manifest.txt'} ({len(ASSETS)} assets, {total} bytes)")


if __name__ == "__main__":
    main()
