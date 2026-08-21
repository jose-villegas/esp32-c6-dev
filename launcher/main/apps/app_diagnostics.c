/*=============================================================================
 * app_diagnostics - shows the POST report on demand.
 *
 * The POST itself is silent when everything passes, which is the right default
 * for a device that should just boot. This is how you look anyway, without
 * attaching a serial cable - a board in the field usually has nothing on its
 * console.
 *
 * Entering it RE-RUNS the checks, so the report is live rather than a record of
 * what boot found - the point of opening it is usually to see whether something
 * is failing now.
 *
 * One check cannot be repeated: the SD card needs SPI2, which the display holds
 * once it is up, and the BSP exposes no way to release the display. That row is
 * carried forward from boot and marked "(at boot)" so the report never looks
 * more current than it is.
 *===========================================================================*/

#include "../app.h"
#include "../gfx.h"
#include "../post_ui.h"

static void diagnostics_enter(void)
{
    /* Re-run on entry rather than per frame: the checks probe I2C and cycle the
     * audio rail, which is fine once but has no business happening 40 times a
     * second. */
    post_rerun();
}

static void diagnostics_frame(uint32_t dt_ms, const input_t *input)
{
    (void)dt_ms;
    (void)input;

    /* Redrawn every frame rather than cached: the shell owns the framebuffer
     * and the previous app may have left anything in it. */
    post_ui_draw_report("POWER-ON SELF TEST");
}

static void diagnostics_exit(void) { }

const app_t app_diagnostics = {
    .name    = "Diagnostics",
    .summary = "Hardware self-test report",
    .enter   = diagnostics_enter,
    .frame   = diagnostics_frame,
    .exit    = diagnostics_exit,
};
