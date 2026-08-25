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
 * By default the PMU also powers itself off on a long PWR hold - REG 0x27
 * (irqlevel/offlevel/onlevel) sets that at 4-10 s, independently of the
 * 1-2.5 s long-press *interrupt* in the same register, and REG 0x22 bit 1
 * (btn_pwroff_en) is what turns the power-off behaviour on at all. Firmware
 * could clear that bit and take the long press for itself; today it does
 * neither, and only the short-press interrupt is enabled (see buttons.c).
 *===========================================================================*/
#pragma once

#include <stdbool.h>

/* One button's state for the current frame.
 *
 * `pressed` and `released` are edges, true only on the frame the transition
 * happened; `down` is the level. Edges are what UI code almost always wants -
 * using the level to toggle something would flip it every frame it was held.
 *
 * For PWR, `down` and `released` are always false: only the short-press
 * interrupt is enabled (see buttons.c), so a completed press is all that
 * arrives today. The PMU also has rising- and falling-edge interrupts (REG
 * 0x41/0x49 bits 0 and 1) that would make a `down` level reconstructible if
 * something ever needed one - it is a firmware choice not to, not a hardware
 * limit. */
typedef struct {
    bool down;
    bool pressed;
    bool released;

    /* Fires exactly once when the button has been held past BUTTON_HOLD_US -
     * see button_fsm.h for the full contract, including that a hold consumes
     * the release edge above so the two never fire for the same press.
     *
     * BOOT-only: always false for PWR. Not because the PMU can't tell - it is
     * the same "not enabled" situation as `down` and `released` above - but
     * because nothing enables the long-press interrupt (REG 0x41/0x49 bit 2)
     * to drive it from. */
    bool held;
} button_t;

/* Starts the polling task. Safe to call when the PMU is absent - the BOOT
 * button still works and PWR simply never fires. */
void buttons_start(void);

/* Reads and CONSUMES the edges accumulated since the last call. */
void buttons_read(button_t *boot, button_t *power);
