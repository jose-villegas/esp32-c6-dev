/*=============================================================================
 * gfx_font_roles - which typeface plays which part, decided once, here.
 *
 * A call site that wants "the font for UI text" should not have to know that
 * currently means &gfx_font_8x8 - see gfx_font.h for what a gfx_font_t is.
 * This file is the one place that answers "which concrete font backs role
 * X", so retyping the UI is a one-line edit here instead of a grep across
 * ui.c, boot_anim.c and whatever else names a font by hand today.
 *
 * RESOLUTION STAYS COMPILE TIME - DO NOT MAKE THIS A RUNTIME SWITCH
 *
 * Each 8bpp coverage atlas is real flash - gfx_font_lmroman_40 alone is
 * 274 KiB (main/gfx/fonts/font_lmroman_40.h) - and the linker only drops an
 * atlas nothing references. Proven, not assumed: boot_anim.c's draw_title()
 * already picks its title font through a comparison on a compile-time
 * constant (BOOT_ANIM_TITLE_FONT, generated from the authored timeline into
 * boot_anim_timeline.h), and the commit that pointed the timeline at the 8x8
 * bitmap made the Computer Modern atlas vanish from launcher.map entirely -
 * zero references, image down from 0x16a200 to 0x137330, about 274 KiB back
 * (see "Bake the bitmap title, and let the title tests follow the chosen
 * font"). A registry that resolved a `gfx_font_role_t` at runtime - one
 * function switching over an enum, or a table of pointers indexed by role -
 * would have to reference every font backing every role from the same
 * translation unit, forcing all of them to link whether or not the build at
 * hand ever selects them. That undoes exactly what draw_title() already
 * gets for free, so it is not what this file does.
 *
 * Each role below is therefore its own `static inline` accessor returning a
 * fixed `&gfx_font_X`, never an enum-plus-switch taking a runtime role value
 * - a caller that writes gfx_font_ui() gets a reference the compiler can see
 * is the ONLY font that call can ever resolve to, the same shape
 * draw_title()'s hand-written ternary already exploits. A Settings screen
 * letting someone swap the UI face live is a legitimate future want, but it
 * would mean this file (or whatever replaced it) referencing every
 * candidate font from one translation unit, paying every candidate's flash
 * cost in every build rather than only the one actually chosen - not
 * something to build until that trade is worth making.
 *
 * THE RULE THAT KEEPS THE ABOVE TRUE: only #include the font header for a
 * typeface actually assigned a role below. Pulling in
 * gfx/fonts/font_lmroman_40.h here "for completeness" would reference it
 * from this translation unit regardless of whether any role uses it,
 * undoing the whole point - see "roles deliberately not defined" below for
 * the one typeface this file stays away from on purpose.
 *
 * ROLES DEFINED HERE
 *
 * UI / body text - gfx_font_ui(). Everything that is not an authored,
 * one-off animation choice draws with this: microui (ui.c), the boot
 * animation's axis labels (boot_anim.c's draw_label()), the POST report,
 * diagnostics. Resolves to gfx_font_8x8 - the exact font every one of those
 * call sites already drew with before this file existed. This is a rename,
 * not a chance to make the UI prettier: a serif reads mushy at UI sizes,
 * and picking a different typeface for it is a product decision nobody has
 * made yet.
 *
 * ROLES DELIBERATELY NOT DEFINED
 *
 * A "label" role was considered and dropped. Every label-sized draw in the
 * tree (draw_label() in boot_anim.c, for the boot animation's axis ticks)
 * uses the SAME typeface as the UI role, just a smaller `scale` argument to
 * gfx_font_width()/gfx_font_height() - scale is a call-site parameter, not
 * a role, and a role that resolved to an identical font would just be a
 * second name for gfx_font_ui() with no typeface behind the distinction.
 *
 * The boot title's typeface is not a role either, even though draw_title()
 * ends up choosing between gfx_font_ui() and gfx_font_lmroman_40. It is an
 * AUTHORED, per-animation knob (title_font in boot_anim_timeline.json,
 * boot_anim_title_font_id_t in boot_anim.h), exposed as a dropdown in
 * tools/boot_anim_editor_server.py, not a system-wide "what plays this
 * part" decision - see BOOT_ANIM_TITLE_FONT's own comment in boot_anim.h
 * for why title_font and title_scale are deliberately paired and kept out
 * of any scheme like this one. draw_title() reaches its 8x8 candidate
 * through gfx_font_ui() rather than naming &gfx_font_8x8 a second time -
 * the same reasoning that already had it call gfx_default_font() before
 * this file existed, that "the shell's idea of its own default is not
 * boot_anim.c's to duplicate" - but gfx_font_lmroman_40 itself is still
 * named directly there, through boot_anim.c's own #include of
 * gfx/fonts/font_lmroman_40.h, precisely so it is NOT referenced from here
 * and stays droppable on a build whose timeline picks the bitmap.
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
