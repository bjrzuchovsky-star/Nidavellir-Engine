"""Draws the TombStone app icons (Dusk palette) and writes multi-size .ico files.

python3 resources/windows/make_icons.py  (Pillow). Outputs tombstone.ico
(editor, client, server) and tombstone_vault.ico (the Vault server), plus
tombstone_icon.png (256 px preview).
"""
from PIL import Image, ImageDraw, ImageFilter
import os

S = 1024
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
HERE = os.path.dirname(os.path.abspath(__file__))

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

def rounded_mask(size, r):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], r, fill=255)
    return m

def stone_path(d, cx, top, w, base, fill):
    r = w // 2
    d.pieslice([cx - r, top, cx + r, top + w], 180, 360, fill=fill)
    d.rectangle([cx - r, top + r, cx + r, base], fill=fill)

def draw(variant):
    img = Image.new("RGBA", (S, S))
    sky = Image.new("RGBA", (S, S))
    px = sky.load()
    top, mid, low = (52, 30, 62), (196, 84, 52), (242, 168, 82)  # night plum, sunset, amber
    for y in range(S):
        t = y / (S - 1)
        c = lerp(top, mid, t / 0.62) if t < 0.62 else lerp(mid, low, (t - 0.62) / 0.38)
        for x in range(S):
            px[x, y] = c + (255,)
    d = ImageDraw.Draw(sky)
    # Setting sun behind the stone.
    d.ellipse([S * 0.18, S * 0.42, S * 0.82, S * 1.06], fill=(255, 196, 108, 255))
    # Mesa horizon.
    ground = (40, 22, 30, 255)
    d.polygon([(0, S * 0.80), (S * 0.12, S * 0.74), (S * 0.30, S * 0.74), (S * 0.36, S * 0.79),
               (S * 0.70, S * 0.79), (S * 0.76, S * 0.71), (S * 0.92, S * 0.71), (S, S * 0.77),
               (S, S), (0, S)], fill=ground)
    # The tombstone: dark silhouette with a warm rim.
    cx, w, t, b = S // 2, int(S * 0.40), int(S * 0.24), int(S * 0.86)
    stone_path(d, cx, t - 18, w + 36, b + 10, (255, 214, 140, 255))
    stone_path(d, cx, t, w, b, (30, 18, 24, 255))
    # Cross (or a keyhole for the Vault) carved in brass.
    brass = (232, 176, 84, 255)
    if variant == "vault":
        d.ellipse([cx - 58, t + 150, cx + 58, t + 266], fill=brass)
        d.polygon([(cx - 40, t + 240), (cx + 40, t + 240), (cx + 62, t + 420), (cx - 62, t + 420)], fill=brass)
    else:
        d.rectangle([cx - 26, t + 110, cx + 26, t + 430], fill=brass)
        d.rectangle([cx - 110, t + 190, cx + 110, t + 242], fill=brass)
    # Base slab.
    d.rounded_rectangle([cx - w // 2 - 70, b - 10, cx + w // 2 + 70, b + 60], 18, fill=(30, 18, 24, 255))
    img = Image.composite(sky, img, rounded_mask(S, int(S * 0.18)))
    return img

def save(variant, name):
    big = draw(variant)
    frames = [big.resize((s, s), Image.LANCZOS) for s in SIZES]
    frames = [f.filter(ImageFilter.UnsharpMask(1, 60, 2)) if f.width <= 48 else f for f in frames]
    frames[-1].save(os.path.join(HERE, name), format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    return frames[-1]

prev = save("main", "tombstone.ico")
vault = save("vault", "tombstone_vault.ico")
sheet = Image.new("RGBA", (256 * 2 + 48 + 32 + 16 + 5 * 16, 256), (0, 0, 0, 0))
sheet.paste(prev, (0, 0)); sheet.paste(vault, (272, 0))
x = 272 + 272
for s in (48, 32, 16):
    sheet.paste(prev.resize((s, s), Image.LANCZOS), (x, 0)); x += s + 16
sheet.save(os.path.join(HERE, "tombstone_icon.png"))
