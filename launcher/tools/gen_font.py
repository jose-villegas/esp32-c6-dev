#!/usr/bin/env python3
"""Generate a gfx_font_t header from a TTF - an 8bpp coverage atlas with real
proportional advances, the "second font" gfx_font.h's own top comment says
the descriptor was shaped for before one existed.

    python tools/gen_font.py <ttf-path> --size <px> --symbol <c_identifier> \\
        [--first 32 --count 95] > main/gfx/fonts/<name>.h

General-purpose, not title-specific: any TTF, any pixel size, any contiguous
codepoint range up to 256 entries (gfx_font_t's `first` is a uint8_t and
draw_glyph_font()/gfx_font_advance() both index by `unsigned char`, so
first + count cannot pass 256). Regenerate the title's own font with:

    python tools/gen_font.py ../design/fonts/Montserrat/Montserrat-Medium.ttf \\
        --size 40 --symbol gfx_font_montserrat_40 \\
        > main/gfx/fonts/font_montserrat_40.h

WHY 8bpp COVERAGE, NOT A SECOND 1bpp BITMAP

gfx_font_8x8 is a hand-authored 1-bit-per-pixel bitmap: fine for a small
fixed-width terminal font, hopeless for antialiasing a real TrueType outline
at any size worth reading. FreeType's own rasterizer already produces
0..255 alpha coverage per pixel - PIL's default TrueType text rendering IS
that rasterizer - so this generator does the simplest possible thing with
it: render each glyph once, keep the 8-bit alpha PIL hands back verbatim as
this atlas's own coverage byte. gfx.c's draw_glyph_font()/_dither() (bpp==8
path) read that coverage straight back as a blend alpha via
gfx_fill_rect_blend()/gfx_fill_rect_dither() - no thresholding, no second
lossy step between what FreeType drew and what lands in the framebuffer.

WHY EVERY GLYPH SHARES ONE CELL, AND ONE BASELINE

gfx_font_t (gfx_font.h) packs an atlas glyph-by-glyph at a FIXED cell_w x
cell_h - proportional spacing lives entirely in the separate `advance`
table, never in the atlas layout, because draw_glyph_font() indexes
`atlas + glyph_index * cell_w * cell_h` and has no way to skip to a
variable-sized entry. So this generator has to answer two questions no
1bpp font ever had to: how wide does the cell need to be to hold the
WIDEST glyph in the requested range (cell_w), and where does every glyph's
baseline sit so a run of mixed ascenders/descenders (`Autana`, say) does
not visibly bounce up and down as it draws (the common baseline).

PIL's font.getbbox(ch) answers both, because it reports every glyph's ink
box in one shared coordinate frame already: (0, 0) is the pen position a
plain draw.text((0, 0), ch, font=font) would use, y increasing downward,
and - critically - that frame's origin is the font's own ascent line, not
each glyph's own top, so two different characters' bbox.top/bbottom are
directly comparable and the vertical GAP between "0" and where a
particular glyph's ink starts/ends already encodes how far above/below
the shared baseline it sits. cell_h is therefore built once from the
font's own ascent+descent metrics (font.getmetrics()), not by unioning
this range's own bboxes - the metrics bound EVERY glyph the font can
render, not just the ones asked for here, which is the standard, robust
way to guarantee headroom for whichever range a future caller picks.
cell_w has no such font-wide constant (width is exactly what varies per
glyph), so it is the tightest bound this generator can compute: the widest
requested glyph's own right edge.

TWO PASSES, NOT ONE

Pass 1 measures every requested glyph's bbox and advance without rendering
anything, so cell_w/cell_h/the left/top padding (below) are known constants
before a single pixel is drawn. Pass 2 renders each glyph into a
freshly-cleared cell-sized canvas at that ONE now-fixed offset and reads
its pixels straight back as the atlas bytes. Drawing before knowing the
final cell size would mean re-drawing everything once the true maximum
was found, or (worse) accepting a canvas sized off a guess and silently
clipping whichever glyph turns out to be the real long pole.

LEFT/TOP PADDING

A glyph's own bbox.left can be genuinely negative - Montserrat's "j" at
40px overshoots its own advance origin by a few pixels on the left (its
hook curls back under the dot) - and top can in principle be negative too
for a tall diacritic outside plain ASCII. Drawing straight at the pen's
own (0, 0) would clip that overshoot clean off the left/top edge of the
canvas, which is a real, silent bug this generator refuses to ship: every
glyph in the requested range is measured FIRST (pass 1), and if any
bbox.left/top is negative the whole cell is shifted right/down by that
amount - uniformly, for every glyph, so relative glyph-to-glyph spacing
(which depends only on the advance table, untouched by this) is
unaffected. The self-check below (validate_bboxes()) re-derives, from the
same pass-1 measurements, that every glyph's ink actually lands inside
[0, cell_w) x [0, cell_h) once that padding is applied - failing loudly
before rendering anything if it does not, rather than shipping a
plausible-looking atlas with a clipped glyph in it.
"""

