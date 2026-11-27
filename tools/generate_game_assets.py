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


def dirt_shadow(y: int) -> float:
    return 0.55 if y < 5 else (0.75 if y < 9 else 1.0)


def convert_rgb565(path: Path, shade: float = 1.0, angle: int = 0,
                   row_shade=None) -> tuple[int, int, bytes]:
    image = Image.open(path).convert("RGBA").rotate(angle, expand=True)
    pixels = bytearray()
    for y in range(image.height):
        scale = row_shade(y) if row_shade is not None else shade
        for x in range(image.width):
            r, g, b, alpha = image.getpixel((x, y))
            r, g, b = int(r * scale), int(g * scale), int(b * scale)
            if alpha < 128:
                color = TRANSPARENT
            else:
                color = rgb565(r, g, b)
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
    ("super/dive.rgb565", SUPER_ART / "FlashyBirdDive.png", "rgb565", 1.0),
    ("super/dirt_shadow.rgb565", SUPER_ART / "DirtTile.png", "rgb565", 1.0),
    ("super/green_egg.rgb565", SUPER_ART / "GreenEgg.png", "rgb565", 1.0),
    ("super/gold_egg.rgb565", SUPER_ART / "GoldenEgg.png", "rgb565", 1.0),
    ("super/infinity_egg.rgb565", SUPER_ART / "InfinityEgg.png",
     "rgb565", 1.0),
    ("super/bean.rgb565", SUPER_ART / "MagicBean.png", "rgb565", 1.0),
    ("super/arrow_right.rgb565", SUPER_ART / "DirectionalBlock.png",
     "rgb565", 1.0),
    ("super/arrow_left.rgb565", SUPER_ART / "DirectionalBlock.png",
     "rgb565", 1.0),
    ("super/arrow_up.rgb565", SUPER_ART / "DirectionalBlock.png",
     "rgb565", 1.0),
    ("super/arrow_down.rgb565", SUPER_ART / "DirectionalBlock.png",
     "rgb565", 1.0),
    ("super/grassy_arrow_right.rgb565", SUPER_ART / "GrassyDirectionalBlock.png",
     "rgb565", 1.0),
    ("super/grassy_arrow_left.rgb565", SUPER_ART / "GrassyDirectionalBlock.png",
     "rgb565", 1.0),
    ("super/grassy_arrow_up.rgb565", SUPER_ART / "GrassyDirectionalBlock.png",
     "rgb565", 1.0),
    ("super/grassy_arrow_down.rgb565", SUPER_ART / "GrassyDirectionalBlock.png",
     "rgb565", 1.0),
]

# Same rotations the firmware sprites use: the source art points right.
ROTATE = {
    "super/arrow_left.rgb565": 180,
    "super/arrow_up.rgb565": 90,
    "super/arrow_down.rgb565": 270,
    "super/grassy_arrow_left.rgb565": 180,
    "super/grassy_arrow_up.rgb565": 90,
    "super/grassy_arrow_down.rgb565": 270,
}


def center_in_tile(width: int, height: int, data: bytes, tile: int = 16) -> bytes:
    """Center a sprite in a transparent tile, matching the old flashed art."""
    if width > tile or height > tile:
        raise ValueError(f"sprite {width}x{height} does not fit in {tile}x{tile}")
    pixels = [TRANSPARENT] * (tile * tile)
    small = [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]
    left, top = (tile - width) // 2, (tile - height) // 2
    for y in range(height):
        start = (top + y) * tile + left
        pixels[start:start + width] = small[y * width:(y + 1) * width]
    return b"".join(struct.pack("<H", value) for value in pixels)


def rotate_tile(pixels: list[int], quarter_turns: int, tile: int = 16) -> list[int]:
    """Turn a tile counterclockwise, one quarter turn at a time."""
    for _ in range(quarter_turns % 4):
        pixels = [pixels[x * tile + (tile - 1 - y)]
                  for y in range(tile) for x in range(tile)]
    return pixels


# Pictures that used to be compiled into the firmware. Each is stored as a
# 16 x 16 tile, with smaller art centered on transparent pixels.
BUILTIN_TILES = [
    ("super/cloud.rgb565", SUPER_ART / "CloudTile.png"),
    ("super/coin.rgb565", SUPER_ART / "Coin.png"),
    ("super/yellow_grass.rgb565", SUPER_ART / "YellowGrassTile.png"),
    ("super/cloud_background.rgb565", SUPER_ART / "CloudBackgroundTile.png"),
    ("super/cardboard_box.rgb565", SUPER_ART / "CardboardBox.png"),
    ("super/tennis_ball.rgb565", SUPER_ART / "TennisBall.png"),
]
WHITE_ARROW = SUPER_ART / "WhiteDirectionalBlock.png"
WHITE_ARROW_TURNS = [
    ("super/white_arrow_right.rgb565", 0),
    ("super/white_arrow_up.rgb565", 1),
    ("super/white_arrow_left.rgb565", 2),
    ("super/white_arrow_down.rgb565", 3),
]
GOLF_ART = ART / "Flashy Golf"
GOLF_SPRITES = [
    ("golf/egg_up.rgb565", GOLF_ART / "GolfEggUp.png"),
    ("golf/egg_down.rgb565", GOLF_ART / "GolfEggDown.png"),
]


