/*=============================================================================
 * gfx_font_roles - which typeface plays which part, decided once, here.
 *
 * A call site wanting "the font for UI text" should not have to know that
 * currently means gfx_font_8x8, so retyping the UI is a one-line edit here
 * rather than a grep across everything that names a font by hand.
 *
 * RESOLUTION STAYS COMPILE TIME - DO NOT MAKE THIS A RUNTIME SWITCH
 *
 * An 8bpp coverage atlas is real flash - gfx_font_lmroman_40 alone is
 * 274 KiB - and the linker only drops one nothing references. Measured, not
 * assumed: pointing the boot timeline at the 8x8 bitmap made that atlas
 * vanish from launcher.map and gave the 274 KiB back. A registry resolving
 * a role at runtime - a switch over an enum, a table indexed by role - would
 * reference every font backing every role from one translation unit, forcing
 * all of them to link whether or not the build selects them.
 *
 * So each role is a static inline accessor returning one fixed font, never
 * an enum plus switch: the compiler can see gfx_font_ui() is the only font
 * that call resolves to. A Settings screen swapping the UI face live is a
 * legitimate future want, and it would cost every candidate's flash in every
 * build - not a trade worth making yet.
 *
 * THE RULE THAT KEEPS THAT TRUE: only #include the font header for a
 * typeface actually assigned a role below. Adding one "for completeness"
 * references it from this translation unit and undoes the whole point.
 *
 * ROLES DEFINED HERE
 *
 * UI / body text - gfx_font_ui(), resolving to gfx_font_8x8: microui, the
 * boot animation's axis labels, the POST report, diagnostics. The exact font
 * all of those already drew with, so this is a rename rather than a chance
 * to make the UI prettier.
 *
 * ROLES DELIBERATELY NOT DEFINED
 *
 * A "label" role: label-sized draws use the UI typeface at a smaller scale,
 * and scale is a call-site argument, not a role - it would be a second name
 * for gfx_font_ui() with no typeface behind the distinction.
 *
 * The boot title's typeface: an authored, per-animation knob (title_font in
 * the timeline, a dropdown in the editor), not a system-wide decision - see
 * BOOT_ANIM_TITLE_FONT's own comment in boot_anim.h. draw_title() reaches
 * its 8x8 candidate through gfx_font_ui(), but names gfx_font_lmroman_40
 * directly through its own include, precisely so this file never references
 * it and it stays droppable.
 *===========================================================================*/
#pragma once

#include "gfx/gfx_font.h"

/* The UI/body-text role - see this file's top comment for what draws with
 * it and why no second role exists yet. `static inline` rather than an
 * exported symbol, so a call to gfx_font_ui() compiles down to exactly what
 * naming `&gfx_font_8x8` by hand would have - the role's NAME appears in the
 * source, but nothing about how the reference resolves at compile time
 * changes from naming the font directly. */
static inline const gfx_font_t *gfx_font_ui(void)
{
    return &gfx_font_8x8;
}
