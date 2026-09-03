#!/usr/bin/env python3
"""Generate main/boot/boot_anim_image.h - the photograph the boot animation
crossfades to.

    python tools/gen_boot_anim_image.py ../design/boot/boot.png > main/boot/boot_anim_image.h

WHY THIS IS A TABLE AND NOT A RUNTIME DECODE

There is no PNG decoder in this firmware and no PSRAM to decode into even if
there were - every byte of a decoded 368x448 frame is exactly one
framebuffer's worth (see gfx.h's own top comment), and this board has
exactly one to spare. Converting once, here, and shipping the already-panel-
format result in .rodata costs zero RAM (it is memory-mapped from flash,
like every other generated table in this tree) at the price of about 322
KiB of a 3 MB app partition that is currently under 15% full.

WHY THE ROTATION HAPPENS HERE AND NOT AT DRAW TIME

design/boot/boot.png is 448x368 - exactly boot_anim.h's own
BOOT_ANIM_TITLE_VIEW_W/H, the "landscape" frame the title's own letters are
already laid out in before being turned a quarter into the panel's native
368x448 portrait framebuffer. Doing that same turn here, once, means
boot_anim.c's draw_image() reads this table row-major into a row-major
framebuffer - the fast case for this chip's cache. Rotating per pixel at
draw time instead would mean a transposed read on one side of that copy,
every frame the photo is on screen, which is the thrashing case (see
docs/Notes/Board-and-Memory.md's own note on sequential vs. random flash-
mapped reads).

THE ROTATION ITSELF

Derived from boot_anim.c's title_glyph_origin() - the same mapping the
title's own letters already go through - not reinvented: that function
maps a BOX (a glyph cell) from the "view" frame to the panel; a single
pixel is a 1x1 box, so its own glyph_h correction drops out and what is
left is

    panel_x = PANEL_W - 1 - view_y
    panel_y = view_x

(PANEL_W - 1, not PANEL_W: title_glyph_origin's own GFX_WIDTH - view_y is a
box's ORIGIN, valid because the box's own far edge is what has to stay in
bounds; a single pixel has no such far edge, so the top of view_y's range
(367) has to land on the top of panel_x's own valid range (367), not one
past it). Inverting for this file's own panel-major output loop:

    view_x = panel_y
    view_y = PANEL_W - 1 - panel_x

Cross-checked two more ways before trusting it (see main()'s own
self-validation below, which asserts this outright rather than merely
assuming it): ui_transform.h's ui_transform_quarter_turn(1, w, h) is the
same mapping in its general corner-to-corner form, and
tools/boot_anim_editor.html's own render-preview CSS applies the exact
inverse (a -90deg rotation) with a comment recording that the OTHER sign
was tried first and was visibly wrong by 180 degrees.

HOW A PIXEL IS PACKED

gfx_color_t is RGB565 with the two bytes swapped (see gfx_color.h's own
comment for why - it is what this panel's QSPI controller wants, the
opposite of the chip's native order), not plain RGB565 and not 0xRRGGBB.
gfx_rgb565()/gfx_rgb() below replicate gfx_color.h's GFX_RGB565/GFX_RGB
macros exactly, channel-by-channel rather than via the packed-integer shift
form the C macros use, because that is the form easiest to check against
known GFX_RGB(...) values independently (see the self-check in main()) -
the two are algebraically identical, verified there rather than assumed.
"""

import sys

# --- panel/view geometry - MUST match boot_anim.h and gfx.h ----------------

PANEL_W = 368   # gfx.h GFX_WIDTH
PANEL_H = 448   # gfx.h GFX_HEIGHT
VIEW_W = 448    # boot_anim.h BOOT_ANIM_TITLE_VIEW_W
VIEW_H = 368    # boot_anim.h BOOT_ANIM_TITLE_VIEW_H


def gfx_rgb565(r, g, b):
    """gfx_color.h's GFX_RGB565(0xRRGGBB), from separate 8-bit channels."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def gfx_rgb(r, g, b):
    """gfx_color.h's GFX_RGB(0xRRGGBB) - GFX_RGB565 with the bytes swapped."""
    v = gfx_rgb565(r, g, b)
    return ((v >> 8) | (v << 8)) & 0xFFFF


