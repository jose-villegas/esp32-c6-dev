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
#include "microui.h"
#include "ui_style.h"
#include "ui_transform.h"

/* Shared metrics, so the shell and any app UI look like one product. */
#define UI_TITLE_HEIGHT   56
#define UI_ROW_HEIGHT     64
#define UI_ROW_GAP        8
#define UI_MARGIN         16

/* The strip across the top of the home screen, reserved and deliberately
 * empty. It is where status belongs - battery, connection, the clock - and
 * holding the space open now means adding any of that later moves nothing
 * below it. A row of the menu was there before; a status bar and a heading
 * that only ever said "APPS" cannot both have the top of a 448px screen. */
#define UI_BANNER_HEIGHT  56

/* Pass as ui_end()'s background to draw without clearing first - for a UI laid
 * over an app's own output rather than replacing it. */
#define UI_NO_BACKGROUND 0xFFFFFFFFu

void ui_init(void);

/* The microui context, for building the UI between ui_begin and ui_end. */
mu_Context *ui_context(void);

/* Start a UI frame: translates touch into the mouse events microui expects,
 * then opens the frame.
 *
 * Also resets the button style to UI_BUTTON_FLAT. Style is part of the frame's
 * description, like everything else in an immediate-mode UI - a caller that
 * wants a style states it every frame, and one that does not is never handed
 * another screen's looks. That matters here because the whole shell shares one
 * mu_Context: without the reset, the launcher opting into a bezel would leave
 * the sand app's overlay buttons bezelled too. */
void ui_begin(const input_t *input);

/* Choose how button frames are drawn for the rest of this frame.
 *
 * Call it after ui_begin() and before the buttons it should apply to; it can
 * be changed again mid-frame, so one UI can mix styles. See ui_style.h for
 * what each style is and why only buttons are affected. */
void ui_set_button_style(ui_button_style_t style);

/* Choose how MU_COMMAND_TEXT is drawn for the rest of this frame.
 *
 * Unlike ui_set_button_style(), this is not reset by ui_begin() and is not
 * part of the frame's description in the sense that matters for the repaint
 * hash - see the comment above ui_set_text_style()'s definition in ui.c for
 * why a text style needs its own invalidation and a button style does not. */
void ui_set_text_style(ui_text_style_t style);

/* Choose the transform every command is mapped through before it is drawn -
 * see ui_transform.h for what a transform is and why it is fixed point.
 * Identity until this is called; ui_init() sets it explicitly so nothing
 * relies on a zeroed struct happening to mean identity.
 *
 * Like ui_set_text_style() and unlike ui_set_button_style(), this has to call
 * ui_invalidate() itself when the transform actually changes, and for the
 * same reason: the transform is applied at render time, inside draw_command(),
 * so the command list microui hands to hash_canvas() is byte-identical
 * whatever the transform is. See the comment above ui_set_text_style()'s
 * definition in ui.c for the full argument; this is that same trap.
 *
 * ONE TRANSFORM PER RENDERED PASS, NOT A STACK
 *
 * microui's command list has no way to carry a transform change partway
 * through it, and components/microui/ is deliberately unpatched (see ui.c's
 * top comment), so there is no per-widget nesting to hook a push/pop onto -
 * whatever is in force here applies to every command from every canvas drawn
 * by the next ui_end(). An app that genuinely wants two differently
 * transformed surfaces gets there by rendering two passes, each with its own
 * ui_begin()/ui_set_transform()/ui_end(), not by nesting calls to this. */
void ui_set_transform(ui_transform_t t);

/* The logical canvas size: the physical viewport (GFX_WIDTH x GFX_HEIGHT)
 * mapped through the inverse of the current transform. Callers building a UI
 * must ask these instead of assuming GFX_WIDTH/GFX_HEIGHT directly - under a
 * quarter-turn transform the two swap, and code that measured against the
 * physical panel would lay out past the edge of the rotated canvas or leave
 * a gap at it. */
int ui_width(void);
int ui_height(void);

/* Close the frame and paint it, but only if it would look any different from
 * what is already on screen. Returns whether it drew.
 *
 * It repaints when any of these is true:
 *   - the UI itself changed (a hover, a new item, different text)
 *   - something else has already dirtied the screen, so the UI's pixels are
 *     gone - an app drawing underneath an overlay, for instance
 *   - ui_invalidate() was called
 *
 * `background_rgb` is cleared to first, or UI_NO_BACKGROUND to paint over
 * whatever is already there. */
bool ui_end(uint32_t background_rgb);

/* Declare that the framebuffer no longer holds this UI's output, so the next
 * ui_end() must repaint even if the UI is unchanged.
 *
 * Needed whenever something has replaced the screen without going through gfx
 * in a way ui_end() can detect - in practice, returning to the launcher after
 * an app has been running. Without it the launcher would compare its unchanged
 * command list, skip the repaint, and leave the app's last frame on screen. */
void ui_invalidate(void);
