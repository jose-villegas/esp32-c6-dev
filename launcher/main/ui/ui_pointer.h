/*
 * ui_pointer - input_t to a short list of pointer events, held not tapped.
 *
 * Pure logic, no microui calls and no gfx: what makes the touch-to-mouse
 * bridge host-testable at all - ui.c's own feed_input() has no host coverage
 * today because suite_ui.c is device-only. Same split input/touch_fsm.c and
 * input/button_fsm.c already use.
 *
 * Coordinates in and out are PHYSICAL (screen) coordinates. Mapping each
 * point through ui.c's to_logical() is that caller's job, not this module's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

typedef enum {
    UI_POINTER_MOVE,
    UI_POINTER_DOWN,
    UI_POINTER_UP,
} ui_pointer_kind_t;

typedef struct {
    ui_pointer_kind_t kind;
    int x, y;
} ui_pointer_event_t;

/* One press-to-release cycle never needs more than a move, a down and an
 * up, even when a tap resolves before the natural hover-then-press cadence
 * can play out - see ui_pointer_step()'s own comment. */
#define UI_POINTER_MAX_EVENTS 3

/* MOVE-only frames a press waits through before its DOWN is fed, and both
 * are load-bearing. microui resolves a control's hover only when the mouse
 * is over it AND not already down, and mu_mouse_over() needs hover_root,
 * which mu_begin() copies from the PREVIOUS frame. So frame one only tells
 * microui which window the finger is in, frame two is the first that can
 * actually mark the control hovered, and a DOWN before that lands with
 * nothing hovered - which means nothing focused, and a button that draws
 * its pressed state but never submits. */
#define UI_POINTER_HOVER_FRAMES 2

typedef struct {
    /* 0 when no press is being staged, else how many hover frames have gone
     * out so far - the DOWN follows the UI_POINTER_HOVER_FRAMES'th. */
    uint8_t press_stage;
    int press_x, press_y;
    bool down; /* a DOWN went out with no matching UP yet */
} ui_pointer_t;

/* Feed one frame's input_t; get back 0-UI_POINTER_MAX_EVENTS events in `out`,
 * in playback order. Returns the count written, or 0 if `max` can't hold the
 * largest possible result - ui_bezel_spans()'s all-or-nothing rule, so a
 * partial count never lets a caller read past a too-small array. */
int ui_pointer_step(ui_pointer_t *p, const input_t *input, ui_pointer_event_t *out, int max);
