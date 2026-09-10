/*=============================================================================
 * ui - shared microui integration, for the shell and for apps.
 *
 * Everything an app needs to draw a UI, and nothing about any particular UI.
 * The launcher is one caller; an app wanting a settings panel or a readout is
 * another, and gets the same touch handling and the same redraw skipping for
 * free.
 *
 * microui is immediate-mode and does no drawing itself: each frame it turns a
 * UI description into a list of rectangles, text and icons, and this module
 * walks that list painting into the shared framebuffer.
 *
 * That command-list model is why microui suits this device. A retained-mode
 * toolkit wants to own the display and the refresh cycle, which fights an app
 * like the cube that owns its own framebuffer. Here we render a list of
 * primitives whenever we like, into whatever we like.
 *
 * WHY THIS MODULE KNOWS ABOUT DIRTY BANDS
 *
 * Immediate mode rebuilds and repaints the whole UI every frame, which
 * normally means clearing the screen every frame, which marks every band dirty
 * and forces a full 9.6 ms transfer - throwing away the saving that partial
 * updates exist to provide. That is not a launcher problem; it would hit any
 * app that drew a UI.
 *
 * The fix is that an immediate-mode UI is only *rebuilt* every frame, not
 * necessarily *changed*. microui's command list is a complete description of
 * the output, so if it hashes the same as last frame the picture is identical
 * and both the repaint and the transfer can be skipped. A static menu then
 * costs nothing at all.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "gfx/gfx_font.h"
#include "gfx/icon.h"
#include "microui.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"

/* Shared metrics, so the shell and any app UI look like one product. */
#define UI_TITLE_HEIGHT   56
#define UI_ROW_HEIGHT     64
#define UI_ROW_GAP        8
#define UI_MARGIN         16

/* ui_slider_int()'s knob width - chunky enough for a finger, not tuned
 * finer than that until Phase 5 puts a screenshot next to the design. */
#define UI_SLIDER_KNOB_W  40

/* The strip across the top of the home screen, reserved and deliberately
 * empty. It is where status belongs - battery, connection, the clock -
 * and holding the space open now means adding any of that later moves
 * nothing below it. A row of the menu was there before; a status bar and
 * a heading that only ever said "APPS" cannot both have the top of a
 * 448px screen. */
#define UI_BANNER_HEIGHT  56

/* Pass as ui_end()'s background to draw without clearing first - for a UI laid
 * over an app's own output rather than replacing it. */
#define UI_NO_BACKGROUND 0xFFFFFFFFu

void ui_init(void);

/* The microui context, for building the UI between ui_begin and ui_end. */
mu_Context *ui_context(void);

/* Start a UI frame: translates touch into the mouse events microui
 * expects, then opens the frame. Also resets the button style to
 * UI_BUTTON_FLAT. Style is part of the frame's description, like
 * everything else in an immediate-mode UI - a caller that wants a style
 * states it every frame. That matters here because the whole shell
 * shares one mu_Context: without the reset, the launcher opting into a
 * bezel would leave the sand app's overlay buttons bezelled too. */
void ui_begin(const input_t *input);

/* Choose how button frames are drawn for the rest of this frame.
 *
 * Call it after ui_begin() and before the buttons it should apply to; it can
 * be changed again mid-frame, so one UI can mix styles. See ui_style.h for
 * what each style is and why only buttons are affected. */
void ui_set_button_style(ui_button_style_t style);

/* Choose how MU_COMMAND_TEXT is drawn for the rest of this frame. Unlike
 * ui_set_button_style(), this is not reset by ui_begin() and is not part
 * of the frame's description in the sense that matters for the repaint
 * hash - see the comment above ui_set_text_style()'s definition in ui.c
 * for why a text style needs its own invalidation and a button style
 * does not. */
void ui_set_text_style(ui_text_style_t style);

/* Choose the font microui measures and draws MU_COMMAND_TEXT with, until
 * called again - ui_init() seeds it with gfx_font_ui(). NULL falls back
 * to gfx_font_ui() rather than being stored. Unlike ui_set_text_style()
 * and ui_set_transform() below, this does NOT need ui_invalidate() - see
 * ui_set_font()'s ui.c comment for why. Equivalent to
 * ui_set_font_scaled(font, GFX_GLYPH_SCALE). */
void ui_set_font(const gfx_font_t *font);

/* Like ui_set_font(), but at `scale` glyph cells instead of the fixed
 * GFX_GLYPH_SCALE - see ui_set_font()'s ui.c comment for why carrying the
 * scale inside the font, rather than a separate render-time setting, is
 * what lets a screen mix two text sizes without paying ui_invalidate()
 * every frame. Clamped to at least 1. */
void ui_set_font_scaled(const gfx_font_t *font, int scale);

/* The width `str` would measure at the CURRENT font and scale - what
 * ui_set_font()/ui_set_font_scaled() last set. For right-aligning a
 * string (e.g. against a caption on the same row) without re-deriving
 * the font role and scale at the call site. */
int ui_measure_text(const char *str);

/* Choose the transform every command is mapped through before it is
 * drawn - see ui_transform.h for what a transform is and why it is
 * fixed point. Identity until called; ui_init() sets it explicitly. Like
 * ui_set_text_style(), unlike ui_set_button_style(), this must call
 * ui_invalidate() when the transform changes: it applies at render time
 * inside draw_command(), so the command list hash_canvas() sees is
 * byte-identical regardless - see ui_set_text_style()'s ui.c comment for
 * the full argument. */

