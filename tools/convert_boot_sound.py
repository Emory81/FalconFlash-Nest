"""Convert a WAV file to normalized mono 8-bit PCM for the ESP32 DAC."""

from __future__ import annotations

import argparse
import math
import struct
import wave
from pathlib import Path


TARGET_RATE = 16_000


def read_mono_samples(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frame_count = wav.getnframes()
        raw = wav.readframes(frame_count)

    if sample_width != 2:
        raise ValueError("Only 16-bit PCM WAV files are supported")

    values = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    mono = [
        sum(values[index + channel] for channel in range(channels)) // channels
        for index in range(0, len(values), channels)
    ]
    return mono, sample_rate


def resample(samples: list[int], source_rate: int) -> list[int]:
    if source_rate == TARGET_RATE:
        return samples

    output_count = (len(samples) * TARGET_RATE) // source_rate
    output: list[int] = []
    for index in range(output_count):
        start = (index * source_rate) // TARGET_RATE
        end = ((index + 1) * source_rate) // TARGET_RATE
        end = max(end, start + 1)
        block = samples[start:min(end, len(samples))]
        output.append(sum(block) // len(block))
    return output


def normalize_to_dac(samples: list[int]) -> list[int]:
    rms = math.sqrt(
        sum(sample * sample for sample in samples) / max(1, len(samples))
    )
    target_rms = 0.25 * 32767.0
    gain = target_rms / max(1.0, rms)

    output: list[int] = []
    for sample in samples:
        # Boost quiet recordings and soft-limit peaks to avoid harsh clipping.
        limited = 32767.0 * math.tanh((sample * gain) / 32767.0)
        output.append(max(0, min(255, round((limited / 32767.0) * 127 + 128))))
    return output


def write_header(path: Path, samples: list[int], prefix: str) -> None:
    prefix_upper = prefix.upper()
    sample_symbol = prefix.split("_")[0] + "".join(
        part.capitalize() for part in prefix.split("_")[1:]
    )
    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        f"constexpr uint32_t {prefix_upper}_SAMPLE_RATE = {TARGET_RATE};",
        f"constexpr size_t {prefix_upper}_SAMPLE_COUNT = {len(samples)};",
        "",
        f"const uint8_t {sample_symbol}Samples[] PROGMEM = {{",
    ]
    for index in range(0, len(samples), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in samples[index:index + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("include/boot_sound_data.h"),
    )
    parser.add_argument("--prefix", default="boot_sound")
    args = parser.parse_args()

    samples, source_rate = read_mono_samples(args.input)
    samples = resample(samples, source_rate)
    samples = normalize_to_dac(samples)
    write_header(args.output, samples, args.prefix)
    print(
        f"Wrote {args.output}: {len(samples)} samples at {TARGET_RATE} Hz "
        f"({len(samples) / TARGET_RATE:.2f} seconds)"
    )


if __name__ == "__main__":
    main()
