/*=============================================================================
 * display - which way is "up", decided once for the whole shell.
 *
 * Orientation is a property of the physical device, not of any one app's
 * panel. Before this existed, app_sand.c derived a quarter turn from gravity
 * purely for its own palette (see its old gravity_quarter_turn()), so the
 * launcher and the boot menus never rotated - nobody told them to. This is
 * the one place that decision gets made, so main.c can apply it once and
 * every UI surface follows.
 *
 * PURE, HOST-TESTABLE, IN THE MANNER OF gesture.c AND tilt.c
 *
 * No IMU, no gfx, no ui: the gravity vector is fed in already read (and, in
 * practice, already smoothed - see input/imu.h and apps/sand/tilt.h for what
 * that looks like) and this module only decides. Applying the decision -
 * calling ui_set_transform() - is main.c's job. Keeping "decide" and "apply"
 * apart is what lets this link and run on a host; see
 * test/suites/suite_display.c.
 *
 * HYSTERESIS IS THE POINT, NOT AN EXTRA
 *
 * The obvious implementation snaps to whichever of gx/gy has the larger
 * magnitude - that is exactly what app_sand.c's old gravity_quarter_turn()
 * did. Its boundary sits at 45 degrees from "up", and a board held near that
 * angle flips the whole UI back and forth every single frame the tilt
 * wobbles across it. Tolerable for a palette panel nobody stares at edge-on;
 * intolerable once the whole shell - launcher included - rotates with it.
 *
 * The fix is a Schmitt trigger, expressed directly in the gravity
 * components rather than in degrees (there is no trig here, and does not
 * need to be - see the arithmetic below). For whichever quarter is
 * CURRENTLY committed, split (gx, gy) into two parts:
 *
 *   aligned        the component along that quarter's own "down" direction -
 *                   positive and large while the board is still held roughly
 *                   the way this quarter expects.
 *   perpendicular   the other component - how far off to the side gravity
 *                   has drifted.
 *
 * A switch away from the current quarter fires once
 *
 *   |perpendicular| * DISPLAY_HYST_DEN  >  aligned * DISPLAY_HYST_NUM
 *
 * DISPLAY_HYST_NUM/DEN = 7/4 = 1.75, a small-integer stand-in for
 * tan(60 degrees) = 1.732 - so leaving a quarter needs the tilt to have
 * drifted about 60 degrees from where that quarter calls "down". (A negative
 * `aligned` - tilt past 90 degrees - satisfies the inequality on its own,
 * since the right side goes negative while the left stays non-negative, so a
 * hard flip clears the threshold in one step rather than getting stuck.)
 *
 * That single ratio, applied relative to whichever quarter is current, is
 * what produces the asymmetric "60 out, 30 back" the task calls for, with no
 * second constant needed: aligned and perpendicular are the same two gravity
 * axes, just relabelled after a switch, because the quarter that was
 * "perpendicular" a moment ago is now the aligned one. So returning to the
 * ORIGINAL quarter needs the tilt back within 30 degrees of it (the
 * complement of 60) even though the code runs the identical comparison
 * against the identical ratio on both sides of the switch - it is just
 * asking the question of whichever quarter happens to be current at the
 * time. A vector parked exactly on the old 45-degree boundary (|gx| == |gy|)
 * satisfies neither the outbound nor the inbound test at either quarter, so
 * it never oscillates.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

/* tan(60 deg) = 1.732..., approximated as a small integer ratio so the
 * hysteresis test is exact integer (cross-multiplied) arithmetic - no
 * division, no float, no rounding to reason about. See this header's top
 * comment for why one ratio, applied relative to whichever quarter is
 * currently committed, is enough to give both the 60-degrees-out and the
 * 30-degrees-back behaviour. */
#define DISPLAY_HYST_NUM 7
#define DISPLAY_HYST_DEN 4

