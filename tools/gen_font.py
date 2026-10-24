#!/usr/bin/env python3
"""Rasterise a TrueType/OpenType font into the 1-bit "HKF1" format used by
the firmware (components/hkbus_core/font.cc).

Usage:
  python3 tools/gen_font.py FONT.ttc FACE_INDEX PIXEL_SIZE CHARSET.txt OUT.bin
                           [--threshold 120]

The default font is Noto Sans CJK HK Bold (SIL Open Font License 1.1):
  FONT=/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc FACE_INDEX=4

File layout (little endian):
  header  : "HKF1" u16 pixel_size, u16 reserved, u32 glyph_count,
            i16 ascent (pixels above baseline of a full CJK glyph),
            i16 descent (pixels below baseline)
  entries : glyph_count x 16 bytes, sorted by code point
            u32 codepoint, u32 bitmap_offset, u8 width, u8 height,
            i8 x_offset (from pen), i8 y_offset (top, relative to baseline),
            u8 advance, 3 bytes padding
  bitmaps : each glyph row-major, ceil(width/8) bytes per row, MSB first,
            1 = ink
"""
import argparse
import struct

from PIL import Image, ImageDraw, ImageFont


def render_glyph(font, ch, threshold):
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    advance = int(round(font.getlength(ch)))
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return dict(w=0, h=0, xo=0, yo=0, adv=advance, bits=b"")
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    # Trim empty borders left by the bbox estimate.
    cols = [x for x in range(w) if any(px[x, y] >= threshold for y in range(h))]
    rows = [y for y in range(h) if any(px[x, y] >= threshold for x in range(w))]
    if not cols or not rows:
        return dict(w=0, h=0, xo=0, yo=0, adv=advance, bits=b"")
    cx0, cx1, cy0, cy1 = cols[0], cols[-1] + 1, rows[0], rows[-1] + 1
    gw, gh = cx1 - cx0, cy1 - cy0
    stride = (gw + 7) // 8
    out = bytearray(stride * gh)
    for y in range(gh):
        for x in range(gw):
            if px[cx0 + x, cy0 + y] >= threshold:
                out[y * stride + x // 8] |= 0x80 >> (x % 8)
    return dict(w=gw, h=gh, xo=x0 + cx0, yo=y0 + cy0, adv=advance,
                bits=bytes(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("face", type=int)
    ap.add_argument("size", type=int)
    ap.add_argument("charset")
    ap.add_argument("out")
    ap.add_argument("--threshold", type=int, default=120)
    ap.add_argument("--only", help="restrict to these characters (e.g. digits)")
    args = ap.parse_args()

    font = ImageFont.truetype(args.font, args.size, index=args.face)
    with open(args.charset, encoding="utf-8") as f:
        chars = sorted(set(f.read().replace("\n", "")))
    if args.only is not None:
        chars = sorted(set(args.only) | {" "})

    ascent = -font.getbbox("國", anchor="ls")[1]
    descent = font.getbbox("gy", anchor="ls")[3]

    entries, blob = [], bytearray()
    for ch in chars:
        g = render_glyph(font, ch, args.threshold)
        for key, lo, hi in (("w", 0, 255), ("h", 0, 255), ("xo", -128, 127),
                            ("yo", -128, 127), ("adv", 0, 255)):
            if not lo <= g[key] <= hi:
                raise SystemExit(f"glyph {ch!r} {key}={g[key]} out of range")
        entries.append((ord(ch), len(blob), g))
        blob += g["bits"]

    with open(args.out, "wb") as f:
        f.write(struct.pack("<4sHHIhh", b"HKF1", args.size, 0, len(entries),
                            ascent, descent))
        for cp, off, g in entries:
            f.write(struct.pack("<IIBBbbB3x", cp, off, g["w"], g["h"],
                                g["xo"], g["yo"], g["adv"]))
        f.write(blob)
    print(f"{args.out}: {len(entries)} glyphs, {16 + 16 * len(entries) + len(blob)} bytes,"
          f" ascent={ascent} descent={descent}")


if __name__ == "__main__":
    main()