import argparse
import re
import sys


def die(msg):
    sys.exit("gen_font.py: %s" % msg)


def load_pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        die("Pillow (with FreeType) is required to rasterize a TTF.\n"
            "  pip install Pillow")
    return Image, ImageDraw, ImageFont


# --- pass 1: measure everything before drawing anything --------------------

def measure_glyphs(font, first, count):
    """Per-codepoint (bbox, advance) for [first, first+count), plus the
    range's own extremes - bbox is (l, t, r, b) in font.getbbox()'s shared
    ascent-line-relative frame (None for a glyph with literally no ink,
    e.g. control codepoints a caller asked for by mistake)."""
    glyphs = {}
    min_left = 0
    max_right = 0
    min_top = 0
    max_bottom = 0

    for cp in range(first, first + count):
        ch = chr(cp)
        bbox = font.getbbox(ch)
        advance = font.getlength(ch)
        glyphs[cp] = (bbox, advance)
        if bbox is None:
            continue
        l, t, r, b = bbox
        min_left = min(min_left, l)
        max_right = max(max_right, r)
        min_top = min(min_top, t)
        max_bottom = max(max_bottom, b)

    return glyphs, min_left, max_right, min_top, max_bottom


def validate_bboxes(glyphs, left_pad, top_pad, cell_w, cell_h):
    """Re-derives, from the pass-1 measurements alone, that every glyph's
    ink lands inside [0, cell_w) x [0, cell_h) once shifted by
    (left_pad, top_pad) - see this file's own top comment ("LEFT/TOP
    PADDING") for why this has to be checked before anything is drawn,
    not discovered after by eyeballing a clipped glyph. """
    for cp, (bbox, _advance) in sorted(glyphs.items()):
        if bbox is None:
            continue
        l, t, r, b = bbox
        sl, st, sr, sb = l + left_pad, t + top_pad, r + left_pad, b + top_pad
        if sl < 0 or st < 0 or sr > cell_w or sb > cell_h:
            die("glyph U+%04X (%r) ink box (%d,%d)-(%d,%d) does not fit "
                "the computed %dx%d cell after padding (%d,%d) - the "
                "cell-sizing math above this function is wrong, not this "
                "one glyph" % (cp, chr(cp), sl, st, sr, sb, cell_w, cell_h,
                               left_pad, top_pad))


# --- pass 2: render into the now-fixed cell size ----------------------------

