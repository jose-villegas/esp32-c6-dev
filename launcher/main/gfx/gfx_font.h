/*=============================================================================
 * gfx_font - a font DESCRIPTOR, so gfx can carry more than one font.
 *
 * Pure, like gfx_color.h: no gfx.h, no BSP, no drivers. That is what lets a
 * font's metrics be computed and tested on a host, the same way a colour
 * table can be built and tested without the panel - see gfx_color.h's own
 * top comment for why that split matters on this project.
 *
 * Two fonts exist: the 8x8 bitmap in font8x8_basic.h, wrapped below as
 * gfx_font_8x8 so it is an ordinary entry in this scheme rather than a
 * special case something else routes around, and an 8bpp coverage atlas
 * with anti-aliasing and real proportional advances - generated from a TTF
 * by tools/gen_font.py, e.g. main/gfx/fonts/font_lmroman_40.h - which is
 * exactly the "second font" this scheme was originally shaped for before
 * one existed: `bpp` was a field rather than an assumption, and advances
 * were per-glyph-capable, specifically so a font like that could slot in
 * as an ordinary gfx_font_t with no change here. See gfx.c's
 * draw_glyph_font() for how a bpp==8 atlas is actually drawn (blended, not
 * masked) and gfx_font.h - this file - stays exactly what it always was:
 * pure metrics, no drawing, so both fonts' widths/advances/heights are
 * computable and testable on a host with neither gfx.h nor a framebuffer.
 *===========================================================================*/
#pragma once

#include <stddef.h>   /* NULL, used by the advance field of a monospace font */
#include <stdint.h>
#include <stdbool.h>

#include "gfx/font8x8_basic.h"

/* Describes one font: where its glyph bitmaps live, how big a cell is, which
 * codepoints it covers, and how far the cursor moves per glyph.
 *
 * `atlas` is const so it lands in flash at zero RAM cost - see gfx_color.h's
 * top comment for the same reasoning applied to a colour table. Glyphs are
 * packed cell by cell, one glyph after another starting at codepoint
 * `first`; within a cell, a 1bpp glyph is `cell_h` bytes, one byte per row,
 * bit 0 (LSB) the LEFTMOST pixel of that row - see gfx_font_8x8 below for
 * how the existing 8x8 font matches this exactly. An 8bpp coverage atlas
 * instead uses `cell_w * cell_h` bytes per glyph, one byte per pixel,
 * row-major, 0 background .. 255 full ink - see tools/gen_font.py's own
 * top comment for how that layout is built from a TTF, and gfx.c's
 * draw_glyph_font() for how it is drawn (blended through
 * gfx_fill_rect_blend(), not masked). */
typedef struct {
    const uint8_t *atlas;      /* glyph bitmaps, cell by cell */
    uint8_t  bpp;              /* 1 = bitmask (gfx_font_8x8); 8 = coverage */
    uint8_t  cell_w, cell_h;   /* one glyph's cell, in atlas pixels */
    uint8_t  first;            /* first codepoint the atlas covers */
    uint16_t count;            /* how many glyphs follow it, from `first` */
    const uint8_t *advance;    /* per-glyph advance in pixels, indexed from
                                 * `first`; NULL means monospace at cell_w */
} gfx_font_t;

/* The existing 8x8 font, as an ordinary gfx_font_t.
 *
 * font8x8_basic covers U+0000-U+007F (128 codepoints) - see its own file
 * comment - one row of bits per byte, bit 0 (LSB) the leftmost pixel. That
 * is exactly the layout `atlas` above promises for a 1bpp font, so the
 * table can be pointed at directly with no repacking. Monospace: `advance`
 * is NULL, so every glyph (in range or not) advances by cell_w - see
 * gfx_font_advance() below, and gfx.c's old draw_glyph_turned(), which
 * advanced by a fixed 8*scale regardless of whether the glyph it just
 * stepped over was even in range.
 *
 * `static const`, not `extern`, for the same reason font8x8_basic itself is
 * now `static const` (see that header's own comment): this header is pure
 * and gets included from more than one translation unit, and internal
 * linkage is what keeps that safe. */
static const gfx_font_t gfx_font_8x8 = {
    .atlas   = (const uint8_t *)font8x8_basic,
    .bpp     = 1,
    .cell_w  = 8,
    .cell_h  = 8,
    .first   = 0,
    .count   = 128,
    .advance = NULL,
};

/* How far the cursor moves for one glyph of `ch`, at `scale`.
 *
 * A codepoint outside [f->first, f->first + f->count) - or any codepoint at
 * all, for a monospace font - falls back to cell_w * scale. That matches the
 * font this module ships today exactly, where gfx_text_turned() advanced by
 * a fixed cell every character regardless of whether that character was in
 * range; see gfx.c. A font WITH an advance table looks up the per-glyph
 * value only when `ch` is in range, and falls back the same way otherwise -
 * a reasonable default for the one case with no real font behind it yet. */
static inline int gfx_font_advance(const gfx_font_t *f, unsigned char ch, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    if (f->advance != NULL && ch >= f->first &&
        (unsigned)(ch - f->first) < f->count) {
        return f->advance[ch - f->first] * scale;
    }
    return f->cell_w * scale;
}

/* Width in pixels of `len` characters of `s`, at `scale`. `len < 0` means
 * NUL-terminated - the same contract gfx_text_width() already has (it does
 * `len = strlen(text)` when passed a negative length; see gfx.c).
 *
 * For a monospace font (advance == NULL) this is len * cell_w * scale and
 * never looks at `s`'s content at all, matching the existing
 * gfx_text_width() exactly - it computes `len * GFX_CHAR_W` without
 * inspecting the string either, so a caller passing a `len` longer than
 * what `s` actually holds is not reading past it today, and must not start
 * to just because this got more general. A font with a real advance table
 * has no such shortcut available and does read each of the `len`
 * characters, which is inherent to a proportional width. */
static inline int gfx_font_text_width(const gfx_font_t *f, const char *s,
                                      int len, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    if (len < 0) {
        len = 0;
        while (s[len] != '\0') {
            len++;
        }
    }
    if (f->advance == NULL) {
        return len * f->cell_w * scale;
    }
    int width = 0;
    for (int i = 0; i < len; i++) {
        width += gfx_font_advance(f, (unsigned char)s[i], scale);
    }
    return width;
}

/* Height in pixels of one line of `f`, at `scale`. */
static inline int gfx_font_height(const gfx_font_t *f, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    return f->cell_h * scale;
}
