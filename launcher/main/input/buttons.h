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
 * enables the long-press interrupt (see buttons.c) but does not touch
 * btn_pwroff_en, so both fire from the same hold: this module reports it as
 * `held` well before the PMU's own, longer power-off threshold is ever
 * reached (1-2.5 s long-press vs 4-10 s power-off - see the levels
 * pmu_init() logs at boot for what this board actually has both set to). */
#pragma once

#include <stdbool.h>

/* One button's state for the current frame.
 *
 * `pressed` and `released` are edges, true only on the frame the transition
 * happened; `down` is the level. Edges are what UI code almost always wants -
 * using the level to toggle something would flip it every frame it was held.
 *
 * For PWR, `down` and `released` are always false: the PMU's rising- and
 * falling-edge interrupts (REG 0x41/0x49 bits 0 and 1) are not enabled, so
 * there is no level to report - only the short-press and long-press
 * interrupts are (see buttons.c), which drive `pressed` and `held` below.
 * It is a firmware choice not to enable the other two, not a hardware
 * limit. */
typedef struct {
    bool down;
    bool pressed;
    bool released;

    /* BOOT: fires exactly once when the button has been held past
     * BUTTON_HOLD_US - see button_fsm.h for the full contract, including
     * that a hold consumes the release edge above so the two never fire for
     * the same press.
     *
     * PWR: fires exactly once when the PMU's own long-press interrupt
     * latches - see buttons.h's top comment for the threshold (1-2.5 s,
     * board-dependent, not configured by this firmware) and buttons.c for
     * how it is read and cleared. Unlike BOOT's, this edge does not consume
     * `released` - PWR never reports one at all (see above) - and a `pressed`
     * for the same physical hold may still arrive separately, on whatever
     * edge the PMU's own short-press logic uses; a caller that binds both to
     * different actions should expect either or both to fire for one hold,
     * not treat them as mutually exclusive. */
    bool held;
} button_t;

/* Starts the polling task. Safe to call when the PMU is absent - the BOOT
 * button still works and PWR simply never fires. */
void buttons_start(void);

/* Reads and CONSUMES the edges accumulated since the last call. */
void buttons_read(button_t *boot, button_t *power);
