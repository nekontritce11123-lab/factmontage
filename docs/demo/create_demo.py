#!/usr/bin/env python3
"""Generate public procedural footage. Python stdlib + ffmpeg, no downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def ppm(path, kind, width=1920, height=1080):
    """Own geometry, not a photograph or an edited reference image."""
    with path.open("wb") as stream:
        stream.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        for y in range(height):
            row = bytearray()
            for x in range(width):
                if kind == "key":
                    rgb = (0, 255, 0)
                    if (x - 960) ** 2 + (y - 510) ** 2 < 240 ** 2:
                        rgb = (247, 155, 50)
                    if 770 < x < 1150 and 720 < y < 860:
                        rgb = (242, 243, 239)
                else:
                    u, v = x / width, y / height
                    rgb = ((int(24 + 34 * u), int(45 + 45 * v), int(95 + 65 * u)) if kind == "blue"
                           else (int(86 + 80 * u), int(40 + 36 * v), int(75 + 52 * v)))
                    if (x - 1390) ** 2 + (y - 400) ** 2 < 240 ** 2:
                        rgb = (73, 180, 222) if kind == "blue" else (246, 177, 72)
                    if 220 < x < 960 and 340 < y < 660:
                        rgb = (220, 229, 239)
                    if 270 < x < 890 and 405 < y < 465:
                        rgb = (34, 62, 102)
                    if 270 < x < 730 and 520 < y < 555:
                        rgb = (76, 116, 158)
                    if y > 850 and x % 200 < 110:
                        rgb = (45, 86, 130)
                row.extend(rgb)
            stream.write(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New external demo directory")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        parser.error("ffmpeg is required; install/prepare it explicitly before this command")
    root = args.output.resolve()
    if root.exists() and any(root.iterdir()):
        parser.error("output must be empty; existing project or evidence will not be overwritten")
    assets = root / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    provenance = {"license": "CC0-1.0", "origin": "Original procedural geometry and synthesized tone",
                  "fps": 60, "width": 1920, "height": 1080, "files": [], "commands": [],
                  "ffmpeg_version": subprocess.check_output([ffmpeg, "-version"], text=True).splitlines()[0]}
    def run(arguments):
        command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", *arguments]
        provenance["commands"].append(command)
        subprocess.run(command, check=True, timeout=180)
    for kind in ("warm", "blue", "key"):
        image = assets / f"{kind}.ppm"
        ppm(image, kind)
        motion = ("zoompan=z=1:d=360:s=1920x1080:fps=60" if kind == "key" else
                  "zoompan=z='1.04+0.02*sin(on/60)':x='iw/2-iw/zoom/2':y='ih/2-ih/zoom/2':d=360:s=1920x1080:fps=60")
        run(["-i", str(image), "-vf", motion, "-frames:v", "360", "-an", "-c:v", "libx264",
             "-preset", "ultrafast", "-crf", "18", "-threads", "2", "-pix_fmt", "yuv420p",
             "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
             str(assets / f"{kind}.mp4")])
    run(["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=6",
         "-af", "volume=0.25", "-c:a", "pcm_s16le", str(assets / "tone.wav")])
    for path in sorted(assets.iterdir()):
        provenance["files"].append({"path": str(path.relative_to(root)), "bytes": path.stat().st_size,
                                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    (root / "generated-sources.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(root)


if __name__ == "__main__":
    main()
