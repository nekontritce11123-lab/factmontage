#!/usr/bin/env python3
"""Render all panel previews through a built sunimo_transition module."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT.parent / "Studio" / "previews"
if len(sys.argv) != 2:
    raise SystemExit("Usage: generate_previews.py /path/to/sunimo_transition.so")
MODULE = Path(sys.argv[1]).resolve()


def run(arguments, environment=None):
    result = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def demo(path, left, right, letter):
    width, height = 144, 81
    pixels = bytearray()
    glyphs = {
        "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
        "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    }
    for y in range(height):
        for x in range(width):
            amount = x / (width - 1)
            color = [int(left[channel] + (right[channel] - left[channel]) * amount) for channel in range(3)]
            if 25 <= x < 120 and 16 <= y < 72:
                gx, gy = (x - 47) // 7, (y - 16) // 7
                if 0 <= gx < 5 and 0 <= gy < 7 and glyphs[letter][gy][gx] == "1":
                    color = [246, 248, 252]
            pixels.extend(color)
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def main():
    if not MODULE.is_file():
        raise SystemExit(f"Transition module not found: {MODULE}")
    manifest = json.loads((ROOT / "kdenlive/transition_previews.json").read_text(encoding="utf-8"))
    environment = os.environ.copy()
    environment["FREI0R_PATH"] = str(MODULE.parent)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="studio-transition-previews-") as folder:
        work = Path(folder)
        first, second = work / "a.ppm", work / "b.ppm"
        demo(first, (19, 74, 112), (42, 157, 143), "A")
        demo(second, (111, 45, 95), (238, 126, 52), "B")
        profile = work / "profile"
        profile.write_text("description=Studio transitions\nframe_rate_num=30\nframe_rate_den=1\nwidth=144\nheight=81\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n")
        for item in manifest["items"]:
            style = item["style"]
            video = work / f"transition_{style:02d}.mkv"
            run(["melt", "-profile", str(profile), f"qimage:{first}", "out=44", f"qimage:{second}", "out=44",
                 "-mix", "30", "-mixer", "frei0r.sunimo_transition", "0=0=0;-1=1", f"1={item['code']}",
                 "2=.45", "3=0", "4=.35", "5=.5 .5", "6=0", "-consumer", f"avformat:{video}",
                 "vcodec=ffv1", "pix_fmt=bgra", "an=1", "real_time=-1"], environment)
            gif_path, png_path = OUTPUT / item["gif"], OUTPUT / item["png"]
            run(["ffmpeg", "-y", "-v", "error", "-i", str(video), "-vf",
                 "fps=12,split[a][b];[a]palettegen[p];[b][p]paletteuse", "-loop", "0", str(gif_path)])
            run(["ffmpeg", "-y", "-v", "error", "-ss", "1", "-i", str(video), "-frames:v", "1", str(png_path)])
            item["gif_sha256"] = hashlib.sha256(gif_path.read_bytes()).hexdigest()
            item["png_sha256"] = hashlib.sha256(png_path.read_bytes()).hexdigest()
    (OUTPUT / "transition_previews.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Rendered and hashed 15 Studio transition previews through MLT")


if __name__ == "__main__":
    main()