/* ONE TRANSFORM PER RENDERED PASS, NOT A STACK. microui's command list has no
 * way to carry a transform change partway through it, and components/microui/
 * is deliberately unpatched, so there is no per-widget nesting. */
void ui_set_transform(ui_transform_t t);

/* Increments whenever the canvas shape genuinely changes - currently,
 * whenever ui_set_transform() gets a transform differing from the one
 * in force. Starts at 0, set in ui_init(). THIS IS NOT A CALLBACK,
 * NOBODY SUBSCRIBES TO IT: a cheap number, nothing more - a caller reads
 * it, remembers it, compares a fresh read later. No listeners, no event
 * fired - resist a subscriber list; the value comes from being pulled
 * and diffed on a caller's own schedule, same as `palette_drawn_quarter`
 * in app_sand.c. */
uint32_t ui_layout_generation(void);

/* The logical canvas size: the physical viewport (GFX_WIDTH x GFX_HEIGHT)
 * mapped through the inverse of the current transform. Callers building
 * a UI must ask these instead of assuming GFX_WIDTH/GFX_HEIGHT directly
 * - under a quarter-turn transform the two swap, and code that measured
 * against the physical panel would lay out past the edge of the rotated
 * canvas or leave a gap at it. */
int ui_width(void);
int ui_height(void);

/* Opens a full-screen window sized to THIS FRAME's canvas, fixing a
 * microui mismatch: mu_begin_window_ex() only trusts its rect the first
 * time a window (by title) opens - right for a desktop manager
 * remembering a drag, wrong here, where every window must always BE
 * (0, 0, ui_width(), ui_height()), which changes at runtime as the
 * canvas rotates. Uncorrected, a rect pins to the first-open
 * orientation, and a bigger later orientation leaves part of the screen
 * uncleared, showing a previous app's frame. */
int ui_begin_screen(mu_Context *ctx, const char *title, int opt);

/*---------------------------------------------------------------------------
 * Fixed-width content
 *
 * A canvas under a changing transform holds two different kinds of content.
 * FILL content - a banner, a status strip - has no natural width of its own
 * and always spans whatever width the canvas currently is; the launcher's
 * banner is the model for this and needs no helper, since mu_layout_row()
 * with a -1 column already does exactly that.
 *
 * FIXED content - a small number of large tap targets sized to what they
 * need to say - should stay that width and centre in the canvas rather than
 * stretch to fill it. The sand app's boot menu is the model this
 * generalises: it already centres a hand-placed pair of buttons this way.
 *
 * ui_centered_rect() is the shared primitive for the fixed case. Pure
 * geometry, `canvas_w` taken as a parameter rather than read internally via
 * ui_width() - that is what keeps it host-testable without pulling in
 * gfx.h/BSP, the same split ui_bezel_spans() (ui_style.h) and
 * ui_transform_rect() (ui_transform.h) already use. */

/* A rect `w` wide, `h` tall, horizontally centred within a canvas
 * `canvas_w` wide, at vertical position `y`. Does not clamp `w` to
 * `canvas_w`: a `w` that exceeds `canvas_w` yields a negative x, which
 * is a caller bug (a button wider than the screen it is centred on)
 * rather than something to paper over silently here. Clamp at the call
 * site if `w` might ever exceed `canvas_w`. */
static inline mu_Rect ui_centered_rect(int canvas_w, int w, int h, int y)
{
    return (mu_Rect){ (canvas_w - w) / 2, y, w, h };
}

/* Draws a baked icons_<name>.h glyph (icon_t) filling `r`, in `color`, via
 * icon_walk_blocks() - streamed rather than collected, so an icon's run
 * count no longer bounds artwork. `rows` is separate from `icon` because
 * icon_t.offset indexes into its own header's blob, not a self-contained
 * pointer - see gfx/icon.h. */
void ui_draw_icon(mu_Context *ctx, mu_Rect r, const icon_t *icon, const uint8_t *rows, mu_Color color);

/* An integer-valued slider over the next layout row - shaped like
 * mu_slider_ex(), but integer: that one's float/"%.2f" thumb is the wrong
 * shape for a "06 PX" control. Writes through `value`, returns whether it
 * changed this frame. */
bool ui_slider_int(mu_Context *ctx, int *value, int lo, int hi, int step);

/* Close the frame and paint it, but only if it would look any different
 * from what is already on screen. Returns whether it drew. It repaints
 * when any of these is true: the UI itself changed (a hover, a new
 * item, different text); something else has already dirtied the screen,
 * so the UI's pixels are gone - an app drawing underneath an overlay,
 * for instance; or ui_invalidate() was called. `background_rgb` is
 * cleared to first, or UI_NO_BACKGROUND to paint over whatever is
 * already there. */
bool ui_end(uint32_t background_rgb);

/* Declare that the framebuffer no longer holds this UI's output, so the
 * next ui_end() must repaint even if the UI is unchanged. Needed
 * whenever something has replaced the screen without going through gfx
 * in a way ui_end() can detect - in practice, returning to the launcher
 * after an app has been running. Without it the launcher would compare
 * its unchanged command list, skip the repaint, and leave the app's
 * last frame on screen. */
void ui_invalidate(void);