def sky_rle() -> tuple[int, int, bytes]:
    """RGB332 sky, run-length encoded the way the game unpacks it."""
    image = Image.open(SUPER_ART / "Sky.png").convert("RGB")
    if image.size != (SCREEN_WIDTH, SCREEN_HEIGHT):
        raise ValueError("Sky.png must be 320x240")
    pixels = []
    for y in range(image.height):
        for x in range(image.width):
            r, g, b = image.getpixel((x, y))
            pixels.append((round(r * 7 / 255) << 5) | (round(g * 7 / 255) << 2)
                          | round(b * 3 / 255))
    runs = []
    starts = []
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
    payload = struct.pack("<HHH", image.width, image.height, len(runs))
    payload += struct.pack("<" + "H" * len(starts), *starts)
    payload += bytes(runs)
    return image.width, image.height, payload


def main() -> None:
    OUT.mkdir(exist_ok=True)
    manifest: list[str] = [
        "# FalconFlash raw game assets",
        "# path|width|height|format|size|sha256",
        "manifest_version|1",
    ]
    total = 0
    written = set()

    for relative, source, format_name, shade in ASSETS:
        angle = ROTATE.get(relative, 0)
        row_shade = dirt_shadow if relative == "super/dirt_shadow.rgb565" else None
        if format_name == "rgb332":
            width, height, data = convert_background(source)
        else:
            width, height, data = convert_rgb565(source, shade, angle, row_shade)
        output = OUT / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        manifest.append(
            f"{relative}|{width}|{height}|{format_name}|{len(data)}|{digest}"
        )
        written.add(relative)
        total += len(data)
        print(f"{relative}: {width}x{height}, {len(data)} bytes")

    sky_w, sky_h, sky = sky_rle()
    sky_path = OUT / "super" / "sky.rle"
    sky_path.write_bytes(sky)
    sky_digest = hashlib.sha256(sky).hexdigest()
    manifest.append(
        f"super/sky.rle|{sky_w}|{sky_h}|rle332|{len(sky)}|{sky_digest}"
    )
    written.add("super/sky.rle")
    total += len(sky)
    print(f"super/sky.rle: {sky_w}x{sky_h}, {len(sky)} bytes")

    for relative, source in BUILTIN_TILES:
        width, height, data = convert_rgb565(source)
        tiled = center_in_tile(width, height, data)
        output = OUT / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(tiled)
        digest = hashlib.sha256(tiled).hexdigest()
        manifest.append(f"{relative}|16|16|rgb565|{len(tiled)}|{digest}")
        written.add(relative)
        total += len(tiled)
        print(f"{relative}: 16x16, {len(tiled)} bytes")

    arrow_w, arrow_h, arrow_data = convert_rgb565(WHITE_ARROW)
    if (arrow_w, arrow_h) != (16, 16):
        raise ValueError(f"{WHITE_ARROW.name} must be 16x16")
    arrow_pixels = [arrow_data[i] | (arrow_data[i + 1] << 8)
                    for i in range(0, len(arrow_data), 2)]
    for relative, turns in WHITE_ARROW_TURNS:
        turned = rotate_tile(arrow_pixels, turns)
        tiled = b"".join(struct.pack("<H", value) for value in turned)
        output = OUT / relative
        output.write_bytes(tiled)
        digest = hashlib.sha256(tiled).hexdigest()
        manifest.append(f"{relative}|16|16|rgb565|{len(tiled)}|{digest}")
        written.add(relative)
        total += len(tiled)
        print(f"{relative}: 16x16, {len(tiled)} bytes")

    for relative, source in GOLF_SPRITES:
        width, height, data = convert_rgb565(source)
        if (width, height) != (20, 16):
            raise ValueError(f"{source.name} must be 20x16")
        output = OUT / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        manifest.append(
            f"{relative}|{width}|{height}|rgb565|{len(data)}|{digest}"
        )
        written.add(relative)
        total += len(data)
        print(f"{relative}: {width}x{height}, {len(data)} bytes")

    # Keep assets this script does not generate, such as the desk logo.
    if (OUT / "manifest.txt").exists():
        for line in (OUT / "manifest.txt").read_text(encoding="utf-8").splitlines():
            path = line.split("|")[0]
            if "|" in line and path not in written and not path.startswith("#") \
                    and path != "manifest_version":
                manifest.append(line)
                written.add(path)
                parts = line.split("|")
                if len(parts) >= 5 and parts[4].isdigit():
                    total += int(parts[4])
                print(f"kept {path}")

    logo = OUT / "desk" / "spotify_logo.rgb565"
    if logo.exists() and "desk/spotify_logo.rgb565" not in written:
        logo_bytes = logo.read_bytes()
        manifest.append(
            "desk/spotify_logo.rgb565|64|64|rgb565|"
            f"{len(logo_bytes)}|{hashlib.sha256(logo_bytes).hexdigest()}"
        )
        total += len(logo_bytes)

    (OUT / "manifest.txt").write_text("\n".join(manifest) + "\n",
                                      encoding="utf-8")
    print(f"Wrote {OUT / 'manifest.txt'} ({len(written)} assets, {total} bytes)")


if __name__ == "__main__":
    main()