def render_glyph(Image, ImageDraw, font, ch, cell_w, cell_h, left_pad,
                 top_pad):
    """One glyph's cell as cell_w*cell_h coverage bytes, row-major,
    0 = background, 255 = full ink - PIL's own antialiased "L" (8-bit
    grayscale) raster kept verbatim, not thresholded - see this file's own
    top comment ("WHY 8bpp COVERAGE") for why that is the whole point. """
    canvas = Image.new("L", (cell_w, cell_h), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((left_pad, top_pad), ch, font=font, fill=255)
    # tobytes(), not getdata() - mode "L" is one byte per pixel row-major
    # either way, but getdata() is deprecated (Pillow 14) in favour of this.
    return list(canvas.tobytes())


def main():
    ap = argparse.ArgumentParser(
        description="Generate a gfx_font_t header (8bpp coverage atlas, "
                    "proportional advances) from a TTF.")
    ap.add_argument("ttf_path")
    ap.add_argument("--size", type=int, required=True,
                    help="pixel size to rasterize at")
    ap.add_argument("--symbol", required=True,
                    help="C identifier for the emitted gfx_font_t")
    ap.add_argument("--first", type=int, default=32,
                    help="first codepoint covered (default 32, space)")
    ap.add_argument("--count", type=int, default=95,
                    help="how many codepoints from --first (default 95, "
                         "printable ASCII)")
    args = ap.parse_args()

    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", args.symbol):
        die("--symbol %r is not a valid C identifier" % args.symbol)
    if args.size < 1:
        die("--size must be positive")
    if args.first < 0 or args.count < 1 or args.first + args.count > 256:
        die("--first/--count must describe a contiguous range within "
            "0..255 - gfx_font_t.first is a uint8_t and every glyph is "
            "indexed by `unsigned char` (see gfx_font.h)")

    Image, ImageDraw, ImageFont = load_pillow()

    # Windows would otherwise turn every \n on stdout into \r\n - see
    # gen_zeta_curve.py's own identical line for why that matters here.
    sys.stdout.reconfigure(newline="\n")

    try:
        font = ImageFont.truetype(args.ttf_path, args.size)
    except OSError as e:
        die("could not load %r at size %d: %s" %
            (args.ttf_path, args.size, e))

    ascent, descent = font.getmetrics()
    if ascent <= 0 or descent < 0:
        die("font.getmetrics() returned ascent=%d descent=%d, which cannot "
            "build a sane cell" % (ascent, descent))

    glyphs, min_left, max_right, min_top, max_bottom = measure_glyphs(
        font, args.first, args.count)

    if max_right <= 0:
        die("every glyph in [%d, %d) rasterized with zero ink - wrong "
            "range, or the font has no printable glyphs there" %
            (args.first, args.first + args.count))

    # See this file's own top comment ("LEFT/TOP PADDING") and
    # ("WHY EVERY GLYPH SHARES ONE CELL") for the derivation of each of
    # these four numbers.
    left_pad = max(0, -min_left)
    top_pad = max(0, -min_top)
    cell_w = max_right + left_pad
    cell_h = top_pad + max(max_bottom, ascent + descent)

    if cell_w > 255 or cell_h > 255:
        die("computed cell %dx%d does not fit gfx_font_t's uint8_t "
            "cell_w/cell_h - pick a smaller --size" % (cell_w, cell_h))

    validate_bboxes(glyphs, left_pad, top_pad, cell_w, cell_h)

    # advance[] is a uint8_t table (gfx_font_t's own contract, gfx_font.h) -
    # round to nearest rather than truncate, so a fractional advance does
    # not silently narrow every string by a fraction of a pixel per glyph
    # compounding over a long run.
    advances = []
    for cp in range(args.first, args.first + args.count):
        _bbox, advance = glyphs[cp]
        a = round(advance)
        if not (0 <= a <= 255):
            die("glyph U+%04X (%r) advance %.2f does not fit a uint8_t - "
                "pick a smaller --size" % (cp, chr(cp), advance))
        advances.append(a)

    atlas = []
    for cp in range(args.first, args.first + args.count):
        ch = chr(cp)
        px = render_glyph(Image, ImageDraw, font, ch, cell_w, cell_h,
                          left_pad, top_pad)
        if len(px) != cell_w * cell_h:
            die("glyph U+%04X (%r) rendered %d pixels, expected exactly "
                "cell_w*cell_h = %d" % (cp, chr(cp), len(px), cell_w * cell_h))
        for v in px:
            if not (0 <= v <= 255):
                die("glyph U+%04X (%r) produced an out-of-range coverage "
                    "byte %d" % (cp, chr(cp), v))
        atlas.extend(px)

    if len(atlas) != cell_w * cell_h * args.count:
        die("emitted %d atlas bytes, expected exactly cell_w*cell_h*count "
            "= %d" % (len(atlas), cell_w * cell_h * args.count))

    # --- emit -----------------------------------------------------------
    w = sys.stdout.write
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     python tools/gen_font.py %s \\\n" % args.ttf_path)
    w(" *         --size %d --symbol %s --first %d --count %d\n" %
      (args.size, args.symbol, args.first, args.count))
    w(" *\n")
    w(" * An 8bpp coverage atlas (see gfx_font_t in gfx_font.h: bpp==8 means\n")
    w(" * one coverage byte per pixel, 0 background .. 255 full ink) plus a\n")
    w(" * real proportional advance table, rasterized once here from the TTF\n")
    w(" * rather than decoded on device - see tools/gen_font.py's own top\n")
    w(" * comment for the cell-sizing/baseline/padding derivation, and\n")
    w(" * gfx.c's draw_glyph_font()'s bpp==8 path for how this is drawn.\n")
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n")
    w("#include <stdint.h>\n\n")
    w('#include "gfx/gfx_font.h"\n\n')

    atlas_name = "%s_atlas" % args.symbol
    advance_name = "%s_advance" % args.symbol

    w("static const uint8_t %s[%d] = {\n" % (atlas_name, len(atlas)))
    for i in range(0, len(atlas), 20):
        row = atlas[i:i + 20]
        w("    " + ", ".join(str(v) for v in row) + ",\n")
    w("};\n\n")

    w("static const uint8_t %s[%d] = {\n    " % (advance_name, len(advances)))
    w(", ".join(str(v) for v in advances))
    w("\n};\n\n")

    w("static const gfx_font_t %s = {\n" % args.symbol)
    w("    .atlas   = %s,\n" % atlas_name)
    w("    .bpp     = 8,\n")
    w("    .cell_w  = %d,\n" % cell_w)
    w("    .cell_h  = %d,\n" % cell_h)
    w("    .first   = %d,\n" % args.first)
    w("    .count   = %d,\n" % args.count)
    w("    .advance = %s,\n" % advance_name)
    w("};\n")


if __name__ == "__main__":
    main()
