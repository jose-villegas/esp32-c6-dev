/*=============================================================================
 * Portable suite: the falling-sand automaton - liquid depth shading, shine
 * direction, and the depth debounce.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 *===========================================================================*/
#include <math.h>   /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "unity.h"
#include "suites.h"

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

static void test_a_liquid_body_paints_flat_inside(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t body = pal[CELL_MAKE(MAT_WATER, MASS_MAX)];

    enum { COMB_W = 8, COMB_H = 3 };
    cell_t grid[COMB_H][COMB_W];

    for (int y = 0; y < COMB_H; y++) {
        for (int x = 0; x < COMB_W; x++) {
            grid[y][x] = CELL_MAKE(MAT_WATER, MASS_MAX);
        }
    }
    /* The comb itself, dropped into the middle row's interior columns -
     * everything around it stays a solid full-water border. */
    for (int x = 1; x < COMB_W - 1; x++) {
        grid[1][x] = CELL_MAKE(MAT_WATER, (x % 2) ? 7 : MASS_MAX);
    }

    material_set_gravity(0, 0);   /* interior painting must not care either
                                    * way - there is no rim here to shade,
                                    * and every material_colours() call below
                                    * passes an explicit depth of its own */

    gfx_color_t seen = 0;
    bool have_seen = false;
    for (int x = 1; x < COMB_W - 1; x++) {
        const cell_t c = grid[1][x];
        const unsigned mask =
            (CELL_IS_EMPTY(grid[1][x - 1]) ? MATERIAL_EDGE_LEFT  : 0u) |
            (CELL_IS_EMPTY(grid[1][x + 1]) ? MATERIAL_EDGE_RIGHT : 0u) |
            (CELL_IS_EMPTY(grid[0][x])     ? MATERIAL_EDGE_UP    : 0u) |
            (CELL_IS_EMPTY(grid[2][x])     ? MATERIAL_EDGE_DOWN  : 0u);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)mask,
            "the border around the comb must make every varied cell "
            "genuinely interior, or this test is not exercising the case "
            "it claims to");

        gfx_color_t col[3];
        material_colours(c, 0u, mask, 255u, col);

        char why[96];
        snprintf(why, sizeof why,
                 "comb column %d (fill %d) must paint the body colour, not "
                 "its own fill level", x, CELL_VARIANT(c));
        TEST_ASSERT_EQUAL_MESSAGE(body, col[0], why);

        if (have_seen) {
            TEST_ASSERT_EQUAL_MESSAGE(seen, col[0],
                "every interior cell of the comb must paint IDENTICALLY - "
                "that is what makes the comb disappear rather than merely "
                "change colour, and is the whole point of this change");
        }
        seen = col[0];
        have_seen = true;
    }
}

/* DEPTH gives the interior something to shade with again.
 *
 * test_a_liquid_body_paints_flat_inside just above pins that the comb fix
 * makes every interior cell paint the flat body colour, whatever its own
 * fill level says - and that is exactly why depth had to be invented rather
 * than derived from fill: measured on a settled pool, 0 of 747 interior
 * cells were anything but full at 40 degrees settled, 0 of 720 settled
 * flat, and only 5% even 3 steps into a tilt. Fill level cannot carry a
 * gradient a settled pool never varies. `depth` - a cell's own position
 * along gravity, normalised across the grid's projected span - is the new
 * cue, and this is the test that the cue actually reaches the panel.
 *
 * Same cell, same mask (0 - interior, so there is no rim shift or foam
 * anywhere near this to confuse the comparison with), at the two ends of
 * the depth range: 0 (shallowest) and 255 (deepest). The shallow one must
 * paint BRIGHTER - light attenuates with depth, so less of it overhead
 * should read as more of it - and the deep one must paint EXACTLY the body
 * colour, palette[CELL_MAKE(id, MASS_MAX)], with no shift at all: the
 * gradient only ever LIGHTENS a shallower cell relative to that body
 * colour, it never darkens a deep one past it, or a pool would read darker
 * than its own resting colour simply for being deep.
 *
 * `depth` 255 is well past DEPTH_SATURATE_CELLS (material.c's own constant
 * for how many cells of local depth it takes to reach the body colour) -
 * this is deliberately deeper than the clamp, not merely equal to it, so
 * this test also pins that anything past the clamp reads exactly as the
 * clamp does, not as something further darkened past it. */
static void test_a_liquid_interior_is_shaded_by_depth(void)
{
    const gfx_color_t *pal = material_palette();
    const cell_t c = CELL_MAKE(MAT_WATER, MASS_MAX);

    gfx_color_t shallow[3], deep[3];
    material_colours(c, 0u, 0u, 0u, shallow);
    material_colours(c, 0u, 0u, 255u, deep);

    TEST_ASSERT_TRUE_MESSAGE(
        panel_luminance(shallow[0]) > panel_luminance(deep[0]),
        "a shallow interior cell (depth 0) must paint BRIGHTER than a deep "
        "one (depth 255) - depth is the only cue left to shade a settled "
        "pool's interior with, and if it does not lighten as a cell gets "
        "shallower the interior is exactly as flat as it was before this "
        "change");
    TEST_ASSERT_EQUAL_MESSAGE(pal[CELL_MAKE(MAT_WATER, MASS_MAX)], deep[0],
        "and the deepest cell must paint EXACTLY the body colour, with no "
        "shift applied at all - the gradient only ever lightens a "
        "shallower cell relative to that body colour, it never darkens a "
        "deep one past it, or a pool would read darker than its own "
        "resting colour simply for being deep");
}

/* Depth is an INTERIOR-only cue - see material_colours()'s own comment on
 * why a rim already carries two terms (its own fill level, and
 * liquid_spec[]'s specular shift) and a third stacked on top would mostly
 * spend its range clamped against whichever end the other two already
 * reached. This is the test that pins the boundary: nothing outside a
 * liquid's interior may read `depth` at all, whatever paint_row_n() hands
 * it - not a rim cell, and not a material that is not a liquid to begin
 * with.
 *
 * A RIM liquid cell (mask nonzero, a cardinal bit set) must paint
 * identically at depth 0 and depth 255 - its own fill level and
 * liquid_spec[]'s specular are the entire story there, and depth must not
 * add a third, silent one. OIL rather than water for this half: a water
 * rim also runs the foam dither (material_colours()'s own comment on
 * curvature), and at the wrong hash/phase combination foam can overwrite
 * `out[0]` identically regardless of depth, which would let a real depth
 * leak into the rim's own fill-index arithmetic hide behind foam instead
 * of being caught. Oil shares the exact same fill-indexed-plus-specular
 * code path but never foams, so any such leak has nowhere left to hide.
 * And two materials that are not liquids at all - stone and glass, both of
 * which spend their own variant on something depth could plausibly be
 * confused for (temperature) - must paint identically too: depth is
 * meaningless to them, and the parameter has to be silently ignored
 * rather than accidentally read through some shared code path. */
static void test_only_a_liquid_interior_reads_depth(void)
{
    material_set_gravity(0, 0);   /* no specular term to confuse the
                                            * rim comparison with */

    gfx_color_t rim_shallow[3], rim_deep[3];
    material_colours(CELL_MAKE(MAT_OIL, 8), 0u, MATERIAL_EDGE_UP, 0u,
                     rim_shallow);
    material_colours(CELL_MAKE(MAT_OIL, 8), 0u, MATERIAL_EDGE_UP, 255u,
                     rim_deep);
    TEST_ASSERT_EQUAL_MESSAGE(rim_shallow[0], rim_deep[0],
        "a RIM liquid cell must paint identically at depth 0 and depth "
        "255 - depth is the interior's business, not the rim's, which "
        "already has its own fill level and liquid_spec[]'s specular "
        "shift to show instead");

    gfx_color_t glass_shallow[3], glass_deep[3];
    material_colours(CELL_MAKE(MAT_GLASS, 5), 1u, 0u, 0u, glass_shallow);
    material_colours(CELL_MAKE(MAT_GLASS, 5), 1u, 0u, 255u, glass_deep);
    TEST_ASSERT_EQUAL_MESSAGE(glass_shallow[0], glass_deep[0],
        "glass must ignore depth entirely - it is not a liquid, and depth "
        "must not leak into a code path that has nothing to do with it");

    gfx_color_t stone_shallow[3], stone_deep[3];
    material_colours(CELL_MAKE(MAT_STONE, 5), 1u, 0u, 0u, stone_shallow);
    material_colours(CELL_MAKE(MAT_STONE, 5), 1u, 0u, 255u, stone_deep);
    TEST_ASSERT_EQUAL_MESSAGE(stone_shallow[0], stone_deep[0],
        "and neither must stone - the same guarantee, on the other "
        "non-liquid material whose variant could plausibly be confused "
        "for depth");
}

/* A rim cell still shows its own fill level - the other half of the same
 * split, and the half a previous attempt at this fix broke. That attempt
 * composited the WHOLE liquid palette against the background, which
 * flattened the rim along with the interior and erased the thing a
 * shallow edge is FOR: the pale film at water's thin end, lava's bright
 * skim, oil's murky olive and acid's vivid lime all come from the rim
 * reading its own fill level, not some fixed edge tint.
 *
 * Checked under ZERO gravity, so the specular shift the next test covers
 * cannot be what is making shallow and deep differ here - this is purely
 * "does the ordinary fill ramp still work on a rim cell", which is what
 * stops part 1 (the interior fix) from quietly swallowing the rim too.
 *
 * Back on WATER, having been on oil for a while. Water's rim now also
 * carries foam (see material_colours()'s own comment on curvature), which
 * is a dither keyed off hash, phase and mask - and a mask of a single
 * cardinal bit with no diagonals is itself a curved shape (an empty count
 * of 1, two away from the flat count of 3), so it foamed regardless of
 * hash and broke this test the day foam landed. The fix is not to dodge
 * onto a different liquid but to give the cell a mask that is
 * DELIBERATELY FLAT: one cardinal side plus the two diagonals that lean
 * against it is exactly 3 of 8 empty, which is curvature 0 - foam's
 * threshold there is 0, so `(anything) & 7u < 0` can never be true and this
 * cell cannot foam at any hash or any phase. Water is the material that
 * matters most here, and it is back under direct coverage rather than
 * standing in for it. */
static void test_a_liquid_rim_still_shows_its_fill(void)
{
    material_set_gravity(0, 0);   /* no specular term to confuse this with */

    const gfx_color_t *pal = material_palette();
    /* Flat rim on the "up" side: MATERIAL_EDGE_UP plus its two leaning
     * diagonals, exactly 3 of 8 neighbours empty - see this test's own
     * top comment for why that shape, and only that shape, keeps foam out
     * of a test that has nothing to do with it. */
    const unsigned mask = MATERIAL_EDGE_UP | MATERIAL_EDGE_UP_LEFT |
                          MATERIAL_EDGE_UP_RIGHT;

    gfx_color_t shallow[3], deep[3];
    material_colours(CELL_MAKE(MAT_WATER, 1), 0u, mask, 255u, shallow);
    material_colours(CELL_MAKE(MAT_WATER, MASS_MAX), 0u, mask, 255u,
                     deep);

    TEST_ASSERT_EQUAL_MESSAGE(pal[CELL_MAKE(MAT_WATER, 1)], shallow[0],
        "a rim cell must read its own fill level straight from the "
        "palette - flattening this is the mistake a previous attempt at "
        "hiding the comb made, and it erased the surface film the rim "
        "exists to show");
    TEST_ASSERT_EQUAL_MESSAGE(pal[CELL_MAKE(MAT_WATER, MASS_MAX)], deep[0],
        "and a full rim cell must read as full, not as whatever the "
        "interior case would have painted it instead");
    TEST_ASSERT_TRUE_MESSAGE(shallow[0] != deep[0],
        "a shallow rim and a deep one must be visibly different colours, "
        "or the fill ramp is dead on the one cell where it is supposed to "
        "matter most");
}

/* The rim's highlight follows gravity, the way specularity should.
 *
 * A rim cell's brightness is not fixed by its fill level alone any more -
 * it is shifted by how much its empty side faces AGAINST gravity, the same
 * way light catches the top of a real pool and leaves the underside of a
 * drip or an overhang dark. material_set_gravity() computes that shift
 * once a frame into liquid_spec[]; this is the test that pins its SIGN.
 *
 * The sign is not a subtle miscalibration to get wrong. Liquid ramps run
 * pale-to-dark as fill rises (material.h's own top comment), so brightening
 * means moving DOWN the index - flip that and every pool on the board
 * lights up along its underside and goes dark across its top, which is the
 * exact opposite of what a real surface does and not something a glance at
 * the device would necessarily catch, since a pool still looks LIT, just
 * from the wrong side. Comparing against gravity's own direction, twice,
 * at two different tilts, is what catches that rather than trusting the
 * arithmetic by eye.
 *
 * Back on WATER, for the same reason test_a_liquid_rim_still_shows_its_fill
 * is: water is the material that matters most here. Each mask below is a
 * FLAT rim - one cardinal side plus its two leaning diagonals, exactly 3 of
 * 8 empty - not the single cardinal bit this test used before foam
 * existed, because a lone cardinal bit is itself curved (empty count 1,
 * two away from flat) and foamed regardless of hash. liquid_spec[] is
 * indexed by the CARDINAL bits alone (mask & MATERIAL_EDGE_CARDINAL - see
 * that table's own comment in material.c), so adding the leaning diagonals
 * changes nothing about which specular shift applies; it only changes
 * curvature from 2 to 0, which is what keeps foam out of a test about the
 * specular. */
static void test_a_liquid_rim_catches_the_light_from_above(void)
{
    const uint8_t fill = 8;   /* mid-ramp, so a shift in either direction
                               * has somewhere to go without clamping at
                               * either end and hiding the difference */

    material_set_gravity(0, 1000);   /* straight down */

    gfx_color_t up[3], down[3];
    material_colours(CELL_MAKE(MAT_WATER, fill), 0u,
                     MATERIAL_EDGE_UP | MATERIAL_EDGE_UP_LEFT |
                         MATERIAL_EDGE_UP_RIGHT,
                     255u,
                     up);
    material_colours(CELL_MAKE(MAT_WATER, fill), 0u,
                     MATERIAL_EDGE_DOWN | MATERIAL_EDGE_DOWN_LEFT |
                         MATERIAL_EDGE_DOWN_RIGHT,
                     255u,
                     down);

    TEST_ASSERT_TRUE_MESSAGE(
        panel_luminance(up[0]) > panel_luminance(down[0]),
        "with gravity pulling straight down, the empty side facing UP - "
        "the top of a pool - must be the bright one; a sign flipped here "
        "would light the underside of every overhang instead of its top");

    material_set_gravity(1000, 0);   /* tilt: gravity now points right */

    gfx_color_t left[3], right[3];
    material_colours(CELL_MAKE(MAT_WATER, fill), 0u,
                     MATERIAL_EDGE_LEFT | MATERIAL_EDGE_UP_LEFT |
                         MATERIAL_EDGE_DOWN_LEFT,
                     255u,
                     left);
    material_colours(CELL_MAKE(MAT_WATER, fill), 0u,
                     MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP_RIGHT |
                         MATERIAL_EDGE_DOWN_RIGHT,
                     255u,
                     right);

    TEST_ASSERT_TRUE_MESSAGE(
        panel_luminance(left[0]) > panel_luminance(right[0]),
        "and the highlight must follow the tilt rather than stay where it "
        "was - once gravity points right, the side facing LEFT is the one "
        "facing away from it, so that is the side that should catch the "
        "light now");
}

/* material_shine_direction()'s degenerate case: no gravity to sweep toward,
 * so it must hand back the plain (1, 1) diagonal the shine always travelled
 * before gravity had any say in it - not (0, 0), which would collapse the
 * whole band to a single unmoving phase across every pixel of a cell. */
