#!/usr/bin/env python3
"""Cut the characters a screen needs out of one of LVGL's built-in fonts, as a new font in C,
optionally with fewer bits per pixel.

LVGL's built-in fonts carry all of printable ASCII plus symbols, with 16 levels of antialiasing.
At 48 px that is 97 KB of flash for a clock that needs twelve characters, and every
antialiasing level is another colour, which breaks the runs the terminal link compresses. This
reads a font LVGL already ships (an uncompressed `--format lvgl` file, as lv_font_conv writes
them; lv_font_conv itself needs Node), keeps only the glyphs asked for, and can requantise them
to 2 or 1 bpp. Kerning is dropped.

    tools/lv_font_subset.py [--bpp N] SOURCE.c NAME CHARS > lib/LvglRenderer/NAME.c

CHARS may hold symbols as \\uXXXX escapes, e.g. "\\uf1eb" for LV_SYMBOL_WIFI. "ASCII" in CHARS
stands for all of printable ASCII.
"""
import argparse
import re
import sys


def parse_bytes(text):
    return [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})\b", text)]


def unpack(data, bpp, count):
    """`count` pixel values from a glyph's bitstream, most significant bits first."""
    out, acc, bits = [], 0, 0
    it = iter(data)
    for _ in range(count):
        while bits < bpp:
            acc = (acc << 8) | next(it)
            bits += 8
        bits -= bpp
        out.append((acc >> bits) & ((1 << bpp) - 1))
    return out


def pack(values, bpp):
    out, acc, bits = [], 0, 0
    for v in values:
        acc = (acc << bpp) | v
        bits += bpp
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xFF)
    if bits:
        out.append((acc << (8 - bits)) & 0xFF)
    return out


def code_points(text):
    """The font's code point -> glyph id, from its cmaps."""
    mapping = {}
    lists = {m.group(1): [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", m.group(2))]
             for m in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", text, re.S)}
    for m in re.finditer(r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
                         r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), \.type = (\w+)", text):
        start, length, gid = int(m.group(1)), int(m.group(2)), int(m.group(3))
        ulist, ofs, kind = m.group(4), m.group(5), m.group(7)
        if ofs != "NULL":
            sys.exit("glyph_id_ofs_list cmaps are not supported")
        if kind.endswith("FORMAT0_TINY"):
            for i in range(length):
                mapping[start + i] = gid + i
        elif kind.endswith("SPARSE_TINY"):
            for i, offset in enumerate(lists[ulist]):
                mapping[start + offset] = gid + i
        else:
            sys.exit("cmap type %s is not supported" % kind)
    return mapping


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bpp", type=int, choices=(1, 2, 4), help="bits per pixel of the new font (default: the source's)")
    ap.add_argument("source")
    ap.add_argument("name")
    ap.add_argument("chars")
    ap.add_argument("--fallback", help="an lv_font_t to fall back on for glyphs this font lacks")
    args = ap.parse_args()
    text = open(args.source).read()

    if re.search(r"\.bitmap_format = (\d+)", text).group(1) != "0":
        sys.exit("only uncompressed fonts are supported")
    bitmap = parse_bytes(re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", text, re.S).group(1))
    dsc_text = re.search(r"glyph_dsc\[\] = \{(.*?)\n\};", text, re.S).group(1)
    glyphs = [dict((k, int(v)) for k, v in re.findall(r"\.(\w+) = (-?\d+)", entry))
              for entry in re.findall(r"\{([^{}]*)\}", dsc_text)]
    metrics = {}
    for key in ("line_height", "base_line", "cap_height", "x_height", "underline_position", "underline_thickness"):
        metrics[key] = int(re.search(r"\.%s = (-?\d+)," % key, text).group(1))
    src_bpp = int(re.search(r"\.bpp = (\d+),", text).group(1))
    bpp = args.bpp or src_bpp
    mapping = code_points(text)

    chars = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), args.chars)
    if "ASCII" in chars:
        chars = chars.replace("ASCII", "") + "".join(chr(c) for c in range(32, 127))
    keep = sorted(set(ord(c) for c in chars))
    missing = [c for c in keep if c not in mapping]
    if missing:
        sys.exit("not in the font: %s" % ", ".join("U+%04X" % c for c in missing))

    out_bitmap = []
    out_dsc = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */"]
    for code in keep:
        g = glyphs[mapping[code]]
        count = g.get("box_w", 0) * g.get("box_h", 0)
        start = g["bitmap_index"]
        data = bitmap[start:start + (count * src_bpp + 7) // 8]
        if bpp != src_bpp:
            top_in, top_out = (1 << src_bpp) - 1, (1 << bpp) - 1
            data = pack([(v * top_out + top_in // 2) // top_in for v in unpack(data, src_bpp, count)], bpp)
        index = len(out_bitmap)
        out_bitmap += data
        label = chr(code) if 32 < code < 127 and chr(code) not in "*/\\" else "U+%04X" % code
        out_dsc.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d} /* %s */" % (
            index, g["adv_w"], g.get("box_w", 0), g.get("box_h", 0), g.get("ofs_x", 0), g.get("ofs_y", 0), label))

    first = keep[0]
    lines = []
    w = lines.append
    w("/* Generated by tools/lv_font_subset.py from %s at %d bpp, %d glyphs. Do not edit. */" % (
        args.source.split("/")[-1], bpp, len(keep)))
    w("#include <lvgl.h>")
    w("")
    w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(out_bitmap), 16):
        w("    " + ", ".join("0x%02x" % b for b in out_bitmap[i:i + 16]) + ",")
    w("};")
    w("")
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    w(",\n".join(out_dsc))
    w("};")
    w("")
    w("static const uint16_t unicode_list[] = {")
    offsets = ["0x%x" % (c - first) for c in keep]
    for i in range(0, len(offsets), 12):
        w("    " + ", ".join(offsets[i:i + 12]) + ",")
    w("};")
    w("")
    w("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    w("    {.range_start = %d, .range_length = %d, .glyph_id_start = 1, .unicode_list = unicode_list," % (first, keep[-1] - first + 1))
    w("     .glyph_id_ofs_list = NULL, .list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}," % len(keep))
    w("};")
    w("")
    w("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    w("    .glyph_bitmap = glyph_bitmap,")
    w("    .glyph_dsc = glyph_dsc,")
    w("    .cmaps = cmaps,")
    w("    .kern_dsc = NULL,")
    w("    .kern_scale = 0,")
    w("    .cmap_num = 1,")
    w("    .bpp = %d," % bpp)
    w("    .kern_classes = 0,")
    w("    .bitmap_format = 0,")
    w("};")
    w("")
    if args.fallback:
        w("extern const lv_font_t %s;" % args.fallback)
        w("")
    w("const lv_font_t %s = {" % args.name)
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    for key in ("line_height", "base_line", "cap_height", "x_height"):
        w("    .%s = %d," % (key, metrics[key]))
    w("    .subpx = LV_FONT_SUBPX_NONE,")
    w("    .underline_position = %d," % metrics["underline_position"])
    w("    .underline_thickness = %d," % metrics["underline_thickness"])
    w("    .static_bitmap = 1,")
    w("    .dsc = &font_dsc,")
    w("    .fallback = %s," % ("&" + args.fallback if args.fallback else "NULL"))
    w("    .user_data = NULL,")
    w("};")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
