/*=============================================================================
 * buttons - the board's two physical buttons, for the shell and for apps.
 *
 * Sits beside touch.c as the other source of physical input. Apps never call
 * into it: the shell polls it once per frame and hands the result to the app
 * in `input_t`, exactly as it does with touch.
 *
 * The two buttons are not the same kind of device, which is most of why this
 * module exists at all:
 *
 *   BOOT is a plain GPIO with a pull-up, active low. It is a LEVEL, and it
 *   bounces, so it goes through button_fsm (pure, host-tested) to become
 *   press and release edges. It is also the flashing button, so holding it
 *   through a reset still does what it always did - this only reads it while
 *   an app is running.
 *
 *   PWR is wired to the AXP2101 power-management chip, not to the SoC. There
 *   is no pin to read: the PMU debounces in hardware and latches a finished
 *   "short press" event in an interrupt-status register, which has to be
 *   fetched over the shared I2C bus and then cleared. So it is an EVENT, not
 *   a level, and reporting a `down` state for it would be a fiction.
 *
 * A long press on PWR is handled by the PMU itself and cuts power. That is
 * hardware behaviour and nothing here can override it.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

/* One button's state for the current frame.
 *
 * `pressed` and `released` are edges, true only on the frame the transition
 * happened; `down` is the level. Edges are what UI code almost always wants -
 * using the level to toggle something would flip it every frame it was held.
 *
 * For PWR, `down` and `released` are always false: the PMU reports a completed
 * press and never tells us the button is being held. */
typedef struct {
    bool down;
    bool pressed;
    bool released;
} button_t;

/* Starts the polling task. Safe to call when the PMU is absent - the BOOT
 * button still works and PWR simply never fires. */
void buttons_start(void);

/* Reads and CONSUMES the edges accumulated since the last call. */
void buttons_read(button_t *boot, button_t *power);