static void test_shine_direction_holds_the_old_diagonal_with_no_gravity(void)
{
    int ux_q8 = 0, uy_q8 = 0;
    material_shine_direction(0, 0, &ux_q8, &uy_q8);

    TEST_ASSERT_EQUAL_INT_MESSAGE(181, ux_q8,
        "flat or free fall must fall back to the original diagonal, not "
        "collapse the shine's direction to nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(181, uy_q8, "same for the y half of it");
}

static void test_shine_direction_is_minus_gravity_turned_left(void)
{
    int ux_q8 = 999, uy_q8 = 999;

    material_shine_direction(0, 1000, &ux_q8, &uy_q8);
    TEST_ASSERT_TRUE_MESSAGE(uy_q8 < -150 && uy_q8 > -215,
        "gravity straight down: minus gravity is straight up, and an eighth "
        "of a turn left of that is up-left, so the y half is -1/sqrt 2");
    TEST_ASSERT_TRUE_MESSAGE(ux_q8 < -150 && ux_q8 > -215,
        "and the x half is the same -1/sqrt 2 - a band that still swept "
        "straight up would leave this near zero");

    material_shine_direction(1000, 0, &ux_q8, &uy_q8);
    TEST_ASSERT_TRUE_MESSAGE(ux_q8 < -150 && ux_q8 > -215,
        "gravity pointing right: minus gravity is left, an eighth of a turn "
        "left of THAT is down-left, so the x half is -1/sqrt 2");
    TEST_ASSERT_TRUE_MESSAGE(uy_q8 > 150 && uy_q8 < 215,
        "and the y half is +1/sqrt 2 (downward on screen) - a turn the "
        "other way round would put -1/sqrt 2 here");
}

static void test_shine_direction_is_a_genuine_angle_not_a_snap(void)
{
    int axis_ux_q8, axis_uy_q8, diag_ux_q8, diag_uy_q8;

    material_shine_direction(1000, 0, &axis_ux_q8, &axis_uy_q8);
    material_shine_direction(1000, 1000, &diag_ux_q8, &diag_uy_q8);

    /* Pure-right gravity sweeps down-left and pure-down gravity sweeps
     * up-left (see the test just above), so gravity split evenly between
     * the two must land halfway between those: straight left, with the
     * vertical halves cancelling. Either snap would leave |uy| near
     * 1/sqrt 2 instead. */
    TEST_ASSERT_TRUE_MESSAGE(diag_uy_q8 > -40 && diag_uy_q8 < 40,
        "gravity split evenly between right and down must sweep the shine "
        "straight left, halfway between the two axis cases - a snap-to-"
        "nearest-axis implementation would leave a 1/sqrt 2 vertical half "
        "here");
    TEST_ASSERT_TRUE_MESSAGE(diag_ux_q8 < axis_ux_q8 - 40,
        "and with nothing left over for the vertical, the sideways half "
        "must be the full unit length, well beyond the pure-axis case's "
        "1/sqrt 2");
}

/* Whatever direction comes out must be a unit vector - im_len()'s ~4%
 * approximation is the only slack allowed. */
static void test_shine_direction_is_unit_length(void)
{
    static const int gxs[] = { 1000, 1000, 0, -700, 300 };
    static const int gys[] = { 0, 1000, -1000, 400, -900 };

    for (unsigned i = 0; i < sizeof gxs / sizeof gxs[0]; i++) {
        int ux_q8, uy_q8;
        material_shine_direction(gxs[i], gys[i], &ux_q8, &uy_q8);

        /* im_len() overshoots by 8% at 2:1 ratio, undershoots by 1% on
         * diagonal. Tolerance: [256/1.08, 256/0.99]. */
        const long mag_sq = (long)ux_q8 * ux_q8 + (long)uy_q8 * uy_q8;
        char why[64];
        snprintf(why, sizeof why, "gravity (%d, %d)", gxs[i], gys[i]);
        TEST_ASSERT_TRUE_MESSAGE(mag_sq > 230L * 230L && mag_sq < 265L * 265L,
            why);
    }
}

/*=============================================================================
 * LOCAL DEPTH - the depth signal now follows each puddle's own shape
 * rather than a fixed screen-position gradient, and combines its vertical
 * and horizontal readings continuously instead of switching between them.
 * See LOCAL DEPTH's own long comment in app_sand.c for the full mechanism
 * and the device reports ("almost like platinum"; "follows the shape of the
 * puddle"; "fighting multiple values"... "single source of truth"; "weird
 * rectangles"... "the shade just gains length depending on which direction
 * has more magnitude") that motivated each part of it.
 *
 * THE COMBINER ITSELF CHANGED since some of the comments below were first
 * written: what shipped first as a Q8 BLEND of the two axis counts (`w =
 * 256|gx|/(|gx|+|gy|)`, `(vdepth*(256-w) + hdepth*w) >> 8`) was replaced by
 * a PROJECTION of each axis count onto the gravity direction followed by
 * MAX (`wv_q8 = 256|gy|/im_len(gx,gy)`, `wh_q8 = 256|gx|/im_len(gx,gy)`,
 * `max(vdepth*wv_q8, hdepth*wh_q8) >> 8`) - two device-reported defects in
 * the blend (a shadow of wrongly-shallow water beside a submerged obstacle,
 * and the reported depth of a fixed true depth inflating by up to 46% with
 * tilt alone) that a weighted average of two axis COUNTS cannot fix, because
 * neither count is a distance along gravity until it is projected onto it.
 * See app_sand.c's own "THE COMBINER ITSELF" comment in paint_row_n() for
 * the full account. Every test-local mirror of the combiner below was
 * updated to match; mirrors of the surrounding debounce/climb machinery
 * that do not touch the combiner itself were not, and do not need to be.
 *
 * paint_row_n() itself cannot be linked into this host suite - it lives in
 * app_sand.c, which check_app_sources.sh only compile-checks, the same
 * position FOAM_PHASE_MS's own accumulator has always been in - so each
 * test below mirrors the algorithm it pins with a small test-local helper,
 * built to match app_sand.c's real logic exactly, rather than calling into
 * it.
 *===========================================================================*/

/* Mirrors app_sand.c's REAL vertical local depth - col_stable_depth[]/
 * col_top_row[]'s hold-then-commit, band-saturating climb - for a FIXED
 * straight-down-or-steeper gravity (gy > 0): surface up, "above" toward the
 * surface, ascending row order. Also serves as the vertical half of
 * test_the_blend_has_no_jump_crossing_45_degrees below, whose gravity sweep
 * never lets gy go negative either, so this one fixed direction covers both
 * callers. Reads the LIVE grid via sand_at() rather than assuming a fixed
 * shape, the same way paint_row_n() reads the live framebuffer row/above/
 * below pointers - off-grid rows read as MAT_STONE (sand_at()'s own
 * convention), which is "not the same material" as water and correctly
 * reads as a boundary.
 *
 * THIS USED TO mirror col_local_depth[]'s dead raw walk instead - immediate
 * reset to 0 on any disagreement, saturating at a byte's own 255 - which is
 * NOT what the shipped mechanism does and was never exercised by anything
 * else in this file: col_local_depth[] was write-only (nothing but its own
 * next iteration ever read it) and was removed from app_sand.c once this was
 * noticed. This mirror now implements the array that actually reaches the
 * screen - see col_stable_depth[]'s own comment in app_sand.c for the full
 * mechanism this reproduces, including the two bugs its own earlier
 * versions had.
 *
 * `stable`/`top_row` are IN/OUT, the same way the real col_stable_depth[cx]/
 * col_top_row[cx] persist across the separate paint_row_n() calls that walk
 * one column down through different rows in different frames - callers that
 * want a fresh column (matching the real arrays' BSS-zero start) pass in
 * { 0, 255 }, and a caller that wants to model a SECOND frame's repaint of
 * the same column (see the hold-then-commit's own two-consecutive-frames
 * rule) calls this again with the same state pointers. */

#define SUITE_LOCAL_DEPTH_COUNT_CEILING MATERIAL_LIQUID_DEPTH_BAND

/* PASSING `inc=1, ceiling=MATERIAL_LIQUID_DEPTH_BAND` REPRODUCES OLD
 * ARITHMETIC EXACTLY - TESTS STILL PASS UNCHANGED.
 * TEST_THE_BLEND_HAS_NO_JUMP_CROSSING_45_DEGREES NEEDS RAISED CEILING. */
static void mirror_local_depth_column(sand_t *g, int cx, int h,
                                      unsigned char *stable,
                                      unsigned char *top_row,
                                      unsigned depth_out[],
                                      unsigned inc, unsigned ceiling)
{
    for (int cy = 0; cy < h; cy++) {
        const cell_t here  = sand_at(g, cx, cy);
        const cell_t above = sand_at(g, cx, cy - 1);
        const bool same = CELL_MATERIAL(above) == CELL_MATERIAL(here);
        unsigned depth;

        if (material_of(here)->kind != KIND_LIQUID) {
            depth = 0u;
        } else if (same) {
            depth = *stable < ceiling - inc ? *stable + inc : ceiling;
        } else if (*top_row == (unsigned char)cy) {
            depth = 0u;
        } else {
            depth = *stable < ceiling - inc ? *stable + inc : ceiling;
            *top_row = (unsigned char)cy;
        }
        *stable = (unsigned char)depth;
        depth_out[cy] = depth;
    }
}

/* THE DEVICE COMPLAINT THIS PINS, verbatim: "the sensibility against
 * gravity makes it behave almost like platinum", and "there is no arcs...
 * maybe it's better if the depth just follows the shape of the puddle."
 * Modelled before this change was written (an irregular pool with a rock
 * island poking through it; a screen-position depth paints straight bands
 * across the rock as if it were not there) and this is that same case,
 * through the real automaton rather than a hand-authored grid.
 *
 * Builds a settled pool, via real sand_t/sand_step(), with a stone plug
 * through the middle of ONE column's water and nothing interrupting a
 * SECOND column elsewhere in the same pool. The two columns must see
 * IDENTICAL local depth for every row ABOVE the plug - both are plain,
 * uninterrupted water there - and must DIVERGE starting exactly at the row
 * where the plugged column's water resumes below the rock: that column
 * resets to a small depth right there, while the other keeps climbing. A
 * pure screen-position depth - an affine function of cy alone, exactly what
 * this change replaces - cannot tell the two columns apart at all, which is
 * exactly the bug this test exists to catch a regression back into.
 *
 * STILL VALID after the axis-hysteresis switch was replaced first by a
 * vertical/horizontal BLEND and later by a projection-then-max COMBINER
 * (see LOCAL DEPTH's own top comment in app_sand.c), without changing a
 * single assertion below through either change, because the gravity this
 * test feeds sand_step() is straight down - gx exactly 0. At gx == 0 the
 * combiner's own weight formula (update_local_depth_gravity() in
 * app_sand.c) puts wv_q8 at EXACTLY 256 and wh_q8 at EXACTLY 0 - im_len(0,
 * gy) reduces to |gy| exactly, so this is not even subject to im_len()'s own
 * approximation error - so the combined depth mirror_local_depth_column()
 * checks here is not an approximation of what the shipped mechanism does at
 * this gravity - it is EXACTLY what it does, the same way it was when this
 * was still a single dominant-axis choice, and the same way it was under
 * the blend this replaced. The combiner's own behaviour AWAY from gx == 0 is
 * a different claim, pinned separately by
 * test_the_blend_has_no_jump_crossing_45_degrees below (kept its old name;
 * see that test's own top comment for why). */
/* A grid of its own, sized for a settled pool deep enough to carry a
 * two-cell rock plug with real water left above and below it - the
 * standard W x H fixture (8x8) has no room for that. Named OBST_POOL_*,
 * not the shorter POOL_W/POOL_H/pool/pool_cells this file already has, to
 * avoid colliding with that unrelated fixture (the pour-source tests
 * further down) - a different shape for a different purpose entirely. */
#define OBST_POOL_W 6
#define OBST_POOL_H 14
static uint8_t obst_pool_cells[OBST_POOL_W * OBST_POOL_H];

static void test_local_depth_follows_the_puddles_own_shape(void)
{
    enum { PW = OBST_POOL_W, PH = OBST_POOL_H };
    sand_init(&fx.obst_pool, obst_pool_cells, PW, PH, 4242u);

    for (int y = 2; y < PH; y++) {
        for (int x = 0; x < PW; x++) {
            sand_set(&fx.obst_pool, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    /* The obstacle: a two-cell rock plug straight through column OBST_X's
     * water, with water left continuous above and below it - "an irregular
     * pool with a rock island poking through it", the exact case that first
     * suggested this whole change. */
    enum { OBST_X = 3, OBST_Y0 = 7, OBST_Y1 = 8 };
    sand_set(&fx.obst_pool, OBST_X, OBST_Y0,
             CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.obst_pool, OBST_X, OBST_Y1,
             CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    for (int i = 0; i < 40; i++) {
        sand_step(&fx.obst_pool, 0, 1000, 0);   /* straight down, matching the
                                               * mirror's own fixed gravity */
    }

    enum { CLEAR_X = 0 };   /* an unobstructed column, elsewhere in the same
                             * pool */

    /* Fresh, independent state per column, ONE call each - matching the real
     * arrays' BSS-zero start and a single frame's repaint of a just-settled
     * pool. See the DIVERGENCE assertion below for why a single call is the
     * right thing to check here, not an artifact of not bothering to settle
     * further. */
    unsigned depth_obstructed[PH], depth_clear[PH];
    unsigned char obst_stable = 0, obst_top_row = 255;
    unsigned char clear_stable = 0, clear_top_row = 255;
    mirror_local_depth_column(&fx.obst_pool, OBST_X, PH,
                              &obst_stable, &obst_top_row, depth_obstructed,
                              1u, MATERIAL_LIQUID_DEPTH_BAND);
    mirror_local_depth_column(&fx.obst_pool, CLEAR_X, PH,
                              &clear_stable, &clear_top_row, depth_clear,
                              1u, MATERIAL_LIQUID_DEPTH_BAND);

    /* Sanity: the obstacle actually landed where this test built it, and
     * water survived on both sides of it - otherwise the rest of this test
     * proves nothing. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&fx.obst_pool, OBST_X, OBST_Y0)),
        "setup: the rock plug must still be stone after settling");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&fx.obst_pool, OBST_X, OBST_Y1 + 1)),
        "setup: water must still be there just below the plug, or this "
        "test is not exercising the case it claims to");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&fx.obst_pool, CLEAR_X, OBST_Y1 + 1)),
        "setup: the comparison column must be plain water all the way "
        "down, with nothing of its own to reset against");

    /* ABOVE the plug: both columns are uninterrupted water from the same
     * surface, so local depth must agree row for row - the two only differ
     * once the obstacle actually intervenes. */
    for (int y = 2; y < OBST_Y0; y++) {
        char why[160];
        snprintf(why, sizeof why,
                 "row %d is above the obstacle in both columns - local "
                 "depth must agree there, or this test is not isolating "
                 "the obstacle's own effect", y);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(depth_clear[y], depth_obstructed[y],
                                       why);
    }

    /* NEITHER reset point commits; both oscillate indefinitely. Asserting <=
     * 1 matches actual behaviour. */
    const int first_row_below_plug = OBST_Y1 + 1;
    TEST_ASSERT_TRUE_MESSAGE(depth_obstructed[first_row_below_plug] <= 1u,
        "the water cell right below the rock plug must show a freshly-reset "
        "local depth (0 if the reset commits, 1 if it is permanently held by "
        "the surface boundary competing for the same tracking slot - see "
        "col_stable_depth[]'s own KNOWN LIMITATION comment in app_sand.c) - "
        "not a continuation of the deep climb from above the rock");
    TEST_ASSERT_TRUE_MESSAGE(
        depth_clear[first_row_below_plug] >
            depth_obstructed[first_row_below_plug],
        "at the same row, the CLEAR column must show strictly more local "
        "depth than the obstructed one - it never reset, so it has been "
        "climbing since the real surface while the obstructed column just "
        "started over at the rock");

    /* And it keeps climbing from there rather than staying stuck at the
     * reset: a few more cells into the water below the plug, the
     * obstructed column's own local depth must have grown past its
     * post-reset value, exactly the way the clear column already has all
     * along - the reset is a restart, not a ceiling. */
    TEST_ASSERT_TRUE_MESSAGE(
        depth_obstructed[PH - 1] > depth_obstructed[first_row_below_plug],
        "local depth must keep climbing below the plug too, the same way "
        "it does above it - a reset back to 0 that never climbs again "
        "would mean the walk stopped working after the first obstacle, "
        "not merely reset at it");

}

/*=============================================================================
 * THE SINGLE RAY WALK - one shared mirror of app_sand.c's replacement for
 * the two axis-aligned walks (col_stable_depth[]/row_stable_depth[] and
 * their max combiner) mirror_local_depth_column() above still pins at
 * gx == 0. See LOCAL DEPTH's own top comment in app_sand.c for the full
 * account of why the shadow needed to follow gravity, the six-tilt bearing
 * table that proved it did not, and the two regimes this walks between.
 *
 * ONE mirror now, not one per axis - every test below that needs the walk
 * at a GENERAL gravity angle (not gx == 0, where mirror_local_depth_column()
 * alone is already exact) calls THIS, rather than each duplicating the same
 * row-major recurrence. Callers own all persisting state (`ray_walk_state_t`
 * below), matching the real file-static local_depth_row_a[]/local_depth_
 * row_b[]/local_depth_top_row[]/local_depth_prev_cy's own persistence
 * across frames - a fresh state (`ray_walk_state_reset()`: zeroed counts,
 * "untracked" debounce keys, and no row described by prev_row[] yet)
 * matches BSS-zero-then-untracked, exactly like the real statics start. */

/* Sized for this suite's own test grids, not the device's GRID_W_MAX - see
 * each test's own W constant for the actual grid width it uses; comfortably
 * larger than every one of them. */
#define RAY_WALK_STATE_W 128

/* prev_cy mirrors app_sand.c's local_depth_prev_cy for real neighbour
 * detection. RAY_WALK_NO_ROW is LOCAL_DEPTH_NO_ROW: -2, not -1, because -1
 * can be a genuine value. See local_depth_prev_cy's comment for details. */
#define RAY_WALK_NO_ROW (-2)

typedef struct {
    uint8_t cur_row[RAY_WALK_STATE_W];
    uint8_t prev_row[RAY_WALK_STATE_W];
    uint8_t top_row[RAY_WALK_STATE_W];
    int     prev_cy;
    bool    ignore_chain_break;
} ray_walk_state_t;

static void ray_walk_state_reset(ray_walk_state_t *st)
{
    memset(st->cur_row, 0, sizeof st->cur_row);
    memset(st->prev_row, 0, sizeof st->prev_row);
    memset(st->top_row, 255, sizeof st->top_row);
    st->prev_cy = RAY_WALK_NO_ROW;
    st->ignore_chain_break = false;
}

static void mirror_ray_walk_row(sand_t *g, int cy, int grid_w, int grid_h,
                                bool vertical_dominant, bool v_reverse,
                                bool h_reverse, unsigned ax, unsigned ay,
                                unsigned scale_q8, unsigned ceiling,
                                ray_walk_state_t *st, unsigned depth_out[])
{
    const int vdir = v_reverse ? -1 : 1;
    const int hdir = h_reverse ? -1 : 1;
    const int ysign = v_reverse ? 1 : -1;
    const int surf_cy = cy - vdir;
    /* paint_row_n()'s own local_depth_chain_ok, once per row. */
    const bool chain_ok = st->ignore_chain_break || (st->prev_cy == surf_cy);

    int row_step = 0;
    if (vertical_dominant && ay > 0u) {
        const int xsign = h_reverse ? 1 : -1;
        const int n = (vdir > 0) ? cy : (grid_h - 1 - cy);
        const int cum_n  = (int)(((long)(n)     * (long)ax) / (long)ay);
        const int cum_n1 = (int)(((long)(n + 1) * (long)ax) / (long)ay);
        row_step = xsign * (cum_n1 - cum_n);
    }

    int herr = 0;
    const int cx_first = h_reverse ? grid_w - 1 : 0;
    const int cx_step  = h_reverse ? -1 : 1;

    for (int i = 0; i < grid_w; i++) {
        const int cx = cx_first + i * cx_step;
        const cell_t here = sand_at(g, cx, cy);
        const bool here_liquid = !CELL_IS_EMPTY(here) &&
            material_of(here)->kind == KIND_LIQUID;

        int step = 0;
        if (!vertical_dominant) {
            herr += (int)ay;
            if (ax > 0u && herr >= (int)ax) {
                herr -= (int)ax;
                step = ysign;
            }
        }

        const int qx = vertical_dominant ? (cx + row_step) : (cx - hdir);
        const bool qx_ok = (qx >= 0 && qx < grid_w);
        const bool cross_row = vertical_dominant || (step != 0);
        const bool row_ok = !cross_row || (surf_cy >= 0 && surf_cy < grid_h);
        const cell_t src_cell = (qx_ok && row_ok)
            ? sand_at(g, qx, cross_row ? surf_cy : cy) : (cell_t)0;
        const unsigned src_count = (qx_ok && row_ok)
            ? (cross_row ? st->prev_row[qx] : st->cur_row[qx]) : 0u;
        const bool same = here_liquid && qx_ok && row_ok &&
            (CELL_MATERIAL(src_cell) == CELL_MATERIAL(here));

        unsigned count;
        if (!here_liquid) {
            count = 0u;
        } else if (same) {
            count = src_count < ceiling ? src_count + 1u : ceiling;
            if (!vertical_dominant) {
                st->top_row[cx] = 255u;
            }
        } else {
            const bool committed = vertical_dominant
                ? (st->top_row[cx] == (uint8_t)cy)
                : (st->top_row[cx] != 255u);
            if (committed) {
                count = 0u;
            } else {
                /* paint_row_n()'s own `carry`: a HOLD may only climb from
                 * the neighbour's count when the buffer really holds that
                 * neighbour's count. */
                const unsigned carry = (cross_row && !chain_ok) ? 0u : src_count;
                count = carry < ceiling ? carry + 1u : ceiling;
                st->top_row[cx] = vertical_dominant ? (uint8_t)cy : 0u;
            }
        }
        st->cur_row[cx] = (uint8_t)count;

        const unsigned depth_raw = (count * scale_q8) >> 8;
        depth_out[cx] = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;
    }

    for (int i = 0; i < RAY_WALK_STATE_W; i++) {
        const uint8_t t = st->cur_row[i];
        st->cur_row[i] = st->prev_row[i];
        st->prev_row[i] = t;
    }
    st->prev_cy = cy;
}

/* From (gx, gy), the same three per-frame facts update_local_depth_gravity()
 * computes in app_sand.c - `vertical_dominant`, `scale_q8`, and the two scan
 * reverse flags (returned via out-parameters since C has no multiple return
 * values worth the struct ceremony here). */
static void ray_walk_frame_facts(int gx, int gy, bool *vertical_dominant,
                                 bool *v_reverse, bool *h_reverse,
                                 unsigned *ax, unsigned *ay,
                                 unsigned *scale_q8)
{
    *ax = (unsigned)(gx < 0 ? -gx : gx);
    *ay = (unsigned)(gy < 0 ? -gy : gy);
    const int len = im_len(gx, gy);
    *vertical_dominant = (*ay >= *ax);
    const unsigned dom_axis = *vertical_dominant ? *ay : *ax;
    *scale_q8 = dom_axis ? (256u * (unsigned)len) / dom_axis : 256u;
    *v_reverse = (gy < 0);
    *h_reverse = (gx < 0);
}

/*=============================================================================
 * THE COMBINER'S REPLACEMENT HAS NO JUMP EITHER - kept its old name and its
 * old top comment's own history (below), because it still pins the same
 * property against whichever mechanism currently ships: hysteresis fixed
 * frequency, the blend and then the max combiner fixed severity, and now a
 * single ray walk removes the discrete axis choice's OWN severity problem
 * (an axis-aligned shadow) that neither of those two fixed - see LOCAL
 * DEPTH's own top comment in app_sand.c, "THIS IS NOT SHAPE (1) AGAIN", for
 * why a regime switch is safe here where shape (1)'s own axis switch was
 * not: both regimes measure the SAME quantity (distance along the gravity
 * ray), unlike shape (1)'s two sides, which measured two different
 * quantities related to the true depth by two different, angle-dependent
 * factors - so there is no discrete jump in the reported VALUE for this
 * test to catch a regression back into, only a discrete change in
 * bookkeeping mechanics.
 *
 * Modelled the same way every earlier shape's own fix was modelled: sweep
 * GRAVITY (not the grid, which stays fixed throughout) through the same
 * 30-to-60-degree range straddling the 45-degree tie point, one coherent
 * full-grid pass per sample (every row painted once, in the correct
 * surface-first order - not sparse, so this checks the WALK's own
 * continuity, not the sparse-repaint staleness a separate test already
 * covers), and check the rendered luminance at one fixed cell never moves
 * by more than a fraction of the ramp's own full span in a single 3-degree
 * step. */
static const struct { int gx, gy; } BLEND_SWEEP[] = {
    { 500, 866 },   /* 30 degrees */
    { 545, 839 },   /* 33 degrees */
    { 588, 809 },   /* 36 degrees */
    { 629, 777 },   /* 39 degrees */
    { 669, 743 },   /* 42 degrees */
    { 707, 707 },   /* 45 degrees - gx == gy, the exact tie point */
    { 743, 669 },   /* 48 degrees */
    { 777, 629 },   /* 51 degrees */
    { 809, 588 },   /* 54 degrees */
    { 839, 545 },   /* 57 degrees */
    { 866, 500 },   /* 60 degrees */
};
#define BLEND_SWEEP_N (sizeof BLEND_SWEEP / sizeof BLEND_SWEEP[0])

/* A narrow, deep, plain rectangular pool, no obstacle - a genuinely
 * saturated column at every angle in the sweep (BLEND_POOL_H rows, well
 * past MATERIAL_LIQUID_DEPTH_BAND), so this test measures the WALK's own
 * continuity independent of any obstacle-shadow behaviour (a separate test
 * covers that). */
enum { BLEND_POOL_W = 4, BLEND_POOL_H = 40 };
enum { BLEND_TEST_CX = 0, BLEND_TEST_CY = BLEND_POOL_H - 1 };
static uint8_t blend_pool_cells[BLEND_POOL_W * BLEND_POOL_H];

static void test_the_blend_has_no_jump_crossing_45_degrees(void)
{
    enum { PW = BLEND_POOL_W, PH = BLEND_POOL_H };
    sand_init(&fx.blend_pool, blend_pool_cells, PW, PH, 9001u);

    for (int y = 2; y < PH; y++) {
        for (int x = 0; x < PW; x++) {
            sand_set(&fx.blend_pool, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    int lum[BLEND_SWEEP_N];
    for (size_t i = 0; i < BLEND_SWEEP_N; i++) {
        const int gx = BLEND_SWEEP[i].gx, gy = BLEND_SWEEP[i].gy;
        bool vdom, vrev, hrev;
        unsigned ax, ay, scale_q8;
        ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

        ray_walk_state_t st;
        ray_walk_state_reset(&st);
        const bool asc = !vrev;
        unsigned depth = 0;
        for (int r = 0; r < PH; r++) {
            const int cy = asc ? r : (PH - 1 - r);
            unsigned row_depth[RAY_WALK_STATE_W];
            mirror_ray_walk_row(&fx.blend_pool, cy, PW, PH, vdom, vrev, hrev,
                                ax, ay, scale_q8, MATERIAL_LIQUID_DEPTH_BAND,
                                &st, row_depth);
            if (cy == BLEND_TEST_CY) {
                depth = row_depth[BLEND_TEST_CX];
            }
        }

        gfx_color_t out[3];
        material_colours(CELL_MAKE(MAT_WATER, MASS_MAX), 0u, 0u, depth, out);
        lum[i] = panel_luminance(out[0]);
    }

    /* THE BOUND: derived from the ramp's own full span (depth 0 versus
     * depth 255), not a hand-picked luminance number - robust to the ramp
     * ever being retuned. Three quarters of that span is the threshold,
     * matching every earlier shape's own version of this test. */
    gfx_color_t shallow[3], deep[3];
    material_colours(CELL_MAKE(MAT_WATER, MASS_MAX), 0u, 0u, 0u, shallow);
    material_colours(CELL_MAKE(MAT_WATER, MASS_MAX), 0u, 0u, 255u, deep);
    const int full_span = panel_luminance(shallow[0]) - panel_luminance(deep[0]);
    const int max_step = (full_span * 3) / 4;

    for (size_t i = 1; i < BLEND_SWEEP_N; i++) {
        const int step = lum[i] - lum[i - 1];
        const int abs_step = step < 0 ? -step : step;
        char why[300];
        snprintf(why, sizeof why,
                 "luminance jumped %d between two 3-degree gravity steps "
                 "(sample %zu -> %zu), more than three quarters of the "
                 "ramp's own full %d-luminance span - the ray walk must "
                 "crossfade across the regime switch, not pop, however far "
                 "it is from the 45-degree tie point",
                 abs_step, i - 1, i, full_span);
        TEST_ASSERT_TRUE_MESSAGE(abs_step <= max_step, why);
    }
}

/* PAINT_ROW_N()'S DEBOUNCED WALK - the flicker half of the pour/flicker fix.
 *
 * A first attempt gated the ENTIRE depth gradient behind
 * sand_block_settled() - a block only settles once nothing in the whole
 * SAND_BLOCK_W x SAND_BLOCK_H block moved for a step. Verified on device and
 * reverted: a real hand never holds the board perfectly still, so that bit
 * rarely latches at all under real handling, and the gradient was gone
 * almost everywhere rather than merely steadied - "we lost the bands" was
 * the report, not "the flicker is gone." A literal per-CELL debounce history
 * was considered next and is not affordable either: one byte per cell over
 * the finest-quality grid is 41,216 bytes, the exact size of the grid buffer
 * itself, on a device whose free heap has been observed as low as 2,364
 * bytes mid-session.
 *
 * A SECOND attempt shipped col_stable_depth[]/col_pending_reset[] - a plain
 * per-column accumulator running in parallel with col_local_depth[], same
 * row-to-row chaining within a frame's walk, committing a reset only after
 * two consecutive frames asked for it. It was pinned with a unit test
 * exactly as deterministic as the one below - and STILL failed on device,
 * for a reason that single-state unit test could not see: real per-column
 * state is read and written ONCE PER ROW, chained across rows within one
 * frame, and two EMPTY cells compare as "same material" the way
 * col_local_depth[] already harmlessly tolerates - harmless there because
 * nothing reads a raw depth computed over empty space, but this array's
 * value at a reset IS read, so any run of open air above a pool's real
 * boundary climbed the accumulator through that air before the walk ever
 * reached real water, and it never got a chance to reset between frames
 * either - saturating to 255 within a couple of frames for every column
 * with open air above it, which is most of them. Confirmed by comparing
 * device screenshots: the vertical-dominant case (the one this mechanism
 * gated) went flat; the horizontal-dominant case (still the untouched raw
 * walk) kept its bands.
 *
 * THIS THIRD VERSION - col_stable_depth[]/col_top_row[] (app_sand.c) - only
 * ever accumulates through LIQUID cells (a non-liquid cell resets it to a
 * clean 0, so a run of open air can no longer pollute anything), and keys
 * the commit decision by ROW INDEX rather than column-chain position:
 * col_top_row[cx] holds which row most recently asked for a reset, and a
 * NEW request only commits if it comes from that SAME row again. See
 * col_stable_depth[]'s own comment in app_sand.c for the full mechanism and
 * its one accepted limitation (a column with two reset points - an
 * obstacle inside an open pool - has them compete for the single tracking
 * slot).
 *
 * TWO TESTS below, not one - deliberately, after what the single-state test
 * above already proved is not enough on its own:
 *
 *   - test_a_same_row_reset_commits_but_a_different_row_does_not pins the
 *     row-keyed DECISION in isolation, the same idiom this file's LOCAL
 *     DEPTH story has always used for pinning a state-machine decision on
 *     its own, apart from the integration test that exercises it for real.
 *
 *   - test_the_debounce_survives_open_air_above_the_pool is the regression
 *     guard for the SPECIFIC integration bug that shipped: it walks a real
 *     column, top to bottom, through real open air, exactly the way
 *     paint_row_n() actually calls this logic - one call per frame, state
 *     persisting across calls the way the real arrays persist across
 *     frames - because that is precisely the shape of call the previous
 *     unit test did not exercise. */

/* Mirrors app_sand.c's decision; `stable`/`top_row` IN/OUT, does not touch
 * the grid. */
static unsigned mirror_debounce_decide(unsigned char *stable,
                                       unsigned char *top_row,
                                       bool same_material, int cy)
{
    unsigned stable_depth;
    if (same_material) {
        stable_depth = *stable < 255u ? *stable + 1u : 255u;
    } else if (*top_row == (unsigned char)cy) {
        stable_depth = 0u;
    } else {
        stable_depth = *stable < 255u ? *stable + 1u : 255u;
        *top_row = (unsigned char)cy;
    }
    *stable = (unsigned char)stable_depth;
    return stable_depth;
}

static void test_a_same_row_reset_commits_but_a_different_row_does_not(void)
{
    unsigned char stable = 0, top_row = 255;

    unsigned last = 0;
    for (int i = 0; i < 10; i++) {
        last = mirror_debounce_decide(&stable, &top_row, true, 0);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(10u, last,
        "setup: ten consecutive same_material steps must climb to depth "
        "10, or this test is not starting from a real climbed state");

    /* Row 7 asks for a reset for the first time - held, not committed,
     * and now tracked. */
    const unsigned first_ask = mirror_debounce_decide(&stable, &top_row,
                                                       false, 7);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(11u, first_ask,
        "a row asking for a reset for the FIRST time must be held, keeping "
        "the old climbed value, not committed immediately");

    /* A DIFFERENT row (8) asks next - this must ALSO be held, not treated
     * as confirming row 7's request; row 7 and row 8 are different rows,
     * and conflating them is exactly the bug the row-keyed design exists
     * to avoid (the previous, column-chained design could not tell them
     * apart). */
    const unsigned different_row = mirror_debounce_decide(&stable, &top_row,
                                                           false, 8);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(12u, different_row,
        "a DIFFERENT row asking for a reset must be held too, not treated "
        "as confirmation of the previous row's own pending request");

    /* Row 8 asks AGAIN - now it matches what is tracked, and commits. */
    const unsigned second_ask = mirror_debounce_decide(&stable, &top_row,
                                                        false, 8);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, second_ask,
        "the SAME row asking for a reset a second time must commit - a "
        "real, lasting boundary must still show up");

    const unsigned after_commit = mirror_debounce_decide(&stable, &top_row,
                                                          true, 9);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, after_commit,
        "the walk must resume climbing normally after a committed reset");
}

/* Mirrors app_sand.c's debounce logic on top of mirror_local_depth_column()'s
 * walk; state persists across calls, driven by paint_row_n(). */
static void mirror_debounced_depth_column(sand_t *g, int cx, int h,
                                          unsigned char *stable,
                                          unsigned char *top_row,
                                          unsigned depth_out[])
{
    for (int cy = 0; cy < h; cy++) {
        const cell_t here  = sand_at(g, cx, cy);
        const cell_t above = sand_at(g, cx, cy - 1);
        const bool same = CELL_MATERIAL(above) == CELL_MATERIAL(here);
        unsigned stable_depth;

        if (material_of(here)->kind != KIND_LIQUID) {
            stable_depth = 0u;
        } else if (same) {
            stable_depth = *stable < 255u ? *stable + 1u : 255u;
        } else if (*top_row == (unsigned char)cy) {
            stable_depth = 0u;
        } else {
            stable_depth = *stable < 255u ? *stable + 1u : 255u;
            *top_row = (unsigned char)cy;
        }
        *stable = (unsigned char)stable_depth;
        depth_out[cy] = stable_depth;
    }
}

#define DEBOUNCE_TEST_W 4
#define DEBOUNCE_TEST_H 20
static uint8_t debounce_test_cells[DEBOUNCE_TEST_W * DEBOUNCE_TEST_H];

/* DIAGNOSTIC PROBE checks if boundary moves NEW row consecutively, unlike
 * BLINK. Ensures col_top_row[cx] never repeats, so never COMMITS. Prevents
 * HOLD from reading deep, saturated values. Reproduced without gravity or
 * sand_step(). */
static void test_a_continuously_moving_boundary_does_not_run_away(void)
{
    enum { CX = 1, START_TOP = 5, DRAIN_ROWS = 8 };
    sand_init(&fx.debounce_test, debounce_test_cells, DEBOUNCE_TEST_W,
             DEBOUNCE_TEST_H, 2u);
    for (int y = START_TOP; y < DEBOUNCE_TEST_H; y++) {
        sand_set(&fx.debounce_test, CX, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    unsigned char stable = 0, top_row = 255;
    unsigned depth[DEBOUNCE_TEST_H];

    /* Settle once, exactly like the sibling test below, before the drain
     * begins. */
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);

    /* The drain: the boundary recedes by exactly one row every frame, for
     * several frames running - never landing on the same row twice, so
     * col_top_row[] can never confirm a commit for any of them. */
    for (int i = 0; i < DRAIN_ROWS; i++) {
        sand_erase(&fx.debounce_test, CX, START_TOP + i, 0);
        mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                      &stable, &top_row, depth);

        const int new_top = START_TOP + i + 1;
        char why[384];
        snprintf(why, sizeof why,
            "after %d frame(s) of a boundary receding one row per frame "
            "(never settling long enough to commit), the new boundary "
            "(row %d) reads depth %u - if this climbs unbounded rather "
            "than staying small, HOLD is compounding across frames "
            "instead of tracking the true raw depth, and a dead-zone "
            "freeze grabbing this column mid-drain would lock in that "
            "wrong, saturated value", i + 1, new_top, depth[new_top]);
        TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(3u, depth[new_top], why);
    }
}

/* THE ACTUAL REGRESSION: a pool with real open air above it (every real
 * pool has this - the whole grid above the waterline), walked frame by
 * frame, exactly the shape that broke the previous version. */
static void test_the_debounce_survives_open_air_above_the_pool(void)
{
    enum { CX = 1, WATER_TOP = 5 };
    sand_init(&fx.debounce_test, debounce_test_cells, DEBOUNCE_TEST_W,
             DEBOUNCE_TEST_H, 1u);
    for (int y = WATER_TOP; y < DEBOUNCE_TEST_H; y++) {
        sand_set(&fx.debounce_test, CX, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    unsigned char stable = 0, top_row = 255;
    unsigned depth[DEBOUNCE_TEST_H];

    /* FRAME 1: first-ever paint. The boundary gets at most a one-frame
     * cold-start grace, not a value climbed through the five empty rows
     * above it - THE EXACT BUG the previous version shipped with. */
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(1u, depth[WATER_TOP],
        "the boundary's first-ever reading must be at most 1 (the accepted "
        "cold-start grace), not a value saturated by climbing through the "
        "open air above it");

    /* FRAME 2: nothing changed. The boundary must now be fully committed -
     * exactly 0 - and every row below it must show a small, correctly
     * climbed depth, not something still recovering from a saturated
     * start. */
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    for (int y = WATER_TOP; y < DEBOUNCE_TEST_H; y++) {
        char why[144];
        snprintf(why, sizeof why,
            "row %d must read exactly %d once settled - not a value still "
            "recovering from a run through open air above the pool", y,
            y - WATER_TOP);
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)(y - WATER_TOP), depth[y],
            why);
    }

    /* THE BLINK: the topmost cell vanishes for exactly one frame, then
     * comes back - the reported flicker's own shape. Every row below it
     * must be unaffected. */
    unsigned settled[DEBOUNCE_TEST_H];
    memcpy(settled, depth, sizeof depth);

    sand_erase(&fx.debounce_test, CX, WATER_TOP, 0);
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    for (int y = WATER_TOP + 1; y < DEBOUNCE_TEST_H; y++) {
        char why[160];
        snprintf(why, sizeof why,
            "row %d changed during a ONE-FRAME blink of the cell above it "
            "- the debounce must absorb this, not let it cascade", y);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(settled[y], depth[y], why);
    }

    sand_set(&fx.debounce_test, CX, WATER_TOP, CELL_MAKE(MAT_WATER, MASS_MAX));
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);   /* revert */
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);   /* settle */
    for (int y = WATER_TOP; y < DEBOUNCE_TEST_H; y++) {
        char why[160];
        snprintf(why, sizeof why,
            "row %d must be back to its settled depth once the blink "
            "reverts and one further frame has confirmed it", y);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(settled[y], depth[y], why);
    }

    /* A REAL, LASTING change - the topmost cell empties and STAYS empty -
     * must still commit within a couple of frames, or genuine changes
     * would be hidden forever, not just one-frame blinks. */
    sand_erase(&fx.debounce_test, CX, WATER_TOP, 0);
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    mirror_debounced_depth_column(&fx.debounce_test, CX, DEBOUNCE_TEST_H,
                                  &stable, &top_row, depth);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, depth[WATER_TOP + 1],
        "a boundary that genuinely moved - the old top cell erased and not "
        "coming back - must commit to its new position within a couple of "
        "frames, not be absorbed the way a one-frame blink is");
}

static void mirror_debounced_depth_row(sand_t *g, int cy, int w,
                                       unsigned char *stable,
                                       unsigned char *top_col,
                                       unsigned depth_out[])
{
    for (int cx = 0; cx < w; cx++) {
        const cell_t here = sand_at(g, cx, cy);
        const cell_t left  = sand_at(g, cx - 1, cy);
        const bool same = CELL_MATERIAL(left) == CELL_MATERIAL(here);
        unsigned stable_depth;

        if (material_of(here)->kind != KIND_LIQUID) {
            stable_depth = 0u;
        } else if (same) {
            stable_depth = *stable < 255u ? *stable + 1u : 255u;
        } else if (*top_col == (unsigned char)cx) {
            stable_depth = 0u;
        } else {
            stable_depth = *stable < 255u ? *stable + 1u : 255u;
            *top_col = (unsigned char)cx;
        }
        *stable = (unsigned char)stable_depth;
        depth_out[cx] = stable_depth;
    }
}

#define HDEBOUNCE_TEST_W 20
#define HDEBOUNCE_TEST_H 4
static uint8_t hdebounce_test_cells[HDEBOUNCE_TEST_W * HDEBOUNCE_TEST_H];

static void test_the_horizontal_debounce_survives_open_air_beside_the_pool(void)
{
    enum { CY = 1, WATER_LEFT = 5 };
    sand_init(&fx.hdebounce_test, hdebounce_test_cells, HDEBOUNCE_TEST_W,
             HDEBOUNCE_TEST_H, 1u);
    for (int x = WATER_LEFT; x < HDEBOUNCE_TEST_W; x++) {
        sand_set(&fx.hdebounce_test, x, CY, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    unsigned char stable = 0, top_col = 255;
    unsigned depth[HDEBOUNCE_TEST_W];

    /* FRAME 1: first-ever paint. The boundary gets at most a one-frame
     * cold-start grace, not a value climbed through the five empty columns
     * beside it - the same bug class the vertical test's own frame 1 guards
     * against. */
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);
    TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(1u, depth[WATER_LEFT],
        "the boundary's first-ever reading must be at most 1 (the accepted "
        "cold-start grace), not a value saturated by climbing through the "
        "open air beside it");

    /* FRAME 2: nothing changed. The boundary must now be fully committed -
     * exactly 0 - and every column past it must show a small, correctly
     * climbed depth, not something still recovering from a saturated
     * start. */
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);
    for (int x = WATER_LEFT; x < HDEBOUNCE_TEST_W; x++) {
        char why[160];
        snprintf(why, sizeof why,
            "column %d must read exactly %d once settled - not a value "
            "still recovering from a run through open air beside the pool",
            x, x - WATER_LEFT);
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)(x - WATER_LEFT), depth[x],
            why);
    }

    unsigned settled[HDEBOUNCE_TEST_W];
    memcpy(settled, depth, sizeof depth);

    sand_erase(&fx.hdebounce_test, WATER_LEFT, CY, 0);
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);
    for (int x = WATER_LEFT + 1; x < HDEBOUNCE_TEST_W; x++) {
        char why[224];
        snprintf(why, sizeof why,
            "column %d changed during a ONE-FRAME blink of the cell beside "
            "it - the horizontal debounce must absorb this, not let it "
            "cascade into the blended depth the way an undebounced "
            "h_running_depth used to", x);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(settled[x], depth[x], why);
    }

    sand_set(&fx.hdebounce_test, WATER_LEFT, CY, CELL_MAKE(MAT_WATER, MASS_MAX));
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);   /* revert */
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);   /* settle */
    for (int x = WATER_LEFT; x < HDEBOUNCE_TEST_W; x++) {
        char why[160];
        snprintf(why, sizeof why,
            "column %d must be back to its settled depth once the blink "
            "reverts and one further frame has confirmed it", x);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(settled[x], depth[x], why);
    }

    /* A REAL, LASTING change - the leftmost cell empties and STAYS empty -
     * must still commit within a couple of frames, or genuine changes would
     * be hidden forever, not just one-frame blinks. */
    sand_erase(&fx.hdebounce_test, WATER_LEFT, CY, 0);
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);
    mirror_debounced_depth_row(&fx.hdebounce_test, CY, HDEBOUNCE_TEST_W,
                               &stable, &top_col, depth);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, depth[WATER_LEFT + 1],
        "a boundary that genuinely moved - the old leftmost cell erased and "
        "not coming back - must commit to its new position within a couple "
        "of frames, not be absorbed the way a one-frame blink is");
}

