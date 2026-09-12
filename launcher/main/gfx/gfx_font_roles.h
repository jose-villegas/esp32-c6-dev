/*
 * gfx_font_roles - which typeface plays which part, decided once, here, so
 * retyping the UI is an edit here rather than a grep.
 *
 * RESOLUTION STAYS COMPILE TIME. An 8bpp atlas is real flash - lmroman_40
 * alone is 274 KiB - and the linker only drops one nothing references;
 * measured, by pointing the boot timeline at the bitmap font and watching
 * that atlas leave launcher.map. A role resolved at runtime would reference
 * every candidate from one translation unit and link them all. Hence a
 * static inline accessor per role, and: only #include a font header for a
 * typeface that has a role below.
 *
 * No "label" role - label draws are the UI face at a smaller scale, and
 * scale is a call-site argument. The boot title is an authored per-animation
 * knob, not a role; boot_anim.c names lmroman_40 through its own include so
 * this file never references it.
 */
#pragma once

#include "gfx/gfx_font.h"

/* The UI/body-text role - see this file's top comment for what draws with
 * it and why no second role exists yet. `static inline` rather than an
 * exported symbol, so a call to gfx_font_ui() compiles down to exactly what
 * naming `&gfx_font_8x8` by hand would have - the role's NAME appears in the
 * source, but nothing about how the reference resolves at compile time
 * changes from naming the font directly. */
static inline const gfx_font_t*
gfx_font_ui(void) {
    return &gfx_font_8x8;
}
