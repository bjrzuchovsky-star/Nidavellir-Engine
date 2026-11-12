"""Draw the Nidavellir hammer-and-anvil icon and write multi-size .ico files.

python3 resources/windows/make_icons.py

No Pillow: PNGs are packed with the standard library (zlib) and stored in
the ICO. Forge colours — charcoal ground, sand anvil, molten-orange hammer.
nidavellir.ico is the app / installer icon. tombstone.ico is the same file
for older paths. tombstone_vault.ico uses the same mark.
"""
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
HERE = os.path.dirname(os.path.abspath(__file__))

CHAR = (16, 17, 19, 255)
SAND = (180, 176, 168, 255)
AMBER = (232, 121, 43, 255)
COPPER = (176, 84, 36, 255)


def png(w, h, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def ico(images):
    n = len(images)
    hdr = struct.pack("<HHH", 0, 1, n)
    off = 6 + 16 * n
    entries = b""
    blobs = b""
    for size, blob in images:
        s = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
        blobs += blob
    return hdr + entries + blobs


class Canvas:
    def __init__(self, n):
        self.n = n
        self.px = bytearray(n * n * 4)

    def put(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            i = (y * self.n + x) * 4
            self.px[i:i + 4] = bytes(c)

    def rect(self, x0, y0, x1, y1, c):
        xa, xb = int(round(min(x0, x1))), int(round(max(x0, x1)))
        ya, yb = int(round(min(y0, y1))), int(round(max(y0, y1)))
        for y in range(ya, yb):
            for x in range(xa, xb):
                self.put(x, y, c)

    def tri(self, ax, ay, bx, by, cx, cy, c):
        pts = [(ax, ay), (bx, by), (cx, cy)]
        ys = [p[1] for p in pts]
        y0, y1 = int(min(ys)), int(max(ys)) + 1
        for y in range(y0, y1):
            xs = []
            for i in range(3):
                xA, yA = pts[i]
                xB, yB = pts[(i + 1) % 3]
                if (yA <= y < yB) or (yB <= y < yA):
                    t = (y - yA) / (yB - yA)
                    xs.append(xA + t * (xB - xA))
            if len(xs) >= 2:
                a, b = int(min(xs)), int(max(xs)) + 1
                for x in range(a, b):
                    self.put(x, y, c)

    def disc(self, cx, cy, r, c):
        r2 = r * r
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r2:
                    self.put(x, y, c)

    def line(self, x0, y0, x1, y1, thick, c):
        steps = max(int(abs(x1 - x0)), int(abs(y1 - y0)), 1)
        r = max(0.6, thick * 0.5)
        for i in range(steps + 1):
            t = i / steps
            self.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r, c)


def draw(n):
    c = Canvas(n)
    # Rounded charcoal plate.
    rad = max(1.0, n * 0.18)
    for y in range(n):
        for x in range(n):
            # Inside the rounded rect?
            def inset(v):
                if v < rad:
                    return rad - v
                if v > n - 1 - rad:
                    return v - (n - 1 - rad)
                return 0.0
            dx, dy = inset(x), inset(y)
            if dx * dx + dy * dy <= rad * rad:
                c.put(x, y, CHAR)
    s = float(n)
    # Anvil in the lower half.
    face_y = s * 0.52
    face_h = max(2.0, s * 0.10)
    face_l = s * 0.22
    face_r = s * 0.72
    c.rect(face_l, face_y, face_r, face_y + face_h, SAND)
    c.tri(face_l, face_y, face_l, face_y + face_h, s * 0.08, face_y + face_h * 0.55, SAND)
    c.tri(face_r, face_y, face_r, face_y + face_h, s * 0.86, face_y + face_h * 0.4, SAND)
    waist_top = face_y + face_h
    waist_bot = s * 0.78
    c.rect(s * 0.40, waist_top, s * 0.58, waist_bot, SAND)
    c.rect(s * 0.18, waist_bot, s * 0.82, s * 0.90, SAND)
    # Hammer.
    head_w = s * 0.34
    head_h = max(2.0, s * 0.12)
    head_cx = s * 0.50
    head_bot = face_y - s * 0.04
    c.rect(head_cx - head_w * 0.5, head_bot - head_h, head_cx + head_w * 0.5, head_bot, AMBER)
    c.line(head_cx + head_w * 0.15, head_bot - head_h * 0.45, s * 0.78, s * 0.14, max(1.5, s * 0.045), COPPER)
    return c.px


def main():
    frames = [(s, png(s, s, draw(s))) for s in SIZES]
    blob = ico(frames)
    for name in ("nidavellir.ico", "tombstone.ico", "tombstone_vault.ico"):
        with open(os.path.join(HERE, name), "wb") as f:
            f.write(blob)
    # 256 preview, plus a couple of small sizes beside it.
    preview = draw(256)
    sheet_w = 256 + 16 + 48 + 16 + 32 + 16 + 16
    sheet = bytearray(sheet_w * 256 * 4)
    def blit(src_n, src, dx, dy):
        for y in range(src_n):
            if dy + y >= 256:
                break
            for x in range(src_n):
                if dx + x >= sheet_w:
                    break
                si = (y * src_n + x) * 4
                di = ((dy + y) * sheet_w + dx + x) * 4
                sheet[di:di + 4] = src[si:si + 4]
    blit(256, preview, 0, 0)
    x = 256 + 16
    for s in (48, 32, 16):
        blit(s, draw(s), x, 0)
        x += s + 16
    with open(os.path.join(HERE, "tombstone_icon.png"), "wb") as f:
        f.write(png(sheet_w, 256, sheet))
    print("wrote nidavellir.ico (%d bytes)" % len(blob))


if __name__ == "__main__":
    main()