typedef struct {
    /* Which quarter turn currently reads as "upright" - numbered the same
     * way gfx_text_turned() and ui_transform_quarter_turn() do: 0 upright, 1
     * top-to-bottom, 2 upside down, 3 bottom-to-top.
     *
     * The only state this module keeps. The hysteresis test above is a pure
     * function of (quarter, gx, gy) - nothing here accumulates over time or
     * needs a clock, which is also why display_update() takes no dt: main.c
     * controls how often it is called (see its own comment on sampling this
     * at a modest, fixed rate), and the decision itself does not care. */
    int quarter;
} display_t;

/* Starts upright (quarter 0). There is no "unknown" orientation to
 * represent - a board that has not been read yet is assumed held the most
 * common way, and the first real reading corrects it if that guess was
 * wrong, the same as any other update. */

/* WHAT EACH QUARTER ACTUALLY IS, MEASURED - NOT DERIVED
 *
 * 0/1/2/3 name a quarter-turn from the panel's native upright (GFX_WIDTH x
 * GFX_HEIGHT, 368 x 448 - a "portrait" shape). Which physical orientation
 * that actually corresponds to is not visible from source - it depends on
 * how the case is held versus how the panel's native rows and columns are
 * wired - so it could not be derived, only measured, the same way
 * GRAVITY_SCREEN_X/Y in apps/sand/app_sand.c was found: hold the board,
 * flash a build with the Diagnostics app's "show orientation" toggle on
 * (see app_diagnostics.c), read the reported quarter off for each hold.
 *
 * Measured against this board on 2026-08-27:
 *
 *     0   Portrait
 *     1   Landscape             (USB connector to the right - see below)
 *     2   Portrait, upside down
 *     3   Landscape, upside down
 *
 * These four names are the start of the vocabulary this shell's apps
 * should share for "which way is the board being held" - use them instead
 * of a bare quarter number anywhere the number is standing in for one of
 * these four physical facts rather than for arithmetic on the transform
 * itself (composing turns, deriving ui_width()/height(), and the like stay
 * plain ints - that math does not care what a quarter is CALLED). */
#define DISPLAY_PORTRAIT              0
#define DISPLAY_LANDSCAPE             1
#define DISPLAY_PORTRAIT_UPSIDE_DOWN  2
#define DISPLAY_LANDSCAPE_UPSIDE_DOWN 3

/* The orientation the SHELL applies at boot, before the first gravity
 * sample arrives - main.c sets this once, right after display_init(),
 * which itself stays a neutral 0 with no opinion about it (see that
 * function's own comment: this is a physical fact about one board, not
 * something a device-agnostic module should bake into its own reset).
 *
 * DISPLAY_LANDSCAPE, not a bare 1 - this board is normally held sideways
 * to its native upright, USB connector to the right, and the table above
 * is what confirms that is quarter 1 specifically. It was a first guess
 * when this constant was written; it no longer is. */
#define DISPLAY_DEFAULT_QUARTER DISPLAY_LANDSCAPE

void display_init(display_t *d);

/* Feed the current gravity vector, in whatever consistent units the caller's
 * IMU reading uses (screen X/Y axes, not raw sensor axes - see main.c's own
 * mapping). Returns true when d->quarter actually changed, which is main.c's
 * cue to push a new ui_set_transform(). */
bool display_update(display_t *d, int gx, int gy);

int display_quarter(const display_t *d);

/* The shell's own orientation - the quarter main.c last set the UI transform
 * to. Declared here because display.h is the layer an app already depends on
 * for the quarter numbering, but defined in main.c, not display.c: main.c is
 * the only thing that ever calls display_update(), and owns the display_t
 * that decision is made against. This module stays a plain, instantiable
 * decision function with no state of its own beyond what a caller passes it
 * (see this header's top comment) - the same split app_register()/app_list()
 * draw in app.h between "declared where callers already look" and "defined
 * where the one real instance lives".
 *
 * For an app that draws through the shell's UI transform rather than owning
 * it: knowing when that transform changed underneath you, without reading
 * the IMU a second time or duplicating the hysteresis above. See
 * app_sand.c's palette repaint for why a panel needs to ask this. */
int display_shell_quarter(void);