/*=============================================================================
 * mark_depth_band() (sand_priv.h) - the pour-staleness half of the same fix.
 *
 * The reported bug: water that looked settled kept showing its OLD,
 * shallower depth shading after more water was poured on top of it,
 * because nothing below the pour ever touched dirty_rows - only the very
 * top of the reservoir, where mass actually moved, marked anything at all.
 * pour_into() (sand_liquid.c) now reports whether the cell it just filled
 * was previously empty - the only event that can move where a puddle's
 * surface sits - and equalise_one_cell() uses that to call
 * mark_depth_band() a bounded run of rows around the new surface, so the
 * render catches up without repainting the whole reservoir. give_mass()
 * (move_liquid_grain()'s per-grain fall, the hottest path in the
 * simulation) deliberately does NOT call it - see that call site's own
 * comment in sand_liquid.c for why.
 *===========================================================================*/
#define DEPTH_TEST_W 4
#define DEPTH_TEST_H 80
static uint8_t depth_test_cells[DEPTH_TEST_W * DEPTH_TEST_H];
static uint8_t depth_test_dirty[DEPTH_TEST_H];

static void test_pouring_onto_a_settled_pool_redirties_a_bounded_band_below(void)
{
    sand_init(&fx.depth_test, depth_test_cells, DEPTH_TEST_W, DEPTH_TEST_H, 99u);

    /* A deep reservoir, full width, so it starts already level and settles
     * in essentially one step - nothing here needs the settling itself to
     * be interesting, only what happens once it is poured onto. */
    const int fill_top = 10;
    for (int y = fill_top; y < DEPTH_TEST_H; y++) {
        for (int x = 0; x < DEPTH_TEST_W; x++) {
            sand_set(&fx.depth_test, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.depth_test, 0, 1000, 0);
    }

    uint8_t settled_snapshot[DEPTH_TEST_W * DEPTH_TEST_H];
    memcpy(settled_snapshot, depth_test_cells, sizeof settled_snapshot);

    sand_track_dirty_rows(&fx.depth_test, depth_test_dirty);
    memset(depth_test_dirty, 0, sizeof depth_test_dirty);

    /* The pour: new water dropped at the very top, well above the
     * reservoir's current surface. */
    for (int i = 0; i < 60; i++) {
        sand_spawn(&fx.depth_test, DEPTH_TEST_W / 2, 1, 1, MAT_WATER);
        sand_step(&fx.depth_test, 0, 1000, 0);
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&fx.depth_test, 0, 1000, 0);
    }

    /* The reservoir's NEW surface: the shallowest row that is now water in
     * EVERY column - the new top of the fully-flooded body, not a single
     * splashed cell still finding its way down. */
    int new_surface = -1;
    for (int y = 0; y < DEPTH_TEST_H; y++) {
        bool full_row = true;
        for (int x = 0; x < DEPTH_TEST_W; x++) {
            if (CELL_MATERIAL(sand_at(&fx.depth_test, x, y)) != MAT_WATER) {
                full_row = false;
                break;
            }
        }
        if (full_row) {
            new_surface = y;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(new_surface >= 0,
        "setup: the pour must actually produce a fully-flooded row, or "
        "this test is not exercising the case it claims to");
    TEST_ASSERT_TRUE_MESSAGE(new_surface < fill_top,
        "setup: the pour must raise the surface above where it started, "
        "or there is nothing here for mark_depth_band() to catch");

    const int near_row = new_surface + MATERIAL_LIQUID_DEPTH_BAND - 2;
    const int far_row  = fill_top + MATERIAL_LIQUID_DEPTH_BAND + 8;
    TEST_ASSERT_TRUE_MESSAGE(far_row < DEPTH_TEST_H,
        "setup: the fixture must be tall enough to hold a row outside the "
        "band too, or the 'bounded' half of this test proves nothing");
    TEST_ASSERT_TRUE_MESSAGE(near_row >= fill_top,
        "setup: near_row must fall inside the ORIGINAL, already-settled "
        "reservoir, or this is not the case the bug report was about");

    /* Mass conservation: a cell already at MASS_MAX has no room for more,
     * so nothing at or below the ORIGINAL settled surface can have
     * changed CONTENT at all - confirming any dirty mark down there is
     * mark_depth_band()'s doing, not the pour actually having reached
     * that deep. */
    for (int x = 0; x < DEPTH_TEST_W; x++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            settled_snapshot[near_row * DEPTH_TEST_W + x],
            depth_test_cells[near_row * DEPTH_TEST_W + x],
            "setup: a cell already at MASS_MAX before the pour cannot "
            "have changed content - if it did, this is not isolating "
            "mark_depth_band()'s own effect");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            settled_snapshot[far_row * DEPTH_TEST_W + x],
            depth_test_cells[far_row * DEPTH_TEST_W + x],
            "setup: same, for the control row outside the band");
    }

    TEST_ASSERT_TRUE_MESSAGE(depth_test_dirty[near_row] != 0,
        "a row within MATERIAL_LIQUID_DEPTH_BAND of the new surface, "
        "whose own content never changed, must still be marked dirty "
        "after a pour raised the surface above it - otherwise its "
        "rendered depth shading is left stale, exactly the reported bug");
    TEST_ASSERT_TRUE_MESSAGE(depth_test_dirty[far_row] == 0,
        "a row well outside MATERIAL_LIQUID_DEPTH_BAND must NOT be "
        "marked dirty just because a pour happened above it - bounding "
        "the mark is what keeps a pour from repainting a reservoir far "
        "deeper than any shading could possibly need to change");
}


