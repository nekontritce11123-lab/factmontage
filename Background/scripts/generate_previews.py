#!/usr/bin/env python3
"""Generate small deterministic before/after previews for the six background presets."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "previews"
W, H = 240, 135


def checker():
    image = Image.new("RGB", (W, H), "#d9dde3")
    draw = ImageDraw.Draw(image)
    for y in range(0, H, 12):
        for x in range(0, W, 12):
            if (x // 12 + y // 12) % 2:
                draw.rectangle((x, y, x + 11, y + 11), fill="#f7f8fa")
    return image


def scene(screen):
    image = Image.new("RGB", (W, H), screen)
    draw = ImageDraw.Draw(image)
    draw.ellipse((84, 18, 154, 86), fill="#e7b18f")
    draw.pieslice((82, 13, 156, 78), 180, 360, fill="#50342d")
    draw.rounded_rectangle((67, 72, 171, 148), 24, fill="#e26455")
    draw.ellipse((100, 47, 107, 54), fill="#33373d")
    draw.ellipse((133, 47, 140, 54), fill="#33373d")
    draw.arc((111, 53, 131, 69), 15, 165, fill="#9a4b43", width=2)
    return image


def person_mask(soft=False, inward=False):
    mask = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse((82, 13, 156, 87), fill=255)
    draw.rounded_rectangle((67, 72, 171, 148), 24, fill=255)
    if inward:
        mask = mask.filter(ImageFilter.MinFilter(5))
    return mask.filter(ImageFilter.GaussianBlur(2 if soft else 0))


def composite(foreground, background, soft=False, inward=False):
    return Image.composite(foreground, background, person_mask(soft, inward))


def result(identifier):
    source = scene("#38b650" if identifier != "blue" else "#226bca")
    if identifier in {"green", "blue", "soft_chroma"}:
        background = checker()
        clean = composite(source, background)
        return source, composite(clean, background, True, True) if identifier == "soft_chroma" else clean
    source = scene("#6f89a5")
    if identifier == "person_blur":
        return source, composite(source, source.filter(ImageFilter.GaussianBlur(10)), True)
    if identifier == "person_alpha":
        return source, composite(source, checker(), True)
    return source, composite(source, Image.new("RGB", (W, H), "#1f2430"), True)


def frames(before, after):
    output = []
    for step in list(range(0, 13)) + list(range(12, -1, -1)):
        cut = round(W * step / 12)
        frame = before.copy()
        frame.paste(after.crop((0, 0, cut, H)), (0, 0))
        draw = ImageDraw.Draw(frame)
        if 0 < cut < W:
            draw.line((cut, 0, cut, H), fill="#ffffff", width=2)
        output.append(frame)
    return output


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for identifier in ("green", "blue", "soft_chroma", "person_blur", "person_alpha", "person_color"):
        before, after = result(identifier)
        animation = frames(before, after)
        after.save(OUT / f"background_{identifier}.png", optimize=True)
        animation[0].save(OUT / f"background_{identifier}.gif", save_all=True,
                          append_images=animation[1:], duration=65, loop=0, optimize=True)
    print("Generated 6 Background previews")


if __name__ == "__main__":
    main()