def panel_index(view_x, view_y):
    """Where a (view_x, view_y) source pixel lands in the panel-major
    output array - see this file's own top comment for the derivation."""
    panel_x = PANEL_W - 1 - view_y
    panel_y = view_x
    return panel_y * PANEL_W + panel_x


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_boot_anim_image.py <path-to-png>")

    try:
        from PIL import Image
    except ImportError:
        sys.exit(
            "gen_boot_anim_image.py: Pillow is required to read the source "
            "PNG.\n  pip install Pillow")

    # Windows would otherwise turn every \n on stdout into \r\n - see
    # gen_zeta_curve.py's own identical line for why that matters here.
    sys.stdout.reconfigure(newline="\n")

    # --- self-check 1: the packing helper, against known values --------
    # Not a round-trip of gfx_rgb() against itself - against the literal
    # constants gfx_color.h's own GFX_RGB(0xRRGGBB) macro produces for
    # black/white/red/green/blue, so a bug in the channel math here (a
    # swapped shift, a wrong mask) fails loudly instead of shipping a
    # plausible-but-wrong table.
    known = {
        (0, 0, 0): 0x0000,
        (255, 255, 255): 0xFFFF,
        (255, 0, 0): 0x00F8,
        (0, 255, 0): 0xE007,
        (0, 0, 255): 0x1F00,
    }
    for (r, g, b), want in known.items():
        got = gfx_rgb(r, g, b)
        if got != want:
            sys.exit("gen_boot_anim_image.py: gfx_rgb(%d,%d,%d) = 0x%04X, "
                      "expected 0x%04X - the RGB565 packing does not match "
                      "gfx_color.h's GFX_RGB()" % (r, g, b, got, want))

    # --- self-check 2: the rotation, against the four corners derived in
    # this file's own top comment ----------------------------------------
    corners = {
        (0, 0): (PANEL_W - 1, 0),
        (VIEW_W - 1, 0): (PANEL_W - 1, VIEW_W - 1),
        (0, VIEW_H - 1): (0, 0),
        (VIEW_W - 1, VIEW_H - 1): (0, VIEW_W - 1),
    }
    for (vx, vy), (want_px, want_py) in corners.items():
        i = panel_index(vx, vy)
        got_px, got_py = i % PANEL_W, i // PANEL_W
        if (got_px, got_py) != (want_px, want_py):
            sys.exit("gen_boot_anim_image.py: view (%d,%d) mapped to panel "
                      "(%d,%d), expected (%d,%d) - the rotation formula in "
                      "this file's own top comment does not match what it "
                      "actually computes" %
                      (vx, vy, got_px, got_py, want_px, want_py))

    im = Image.open(sys.argv[1])
    if im.size != (VIEW_W, VIEW_H):
        sys.exit("gen_boot_anim_image.py: %s is %dx%d, expected exactly "
                  "%dx%d (boot_anim.h's own BOOT_ANIM_TITLE_VIEW_W/H) - "
                  "this generator does not scale or crop, by design (see "
                  "this file's own top comment on why the source is sized "
                  "to need neither)" %
                  (sys.argv[1], im.size[0], im.size[1], VIEW_W, VIEW_H))

    rgba = im.convert("RGBA")
    alpha_min = rgba.getchannel("A").getextrema()[0]
    if alpha_min != 255:
        print("gen_boot_anim_image.py: warning: source has partial "
              "transparency (min alpha %d) - composited over black "
              "(COL_BG) below, which may not be what was intended" %
              alpha_min, file=sys.stderr)

    # Composite over black now, once, rather than shipping alpha and
    # blending it every frame - COL_BG is what boot_anim.c already treats
    # as "nothing here" everywhere else, so this is consistent with that,
    # and today alpha_min == 255 makes it a no-op in practice.
    rgb = Image.new("RGB", rgba.size, (0, 0, 0))
    rgb.paste(rgba, mask=rgba.getchannel("A"))

    # --- the actual conversion, with a bijection check as it goes -------
    # The honest self-check for an index formula: not "does it look right
    # at the corners" (checked above, but a corner-only check misses an
    # off-by-one or a transposition affecting only interior pixels) but
    # "does every one of the PANEL_W*PANEL_H output slots get written
    # exactly once" - filled with None first so a slot silently skipped
    # (not overwritten, just never reached) is caught too.
    out = [None] * (PANEL_W * PANEL_H)
    px = rgb.load()
    for vy in range(VIEW_H):
        for vx in range(VIEW_W):
            i = panel_index(vx, vy)
            if out[i] is not None:
                sys.exit("gen_boot_anim_image.py: panel slot %d written "
                          "twice (view (%d,%d) collides with an earlier "
                          "pixel) - the rotation is not a bijection" %
                          (i, vx, vy))
            r, g, b = px[vx, vy]
            v = gfx_rgb(r, g, b)
            if not (0 <= v <= 0xFFFF):
                sys.exit("gen_boot_anim_image.py: packed pixel 0x%X out of "
                          "range at view (%d,%d)" % (v, vx, vy))
            out[i] = v

    missing = out.count(None)
    if missing:
        sys.exit("gen_boot_anim_image.py: %d of %d panel slots were never "
                  "written - the rotation does not cover the whole panel" %
                  (missing, len(out)))

    w = sys.stdout.write
    w("/*=============================================================================\n")
    w(" * GENERATED FILE - do not edit.\n")
    w(" *\n")
    w(" *     python tools/gen_boot_anim_image.py ../design/boot/boot.png > main/boot/boot_anim_image.h\n")
    w(" *\n")
    w(" * %s - Cerro Autana, the tepui the boot animation's own title is\n" % sys.argv[1])
    w(" * named after - turned a quarter into the panel's native %dx%d\n" % (PANEL_W, PANEL_H))
    w(" * portrait framebuffer and packed into gfx_color.h's byte-swapped\n")
    w(" * RGB565, so drawing it is a blend and nothing else. See\n")
    w(" * tools/gen_boot_anim_image.py for the rotation's derivation and\n")
    w(" * boot_anim.c's draw_image() for what is done with it.\n")
    w(" *===========================================================================*/\n")
    w("#pragma once\n\n#include <stdint.h>\n\n")

    w("#define BOOT_ANIM_IMAGE_W %d\n" % PANEL_W)
    w("#define BOOT_ANIM_IMAGE_H %d\n\n" % PANEL_H)

    w("static const uint16_t boot_anim_image[BOOT_ANIM_IMAGE_W * BOOT_ANIM_IMAGE_H] = {\n")
    for row_start in range(0, len(out), 12):
        row = out[row_start:row_start + 12]
        w("    " + ", ".join("0x%04X" % v for v in row) + ",\n")
    w("};\n")


if __name__ == "__main__":
    main()
