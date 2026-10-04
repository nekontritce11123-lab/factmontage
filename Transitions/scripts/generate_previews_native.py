#!/usr/bin/env python3
"""Render missing previews through the native Frei0r module when MLT is unavailable."""
from pathlib import Path
import ctypes as C
import hashlib
import json
import sys

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT.parent / "Studio" / "previews"
WIDTH, HEIGHT = 144, 81


def demo(color, accent, letter):
    image = Image.new("RGBA", (WIDTH, HEIGHT), color + (255,))
    pen = ImageDraw.Draw(image)
    for x in range(WIDTH):
        pen.line((x, 0, x, HEIGHT), fill=tuple(round(color[c] * (1 - x / WIDTH) + accent[c] * x / WIDTH) for c in range(3)) + (255,))
    glyphs = {
        "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
        "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    }
    for row, pixels in enumerate(glyphs[letter]):
        for column, pixel in enumerate(pixels):
            if pixel == "1":
                x, y = 47 + column * 7, 16 + row * 7
                pen.rectangle((x, y, x + 6, y + 6), fill=(246, 248, 252, 255))
    return image


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: generate_previews_native.py /path/to/sunimo_transition.dll")
    plugin = C.CDLL(str(Path(sys.argv[1]).resolve()))
    plugin.f0r_construct.argtypes = [C.c_uint, C.c_uint]
    plugin.f0r_construct.restype = C.c_void_p
    plugin.f0r_destruct.argtypes = [C.c_void_p]
    plugin.f0r_set_param_value.argtypes = [C.c_void_p, C.c_void_p, C.c_int]
    plugin.f0r_update2.argtypes = [C.c_void_p, C.c_double, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
    first = demo((19, 74, 112), (42, 157, 143), "A")
    second = demo((111, 45, 95), (238, 126, 52), "B")
    a = C.create_string_buffer(first.tobytes())
    b = C.create_string_buffer(second.tobytes())
    output = C.create_string_buffer(WIDTH * HEIGHT * 4)
    manifest = json.loads((ROOT / "kdenlive/transition_previews.json").read_text(encoding="utf-8"))
    OUTPUT.mkdir(exist_ok=True)
    for item in manifest["items"]:
        png, gif = OUTPUT / item["png"], OUTPUT / item["gif"]
        if not png.exists() or not gif.exists():
            instance = plugin.f0r_construct(WIDTH, HEIGHT)
            if not instance:
                raise RuntimeError("Cannot create transition renderer")
            style = C.c_double(item["code"])
            strength = C.c_double(.45)
            softness = C.c_double(.35)
            for index, value in ((1, style), (2, strength), (4, softness)):
                plugin.f0r_set_param_value(instance, C.byref(value), index)
            images = []
            for frame in range(25):
                progress = C.c_double(frame / 24)
                plugin.f0r_set_param_value(instance, C.byref(progress), 0)
                plugin.f0r_update2(instance, 0, a, b, None, output)
                images.append(Image.frombytes("RGBA", (WIDTH, HEIGHT), output.raw).convert("RGB"))
            images[12].save(png)
            images[0].save(gif, save_all=True, append_images=images[1:], duration=83, loop=0, optimize=True)
            plugin.f0r_destruct(instance)
        item["png_sha256"] = hashlib.sha256(png.read_bytes()).hexdigest()
        item["gif_sha256"] = hashlib.sha256(gif.read_bytes()).hexdigest()
    (OUTPUT / "transition_previews.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