/* test_water_interior_hazes_toward_fog_colour, test_only_water_hazes_its_
 * interior, test_the_wave_table_matches_its_formula, test_wave_bands_vary_
 * across_depth and test_wave_bands_drift_with_phase, plus TEST_DEPTH_RANGE
 * and TEST_WAVE_FRAC_BITS, used to live here - the whole "WATER'S FOG"
 * section, covering the haze blend and the wave-table bands stacked on top
 * of local depth for water alone.
 *
 * REMOVED, not merely updated: on the device the fog blend's own arithmetic
 * (tuned for the OLD screen-position depth, which legitimately spanned the
 * full 0-255 range) turned out to be pinned near-maximum haze across every
 * local depth a real pool in this app ever reaches, so water lost its
 * saturated blue almost entirely - a genuine bug, not a taste call. The wave
 * bands had their own, separate problem: local depth commits to a single
 * dominant axis with a hard, unsmoothed switch between vertical and
 * horizontal, and the bands rode straight over that seam, reading as rigid
 * columns rather than an organic gradient. Water's interior now uses
 * EXACTLY the same plain shade-index shift oil, lava and acid always have -
 * see material_colours()'s own comment on the liquid interior branch, and
 * DEPTH_SATURATE_CELLS's comment just above it for the scale fix that came
 * out of the same round. The wave-table technique and the fog-blend
 * arithmetic both still exist, untouched, on the water-wave-fog-depth-
 * banked branch, for anyone who wants to revisit that approach or reuse a
 * piece of it for a different effect later.
 *
 * test_deepest_water_interior_is_exactly_the_body_colour was broadened
 * rather than deleted outright - see test_every_liquid_interior_is_
 * exactly_the_body_colour_when_saturated below, next to test_a_liquid_
 * interior_is_shaded_by_depth, which is now the shared mechanism it
 * belongs beside rather than a water-only fog boundary. */

static void test_every_liquid_interior_is_exactly_the_body_colour_when_saturated(void)
{
    static const uint8_t liquids[] = { MAT_WATER, MAT_OIL, MAT_LAVA,
                                       MAT_ACID };
    const gfx_color_t *pal = material_palette();

    for (unsigned k = 0; k < sizeof liquids / sizeof liquids[0]; k++) {
        const uint8_t id = liquids[k];
        const gfx_color_t body = pal[CELL_MAKE(id, MASS_MAX)];

        gfx_color_t col[3];
        material_colours(CELL_MAKE(id, MASS_MAX), 0u, 0u, 255u, col);

        char why[192];
        snprintf(why, sizeof why,
                 "%s's deepest interior cell must paint EXACTLY the plain "
                 "body colour, with no shift left at all, now that every "
                 "liquid shares the same saturating shade-index mechanism",
                 material_by_id((material_id_t)id)->name);
        TEST_ASSERT_EQUAL_MESSAGE(body, col[0], why);
    }
}

/* THE REGRESSION THIS WHOLE ROUND EXISTS FOR: a REALISTIC shallow pool -
 * 10 to 20 cells deep, which is what nearly every visible pool in this app
 * actually reaches - must still show a MEANINGFUL luminance difference
 * between a cell near its own surface and one near its own bottom. This is
 * a stronger claim than test_a_liquid_interior_is_shaded_by_depth above,
 * which only proves depth 0 differs from depth 255 in the abstract - the
 * old `/255` divide agreed with that in principle (it does eventually
 * reach the deepest possible shade), it just took on the order of sixty
 * cells of local depth to cross even a single shade step, which pinned any
 * pool this app can actually build dead flat. This test builds a pool of
 * exactly that realistic depth and checks the difference is a real
 * fraction of the ramp's own full span, not merely present in the abstract.
 *
 * A plain pool via sand_set(), no obstacle and no stepping needed - this is
 * a claim about material_colours() given a realistic depth value, not
 * about the solver settling anything - and mirror_local_depth_column()
 * (defined above, for test_local_depth_follows_the_puddles_own_shape)
 * reads the REAL local depth back off the live grid, the same way
 * paint_row_n() would, rather than asserting anything about a hand-picked
 * depth number in isolation.
 *
 * The "meaningful" bar is a quarter of the ramp's own full span (depth 0
 * versus depth 255, computed here rather than hand-typed), not a
 * hand-picked luminance number: robust to the ramp ever being retuned,
 * and still clearly nonzero enough that the old, pinned-flat bug cannot
 * pass it by accident.
 *
 * PROVEN LOAD-BEARING, not merely written and trusted to catch anything:
 * temporarily restoring the old `(...) / 255` divide in material.c's
 * depth_q calculation turns this test RED - the surface and bottom rows
 * land on the identical quantised shade (verified: both idx 12 at depths 1
 * and 17), reproducing the reported bug exactly. Putting back the
 * `/ DEPTH_SATURATE_CELLS` divide turns it GREEN again (idx 12 versus idx
 * 14, a real two-step gap). */
enum { SHALLOW_POOL_W = 4, SHALLOW_POOL_H = 20 };
static uint8_t shallow_pool_cells[SHALLOW_POOL_W * SHALLOW_POOL_H];

