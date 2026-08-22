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

/* Shared metrics, so the shell and any app UI look like one product. */
#define UI_TITLE_HEIGHT   56
#define UI_ROW_HEIGHT     64
#define UI_ROW_GAP        8
#define UI_MARGIN         16

/* Pass as ui_end()'s background to draw without clearing first - for a UI laid
 * over an app's own output rather than replacing it. */
#define UI_NO_BACKGROUND 0xFFFFFFFFu

void ui_init(void);

/* The microui context, for building the UI between ui_begin and ui_end. */
mu_Context *ui_context(void);

/* Start a UI frame: translates touch into the mouse events microui expects,
 * then opens the frame. */
void ui_begin(const input_t *input);

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
