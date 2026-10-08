#!/usr/bin/env python3
"""Convert a BDF bitmap font into a C header for lib/gfx.

The header defines static data and a `static const struct gfx_font font_<name>`.
It contains no Zephyr dependencies, so the emulator's host-side code can include
it as well.

Glyphs are stored as 1 bpp bitmaps with the full cell height, row-major,
(width + 7) / 8 bytes per row, most significant bit = leftmost pixel.

Usage:
  bdf2c.py 5x7.bdf font_text.h --name text --prop --space 3
"""

import argparse
import sys

# Codepoints included when present in the font. ASCII, Latin-1 and a few
# typographic characters that show up in German and English chat text.
DEFAULT_CHARSET = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x100))
    + [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
       0x2022, 0x2026, 0x20AC, 0x2190, 0x2191, 0x2192, 0x2193]
)


def parse_bdf(path):
    font = {"glyphs": {}}
    glyph = None
    in_bitmap = False
    with open(path, encoding="latin-1") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            key = parts[0]
            if in_bitmap:
                if key == "ENDCHAR":
                    in_bitmap = False
                    if glyph["encoding"] >= 0:
                        font["glyphs"][glyph["encoding"]] = glyph
                    glyph = None
                else:
                    glyph["rows"].append(int(key, 16))
                continue
            if key == "FONT_ASCENT":
                font["ascent"] = int(parts[1])
            elif key == "FONT_DESCENT":
                font["descent"] = int(parts[1])
            elif key == "FONTBOUNDINGBOX":
                font["bbx"] = [int(p) for p in parts[1:5]]
            elif key == "STARTCHAR":
                glyph = {"rows": [], "encoding": -1}
            elif key == "ENCODING":
                glyph["encoding"] = int(parts[1])
            elif key == "DWIDTH":
                glyph["dwidth"] = int(parts[1])
            elif key == "BBX":
                glyph["bbx"] = [int(p) for p in parts[1:5]]
            elif key == "BITMAP":
                in_bitmap = True
    return font


def render_cell(font, glyph):
    """Return the glyph as a list of rows, each a list of 0/1, in the full cell."""
    asc, desc = font["ascent"], font["descent"]
    height = asc + desc
    w, h, xoff, yoff = glyph["bbx"]
    cell_w = max(glyph.get("dwidth", w), xoff + w)
    cell = [[0] * cell_w for _ in range(height)]
    nbytes = (w + 7) // 8
    top = asc - (yoff + h)
    for r, bits in enumerate(glyph["rows"]):
        y = top + r
        if not 0 <= y < height:
            continue
        for c in range(w):
            if bits & (1 << (nbytes * 8 - 1 - c)):
                x = xoff + c
                if 0 <= x < cell_w:
                    cell[y][x] = 1
    return cell


def trim(cell, space):
    """Proportional width: strip empty columns, add one column of spacing."""
    used = [x for x in range(len(cell[0])) if any(row[x] for row in cell)]
    if not used:
        return [[0] * space for _ in cell]
    lo, hi = used[0], used[-1]
    return [row[lo:hi + 1] + [0] for row in cell]


def pack(cell):
    out = []
    w = len(cell[0])
    for row in cell:
        for b in range((w + 7) // 8):
            v = 0
            for i in range(8):
                x = b * 8 + i
                if x < w and row[x]:
                    v |= 0x80 >> i
            out.append(v)
    return out


def ranges_of(codes):
    ranges = []
    for c in codes:
        if ranges and c == ranges[-1][0] + ranges[-1][1]:
            ranges[-1][1] += 1
        else:
            ranges.append([c, 1])
    return ranges


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bdf")
    ap.add_argument("out")
    ap.add_argument("--name", required=True, help="C identifier suffix")
    ap.add_argument("--prop", action="store_true", help="proportional widths")
    ap.add_argument("--space", type=int, default=3, help="space width with --prop")
    ap.add_argument("--chars", help="explicit character list instead of the default set")
    args = ap.parse_args()

    font = parse_bdf(args.bdf)
    wanted = [ord(c) for c in args.chars] if args.chars else DEFAULT_CHARSET
    codes = sorted(c for c in set(wanted) if c in font["glyphs"])
    if 0x3F not in codes:
        sys.exit("font has no '?' glyph for the fallback")

    widths, offsets, bitmap = [], [], []
    for c in codes:
        cell = render_cell(font, font["glyphs"][c])
        if args.prop:
            cell = [[0] * args.space for _ in cell] if c in (0x20, 0xA0) else trim(cell, args.space)
        widths.append(len(cell[0]))
        offsets.append(len(bitmap))
        bitmap.extend(pack(cell))

    ranges = ranges_of(codes)
    n = args.name
    height = font["ascent"] + font["descent"]
    o = []
    o.append(f"/* Generated by tools/fontconv/bdf2c.py from {args.bdf.split('/')[-1]}. Do not edit. */")
    o.append("/* Source font: X11 misc-misc, public domain. */")
    o.append("#pragma once\n")
    o.append('#include "gfx/font.h"\n')
    o.append(f"static const struct gfx_font_range font_{n}_ranges[] = {{")
    base = 0
    for first, count in ranges:
        o.append(f"\t{{0x{first:04X}, {count}, {base}}},")
        base += count
    o.append("};\n")
    o.append(f"static const uint8_t font_{n}_widths[] = {{")
    for i in range(0, len(widths), 16):
        o.append("\t" + ", ".join(str(w) for w in widths[i:i + 16]) + ",")
    o.append("};\n")
    o.append(f"static const uint16_t font_{n}_offsets[] = {{")
    for i in range(0, len(offsets), 12):
        o.append("\t" + ", ".join(str(v) for v in offsets[i:i + 12]) + ",")
    o.append("};\n")
    o.append(f"static const uint8_t font_{n}_bitmap[] = {{")
    for i in range(0, len(bitmap), 16):
        o.append("\t" + ", ".join(f"0x{v:02X}" for v in bitmap[i:i + 16]) + ",")
    o.append("};\n")
    o.append(f"static const struct gfx_font font_{n} = {{")
    o.append(f"\t.height = {height},")
    o.append(f"\t.ascent = {font['ascent']},")
    o.append(f"\t.num_ranges = {len(ranges)},")
    o.append(f"\t.fallback = {codes.index(0x3F)},")
    o.append(f"\t.ranges = font_{n}_ranges,")
    o.append(f"\t.widths = font_{n}_widths,")
    o.append(f"\t.offsets = font_{n}_offsets,")
    o.append(f"\t.bitmap = font_{n}_bitmap,")
    o.append("};")
    with open(args.out, "w") as f:
        f.write("\n".join(o) + "\n")
    print(f"{args.out}: {len(codes)} glyphs, {len(bitmap)} bitmap bytes")


if __name__ == "__main__":
    main()
