#!/usr/bin/env python3
"""Build the transition WAVs from separately downloaded CC0 sound recordings.

No network access is used. Source filenames and licenses are documented in
../sfx/README_RU.md. Run with --sources, --ffmpeg and --output.
"""

import argparse
from array import array
from functools import lru_cache
import math
from pathlib import Path
import subprocess
import wave
from zipfile import ZipFile


RATE = 48000
SOURCES = [
    ("freesound_446010.mp3", None),                       # double pullback
    ("freesound_485241.mp3", None),                       # push forward
    ("freesound_60013.mp3", None),                        # swift swipe
    ("opengameart_wind_loop.ogg", None),                  # smooth slide
    ("freesound_728523.mp3", None),                       # defocus
    ("freesound_475896.mp3", None),                       # diagonal reveal
    ("kenney_scifi.zip", "Audio/engineCircular_000.ogg"),# gentle turn
    ("opengameart_water_wave.flac", None),                # organic dissolve
    ("kenney_scifi.zip", "Audio/forceField_000.ogg"),    # circle reveal
    ("opengameart_swishes.zip", "swishes/swish-1.wav"),  # split, left
    ("freesound_648729.mp3", None),                       # tilt and zoom
    ("opengameart_more_sounds.zip", "Cloth/Cloth_05.wav"),# soft curtain
    ("kenney_interface.zip", "Audio/glass_001.ogg"),     # light flash
    ("freesound_703284.mp3", None),                       # luma reveal
    ("freesound_332711.mp3", None),                       # digital shift
]


def make(ffmpeg, source_dir, output_dir, check=False):
    @lru_cache(maxsize=None)
    def decode(pack, member):
        path = source_dir / pack
        if member:
            with ZipFile(path) as archive:
                data = archive.read(member)
        else:
            data = path.read_bytes()
        result = subprocess.run(
            [str(ffmpeg), "-v", "error", "-i", "pipe:0", "-ar", str(RATE),
             "-ac", "2", "-f", "f32le", "pipe:1"], input=data,
            stdout=subprocess.PIPE, check=True,
        )
        samples = array("f")
        samples.frombytes(result.stdout)
        return samples

    def excerpt(pack, member, seconds=.62):
        samples = decode(pack, member)
        count = min(len(samples) // 2, round(seconds * RATE))
        if len(samples) // 2 == count:
            return list(samples)
        # Keep the most active window, rather than a long silent lead-in.
        powers = [samples[i] ** 2 + samples[i + 1] ** 2 for i in range(0, len(samples), 2)]
        prefix = [0.0]
        for power in powers:
            prefix.append(prefix[-1] + power)
        candidates = range(0, len(powers) - count + 1, 1200)
        start = max(candidates, key=lambda i: prefix[i + count] - prefix[i])
        return list(samples[start * 2:(start + count) * 2])

    def pan(samples, first, last):
        frames = len(samples) // 2
        for i in range(frames):
            direction = first + (last - first) * i / max(1, frames - 1)
            samples[2 * i] *= math.sqrt((1 - direction) / 2)
            samples[2 * i + 1] *= math.sqrt((1 + direction) / 2)
        return samples

    def mix(length, *parts):
        mixed = [0.0] * (round(length * RATE) * 2)
        for part, offset, gain in parts:
            begin = round(offset * RATE) * 2
            for i, value in enumerate(part):
                if begin + i < len(mixed):
                    mixed[begin + i] += value * gain
        return mixed

    def wav_bytes(samples):
        frames = len(samples) // 2
        fade = min(round(.012 * RATE), frames // 4)
        for i in range(fade):
            gain = i / max(1, fade)
            for channel in (0, 1):
                samples[2 * i + channel] *= gain
                samples[2 * (frames - i - 1) + channel] *= gain
        rms = math.sqrt(sum(value * value for value in samples) / len(samples))
        if rms < 1e-5:
            raise ValueError("Source excerpt is silent")
        gain = .05 / rms
        for _ in range(8):
            actual = math.sqrt(sum((.4 * math.tanh(value * gain / .4)) ** 2 for value in samples) / len(samples))
            gain *= .05 / actual
        pcm = array("h", (round(.4 * math.tanh(value * gain / .4) * 32767) for value in samples))
        from io import BytesIO
        buffer = BytesIO()
        with wave.open(buffer, "wb") as stream:
            stream.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(pcm.tobytes())
        return buffer.getvalue()

    output_dir.mkdir(exist_ok=True)
    for style, (pack, member) in enumerate(SOURCES):
        samples = excerpt(pack, member)
        if style == 2:
            variants = (("", pan(samples.copy(), -.8, .8)),
                        ("_right", pan(samples.copy(), .8, -.8)))
        else:
            if style == 5:
                samples = pan(samples, -.7, .7)
            elif style == 6:
                samples = pan(samples, -.55, .55)
            elif style == 9:
                other = excerpt("opengameart_swishes.zip", "swishes/swish-8.wav")
                samples = mix(.46, (pan(samples, -.8, -.8), .04, .85),
                              (pan(other, .8, .8), .23, .85))
            elif style == 10:
                impact = excerpt("freesound_475887.mp3", None)
                samples = mix(.62, (samples, 0, .85), (impact, .19, .35))
            variants = (("", samples),)
        for suffix, values in variants:
            target = output_dir / f"transition_{style:02d}{suffix}.wav"
            content = wav_bytes(values)
            if check:
                assert target.read_bytes() == content, f"Stale sound: {target}"
            else:
                target.write_bytes(content)
            print(target.name, len(content), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "sfx")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    make(args.ffmpeg, args.sources, args.output, args.check)