static void test_a_shallow_puddle_still_shows_real_darkening(void)
{
    enum { PW = SHALLOW_POOL_W, PH = SHALLOW_POOL_H };
    sand_init(&fx.shallow_pool, shallow_pool_cells, PW, PH, 777u);

    /* Rows 0-1 stay empty (the surface); rows 2..PH-1 are water - 18 rows,
     * squarely inside the 10-20 cell range measured as broken. */
    for (int y = 2; y < PH; y++) {
        for (int x = 0; x < PW; x++) {
            sand_set(&fx.shallow_pool, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    enum { NEAR_SURFACE_Y = 3, NEAR_BOTTOM_Y = PH - 1 };
    unsigned depth[PH];
    unsigned char stable = 0, top_row = 255;
    mirror_local_depth_column(&fx.shallow_pool, 0, PH, &stable, &top_row, depth,
                              1u, MATERIAL_LIQUID_DEPTH_BAND);

    const unsigned near_surface_depth = depth[NEAR_SURFACE_Y];
    const unsigned near_bottom_depth  = depth[NEAR_BOTTOM_Y];
    TEST_ASSERT_TRUE_MESSAGE(
        near_bottom_depth >= near_surface_depth + 10 &&
            near_bottom_depth <= near_surface_depth + 20,
        "setup: these two rows must actually be 10-20 cells of local depth "
        "apart, or this test is not exercising the range that was measured "
        "as broken");

    const cell_t c = CELL_MAKE(MAT_WATER, MASS_MAX);
    gfx_color_t near_surface_col[3], near_bottom_col[3];
    gfx_color_t shallowest[3], deepest[3];
    material_colours(c, 0u, 0u, near_surface_depth, near_surface_col);
    material_colours(c, 0u, 0u, near_bottom_depth, near_bottom_col);
    material_colours(c, 0u, 0u, 0u, shallowest);
    material_colours(c, 0u, 0u, 255u, deepest);

    const int near_surface_lum = panel_luminance(near_surface_col[0]);
    const int near_bottom_lum  = panel_luminance(near_bottom_col[0]);
    const int full_span =
        panel_luminance(shallowest[0]) - panel_luminance(deepest[0]);

    char why[384];
    snprintf(why, sizeof why,
             "a realistic shallow pool (local depth %u near the surface, "
             "%u near the bottom) must show a MEANINGFUL luminance "
             "difference between the two (%d vs %d, against a full ramp "
             "span of only %d) - the old /255 divide left exactly this "
             "range pinned flat, which is the bug this whole round exists "
             "to fix", near_surface_depth, near_bottom_depth,
             near_surface_lum, near_bottom_lum, full_span);
    TEST_ASSERT_TRUE_MESSAGE(
        (near_surface_lum - near_bottom_lum) * 4 >= full_span, why);
}

/*=============================================================================
 * LOCAL DEPTH'S WAKE TICK - A SETTLED EDGE MUST NOT FLICKER STALE TO FRESH
 *
 * row_has_liquid[]'s own regression: the periodic wake tick (LOCAL_DEPTH_
 * WAKE_MS, app_sand.c) exists purely to keep a settled, sleeping pool's
 * rendered local depth from going stale while gravity drifts under ordinary
 * hand wobble with nothing physically moving (see that mechanism's own long
 * comment in app_sand.c). An EARLIER version of that tick gated which rows
 * it force-repainted on row_has_liquid_interior[] - true only when a row
 * held a cell currently classified as liquid INTERIOR (mask == 0), not rim.
 * That gate reintroduced the exact staleness bug the tick exists to close,
 * one level down: ordinary grain-level settling noise flips a cell between
 * rim and interior constantly at any real liquid surface, so a row can read
 * as all-rim - no interior cell in it at all, at that instant - for
 * hundreds of consecutive frames if it sits at a wide, shallow pool's edge.
 * While that holds, the OLD gate skipped the row entirely: nothing refreshed
 * what its interior colour would be. The moment a cell in that row flipped
 * back to interior - again, ordinary edge noise - the very next wake tick
 * painted it fresh, replacing whatever was there with a value that could be
 * frozen from an arbitrarily distant past.
 *
 * REPRODUCED BELOW with a REALISTIC scene, not a hand-picked pathological
 * one: a ragged, wide-and-shallow pool (two separate pours plus a stub wall,
 * so the settled surface has real grain-level irregularity - the WIDTH is
 * what matters, since a wide-and-shallow pool is exactly the shape whose
 * horizontal local depth saturates while its vertical local depth stays
 * small, making the blend acutely sensitive to exactly which blend weight
 * was in effect the last time any given cell was painted), settling under
 * near-portrait gravity (gy dominant and stable, gx small and wobbling under
 * an ordinary hand-tremor model - a slow drift plus jitter, both small next
 * to gy), run for thousands of simulated frames while tracking the
 * DISPLAYED (last-painted, possibly stale) interior depth of every liquid
 * cell - exactly what a real screen would be showing.
 *
 * MEASURED, with the OLD (interior-only) gate: the mean displayed interior
 * depth sits stable at 23.671 (of a 24-cell saturating range) for over a
 * thousand frames, then COLLAPSES to 1.958 in a single frame - a 21.71-unit
 * swing out of a 24-unit range - before climbing back to 23.671 over the
 * next several dozen frames as the wake tick catches every affected row up
 * one at a time. Traced to a specific cell whose displayed value was frozen
 * at 24 from some earlier point; when finally repainted, the FRESH
 * computation (vdepth=1, hdepth=44, weight_h=3, nearly all weight on
 * vertical - correctly small, since gravity is near-portrait) is entirely
 * correct. The bug is that 24 was stale, and the correction lands as one
 * visible jump instead of a gradual one - this is the "sharp, rectangular
 * seams" symptom ef10262 already fixed once for the common case, surviving
 * in this one.
 *
 * MEASURED, with the FIX (row_has_liquid[] - drop the mask condition
 * entirely, ANY liquid cell marks its row): the SAME scene, SAME gravity
 * sequence, run for the SAME number of frames, produces a worst single-frame
 * swing of 1.87 - an ordinary, small settling shift, not a collapse. See
 * WAKE_TEST_MAX_SWING below for the bound this is checked against.
 *
 * PROVEN LOAD-BEARING: temporarily restoring the reverted (mask & MATERIAL_
 * EDGE_CARDINAL) == 0 condition inside wake_test_row_has_liquid() below (in
 * place of the shipped `true`) turns this test RED, reproducing the 21.71
 * collapse above almost exactly. Restoring the real condition turns it
 * GREEN again at 1.87. Neither the scene nor the gravity sequence changes
 * between the two runs - only the one condition this fix touches. */

enum {
    WAKE_TEST_W = 48,
    WAKE_TEST_H = 40,
};
/* ~30fps, matches the reproduction that found this. */
#define WAKE_TEST_DT_MS 33u
/* LOCAL_DEPTH_WAKE_MS, mirrored - app_sand.c cannot be linked here (see this
 * section's own top comment), so this is a copy of the constant, not a
 * reference to it. */
#define WAKE_TEST_WAKE_MS 120u

static uint8_t wake_test_blocks[
    ((WAKE_TEST_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
    ((WAKE_TEST_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)];

static union {
    ray_walk_state_t wake_ray_state, band_ray_state, flash_ray_state;
} fx_ray;
/* wake_test_cells/wake_prev_occupied/wake_displayed_depth used to be file
 * statics here, permanently resident .bss even though only wake_test_run()
 * below ever touches them - malloc'd there instead, fresh per call, freed
 * before it returns (this test's own reproduction owns the only call). */

static unsigned wake_test_edge_mask(sand_t *g, int x, int y)
{
    unsigned m = 0;
    if (CELL_IS_EMPTY(sand_at(g, x - 1, y))) m |= MATERIAL_EDGE_LEFT;
    if (CELL_IS_EMPTY(sand_at(g, x + 1, y))) m |= MATERIAL_EDGE_RIGHT;
    if (CELL_IS_EMPTY(sand_at(g, x, y - 1))) m |= MATERIAL_EDGE_UP;
    if (CELL_IS_EMPTY(sand_at(g, x, y + 1))) m |= MATERIAL_EDGE_DOWN;
    return m;
}

/* fabs() without pulling in <math.h> for one call - this file has no other
 * floating-point dependency, and this test's own determinism (integer
 * gravity, an explicitly seeded rng_t) means every platform this runs on
 * computes the exact same sequence of means, so there is nothing here for
 * libm to buy. */
static double fabs_double(double x)
{
    return x < 0.0 ? -x : x;
}

static bool wake_test_row_has_liquid(unsigned mask)
{
    (void)mask;
    return true;
}

/* Returns worst single-frame swing in mean DISPLAYED interior depth. */
static double wake_test_run(int steps)
{
    uint8_t *wake_test_cells = malloc((size_t)WAKE_TEST_W * WAKE_TEST_H);
    bool *wake_prev_occupied = malloc((size_t)WAKE_TEST_W * WAKE_TEST_H *
                                       sizeof *wake_prev_occupied);
    /* int8_t, not int: a displayed depth is -1 (never painted) or 0..
     * MATERIAL_LIQUID_DEPTH_BAND (24), comfortably inside int8_t's range. */
    int8_t *wake_displayed_depth = malloc((size_t)WAKE_TEST_W * WAKE_TEST_H *
                                           sizeof *wake_displayed_depth);
    /* All three checked together, then freed together on failure -
     * asserting straight after each malloc in turn would longjmp out on the
     * first failure (Unity's assert never returns) and leak every
     * allocation that came before it, permanently, for the rest of the run. */
    if (!wake_test_cells || !wake_prev_occupied || !wake_displayed_depth) {
        free(wake_test_cells);
        free(wake_prev_occupied);
        free(wake_displayed_depth);
        TEST_ASSERT_TRUE_MESSAGE(false,
            "wake test buffers (grid/prev-occupied/displayed-depth) must "
            "fit in what the framebuffer leaves");
    }

    sand_init(&fx.wake_test_grid, wake_test_cells, WAKE_TEST_W, WAKE_TEST_H,
              41u);
    sand_enable_sleeping(&fx.wake_test_grid, wake_test_blocks);

    ray_walk_state_reset(&fx_ray.wake_ray_state);
    memset(wake_prev_occupied, 0,
           (size_t)WAKE_TEST_W * WAKE_TEST_H * sizeof *wake_prev_occupied);
    for (int i = 0; i < WAKE_TEST_W * WAKE_TEST_H; i++) {
        wake_displayed_depth[i] = -1;
    }

    /* A RAGGED basin, not one clean rectangle - two separate pours plus a
     * stub wall, so the settled surface has real grain-level irregularity:
     * a hand-authored flat pool never flips a cell between rim and interior
     * classification, so it could never have found this bug. */
    for (int y = WAKE_TEST_H - 6; y < WAKE_TEST_H; y++) {
        for (int x = 0; x < WAKE_TEST_W; x++) {
            sand_set(&fx.wake_test_grid, x, y, CELL_MAKE(MAT_STONE, 0));
        }
    }
    sand_spawn(&fx.wake_test_grid, WAKE_TEST_W / 3, 6, 5, MAT_WATER);
    sand_spawn(&fx.wake_test_grid, 2 * WAKE_TEST_W / 3, 4, 6, MAT_WATER);
    for (int y = WAKE_TEST_H - 9; y < WAKE_TEST_H - 8; y++) {
        sand_set(&fx.wake_test_grid, WAKE_TEST_W / 2, y,
                  CELL_MAKE(MAT_STONE, 0));
    }

    rng_t wobble;
    rng_seed(&wobble, 7u);

    uint32_t wake_elapsed_ms = 0;
    const int settle_steps = 400; /* let the pool actually settle first */
    double worst_jump = 0.0;
    double prev_mean = -1.0;

    for (int f = 0; f < steps; f++) {
        /* Near-perfect PORTRAIT: regime stays vertical-dominant, v_reverse
         * never fires. */
        const int phase = f % 90;
        const int tri = (phase < 45) ? (-40 + (phase * 80) / 45)
                                      : (40 - ((phase - 45) * 80) / 45);
        const int gx = tri + (int)rng_below(&wobble, 21) - 10;
        const int gy = 950 + (int)rng_below(&wobble, 11) - 5;

        sand_step(&fx.wake_test_grid, gx, gy, 0);

        bool vdom, vrev, hrev;
        unsigned ax, ay, scale_q8;
        ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

        /* advance_local_depth_wake(), mirrored. */
        wake_elapsed_ms += WAKE_TEST_DT_MS;
        bool wake_fired = false;
        if (wake_elapsed_ms >= WAKE_TEST_WAKE_MS) {
            wake_elapsed_ms -= (wake_elapsed_ms / WAKE_TEST_WAKE_MS) *
                                WAKE_TEST_WAKE_MS;
            wake_fired = true;
        }

        bool row_dirty[WAKE_TEST_H] = { 0 };
        for (int y = 0; y < WAKE_TEST_H; y++) {
            for (int x = 0; x < WAKE_TEST_W; x++) {
                const bool now =
                    !CELL_IS_EMPTY(sand_at(&fx.wake_test_grid, x, y));
                if (now != wake_prev_occupied[y * WAKE_TEST_W + x]) {
                    row_dirty[y] = true;
                }
            }
        }

        /* THE WAKE TICK'S OWN ROW GATE - row_has_liquid[]'s population,
         * recomputed fresh each frame from the live grid (paint_row_n()
         * recomputes it every time a row is painted; a row that never gets
         * painted keeps whatever this cache last decided, exactly like the
         * real array). */
        for (int y = 0; y < WAKE_TEST_H; y++) {
            bool has_liquid = false;
            for (int x = 0; x < WAKE_TEST_W; x++) {
                const cell_t c = sand_at(&fx.wake_test_grid, x, y);
                if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                    const unsigned mask = wake_test_edge_mask(&fx.wake_test_grid,
                                                              x, y);
                    if (wake_test_row_has_liquid(mask)) {
                        has_liquid = true;
                        break;
                    }
                }
            }
            if (wake_fired && has_liquid) {
                row_dirty[y] = true;
            }
        }

        /* mirror_ray_walk_row()'s own per-cell walk, for every dirty row. */
        const bool asc = !vrev;
        for (int i = 0; i < WAKE_TEST_H; i++) {
            const int y = asc ? i : (WAKE_TEST_H - 1 - i);
            if (!row_dirty[y]) {
                continue;
            }
            unsigned row_depth[RAY_WALK_STATE_W];
            mirror_ray_walk_row(&fx.wake_test_grid, y, WAKE_TEST_W, WAKE_TEST_H,
                                vdom, vrev, hrev, ax, ay, scale_q8,
                                MATERIAL_LIQUID_DEPTH_BAND, &fx_ray.wake_ray_state,
                                row_depth);
            for (int x = 0; x < WAKE_TEST_W; x++) {
                const cell_t here = sand_at(&fx.wake_test_grid, x, y);
                wake_displayed_depth[y * WAKE_TEST_W + x] = CELL_IS_EMPTY(here)
                    ? -1 : (int8_t)row_depth[x];
            }
        }

        for (int y = 0; y < WAKE_TEST_H; y++) {
            for (int x = 0; x < WAKE_TEST_W; x++) {
                wake_prev_occupied[y * WAKE_TEST_W + x] =
                    !CELL_IS_EMPTY(sand_at(&fx.wake_test_grid, x, y));
            }
        }

        /* Measure the DISPLAYED interior mean depth this frame - interior
         * only (mask & MATERIAL_EDGE_CARDINAL) == 0, the only cells whose
         * depth material_colours() ever reads. */
        long sum_d = 0;
        int n = 0;
        for (int y = 0; y < WAKE_TEST_H; y++) {
            for (int x = 0; x < WAKE_TEST_W; x++) {
                const cell_t c = sand_at(&fx.wake_test_grid, x, y);
                if (CELL_IS_EMPTY(c) || CELL_MATERIAL(c) != MAT_WATER) {
                    continue;
                }
                if ((wake_test_edge_mask(&fx.wake_test_grid, x, y) &
                     MATERIAL_EDGE_CARDINAL) != 0) {
                    continue;
                }
                const int d = wake_displayed_depth[y * WAKE_TEST_W + x];
                if (d < 0) {
                    continue;
                }
                sum_d += d;
                n++;
            }
        }
        if (n > 0) {
            const double mean = (double)sum_d / n;
            if (f >= settle_steps && prev_mean >= 0) {
                const double jump = fabs_double(mean - prev_mean);
                if (jump > worst_jump) {
                    worst_jump = jump;
                }
            }
            prev_mean = mean;
        }
    }

    free(wake_test_cells);
    free(wake_prev_occupied);
    free(wake_displayed_depth);

    return worst_jump;
}

#define WAKE_TEST_MAX_SWING 8.0

static void test_a_settled_edge_does_not_flicker_stale_to_fresh(void)
{
    const double worst_swing = wake_test_run(3000);

    char why[640];
    snprintf(why, sizeof why,
             "worst single-frame swing in mean displayed interior depth was "
             "%.2f (of a %d-cell saturating range) - the reported bug is a "
             "settled pool's mean collapsing from 23.671 to 1.958 in one "
             "frame (a 21.71-unit swing) when a row's only liquid happens to "
             "read as all-rim right when the wake tick fires, recovering "
             "over the next several dozen frames as the tick catches every "
             "affected row up one at a time - gating the wake on ANY liquid "
             "cell, not only interior ones, keeps every liquid row on the "
             "same bounded refresh cadence regardless of how its edges "
             "flicker between rim and interior",
             worst_swing, (int)MATERIAL_LIQUID_DEPTH_BAND);
    TEST_ASSERT_TRUE_MESSAGE(worst_swing < WAKE_TEST_MAX_SWING, why);
}

/*=============================================================================
 * A DIRECTION FLIP MUST NOT CORRUPT THE BOUNDARY DEBOUNCE
 *
 * A DIFFERENT bug from the wake-tick staleness just above, in the SAME
 * debounce mechanism (col_stable_depth[]/col_top_row[], app_sand.c) - this
 * one fires even when every row involved IS repainted promptly, so a wake
 * tick has nothing to do with it. See update_local_depth_gravity()'s own
 * comment (app_sand.c, right where col_top_row[]/row_top_col[] get reset)
 * for the full account; summarised here for this test's own reproduction.
 *
 * col_top_row[cx] is ONE slot per column, remembering which ROW most
 * recently asked for a reset - not committing until the SAME row asks
 * twice in a row, exactly like col_stable_depth[]'s own comment describes.
 * That design assumes the boundary's ROW does not move except slowly, by
 * real settling. It does not hold when local_depth_v_reverse itself flips:
 * "the neighbour toward the surface" swaps from `above` to `below` (or
 * back), which relocates WHICH ROW is the genuine boundary between two
 * candidates that are BOTH real - the top of a water column against a
 * wall when gravity points down, the bottom of it when gravity points up.
 * A hand held near "landscape lock" produces exactly this: gx large and
 * steady (pressed against the wall), gy small and tremor-noisy, crossing
 * zero often. Every crossing hands col_top_row[cx] a DIFFERENT row than
 * the one it is tracking, so the "asked twice" commit almost never fires -
 * every encounter takes the HOLD branch instead, climbing col_stable_
 * depth[cx] rather than resetting it, and two alternating boundaries
 * compound that climb without bound (measured on a real trace: 40, 119,
 * 199, 200 across five flips, before finally landing two consecutive asks
 * on the same row by chance and dropping to 0).
 *
 * REPRODUCED BELOW with the worst case for this specific mechanism: a
 * water column that never moves (rows FLIP_TEST_TOP..FLIP_TEST_BOTTOM,
 * against dry "air" above it and off-grid below it - sand_step() is not
 * even run, since this bug lives entirely in the debounce bookkeeping, not
 * the physics), driven by a v_reverse that flips EVERY frame - deterministic
 * on purpose (see FLIP_TEST_FRAMES's own comment), and the harshest case
 * for "the same row never asks twice in a row", since a real hand's tremor
 * at least sometimes holds a direction for two consecutive frames while
 * this never does.
 *
 * WHAT THIS TEST MEASURES CHANGED when the saturating climb landed (see THE
 * SATURATING CLIMB's own comment in app_sand.c): col_stable_depth[] now stops
 * at MATERIAL_LIQUID_DEPTH_BAND rather than at 255, so the old statistic -
 * the worst single-frame SWING in the bottom row's own depth, 37 with the
 * reset against 75 without it - no longer separates the two at all. Worse, it
 * INVERTS: without the reset every cell in the column pins at the clamp and
 * therefore stops swinging entirely, so the old assertion (`swing < 60`)
 * would read a dead, saturated column as a pass. Verified directly rather
 * than assumed - see this section's own numbers below.
 *
 * The statistic that does separate them under the clamp is the MEAN
 * debounced depth across the column, which says whether the column still has
 * a depth GRADIENT at all rather than how much it jitters.
 *
 * THE COLUMN IS DELIBERATELY SHALLOWER THAN THE BAND (13 rows against a
 * 24-cell band). A column deeper than the band saturates at its far end even
 * when everything is working, so both the fixed and the un-fixed run would
 * read the same there and the test would prove nothing. A shallow column is
 * also the case that still MATTERS after the clamp: the clamp bounds how far
 * a corrupted accumulator can stray, but inside the band there is nothing
 * left to bound it, so this reset is what keeps a genuinely 12-cell-deep
 * column from rendering as fully deep.
 *
 * PROVEN LOAD-BEARING: temporarily changing flip_test_sweep_column()'s own
 * `apply_fix` guard below to always skip the reset (as if update_local_
 * depth_gravity() never invalidated col_stable_depth[]/col_top_row[] on a
 * flip) drives the mean debounced depth of this 13-row column to exactly
 * 24.00 - EVERY cell at the saturation point, the column's entire shading
 * gradient collapsed into one flat maximally-deep body colour, and it stays
 * there for the rest of the run. With the reset applied, the same
 * deterministic flip sequence holds a mean of 7.00 - the honest average of a
 * 0..12 gradient plus the one-frame HOLD each flip costs at the boundary
 * before it commits. */

enum {
    FLIP_TEST_H      = 16, /* rows 0..15 */
    FLIP_TEST_TOP    = 2,  /* topmost water row - the boundary the ascending
                               (v_reverse == false) walk asks about */
    FLIP_TEST_BOTTOM = FLIP_TEST_H - 2, /* bottommost water row (14) - its
                               "below" neighbour is dry, so it is ALWAYS a
                               genuine boundary request when the walk runs
                               descending (v_reverse == true). 12 rows of
                               interior depth below the top one, comfortably
                               inside MATERIAL_LIQUID_DEPTH_BAND - see this
                               section's own comment for why that matters */
};

/* Mirrors app_sand.c's col_stable_depth[]/col_top_row[] debounce exactly
 * like mirror_debounced_depth_column() above, generalised for a walk whose
 * direction can REVERSE from one frame to the next - update_local_depth_
 * gravity()'s own comment (app_sand.c) is the mechanism this pins. `top`/
 * `bottom` bound the water column (inclusive); `stable`/`top_row` and
 * `v_reverse_prev` are IN/OUT, persisting across calls exactly like the
 * real file-static arrays persist across frames. `apply_fix` gates ONLY the
 * reset itself, not the flip detection above it - flipping it to false
 * (temporarily, for verification - see this section's own top comment) is
 * exactly reverting update_local_depth_gravity()'s own fix and nothing
 * else.
 *
 * SWEEPS ONLY [top, bottom], DELIBERATELY - not the dry air rows above
 * `top` too, even though paint_row_n() itself would walk those as part of
 * the same grid. A settled pool's dry air never changes and holds no
 * liquid, so draw_dirty_rows() never marks those rows dirty again after
 * the scene's very first paint (dirty_rows[]; row_has_liquid[]'s own wake
 * tick only wakes rows that hold a liquid cell - see that array's own
 * comment in app_sand.c) - real firmware simply never re-invokes
 * paint_row_n() for them. Sweeping them here anyway every frame would feed
 * col_stable_depth[cx] a clean reset (the "not a liquid cell" branch) on
 * every single frame regardless of `apply_fix`, which quietly hides the
 * exact bug this test exists to catch - found by tracing why an early
 * version of this test passed even with `apply_fix` forced false. */
static void flip_test_sweep_column(int top, int bottom,
                                    bool v_reverse, bool apply_fix,
                                    unsigned char *stable,
                                    unsigned char *top_row,
                                    bool *v_reverse_prev,
                                    unsigned depth_out[])
{
    if (apply_fix && v_reverse != *v_reverse_prev) {
        *stable = 0;
        *top_row = 255;
    }
    *v_reverse_prev = v_reverse;

    const int cy_first = v_reverse ? bottom : top;
    const int cy_step  = v_reverse ? -1 : 1;
    const int n = bottom - top + 1;

    for (int i = 0; i < n; i++) {
        const int cy = cy_first + i * cy_step;
        const int neighbour_cy = v_reverse ? cy + 1 : cy - 1;

        /* Every row in [top, bottom] is water by construction - the only
         * way "same material" can be false is the neighbour falling
         * outside that range: off-grid past `bottom` when descending, dry
         * air just above `top` when ascending. Both are genuine boundary
         * requests, never a blip. */
        const bool same = neighbour_cy >= top && neighbour_cy <= bottom;

        /* MATERIAL_LIQUID_DEPTH_BAND, not 255 - mirroring THE SATURATING
         * CLIMB (app_sand.c). Drift risk as warned in material.h. */
        unsigned depth;
        if (same) {
            depth = *stable < MATERIAL_LIQUID_DEPTH_BAND
                ? *stable + 1u : MATERIAL_LIQUID_DEPTH_BAND;
        } else if (*top_row == (unsigned char)cy) {
            depth = 0u;
        } else {
            depth = *stable < MATERIAL_LIQUID_DEPTH_BAND
                ? *stable + 1u : MATERIAL_LIQUID_DEPTH_BAND;
            *top_row = (unsigned char)cy;
        }
        *stable = (unsigned char)depth;
        depth_out[cy] = depth;
    }
}

/* SAME boundary row never asked twice in a row; alternating every frame
 * guarantees this. */
#define FLIP_TEST_FRAMES 40

static double flip_test_run(bool apply_fix)
{
    unsigned char stable = 0, top_row = 255;
    bool v_reverse_prev = false; /* matches local_depth_v_reverse_prev's own
                                     BSS-zero default in app_sand.c */
    unsigned depth[FLIP_TEST_H];

    long sum = 0;
    int n = 0;

    for (int f = 0; f < FLIP_TEST_FRAMES; f++) {
        const bool v_reverse = (f % 2) == 0;

        flip_test_sweep_column(FLIP_TEST_TOP, FLIP_TEST_BOTTOM, v_reverse,
                                apply_fix, &stable, &top_row, &v_reverse_prev,
                                depth);

        if (f < 10) {
            continue;   /* steady state only */
        }
        for (int cy = FLIP_TEST_TOP; cy <= FLIP_TEST_BOTTOM; cy++) {
            sum += (long)depth[cy];
            n++;
        }
    }
    return n > 0 ? (double)sum / n : 0.0;
}

/* Comfortably above the 7.00 measured WITH the fix, comfortably below the
 * 24.00 measured WITHOUT it - same shape as WAKE_TEST_MAX_SWING. Expressed
 * against the band to stay clear of saturation. */
#define FLIP_TEST_MAX_MEAN_DEPTH ((double)MATERIAL_LIQUID_DEPTH_BAND / 2.0)

static void test_a_direction_flip_does_not_corrupt_the_boundary_debounce(void)
{
    const double mean_depth = flip_test_run(true);

    char why[640];
    snprintf(why, sizeof why,
             "mean debounced vertical depth across a %d-row water column "
             "was %.2f, against a saturation point of %d - update_local_"
             "depth_gravity()'s reset on a v_reverse flip (app_sand.c) "
             "exists to keep this column's depth GRADIENT alive (mean 7.00, "
             "the average of an honest 0..%d ramp) rather than letting the "
             "HOLD branch compound every cell in it up to the saturation "
             "point (mean 24.00, one flat maximally-deep colour) when two "
             "different, both legitimate, boundary rows keep alternately "
             "asking for col_top_row[]'s one tracking slot",
             FLIP_TEST_BOTTOM - FLIP_TEST_TOP + 1, mean_depth,
             (int)MATERIAL_LIQUID_DEPTH_BAND,
             FLIP_TEST_BOTTOM - FLIP_TEST_TOP);
    TEST_ASSERT_TRUE_MESSAGE(mean_depth < FLIP_TEST_MAX_MEAN_DEPTH, why);
}

/*=============================================================================
 * A SPARSELY REPAINTED ROW MUST NOT BAND A TALL LIQUID COLUMN
 *
 * The third distinct bug in this same local-depth mechanism, after the wake
 * tick's staleness gap and the boundary debounce's flip corruption above -
 * and unlike either of those, this one is about the DEPTH SCALE rather than
 * about bookkeeping. Full history (all four ceiling shapes tried, the
 * ablation that found the two necessary conditions, the exact numbers) is
 * in git log up to commit 3376c8e and in THE SATURATING CLIMB's own comment
 * there; NOT reproduced in full here, since the walk itself changed shape
 * since then and re-narrating four attempts at a ceiling this walk does not
 * even need any more (see the next paragraph) would be describing history
 * this file no longer carries.
 *
 * WHAT SURVIVES THE REWRITE UNCHANGED: local_depth_cur_row[]/local_depth_
 * prev_row[] are still a RUNNING accumulator walked one row at a time, each
 * painted row's value still the previous PAINTED row's plus one, and
 * draw_dirty_rows() still delivers rows sparsely, not as a contiguous
 * chain - so a row painted in isolation still inherits whatever row
 * happened to be painted before it, describing a different cell entirely,
 * while its vertical neighbours still show the coherent values the last
 * wake tick left them. The clamp to MATERIAL_LIQUID_DEPTH_BAND (applied
 * once, at the very end of paint_row_n()'s own projection) still bounds how
 * far that broken-chain error can swing before it reaches the screen -
 * unchanged in shape or purpose from the two-walk design's own clamp.
 *
 * WHAT DOES NOT SURVIVE: LOCAL_DEPTH_COUNT_CEILING no longer needs to be
 * RAISED above MATERIAL_LIQUID_DEPTH_BAND to avoid a "breathing" saturated
 * cell - see that constant's own comment in app_sand.c for why this walk's
 * own scale (`len / dominant_axis`, always >= 256) points the OPPOSITE
 * direction from the two-walk design's own weight (`component / len`,
 * always <= 256), so a plain band-equal ceiling already reaches the band at
 * every angle without help. `SUITE_LOCAL_DEPTH_COUNT_CEILING` below is
 * still a separate, hand-kept constant, for the same reason it always was
 * (app_sand.c is compile-checked only, not linked here) - it is simply now
 * equal to `MATERIAL_LIQUID_DEPTH_BAND` rather than 34.
 *
 * THE SCENE is unchanged from the device's own report: a water column
 * standing against a side wall and spanning the WHOLE grid height (so the
 * walk's own range is far past the band, which is exactly what portrait's
 * shallow settled pools never do - hence a landscape-lock-only report), a
 * trickle so the pool is never perfectly asleep, and gravity taken from the
 * capture sidecars - gx large and steady, gy small, crossing zero, with the
 * slow few-degree sway of a hand that is holding the board rather than
 * clamping it. Real sand_t/sand_step(), real dirty rows from real cell
 * movement, the real wake tick, and the real row-order reversal - now
 * mirrored through mirror_ray_walk_row() (this file, above) rather than a
 * bespoke two-axis walk, since gravity here stays horizontal-dominant
 * throughout (gx large, gy small - never crossing into vertical-dominant),
 * exercising exactly the regime the device report was about.
 *
 * THE MEASUREMENT: pairs of VERTICALLY ADJACENT interior liquid cells whose
 * rendered depths differ by more than half the saturating range. Gravity
 * points along x here, so the true depth field varies along x and is flat
 * along y - every such pair is a horizontal line drawn across water that has
 * no line in it. Half the range is two of the four shade steps
 * material_colours() has to spend, i.e. "huge jumps" in the report's own
 * terms rather than an ordinary contour.
 *
 * RE-VERIFIED, not merely re-run, against this walk: 0 banded pairs in the
 * worst of 900 frames, at every run length tried - the same GREEN result
 * the two-walk design's own fixed shipped, and (see this test's own
 * assertion below) with more headroom against the threshold than that
 * design had, since this walk's own scale-not-weight direction means a
 * broken chain's swing is bounded the same way at EVERY angle, not only
 * near axis alignment. */

enum {
    BAND_TEST_W = 36,
    BAND_TEST_H = 96,
};
#define BAND_TEST_DT_MS   33u
#define BAND_TEST_WAKE_MS 120u
/* Long enough for the pool to settle and for several full sway periods (250
 * frames each) to run; short enough to stay inside this suite's own budget -
 * about 0.15 s. Green at every length tried; see this section's own comment. */
#define BAND_TEST_FRAMES  900
#define BAND_TEST_SETTLE  300

static uint8_t band_test_blocks[
    ((BAND_TEST_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
    ((BAND_TEST_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)];

/* band_test_cells/band_prev_occupied/band_displayed_depth used to be file
 * statics here - malloc'd inside band_test_run() below instead (its own
 * reproduction owns the only call), for the same reason as wake_test_run()
 * above. */

/* Do NOT raise above MATERIAL_LIQUID_DEPTH_BAND. LOCAL_DEPTH_COUNT_CEILING's
 * comment in app_sand.c explains why no raise is needed. */
static unsigned band_test_ceiling(void)
{
    return MATERIAL_LIQUID_DEPTH_BAND;
}

static int band_test_run(void)
{
    uint8_t *band_test_cells = malloc((size_t)BAND_TEST_W * BAND_TEST_H);
    bool *band_prev_occupied = malloc((size_t)BAND_TEST_W * BAND_TEST_H *
                                       sizeof *band_prev_occupied);
    /* int8_t, not int - see wake_test_run()'s own comment on the same
     * narrowing; the depth values stored here have the same -1..
     * MATERIAL_LIQUID_DEPTH_BAND (24) range. */
    int8_t *band_displayed_depth = malloc((size_t)BAND_TEST_W * BAND_TEST_H *
                                           sizeof *band_displayed_depth);
    /* All three checked together, then freed together on failure - see
     * wake_test_run()'s own comment on the same pattern, above. */
    if (!band_test_cells || !band_prev_occupied || !band_displayed_depth) {
        free(band_test_cells);
        free(band_prev_occupied);
        free(band_displayed_depth);
        TEST_ASSERT_TRUE_MESSAGE(false,
            "band test buffers (grid/prev-occupied/displayed-depth) must "
            "fit in what the framebuffer leaves");
    }

    sand_init(&fx.band_test_grid, band_test_cells, BAND_TEST_W, BAND_TEST_H, 41u);
    sand_enable_sleeping(&fx.band_test_grid, band_test_blocks);

    ray_walk_state_reset(&fx_ray.band_ray_state);
    memset(band_prev_occupied, 0,
           (size_t)BAND_TEST_W * BAND_TEST_H * sizeof *band_prev_occupied);
    for (int i = 0; i < BAND_TEST_W * BAND_TEST_H; i++) {
        band_displayed_depth[i] = -1;
    }

    /* Walls all round, and water standing against the +x one for the WHOLE
     * grid height - the shape a board held at landscape lock puts a pool in,
     * and the reason the walk's range here is the grid's own height rather
     * than a settled pool's few dozen cells. */
    for (int y = 0; y < BAND_TEST_H; y++) {
        sand_set(&fx.band_test_grid, 0, y, CELL_MAKE(MAT_STONE, 0));
        sand_set(&fx.band_test_grid, BAND_TEST_W - 1, y, CELL_MAKE(MAT_STONE, 0));
    }
    for (int x = 0; x < BAND_TEST_W; x++) {
        sand_set(&fx.band_test_grid, x, 0, CELL_MAKE(MAT_STONE, 0));
        sand_set(&fx.band_test_grid, x, BAND_TEST_H - 1, CELL_MAKE(MAT_STONE, 0));
    }
    for (int y = 1; y < BAND_TEST_H - 1; y++) {
        for (int x = BAND_TEST_W - 17; x < BAND_TEST_W - 1; x++) {
            sand_set(&fx.band_test_grid, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    rng_t wobble;
    rng_seed(&wobble, 7u);

    const unsigned ceiling = band_test_ceiling();
    uint32_t wake_elapsed_ms = 0;
    int worst = 0;

    for (int f = 0; f < BAND_TEST_FRAMES; f++) {
        if ((f % 7) == 0) {
            sand_set(&fx.band_test_grid, BAND_TEST_W - 22, 2,
                      CELL_MAKE(MAT_WATER, MASS_MAX));
            sand_set(&fx.band_test_grid, BAND_TEST_W - 21, 2,
                      CELL_MAKE(MAT_WATER, MASS_MAX));
        }

        const int sway_phase = f % 250;
        const int sway = (sway_phase < 125)
            ? (-115 + (sway_phase * 230) / 125)
            : (115 - ((sway_phase - 125) * 230) / 125);
        const int tremor_phase = f % 17;
        const int tremor = (tremor_phase < 8)
            ? (-30 + (tremor_phase * 60) / 8)
            : (30 - ((tremor_phase - 8) * 60) / 9);
        const int gy = sway + tremor + rng_below(&wobble, 21) - 10;
        const int gx = 950 + rng_below(&wobble, 11) - 5;

        sand_step(&fx.band_test_grid, gx, gy, 0);

        bool vdom, vrev, hrev;
        unsigned ax, ay, scale_q8;
        ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

        /* advance_local_depth_wake(), mirrored. */
        wake_elapsed_ms += BAND_TEST_DT_MS;
        bool wake_fired = false;
        if (wake_elapsed_ms >= BAND_TEST_WAKE_MS) {
            wake_elapsed_ms -= (wake_elapsed_ms / BAND_TEST_WAKE_MS) *
                                BAND_TEST_WAKE_MS;
            wake_fired = true;
        }

        bool row_dirty[BAND_TEST_H] = { 0 };
        for (int y = 0; y < BAND_TEST_H; y++) {
            for (int x = 0; x < BAND_TEST_W; x++) {
                const bool now =
                    !CELL_IS_EMPTY(sand_at(&fx.band_test_grid, x, y));
                if (now != band_prev_occupied[y * BAND_TEST_W + x]) {
                    row_dirty[y] = true;
                }
            }
        }
        if (wake_fired) {
            for (int y = 0; y < BAND_TEST_H; y++) {
                for (int x = 0; x < BAND_TEST_W; x++) {
                    const cell_t c = sand_at(&fx.band_test_grid, x, y);
                    if (!CELL_IS_EMPTY(c) &&
                        material_of(c)->kind == KIND_LIQUID) {
                        row_dirty[y] = true;
                        break;
                    }
                }
            }
        }

        const bool asc = !vrev;
        for (int i = 0; i < BAND_TEST_H; i++) {
            const int cy = asc ? i : (BAND_TEST_H - 1 - i);
            if (!row_dirty[cy]) {
                continue;
            }
            unsigned row_depth[RAY_WALK_STATE_W];
            mirror_ray_walk_row(&fx.band_test_grid, cy, BAND_TEST_W, BAND_TEST_H,
                                vdom, vrev, hrev, ax, ay, scale_q8, ceiling,
                                &fx_ray.band_ray_state, row_depth);
            for (int x = 0; x < BAND_TEST_W; x++) {
                band_displayed_depth[cy * BAND_TEST_W + x] =
                    (int8_t)row_depth[x];
            }
        }

        for (int y = 0; y < BAND_TEST_H; y++) {
            for (int x = 0; x < BAND_TEST_W; x++) {
                band_prev_occupied[y * BAND_TEST_W + x] =
                    !CELL_IS_EMPTY(sand_at(&fx.band_test_grid, x, y));
            }
        }

        if (f < BAND_TEST_SETTLE) {
            continue;
        }

        int jumps = 0;
        for (int y = 1; y < BAND_TEST_H; y++) {
            for (int x = 0; x < BAND_TEST_W; x++) {
                const cell_t up = sand_at(&fx.band_test_grid, x, y - 1);
                const cell_t here = sand_at(&fx.band_test_grid, x, y);
                if (CELL_IS_EMPTY(up) || CELL_IS_EMPTY(here)) {
                    continue;
                }
                if (CELL_MATERIAL(up) != MAT_WATER ||
                    CELL_MATERIAL(here) != MAT_WATER) {
                    continue;
                }
                /* Interior only, both of them - a rim cell's depth is never
                 * read by material_colours() at all. */
                if ((wake_test_edge_mask(&fx.band_test_grid, x, y - 1) &
                     MATERIAL_EDGE_CARDINAL) != 0 ||
                    (wake_test_edge_mask(&fx.band_test_grid, x, y) &
                     MATERIAL_EDGE_CARDINAL) != 0) {
                    continue;
                }
                const int a = band_displayed_depth[(y - 1) * BAND_TEST_W + x];
                const int b = band_displayed_depth[y * BAND_TEST_W + x];
                if (a < 0 || b < 0) {
                    continue;
                }
                const int diff = a > b ? a - b : b - a;
                if (diff > MATERIAL_LIQUID_DEPTH_BAND / 2) {
                    jumps++;
                }
            }
        }
        if (jumps > worst) {
            worst = jumps;
        }
    }

    free(band_test_cells);
    free(band_prev_occupied);
    free(band_displayed_depth);

    return worst;
}

/* GREEN is exactly 0 - not "small", absent - at every run length tried, so
 * this bound is headroom against the scene drifting slightly under a future
 * change to the simulation, not against measurement noise. */
#define BAND_TEST_MAX_JUMPS 8

static void test_a_sparse_repaint_does_not_band_a_tall_liquid_column(void)
{
    const int worst = band_test_run();

    char why[900];
    snprintf(why, sizeof why,
             "%d pairs of vertically adjacent interior liquid cells rendered "
             "depths more than half the %d-cell saturating range apart in "
             "one frame - gravity points along x in this scene, so the true "
             "depth field is flat along y and every one of those pairs is a "
             "horizontal line drawn across water that has no line in it. The "
             "reported bug is a row repainted in isolation inheriting a "
             "running accumulator that describes a different row entirely; "
             "clamping the projected depth to MATERIAL_LIQUID_DEPTH_BAND "
             "before it reaches the screen bounds that error to something "
             "the four available shade steps cannot resolve - see this "
             "section's own top comment for why this walk's own ceiling "
             "needs no raise, unlike the two-walk design this replaced",
             worst, (int)MATERIAL_LIQUID_DEPTH_BAND);
    TEST_ASSERT_TRUE_MESSAGE(worst < BAND_TEST_MAX_JUMPS, why);
}

/*=============================================================================
 * TURNING A SETTLED POOL MUST NOT FLASH THE WHOLE BODY
 *
 * THE DEVICE REPORT, in the user's own words: "a brief flip of colours" they
 * could only catch in a single frame, and "tilt flips into straight
 * positions with the water settled seem to cause a huge spike". Their own
 * reproduction, adopted verbatim as this test's scenario: fill the screen to
 * about 40% with water in portrait, let it settle, then turn the board to
 * landscape.
 *
 * WHAT THE FIRST THREE SUSPECTS TURNED OUT NOT TO BE, measured before this
 * test was written, because ruling them out is what made the real mechanism
 * findable - see docs/sand/Shading-and-Colour.md for the full account:
 *
 *   - NOT the near-45-degree regime transient already documented in update_
 *     local_depth_gravity(). The device's own capture sidecars for this
 *     report read tilt_x -12 and -195 against tilt_y 3342 and 4190 - axis
 *     lock, nowhere near the 45-degree line.
 *   - NOT the state reset firing on hand tremor, even though it genuinely
 *     does (40 resets in 40 frames of portrait tremor, 39 in 40 of
 *     landscape) - see test_axis_lock_tremor_does_not_wipe_the_depth_
 *     debounce below for what those resets DO cost, which is real but is a
 *     permanent one-count bias, not a flash.
 *   - NOT the wake tick's own staleness. Repainting every liquid row EVERY
 *     frame instead of every LOCAL_DEPTH_WAKE_MS was measured in the same
 *     harness and made the flash WORSE (1003 cells against 841), which is
 *     what pointed at the walk's own bookkeeping rather than at how often it
 *     is asked to run.
 *
 * THE MECHANISM, in one sentence: a sweep's FIRST row read a count belonging
 * to the LAST row of the previous sweep - the deepest row of the pool,
 * saturated - and imported it at the surface, so the whole body rendered at
 * maximum depth for a frame. See local_depth_prev_cy's own comment in
 * app_sand.c for the full chain.
 *
 * THE SIMULATION IS FROZEN once the pool has settled, and that is the point
 * rather than a convenience: "with the water settled" is the reported
 * condition, and a grid that cannot change makes EVERY displayed change
 * spurious by construction, so this test needs no oracle to say which
 * changes were legitimate. It is also faithful - a settled pool marks no row
 * dirty, so every repaint in this run comes from the wake tick, exactly as
 * on the device.
 *
 * GRAVITY IS SWEPT ALONG A CHORD, not an arc - `(gx, gy)` stepped linearly
 * from (0, G) to (G, 0) - so this needs no trigonometry, the same reason
 * band_test_run() above uses triangle waves. The walk only ever reads
 * direction RATIOS (local_depth_scale_q8 is 256*len/dominant, and both
 * Bresenham accumulators compare ax against ay), so a chord and an arc
 * through the same bearings are indistinguishable to it; the chord simply
 * dwells a little longer near 45 degrees, which is where the interesting
 * frames are anyway.
 *
 * THE STATISTIC is how many interior liquid cells cross a full SHADE STEP in
 * one frame. A shade step is real, not a chosen sensitivity:
 * material_colours() maps depth onto DEPTH_RANGE (4) whole ramp steps out of
 * MATERIAL_LIQUID_DEPTH_BAND (24), so 6 cells of depth is exactly one step of
 * rendered colour and anything smaller cannot be seen at all.
 *
 * MEASURED, both sides in this same run (`ignore_chain_break` reverts the
 * guard and nothing else): 522 cells crossing a shade step in a single frame
 * without it - on the first wake tick after the 45-degree regime flip -
 * against 31 with. The same experiment on the device's own NORMAL-quality
 * 92x112 grid, in the standalone harness this was found in, measures 841
 * against 83: a quarter of the pool, gradient inverted end to end.
 *
 * THE HANDFUL THAT REMAIN are a one-to-five-column strip against the wall
 * the ray exits through (`qx_ok` false, so the walk restarts from 0 by
 * construction), whose rows shift as the per-row drift changes with the
 * tilt: the wall's own shadow moving, not a chain break, and deliberately
 * left alone. */

enum {
    FLASH_TEST_W = 64,
    FLASH_TEST_H = 96,
    /* 40% of the grid, as the user described it, resting on the floor. */
    FLASH_TEST_FILL_ROWS = (FLASH_TEST_H * 2) / 5,
    /* One sample per degree of the quarter turn, which at 33 ms a frame is a
     * deliberate three-second turn - slow enough that a hand really could
     * make it, and slow enough that nothing here is a straw-man jostle. */
    FLASH_TEST_STEPS = 90,
    /* Long enough for a pre-filled flat pool to come fully to rest and for
     * several wake ticks to run over it; nothing here needs the simulation
     * to do anything interesting, only to stop. */
    FLASH_TEST_SETTLE = 400,
};
#define FLASH_TEST_DT_MS   33u
#define FLASH_TEST_WAKE_MS 120u
/* The device's own steady tilt magnitude, from the capture sidecars quoted
 * above (tilt_y 3342 in portrait). */
#define FLASH_TEST_G       3342
#define FLASH_TEST_SHADE_STEP (MATERIAL_LIQUID_DEPTH_BAND / 4)

static uint8_t *flash_test_cells;
static uint8_t  flash_test_blocks[
    ((FLASH_TEST_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
    ((FLASH_TEST_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)];
static uint8_t  flash_test_dirty[FLASH_TEST_H];
static int8_t  *flash_displayed;       /* -1 = never painted */
static int8_t  *flash_displayed_prev;
static uint8_t  flash_row_has_liquid[FLASH_TEST_H];
static bool flash_vdom_prev, flash_vrev_prev, flash_hrev_prev;

/* flash_test_free() must be called after reading buffers to avoid leaks.
 * Split from flash_test_settle() to allow future callers fresh buffers
 * without re-settingtle. Returns false and frees successful allocations if
 * any malloc fails. */
static bool flash_test_alloc(void)
{
    flash_test_cells = malloc((size_t)FLASH_TEST_W * FLASH_TEST_H);
    flash_displayed = malloc((size_t)FLASH_TEST_W * FLASH_TEST_H *
                              sizeof *flash_displayed);
    flash_displayed_prev = malloc((size_t)FLASH_TEST_W * FLASH_TEST_H *
                                   sizeof *flash_displayed_prev);
    if (!flash_test_cells || !flash_displayed || !flash_displayed_prev) {
        free(flash_test_cells);
        free(flash_displayed);
        free(flash_displayed_prev);
        flash_test_cells = NULL;
        flash_displayed = NULL;
        flash_displayed_prev = NULL;
        return false;
    }
    return true;
}

static void flash_test_free(void)
{
    free(flash_test_cells);
    free(flash_displayed);
    free(flash_displayed_prev);
}

static void flash_test_frame_reset(int gx, int gy, int grid_w, int grid_h,
                                   bool gate, ray_walk_state_t *st,
                                   bool *vdom_prev, bool *vrev_prev,
                                   bool *hrev_prev, bool *fired)
{
    bool vdom, vrev, hrev;
    unsigned ax, ay, scale_q8;
    ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

    bool vrev_matters = (vrev != *vrev_prev);
    bool hrev_matters = (hrev != *hrev_prev);
    if (gate) {
        const bool drift_observable = ((long)grid_h * (long)ax >= (long)ay);
        const bool cross_row_observable = ((long)grid_w * (long)ay >= (long)ax);
        vrev_matters = vrev_matters && (vdom || cross_row_observable);
        hrev_matters = hrev_matters && (!vdom || drift_observable);
    }

    *fired = (vdom != *vdom_prev) || vrev_matters || hrev_matters;
    if (*fired) {
        /* Carry `ignore_chain_break` across by hand, as
         * `ray_walk_state_reset()` must leave it off for new states. */
        const bool knob = st->ignore_chain_break;
        ray_walk_state_reset(st);
        st->ignore_chain_break = knob;
        *vdom_prev = vdom;
        *vrev_prev = vrev;
        *hrev_prev = hrev;
    }
}

/* Leaves flash_displayed[] with LAST-PAINTED depth of every cell. */
static void flash_test_paint(int gx, int gy, bool wake_fired)
{
    bool vdom, vrev, hrev;
    unsigned ax, ay, scale_q8;
    ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

    if (wake_fired) {
        for (int y = 0; y < FLASH_TEST_H; y++) {
            if (flash_row_has_liquid[y]) {
                flash_test_dirty[y] = 1;
            }
        }
    }

    memcpy(flash_displayed_prev, flash_displayed,
           (size_t)FLASH_TEST_W * FLASH_TEST_H * sizeof *flash_displayed);

    unsigned row_depth[RAY_WALK_STATE_W];
    for (int i = 0; i < FLASH_TEST_H; i++) {
        const int cy = vrev ? (FLASH_TEST_H - 1 - i) : i;
        if (!flash_test_dirty[cy]) {
            continue;
        }
        flash_test_dirty[cy] = 0;
        flash_row_has_liquid[cy] = 0;
        mirror_ray_walk_row(&fx.flash_test_grid, cy, FLASH_TEST_W, FLASH_TEST_H,
                            vdom, vrev, hrev, ax, ay, scale_q8,
                            MATERIAL_LIQUID_DEPTH_BAND, &fx_ray.flash_ray_state,
                            row_depth);
        for (int x = 0; x < FLASH_TEST_W; x++) {
            const cell_t c = sand_at(&fx.flash_test_grid, x, cy);
            if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID) {
                flash_row_has_liquid[cy] = 1;
            }
            flash_displayed[cy * FLASH_TEST_W + x] = (int8_t)row_depth[x];
        }
    }
}

static bool flash_test_is_interior_liquid(int x, int y)
{
    const cell_t c = sand_at(&fx.flash_test_grid, x, y);
    if (CELL_IS_EMPTY(c) || material_of(c)->kind != KIND_LIQUID) {
        return false;
    }
    return (wake_test_edge_mask(&fx.flash_test_grid, x, y) &
            MATERIAL_EDGE_CARDINAL) == 0;
}

/* Wall-clock carried from the settle into whatever the caller does next, so
 * the wake tick's own phase is continuous across the two - the same reason
 * local_depth_wake_elapsed_ms is a file static in app_sand.c rather than a
 * local. */
static uint32_t flash_wake_elapsed_ms;

/* Builds the pool and settles it under portrait gravity, leaving
 * flash_displayed[] holding what the panel would be showing and the three
 * regime flags where the settle left them. Shared by both tests below, so
 * that neither can differ from the other in how the scene was reached. */
static void flash_test_settle(bool guard_chain, bool gate_reset)
{
    TEST_ASSERT_TRUE_MESSAGE(flash_test_alloc(),
        "flash test buffers (flash_test_cells/flash_displayed/flash_"
        "displayed_prev) must fit in what the framebuffer leaves");

    sand_init(&fx.flash_test_grid, flash_test_cells, FLASH_TEST_W, FLASH_TEST_H,
              1234u);
    sand_enable_sleeping(&fx.flash_test_grid, flash_test_blocks);
    sand_track_dirty_rows(&fx.flash_test_grid, flash_test_dirty);

    for (int y = FLASH_TEST_H - FLASH_TEST_FILL_ROWS; y < FLASH_TEST_H; y++) {
        for (int x = 0; x < FLASH_TEST_W; x++) {
            sand_set(&fx.flash_test_grid, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    ray_walk_state_reset(&fx_ray.flash_ray_state);
    fx_ray.flash_ray_state.ignore_chain_break = !guard_chain;
    memset(flash_row_has_liquid, 0, sizeof flash_row_has_liquid);
    for (int i = 0; i < FLASH_TEST_W * FLASH_TEST_H; i++) {
        flash_displayed[i] = -1;
    }
    /* sand_enter()'s own first full repaint. */
    memset(flash_test_dirty, 1, sizeof flash_test_dirty);

    flash_vdom_prev = true;
    flash_vrev_prev = false;
    flash_hrev_prev = false;
    bool fired = false;
    flash_wake_elapsed_ms = 0;

    /* PORTRAIT, at the steady tilt the device's own sidecars report
     * (tilt_x -12 against tilt_y 3342). */
    for (int f = 0; f < FLASH_TEST_SETTLE; f++) {
        sand_step(&fx.flash_test_grid, -12, FLASH_TEST_G, 0);
        flash_test_frame_reset(-12, FLASH_TEST_G, FLASH_TEST_W, FLASH_TEST_H,
                               gate_reset, &fx_ray.flash_ray_state, &flash_vdom_prev,
                               &flash_vrev_prev, &flash_hrev_prev, &fired);
        flash_wake_elapsed_ms += FLASH_TEST_DT_MS;
        bool wake_fired = false;
        if (flash_wake_elapsed_ms >= FLASH_TEST_WAKE_MS) {
            flash_wake_elapsed_ms -= (flash_wake_elapsed_ms /
                                      FLASH_TEST_WAKE_MS) * FLASH_TEST_WAKE_MS;
            wake_fired = true;
        }
        flash_test_paint(-12, FLASH_TEST_G, wake_fired);
    }
}

/* THE TURN ITSELF, with the simulation frozen - see this section's own
 * comment for why that is the faithful reading of "with the water settled".
 * Returns the worst count of interior cells crossing a full shade step in
 * any one frame of it. */
static int flash_test_run(bool guard_chain, bool gate_reset)
{
    flash_test_settle(guard_chain, gate_reset);

    bool fired = false;
    uint32_t wake_elapsed_ms = flash_wake_elapsed_ms;
    int worst = 0;
    for (int i = 1; i <= FLASH_TEST_STEPS; i++) {
        const int gx = (FLASH_TEST_G * i) / FLASH_TEST_STEPS;
        const int gy = FLASH_TEST_G - gx;

        flash_test_frame_reset(gx, gy, FLASH_TEST_W, FLASH_TEST_H, gate_reset,
                               &fx_ray.flash_ray_state, &flash_vdom_prev,
                               &flash_vrev_prev, &flash_hrev_prev, &fired);
        wake_elapsed_ms += FLASH_TEST_DT_MS;
        bool wake_fired = false;
        if (wake_elapsed_ms >= FLASH_TEST_WAKE_MS) {
            wake_elapsed_ms -= (wake_elapsed_ms / FLASH_TEST_WAKE_MS) *
                                FLASH_TEST_WAKE_MS;
            wake_fired = true;
        }
        flash_test_paint(gx, gy, wake_fired);

        int crossed = 0;
        for (int y = 0; y < FLASH_TEST_H; y++) {
            for (int x = 0; x < FLASH_TEST_W; x++) {
                const int k = y * FLASH_TEST_W + x;
                if (flash_displayed[k] < 0 || flash_displayed_prev[k] < 0 ||
                    !flash_test_is_interior_liquid(x, y)) {
                    continue;
                }
                const int a = flash_displayed_prev[k], b = flash_displayed[k];
                const int diff = a > b ? a - b : b - a;
                if (diff > FLASH_TEST_SHADE_STEP) {
                    crossed++;
                }
            }
        }
        if (crossed > worst) {
            worst = crossed;
        }
    }

    flash_test_free();
    return worst;
}

/* 200 separates "the wall's shadow moved" and "the body inverted". */
#define FLASH_TEST_MAX_CELLS 200

static void test_turning_a_settled_pool_to_landscape_does_not_flash_the_whole_body(void)
{
    const int guarded = flash_test_run(true, true);

    /* THE SAME RUN WITHOUT THE GUARD, so red-before-green is measured here
     * rather than asserted in a commit message - see ray_walk_state_t's own
     * `ignore_chain_break` comment. If this ever stops separating, the
     * scene has drifted and the test is no longer about the bug. */
    const int unguarded = flash_test_run(false, true);

    char why[1000];
    snprintf(why, sizeof why,
             "turning a settled 40%%-full pool from portrait to landscape "
             "made %d interior liquid cells cross a full %d-cell shade step "
             "in a single frame, with the simulation frozen so not one of "
             "them had any physical reason to change. The same scene with "
             "the chain guard reverted measures %d. The bug this pins is a "
             "sweep's FIRST row importing the count of the LAST row of the "
             "previous sweep - the deepest, saturated row - at the surface, "
             "which floods the whole body to maximum depth for a frame; see "
             "local_depth_prev_cy's own comment in app_sand.c",
             guarded, (int)FLASH_TEST_SHADE_STEP, unguarded);
    TEST_ASSERT_TRUE_MESSAGE(guarded < FLASH_TEST_MAX_CELLS, why);
    TEST_ASSERT_TRUE_MESSAGE(unguarded > FLASH_TEST_MAX_CELLS, why);
}

/*=============================================================================
 * TREMOR AT AXIS LOCK MUST NOT WIPE THE DEBOUNCE
 *
 * A SECOND, SEPARATE DEFECT found while chasing the flash above, and worth
 * its own test because it is the one the flash was first blamed on.
 *
 * `local_depth_h_reverse` is just `gx < 0`. At PORTRAIT LOCK gx sits on zero
 * - the device's own capture sidecars for this report read tilt_x -12 and
 * -195 against tilt_y 3342 and 4190 - so ordinary hand tremor flips its SIGN
 * many times a second. update_local_depth_gravity() used to treat any flip
 * of any of its three flags as invalidating the whole walk state, so it
 * wiped local_depth_row_a[]/local_depth_row_b[]/local_depth_top_row[] on
 * essentially every frame: measured, 40 resets in 40 frames of that tremor.
 * Landscape is the mirror image with `local_depth_v_reverse` as the noisy
 * flag: 39 in 40.
 *
 * WHAT THAT COSTS is not a flash - measured, the displayed depth moves by at
 * most 1 of 24 while the tremor runs and no cell crosses a shade step. It is
 * that the hold-then-commit debounce is DISABLED OUTRIGHT: local_depth_top_
 * row[] is wiped back to "untracked" before every single paint, so a
 * boundary can never be asked a second time and can never COMMIT to 0. A
 * settled pool's surface holds at 1 forever instead, and every row beneath
 * inherits the extra count - a permanent, systematic bias that survives for
 * as long as the hand does.
 *
 * THE FIX IS A RELEVANCE GATE, NOT A DEADBAND, and the distinction is
 * deliberate: this mechanism already removed one tuned dead zone and should
 * not quietly grow another. Each condition is exact arithmetic about whether
 * the flipped flag can change a number this grid's walk actually computes -
 * see update_local_depth_gravity()'s own comment in app_sand.c for the
 * derivation. Nothing here is tuned and there is no threshold to re-tune.
 *
 * MEASURED, this scene: 39 resets in 40 tremor frames without the gate,
 * moving 1472 interior cells' displayed depth; 0 resets and 0 moved cells
 * with it. Zero is the right assertion, not a small budget: a sign flip on a
 * component already sitting within a few parts in three thousand of zero,
 * over a pool that cannot move, must change nothing at all. */

enum { TREMOR_TEST_FRAMES = 40 };

/* Runs the tremor and reports (a) how many frames fired a reset and (b) how
 * many interior cells' displayed depth differs from what it read before the
 * tremor began. */
static void tremor_test_run(bool gate_reset, int *resets, int *changed)
{
    /* The same settled 40% pool the flash test uses, with the chain guard
     * always on - this test is about the RESET alone, so nothing else may
     * vary between its two runs. */
    flash_test_settle(true, gate_reset);

    int8_t *before = malloc((size_t)FLASH_TEST_W * FLASH_TEST_H *
                             sizeof *before);
    if (!before) {
        /* flash_test_settle() above already succeeded (it asserts on its
         * own failure), so its three buffers are live here and must be
         * freed before this function's own assert longjmps out, or they
         * leak for the rest of the run - see flash_test_alloc()'s own
         * comment for the same hazard one level down. */
        flash_test_free();
        TEST_ASSERT_TRUE_MESSAGE(false,
            "tremor test's `before` snapshot must fit in what the "
            "framebuffer leaves");
    }
    memcpy(before, flash_displayed,
           (size_t)FLASH_TEST_W * FLASH_TEST_H * sizeof *before);

    bool fired = false;
    uint32_t wake_elapsed_ms = flash_wake_elapsed_ms;
    int fires = 0;

    /* PORTRAIT LOCK, so the noisy component is gx: +/-12 either side of
     * zero, exactly the tilt_x the sidecars report, against a steady tilt_y.
     * The simulation stays frozen, so the pool cannot move and any change in
     * what is displayed came from the bookkeeping alone. */
    for (int f = 0; f < TREMOR_TEST_FRAMES; f++) {
        const int gx = (f & 1) ? 12 : -12;
        const int gy = FLASH_TEST_G;
        flash_test_frame_reset(gx, gy, FLASH_TEST_W, FLASH_TEST_H, gate_reset,
                               &fx_ray.flash_ray_state, &flash_vdom_prev,
                               &flash_vrev_prev, &flash_hrev_prev, &fired);
        fires += fired ? 1 : 0;
        wake_elapsed_ms += FLASH_TEST_DT_MS;
        bool wake_fired = false;
        if (wake_elapsed_ms >= FLASH_TEST_WAKE_MS) {
            wake_elapsed_ms -= (wake_elapsed_ms / FLASH_TEST_WAKE_MS) *
                                FLASH_TEST_WAKE_MS;
            wake_fired = true;
        }
        flash_test_paint(gx, gy, wake_fired);
    }

    int diff = 0;
    for (int y = 0; y < FLASH_TEST_H; y++) {
        for (int x = 0; x < FLASH_TEST_W; x++) {
            const int k = y * FLASH_TEST_W + x;
            if (before[k] < 0 || flash_displayed[k] < 0 ||
                !flash_test_is_interior_liquid(x, y)) {
                continue;
            }
            if (before[k] != flash_displayed[k]) {
                diff++;
            }
        }
    }

    flash_test_free();
    free(before);
    *resets = fires;
    *changed = diff;
}

static void test_axis_lock_tremor_does_not_wipe_the_depth_debounce(void)
{
    int gated_resets = 0, gated_changed = 0;
    tremor_test_run(true, &gated_resets, &gated_changed);

    int ungated_resets = 0, ungated_changed = 0;
    tremor_test_run(false, &ungated_resets, &ungated_changed);

    char why[1000];
    snprintf(why, sizeof why,
             "%d of %d frames of hand tremor on a gravity component that is "
             "already sitting on zero fired a full wipe of the local-depth "
             "walk state, and %d interior cells' displayed depth moved as a "
             "result. Without the relevance gate the same run measures %d "
             "resets and %d moved cells. A sign flip on a component the "
             "active regime's walk cannot observe must cost nothing - see "
             "update_local_depth_gravity()'s own comment in app_sand.c for "
             "why the gate is exact arithmetic rather than a deadband",
             gated_resets, (int)TREMOR_TEST_FRAMES, gated_changed,
             ungated_resets, ungated_changed);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gated_resets, why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gated_changed, why);
    /* ...and the same two measured against the ungated run, so that this
     * test cannot quietly pass because the scene stopped exercising the bug
     * rather than because the gate is working. */
    TEST_ASSERT_TRUE_MESSAGE(ungated_resets > TREMOR_TEST_FRAMES / 2, why);
    TEST_ASSERT_TRUE_MESSAGE(ungated_changed > 0, why);
}

/*=============================================================================
 * A SUBMERGED OBSTACLE'S SHADOW MUST FOLLOW GRAVITY
 *
 * THE SHADOW ITSELF IS A KEPT, DELIBERATE FEATURE - see LOCAL DEPTH's own
 * top comment in app_sand.c. This test's OWN NAME used to claim the
 * opposite (`test_a_submerged_obstacle_does_not_cast_a_depth_shadow`,
 * pinning the projection-then-max combiner's own fix for a DIFFERENT
 * defect - see git log up to commit 3376c8e for that version in full): a
 * submerged obstacle blocking one AXIS-ALIGNED walk while the other still
 * reached the surface produced a wrongly-shallow rectangle beside it,
 * flipping side with the sign of gx. That defect is still closed here (a
 * single ray walk has only one axis to block, so there is no SECOND,
 * unblocked axis for a max-combiner to prefer - the obstacle's own local
 * effect is the only thing that can shorten the walk at all), but it is no
 * longer the interesting claim: the SAME axis-aligned walk that closed it
 * could only ever cast an axis-aligned shadow, straight down or straight
 * sideways, never along the actual tilt - see this section's own measured
 * bearing table below for exactly how wrong, and by how much.
 *
 * MEASURED, on a settled pool with a submerged 3x3 stone dead centre (away
 * from every wall, so nothing here is confounded with "distance to the
 * nearest wall" - the SAME confound test_a_submerged_obstacle_does_not_
 * cast_a_depth_shadow's own comment on the two-walk design already found
 * and fixed the same way, with real side walls, and this test inherits that
 * fix), the shadow's own deficit-weighted centroid bearing against true
 * gravity's own bearing, across six tilts:
 *
 *     gravity (gx, gy)   tilt from vertical   bearing off by
 *          (546,  838)         33.1 deg            1.2 deg
 *          (550,  835)         33.3 deg            0.4 deg
 *          (611,  792)         37.8 deg            0.1 deg
 *          (707,  707)         45.0 deg            0.0 deg
 *          (303,  953)         16.7 deg            1.1 deg
 *          (958,  288)         73.3 deg            1.2 deg
 *
 * THE SAME SIX TILTS a two-walk, axis-aligned combiner (app_sand.c, commit
 * 3376c8e) was measured against separately (see LOCAL DEPTH's own top
 * comment for that table): bearing always exactly +/-90 or 0, off by
 * EXACTLY the tilt angle every time, and no shadow AT ALL at the 45-degree
 * row (both projected axes reach the surface equally there, so max() hides
 * it). THIS walk stays within 1.5 degrees of true gravity at every one of
 * the same six tilts, INCLUDING 45 degrees, where the old design had
 * nothing to measure at all.
 *
 * THE BOUND below (5 degrees) is more than three times this test's own
 * worst measured value (1.2), not a value tuned to just clear it - real
 * headroom against the scene or the walk drifting slightly under a future
 * change, while still failing hard against anything resembling the old
 * design's own tilt-sized error (17-38 degrees across this same six
 * tilts). */

/* NOT a size picked for this test's own runtime convenience. 92x112 gives
 * every one of the six tilts comfortable saturation headroom on every side. */
enum { SHADOW_TEST_W = 92, SHADOW_TEST_H = 112 };
/* shadow_test_cells used to be a file static here (10304 bytes, resident for
 * the whole suite even though only the one test below ever touches it) -
 * malloc'd there instead, freed once that test is done with the grid. */

static bool shadow_test_is_liquid(int x, int y)
{
    if (x < 0 || y < 0 || x >= SHADOW_TEST_W || y >= SHADOW_TEST_H) {
        return false;
    }
    const cell_t c = sand_at(&fx.shadow_test_grid, x, y);
    return !CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID;
}

/* Six tilts, magnitude ~1000 matching this file's own *1000 convention -
 * the SAME six this section's own top comment measures against, chosen to
 * match LOCAL DEPTH's own top comment in app_sand.c exactly so the two
 * tables can be read side by side. */
static const struct { int gx, gy; } SHADOW_SWEEP[] = {
    { 546, 838 },   /* 33.1 degrees from vertical */
    { 550, 835 },   /* 33.3 degrees */
    { 611, 792 },   /* 37.8 degrees */
    { 707, 707 },   /* 45.0 degrees - the old design's own blind spot */
    { 303, 953 },   /* 16.7 degrees */
    { 958, 288 },   /* 73.3 degrees */
};
#define SHADOW_SWEEP_N (sizeof SHADOW_SWEEP / sizeof SHADOW_SWEEP[0])

/* isolates WALK's steady-state shape, uses
 * test_the_blend_has_no_jump_crossing_45_degrees logic, writes depth to
 * `depth_out` */
static void shadow_test_coherent_pass(int gx, int gy, unsigned depth_out[])
{
    bool vdom, vrev, hrev;
    unsigned ax, ay, scale_q8;
    ray_walk_frame_facts(gx, gy, &vdom, &vrev, &hrev, &ax, &ay, &scale_q8);

    ray_walk_state_t st;
    ray_walk_state_reset(&st);
    const bool asc = !vrev;
    for (int r = 0; r < SHADOW_TEST_H; r++) {
        const int cy = asc ? r : (SHADOW_TEST_H - 1 - r);
        unsigned row_depth[RAY_WALK_STATE_W];
        mirror_ray_walk_row(&fx.shadow_test_grid, cy, SHADOW_TEST_W,
                            SHADOW_TEST_H, vdom, vrev, hrev, ax, ay, scale_q8,
                            MATERIAL_LIQUID_DEPTH_BAND, &st, row_depth);
        for (int x = 0; x < SHADOW_TEST_W; x++) {
            depth_out[cy * SHADOW_TEST_W + x] = row_depth[x];
        }
    }
}

/* FLOATING POINT, DELIBERATELY, for bearing comparison to avoid overflow. */
static bool shadow_test_bearing(const unsigned depth[], int sx, int sy,
                                int gx, int gy, double *bearing_off_by)
{
    double tot = 0.0, mx = 0.0, my = 0.0;
    bool any = false;

    for (int dy = -16; dy <= 16; dy++) {
        for (int dx = -16; dx <= 16; dx++) {
            const int x = sx + dx, y = sy + dy;
            if (x < 2 || y < 2 || x >= SHADOW_TEST_W - 2 ||
                y >= SHADOW_TEST_H - 2) {
                continue;
            }
            if (!shadow_test_is_liquid(x, y)) {
                continue;
            }
            const int deficit = (int)MATERIAL_LIQUID_DEPTH_BAND -
                (int)depth[y * SHADOW_TEST_W + x];
            if (deficit <= 0) {
                continue;
            }
            any = true;
            tot += deficit;
            mx += (double)dx * deficit;
            my += (double)dy * deficit;
        }
    }
    if (!any || tot == 0.0) {
        return false;
    }
    mx /= tot;
    my /= tot;

    const double shadow_bearing = atan2(my, mx) * 180.0 / M_PI;
    const double gravity_bearing = atan2((double)gy, (double)gx) * 180.0 / M_PI;
    double diff = shadow_bearing - gravity_bearing;
    while (diff > 180.0) diff -= 360.0;
    while (diff < -180.0) diff += 360.0;
    *bearing_off_by = diff < 0.0 ? -diff : diff;
    return true;
}

/* Comfortably above this section's own worst measured value (1.2 degrees
 * across the six tilts above), comfortably below the two-walk design's own
 * best case (16.7 degrees, its smallest tilt) - see this section's own top
 * comment for the full measured table both designs produce. */
#define SHADOW_TEST_MAX_BEARING_OFF_BY_DEG 5.0

static void test_a_submerged_obstacle_casts_a_gravity_aligned_shadow(void)
{
    enum { PW = SHADOW_TEST_W, PH = SHADOW_TEST_H };
    uint8_t *shadow_test_cells = malloc((size_t)PW * PH);
    TEST_ASSERT_NOT_NULL(shadow_test_cells);
    sand_init(&fx.shadow_test_grid, shadow_test_cells, PW, PH, 777u);

    for (int y = 0; y < PH; y++) {
        sand_set(&fx.shadow_test_grid, 0, y, CELL_MAKE(MAT_STONE, 0));
        sand_set(&fx.shadow_test_grid, PW - 1, y, CELL_MAKE(MAT_STONE, 0));
    }
    for (int x = 0; x < PW; x++) {
        sand_set(&fx.shadow_test_grid, x, 0, CELL_MAKE(MAT_STONE, 0));
        sand_set(&fx.shadow_test_grid, x, PH - 1, CELL_MAKE(MAT_STONE, 0));
    }
    for (int y = 1; y < PH - 1; y++) {
        for (int x = 1; x < PW - 1; x++) {
            sand_set(&fx.shadow_test_grid, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int i = 0; i < 400; i++) {
        sand_step(&fx.shadow_test_grid, 1000, 1000, 0);
    }

    /* One stone obstacle, fully submerged, dead centre - away from every
     * wall by more than this test's own 16-cell measurement radius, so
     * nothing measured here is the pool's own wall-distance gradient (see
     * this section's own top comment for the confound this avoids). */
    enum { OX = PW / 2, OY = PH / 2 };
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            sand_set(&fx.shadow_test_grid, OX + dx, OY + dy,
                     CELL_MAKE(MAT_STONE, 0));
        }
    }
    for (int i = 0; i < 30; i++) {
        sand_step(&fx.shadow_test_grid, 1000, 1000, 0);
    }

    if (CELL_MATERIAL(sand_at(&fx.shadow_test_grid, OX, OY)) != MAT_STONE) {
        free(shadow_test_cells);
        TEST_FAIL_MESSAGE("setup: the obstacle must still be stone after settling");
    }
    if (!(shadow_test_is_liquid(OX - 8, OY) && shadow_test_is_liquid(OX + 8, OY) &&
          shadow_test_is_liquid(OX, OY - 8) && shadow_test_is_liquid(OX, OY + 8))) {
        free(shadow_test_cells);
        TEST_FAIL_MESSAGE(
            "setup: water must remain on all four sides of the obstacle, or "
            "this test is not exercising a genuinely submerged obstacle");
    }

    unsigned *depth = malloc(sizeof *depth * (size_t)SHADOW_TEST_W * SHADOW_TEST_H);
    if (depth == NULL) {
        free(shadow_test_cells);
        TEST_FAIL_MESSAGE("setup: could not allocate the depth field");
    }
    double worst_off_by = 0.0;
    size_t worst_i = 0;

    for (size_t i = 0; i < SHADOW_SWEEP_N; i++) {
        const int gx = SHADOW_SWEEP[i].gx, gy = SHADOW_SWEEP[i].gy;
        shadow_test_coherent_pass(gx, gy, depth);

        double off_by = 0.0;
        const bool has_shadow = shadow_test_bearing(depth, OX, OY, gx, gy,
                                                     &off_by);
        char why_shadow[256];
        snprintf(why_shadow, sizeof why_shadow,
                 "no shadow at all at gravity (%d, %d) - the whole point of "
                 "a single ray walk over the two-axis combiner it replaced "
                 "is that the shadow no longer vanishes at any tilt, "
                 "including the old design's own 45-degree blind spot",
                 gx, gy);
        if (!has_shadow) {
            free(depth);
            free(shadow_test_cells);
            TEST_FAIL_MESSAGE(why_shadow);
        }

        if (off_by > worst_off_by) {
            worst_off_by = off_by;
            worst_i = i;
        }
    }

    free(depth);
    free(shadow_test_cells);

    char why[400];
    snprintf(why, sizeof why,
             "shadow bearing was off from true gravity by %.1f degrees at "
             "gravity (%d, %d) - more than %.1f degrees, the shadow behind a "
             "submerged obstacle must lie along the gravity ray, not along "
             "a screen axis (see this section's own top comment for the "
             "six-tilt table this test checks against)",
             worst_off_by, SHADOW_SWEEP[worst_i].gx, SHADOW_SWEEP[worst_i].gy,
             SHADOW_TEST_MAX_BEARING_OFF_BY_DEG);
    TEST_ASSERT_TRUE_MESSAGE(worst_off_by <= SHADOW_TEST_MAX_BEARING_OFF_BY_DEG,
        why);
}

/*=============================================================================
 * A FIXED TRUE DEPTH MUST READ THE SAME AT EVERY TILT ANGLE
 *
 * DEFECT TWO of the two the projection-then-max combiner closed for the
 * two-walk design (see LOCAL DEPTH's own top comment in app_sand.c) -
 * closed a DIFFERENT way here, since there is no second axis left to
 * project and max against. The claim is unchanged: a cell at a fixed true
 * perpendicular depth D must read D regardless of gravity's own tilt
 * angle, no obstacle, no staleness involved.
 *
 * WHY THIS WALK'S OWN RAW COUNT IS NOT THE OLD DESIGN'S - a genuine trap
 * this test's own first draft fell into and is worth naming so it does not
 * recur: the OLD axis-aligned walk counted cells along a FIXED SCREEN AXIS,
 * so recovering true depth from its raw count meant SHRINKING it by that
 * axis's own share of gravity (`component / len`, always <= 256). THIS
 * walk already follows the gravity ray itself, so its own raw count is a
 * SMALLER number for the same true depth (each step already covers `len /
 * component` cells of true distance, not one screen cell) - recovering
 * true depth means GROWING it back by that same factor instead (`len /
 * component`, always >= 256; see LOCAL_DEPTH_COUNT_CEILING's own comment
 * in app_sand.c for the same point made about the ceiling). Reusing the OLD
 * `defect2_raw_count()` formula (`D * 1000 / component`) unchanged against
 * THIS walk's own scale reads a fixed true depth of 10 as 19 at the
 * 45-degree tie point - a worse defect than the one this test exists to
 * catch - until the formula below (`D * component / 1000`) replaced it.
 *
 * NO SAND GRID HERE - this is a property of the WALK's own projection given
 * an idealised planar input, checked directly rather than via a settled
 * grid, the same reasoning test_a_saturated_liquid_body_reads_the_same_
 * shade_at_every_tilt_angle below uses. */
static const struct { int gx, gy; } DEFECT2_SWEEP[] = {
    {    0, 1000 },   /*  0 degrees - gravity straight down */
    {  259,  966 },   /* 15 degrees */
    {  500,  866 },   /* 30 degrees */
    {  707,  707 },   /* 45 degrees - the tie point */
    {  866,  500 },   /* 60 degrees */
    {  966,  259 },   /* 75 degrees */
    { 1000,    0 },   /* 90 degrees - gravity straight along x */
};
#define DEFECT2_SWEEP_N (sizeof DEFECT2_SWEEP / sizeof DEFECT2_SWEEP[0])
#define DEFECT2_TRUE_DEPTH 10u

static unsigned defect2_raw_count(unsigned component, int len)
{
    if (len <= 0) {
        return 0u;
    }
    const unsigned raw = (DEFECT2_TRUE_DEPTH * component) / (unsigned)len;
    return raw < SUITE_LOCAL_DEPTH_COUNT_CEILING
        ? raw : SUITE_LOCAL_DEPTH_COUNT_CEILING;
}

/* Tighter than the loosest defensible bound (half a shade step, 3) - the
 * fixed walk measures 0-1 deviation across this sweep, while the old
 * two-walk blend read 4 (a full shade step) at 30 degrees. */
#define DEFECT2_MAX_DEVIATION 2

static void test_a_fixed_depth_reads_the_same_at_every_tilt_angle(void)
{
    int worst_deviation = 0;
    int worst_i = -1;

    for (size_t i = 0; i < DEFECT2_SWEEP_N; i++) {
        const int gx = DEFECT2_SWEEP[i].gx, gy = DEFECT2_SWEEP[i].gy;
        const unsigned ax = (unsigned)gx, ay = (unsigned)gy;
        const int len = im_len(gx, gy);
        const bool vdom = (ay >= ax);
        const unsigned dom_axis = vdom ? ay : ax;

        const unsigned count = defect2_raw_count(dom_axis, len);
        const unsigned scale_q8 = dom_axis
            ? (256u * (unsigned)len) / dom_axis : 256u;

        /* Combine-time projection, clamped to MATERIAL_LIQUID_DEPTH_BAND -
         * see paint_row_n()'s own "THE PROJECTION" (app_sand.c). Gravity is
         * static within one sweep sample here, so there is no
         * stale-accumulator concern to model. */
        const unsigned depth_raw = (count * scale_q8) >> 8;
        const int depth = (int)(depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND);

        const int deviation = depth > (int)DEFECT2_TRUE_DEPTH
            ? depth - (int)DEFECT2_TRUE_DEPTH
            : (int)DEFECT2_TRUE_DEPTH - depth;
        if (deviation > worst_deviation) {
            worst_deviation = deviation;
            worst_i = (int)i;
        }
    }

    char why[320];
    snprintf(why, sizeof why,
             "reported depth deviated from the true fixed depth of %u by %d "
             "(sample %d, gravity (%d, %d)) - a fixed true depth must read "
             "the same regardless of gravity's own tilt angle",
             DEFECT2_TRUE_DEPTH, worst_deviation, worst_i,
             worst_i >= 0 ? DEFECT2_SWEEP[worst_i].gx : 0,
             worst_i >= 0 ? DEFECT2_SWEEP[worst_i].gy : 0);
    TEST_ASSERT_TRUE_MESSAGE(worst_deviation <= DEFECT2_MAX_DEVIATION, why);
}

/*=============================================================================
 * A SATURATED LIQUID BODY MUST READ THE SAME SHADE AT EVERY TILT ANGLE
 *
 * DEFECT THREE, for the two-walk design - see LOCAL DEPTH's own top comment
 * (app_sand.c) for the full account: a column genuinely saturated on BOTH
 * axes reported `(BAND * weight) >> 8`, BELOW the band whenever weight <
 * 256 (i.e. whenever gravity was not exactly axis-aligned) - the pool's
 * entire deep interior visibly "breathing" as the device rotated.
 *
 * THIS WALK NEEDS NO RAISED CEILING TO FIX IT - the load-bearing difference
 * from the two-walk design, not an oversight carried over. This walk's own
 * scale (`len / dominant_axis`) is always >= 256 (equal only at perfect
 * axis alignment), the OPPOSITE direction from the two-walk design's own
 * weight (`component / len`, always <= 256) - so a count clamped at the
 * PLAIN band already projects to AT LEAST the band at every angle, and the
 * combiner's own clamp to MATERIAL_LIQUID_DEPTH_BAND does the rest. See
 * LOCAL_DEPTH_COUNT_CEILING's own comment in app_sand.c for the same
 * argument made about the constant this test's own SUITE_LOCAL_DEPTH_
 * COUNT_CEILING mirrors.
 *
 * NO SAND GRID HERE, same reasoning as test_a_fixed_depth_reads_the_same_
 * at_every_tilt_angle above - a property of the walk's own projection given
 * an idealised, fully-saturated input. */
static const struct { int gx, gy; } SATURATED_SWEEP[] = {
    {    0, 1000 },   /*  0 degrees */
    {  174,  985 },   /* 10 degrees */
    {  342,  940 },   /* 20 degrees */
    {  500,  866 },   /* 30 degrees */
    {  574,  819 },   /* 35 degrees */
    {  643,  766 },   /* 40 degrees */
    {  707,  707 },   /* 45 degrees */
    {  766,  643 },   /* 50 degrees */
    {  819,  574 },   /* 55 degrees */
    {  866,  500 },   /* 60 degrees */
    {  940,  342 },   /* 70 degrees */
    {  985,  174 },   /* 80 degrees */
    { 1000,    0 },   /* 90 degrees */
};
#define SATURATED_SWEEP_N (sizeof SATURATED_SWEEP / sizeof SATURATED_SWEEP[0])

static void test_a_saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle(void)
{
    int lum[SATURATED_SWEEP_N];
    for (size_t i = 0; i < SATURATED_SWEEP_N; i++) {
        const int gx = SATURATED_SWEEP[i].gx, gy = SATURATED_SWEEP[i].gy;
        const unsigned ax = (unsigned)gx, ay = (unsigned)gy;
        const int len = im_len(gx, gy);
        const bool vdom = (ay >= ax);
        const unsigned dom_axis = vdom ? ay : ax;
        const unsigned scale_q8 = dom_axis
            ? (256u * (unsigned)len) / dom_axis : 256u;

        /* Fully saturated at the ceiling - no boundary within it in the
         * dominant direction, the worst (and most common, for a large body
         * of liquid) case this defect can produce - projected at combine
         * time and clamped to the band, exactly like every other cell. */
        const unsigned count = SUITE_LOCAL_DEPTH_COUNT_CEILING;
        const unsigned depth_raw = (count * scale_q8) >> 8;
        const unsigned depth = depth_raw < MATERIAL_LIQUID_DEPTH_BAND
            ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;

        gfx_color_t out[3];
        material_colours(CELL_MAKE(MAT_WATER, MASS_MAX), 0u, 0u, depth, out);
        lum[i] = panel_luminance(out[0]);
    }

    for (size_t i = 1; i < SATURATED_SWEEP_N; i++) {
        char why[320];
        snprintf(why, sizeof why,
                 "a fully saturated liquid body read luminance %d at sample "
                 "%zu but %d at sample 0 (gravity (%d, %d) vs (%d, %d)) - a "
                 "cell with no boundary in the dominant direction must read "
                 "EXACTLY the same shade regardless of gravity's own tilt "
                 "angle, not merely a similar one",
                 lum[i], i, lum[0], SATURATED_SWEEP[i].gx, SATURATED_SWEEP[i].gy,
                 SATURATED_SWEEP[0].gx, SATURATED_SWEEP[0].gy);
        TEST_ASSERT_EQUAL_INT_MESSAGE(lum[0], lum[i], why);
    }
}

void run_sand_liquid_depth_suite(void)
{
    RUN_TEST(test_a_liquid_body_paints_flat_inside);
    RUN_TEST(test_a_liquid_interior_is_shaded_by_depth);
    RUN_TEST(test_only_a_liquid_interior_reads_depth);
    RUN_TEST(test_a_liquid_rim_still_shows_its_fill);
    RUN_TEST(test_a_liquid_rim_catches_the_light_from_above);
    RUN_TEST(test_shine_direction_holds_the_old_diagonal_with_no_gravity);
    RUN_TEST(test_shine_direction_is_minus_gravity_turned_left);
    RUN_TEST(test_shine_direction_is_a_genuine_angle_not_a_snap);
    RUN_TEST(test_shine_direction_is_unit_length);
    RUN_TEST(test_local_depth_follows_the_puddles_own_shape);
    RUN_TEST(test_the_blend_has_no_jump_crossing_45_degrees);
    RUN_TEST(test_a_same_row_reset_commits_but_a_different_row_does_not);
    RUN_TEST(test_a_continuously_moving_boundary_does_not_run_away);
    RUN_TEST(test_the_debounce_survives_open_air_above_the_pool);
    RUN_TEST(test_the_horizontal_debounce_survives_open_air_beside_the_pool);
    RUN_TEST(test_pouring_onto_a_settled_pool_redirties_a_bounded_band_below);
    RUN_TEST(test_every_liquid_interior_is_exactly_the_body_colour_when_saturated);
    RUN_TEST(test_a_shallow_puddle_still_shows_real_darkening);
    RUN_TEST(test_a_settled_edge_does_not_flicker_stale_to_fresh);
    RUN_TEST(test_a_direction_flip_does_not_corrupt_the_boundary_debounce);
    RUN_TEST(test_a_sparse_repaint_does_not_band_a_tall_liquid_column);
    RUN_TEST(test_turning_a_settled_pool_to_landscape_does_not_flash_the_whole_body);
    RUN_TEST(test_axis_lock_tremor_does_not_wipe_the_depth_debounce);
    RUN_TEST(test_a_submerged_obstacle_casts_a_gravity_aligned_shadow);
    RUN_TEST(test_a_fixed_depth_reads_the_same_at_every_tilt_angle);
    RUN_TEST(test_a_saturated_liquid_body_reads_the_same_shade_at_every_tilt_angle);
}

SUITE_REGISTER(run_sand_liquid_depth_suite);
