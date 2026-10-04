#!/usr/bin/env python3
"""Check Studio transitions against a stock MLT seam and both playlist orders."""
from contextlib import nullcontext
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def run_melt(command: list[str], video: Path) -> None:
    """Leave replayable evidence on failure; never hide a crashed consumer."""
    video.with_suffix(".command.json").write_text(json.dumps(command), encoding="utf-8")
    with video.with_suffix(".stderr.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=log, timeout=60)
    if result.returncode:
        print(video.with_suffix(".stderr.log").read_text(encoding="utf-8"), flush=True)
        raise subprocess.CalledProcessError(result.returncode, command)


def render(style: int | None, folder: Path) -> list[bytes]:
    video = folder / f"transition-{style}.mkv"
    mixer = "luma" if style is None else "frei0r.sunimo_transition"
    code = 0 if style is None else style / 11 if style < 12 else (2 * (style - 12) + 1) / 22
    run_melt([
        "melt", "-profile", str(folder / "profile"), f"qimage:{folder / 'a.ppm'}", "out=29", f"qimage:{folder / 'b.ppm'}", "out=29", "-mix", "15",
        "-mixer", mixer, "0=0=0;14=1", f"1={code}",
        "2=.45", "3=0", "4=.35", "5=.5 .5", "6=0",
        "-consumer", f"avformat:{video}", "vcodec=ffv1", "an=1", "real_time=-1",
    ], video)
    return decoded(video, style)


def decoded(video: Path, context) -> list[bytes]:
    frames = subprocess.run([
        "ffmpeg", "-v", "error", "-i", str(video), "-vf", "scale=48:27",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1",
    ], check=True, capture_output=True, timeout=60).stdout
    size = 48 * 27 * 3
    assert len(frames) == 45 * size, (context, len(frames))
    return [frames[i:i + size] for i in range(0, len(frames), size)]


def native_pair(style: int, reverse: bool, folder: Path) -> list[bytes]:
    """MLT XML renderer boundary, not a Kdenlive GUI acceptance test."""
    root = ET.Element("mlt", producer="timeline")
    ET.SubElement(root, "profile", description="Studio seam regression", frame_rate_num="25", frame_rate_den="1", width="160", height="90",
                  progressive="1", sample_aspect_num="1", sample_aspect_den="1", display_aspect_num="16", display_aspect_den="9", colorspace="709")
    for name in ("a", "b"):
        producer = ET.SubElement(root, "producer", id=name, **{"in": "0", "out": "29"})
        ET.SubElement(producer, "property", name="mlt_service").text = "qimage"
        ET.SubElement(producer, "property", name="resource").text = str(folder / f"{name}.ppm")
    for index, name in enumerate(("b", "a") if reverse else ("a", "b")):
        playlist = ET.SubElement(root, "playlist", id=f"p{index}")
        if name == "b":
            ET.SubElement(playlist, "blank", length="15")
        ET.SubElement(playlist, "entry", producer=name, **{"in": "0", "out": "29"})
    tractor = ET.SubElement(root, "tractor", id="timeline", **{"in": "0", "out": "44"})
    for index in (0, 1):
        ET.SubElement(tractor, "track", producer=f"p{index}")
    transition = ET.SubElement(tractor, "transition", **{"in": "15", "out": "30"})
    props = {"mlt_service": "frei0r.sunimo_transition", "kdenlive_id": "studio_transition",
             "a_track": int(reverse), "b_track": int(not reverse), "0": "0=0;14=1",
             "1": style / 11 if style < 12 else (2 * (style - 12) + 1) / 22,
             "2": .45, "3": 0, "4": .35, "5": ".5 .5", "6": 0}
    for name, value in props.items():
        ET.SubElement(transition, "property", name=name).text = str(value)
    path = folder / f"native-{style}-{int(reverse)}.mlt"
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)
    video = path.with_suffix(".mkv")
    run_melt(["melt", "-profile", str(folder / "profile"), str(path), "in=0", "out=44", "-consumer",
              f"avformat:{video}", "vcodec=ffv1", "an=1", "real_time=-1"], video)
    return decoded(video, (style, reverse))


def demo(path: Path, red: bool) -> None:
    width, height = 144, 81
    pixels = bytes(channel for y in range(height) for x in range(width)
                   for channel in ((x * 255 // width, y * 255 // height, 32) if red
                                   else (32, x * 255 // width, y * 255 // height)))
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def main() -> None:
    retained = os.environ.get("STUDIO_TRANSITION_TEST_DIR")
    with nullcontext(retained) if retained else tempfile.TemporaryDirectory(prefix="studio-transition-") as temporary:
        folder = Path(temporary)
        folder.mkdir(parents=True, exist_ok=True)
        demo(folder / "a.ppm", True)
        demo(folder / "b.ppm", False)
        # MLT 7.22 clones description with strdup(), even for headless profiles.
        (folder / "profile").write_text("description=Studio seam regression\nframe_rate_num=25\nframe_rate_den=1\nwidth=160\nheight=90\nprogressive=1\n"
                                        "sample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n")
        # A stock mixer is the control for qimage/cache/colour-conversion startup.
        # Compare every untouched frame, including the first, rather than dropping
        # a warm-up frame or assuming the decoder's first frame matches its cache.
        reference = render(None, folder)
        print(f"stock luma control: {len(set(reference[:10]))} distinct first-clip frames", flush=True)
        signatures = set()
        for style in range(15):
            frames = render(style, folder)
            assert frames[:15] == reference[:15], f"style {style}: altered frames before mix"
            assert frames[30:] == reference[30:], f"style {style}: altered frames after mix"
            assert frames[0] != frames[-1], style
            assert any(frame not in (frames[0], frames[-1]) for frame in frames[15:30]), style
            signature = hashlib.sha256(b"".join(frames[15:30])).hexdigest()
            assert signature not in signatures, f"style {style} duplicates another style"
            signatures.add(signature)
            forward = native_pair(style, False, folder)
            reverse = native_pair(style, True, folder)
            assert forward == reverse, f"style {style}: playlist parity changes A/B direction"
            assert forward[0] != forward[-1], style
            assert any(frame not in (forward[0], forward[-1]) for frame in forward[15:30]), style
            print(f"style {style}: stock-control interiors, distinct rendering and both native playlist orders PASS", flush=True)


if __name__ == "__main__":
    main()
