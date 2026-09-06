/*=============================================================================
 * Portable suite: the falling-sand automaton - lava buried in stone - burial,
 * venting, and bursting.
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

#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* Burying lava does not delete it.
 *
 * smothered() clears a burning cell outright when all four neighbours are
 * denser and solid, which is right for a FLAME - burial starves it of air -
 * and wrong for lava, which is not burning anything. It is simply hot, and
 * burying something hot should leave something hot.
 *
 * Reported as "lava is clearly evaporating against stone, I can even see
 * bubbles up". It was, and the bubbles were its own flare going off as it
 * went. A first probe found nothing because it used a clean rectangular
 * vessel, which has no cell with four solid neighbours; every vessel drawn
 * by hand has dozens. */
static void test_lava_buried_in_stone_is_not_deleted(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    sand_set(&s, W / 2, H / 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    /* This scene puts a complete lid over the lava (bd esp32c6-mqt) -
     * pinned off so this test still isolates smothered()'s
     * own exemption, the thing it actually names, rather than flickering
     * on the unrelated burst roll every step this cell stays covered. */
    sand_set_lava_burst(&s, 0);
    const int before = liquid_mass_of(MAT_LAVA);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX, before,
        "fixture check: one full cell of lava, walled in on all four sides");

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, liquid_mass_of(MAT_LAVA),
        "lava walled in by stone must still be there - smothering puts a "
        "FLAME out because burial starves it of air, and lava is not "
        "burning anything");
}

/* bd esp32c6-mqt: burial no longer means "lasts forever" either. A lava
 * cell with a complete gravity-relative lid over it (covered_at(),
 * sand_priv.h - bd esp32c6-a2j) gets a
 * tiny per-step chance to convert to MAT_STONE and burst - the chosen
 * replacement for the vent machinery (bd esp32c6-0f2 removes that
 * separately, later) and the mechanism that reopens a sealed pool's crust
 * so a sustained pour can keep reaching lava (see cool_off_chain()'s own
 * comment, sand_reactions.c).
 *
 * Exactly the same fully-walled scene test_lava_buried_in_stone_is_not_
 * deleted uses, this time with the burst chance pinned to its maximum
 * instead of pinned off - every one of the 8 surrounding cells is stone,
 * so all three lid cells are covered regardless of which way is down.
 *
 * PINS THE ONE BEHAVIOUR THE BRIEF FOR THIS CHANGE CALLS OUT BY NAME:
 * sand_explode() fills a core of radius `radius / SAND_EXPLODE_CORE_
 * DIVISOR` with fire FIRST, unconditionally, before it queues a single
 * flight entry (sand.h, SAND_EXPLODE_CORE_DIVISOR's own comment) - at
 * SAND_LAVA_BURST_RADIUS(8) and divisor 5 that core radius is 1, so the
 * MAT_STONE this feature just wrote at the centre is immediately
 * overwritten by fresh fire. Asserted directly, not assumed - the same
 * discipline test_a_confined_gas_pocket_bursts_instead_of_just_catching
 * already applies to its own centre cell for gas's identical sand_
 * explode() core fill. */
static void test_buried_lava_bursts_into_stone_and_fire(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    const int cx = W / 2, cy = H / 2;
    sand_set(&s, cx, cy, CELL_MAKE(MAT_LAVA, MASS_MAX));

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "buried-lava-burst impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, buf, W * H);
    sand_set_lava_burst(&s, 255);

    bool burst = false;
    for (int i = 0; i < 10 && !burst; i++) {
        sand_step(&s, 0, 1000, 0);
        burst = CELL_MATERIAL(sand_at(&s, cx, cy)) != MAT_LAVA;
    }

    const uint8_t centre_material = CELL_MATERIAL(sand_at(&s, cx, cy));
    const int impulse_count = s.impulse_count;
    /* Cover disturbed - at least one of the four cells that were doing
     * the covering must no longer be plain, undisturbed stone, or nothing
     * actually exploded outward from the centre. */
    int cover_disturbed = 0;
    if (CELL_MATERIAL(sand_at(&s, cx - 1, cy)) != MAT_STONE) cover_disturbed++;
    if (CELL_MATERIAL(sand_at(&s, cx + 1, cy)) != MAT_STONE) cover_disturbed++;
    if (CELL_MATERIAL(sand_at(&s, cx, cy - 1)) != MAT_STONE) cover_disturbed++;
    if (CELL_MATERIAL(sand_at(&s, cx, cy + 1)) != MAT_STONE) cover_disturbed++;

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(buf);

    TEST_ASSERT_TRUE_MESSAGE(burst,
        "a lava cell covered on all four sides, with the burst chance "
        "pinned to its maximum, must stop being lava within this budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, centre_material,
        "sand_explode()'s own core fill (SAND_EXPLODE_CORE_DIVISOR, "
        "sand.h) overwrites the centre cell with fire on its very first "
        "line, occupied or not - the MAT_STONE this feature just wrote "
        "there does not survive to be seen, and that is expected, pinned "
        "behaviour, not a bug");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, impulse_count,
        "a real burst must also queue its own outward annulus, not just "
        "fill a core of fire in place");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, cover_disturbed,
        "the burst must disturb at least one of the four cells that were "
        "covering it, or nothing actually exploded outward");
}

/* A LID WITH GAPS IS NOT A LID - the test that goes green when it should
 * not if covered_at() were ever loosened from "all three" to "any two"
 * or "the middle one alone".
 *
 * A CEILING WITH ALTERNATING HOLES gives the structural guarantee: stone
 * at even columns, open at odd ones. A lava cell under an open column
 * sees BOTH lid diagonals (the even columns either side) covered but the
 * cell directly above it empty; a cell under a solid column sees only
 * the cell above it, both diagonals open. Every column in the scene is
 * one of the two, so no cell in the row ever has all three, regardless
 * of how the liquid itself moves. */
static void test_lava_under_a_lid_with_gaps_never_bursts(void)
{
    fixture();
    sand_clear(&s);
    const int y = H / 2;
    for (int x = 0; x < W; x++) {
        if (x % 2 == 0) {
            sand_set(&s, x, y - 1, STONE);   /* solid ceiling column */
        }                                    /* odd columns: left open */
        sand_set(&s, x, y + 1, STONE);       /* floor, holds the pool down */
        sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }
    sand_set_lava_burst(&s, 255);
    const int before = liquid_mass_of(MAT_LAVA);
    TEST_ASSERT_EQUAL_INT_MESSAGE(W * MASS_MAX, before,
        "fixture check: a full-width row of lava, floored throughout and "
        "ceiled only on every other column");

    for (int i = 0; i < 500; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, liquid_mass_of(MAT_LAVA),
        "a lava cell under a lid with a gap in it - both diagonals covered "
        "and the cell above open, or the cell above covered and both "
        "diagonals open - must never burst, no matter how high the chance "
        "is pinned or how long it runs: all three lid cells, not any two");
}

/* THE SIDES OF A BASIN ARE NOT A LID - the 2026-09-03 revision of bd
 * esp32c6-a2j's rule. A finger-drawn stone wall is never flat: each
 * brush disc bulges past the one below it, so the inner face has a
 * one-cell notch every brush step, and lava settling into that notch
 * sees wall on its side, wall on the diagonal above that side, and wall
 * directly above - three covering cells in a contiguous run. Under the
 * 5-cell semi-disc rule that sealed, so every notch on both walls of an
 * ordinary hand-drawn basin rolled the burst every step and the walls
 * blew out from the inside as the lava settled (reproduced on the host:
 * a clean one-cell wall never produced a single eligible cell, a
 * brush-drawn one did within 16 steps and breached by step 62 at natural
 * odds). The pool's surface is wide open a cell to the side, so nothing
 * is under pressure there; the sides were counting as cover.
 *
 * The rule is now "lid only": the three anti-gravity cells, all of them,
 * and the two perpendiculars never count. Both notch shapes - {up-left,
 * up, left} on the left wall and {up, up-right, right} on the right -
 * must therefore never burst, however long it runs and however high the
 * chance is pinned. The walls' intact-stone count is the assertion, not
 * the notch cells' own material, because a burst that fires is what
 * takes the wall out. */
static void test_lava_in_a_wall_notch_never_bursts(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        sand_set(&s, 0, y, STONE);   /* left wall, two thick */
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);   /* right wall, two thick */
        sand_set(&s, 7, y, STONE);
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 7, STONE);   /* floor */
    }
    sand_set(&s, 2, 2, STONE);       /* the brush bulge over each notch */
    sand_set(&s, 5, 2, STONE);
    for (int y = 3; y <= 6; y++) {
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }
    /*   ##....##
     *   ##....##
     *   ###..###   <- bulges at (2,2) and (5,2)
     *   ##LLLL##   <- notch cells (2,3) and (5,3), open above at x 3..4
     *   ##LLLL##
     *   ##LLLL##
     *   ##LLLL##
     *   ########                                                        */
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 2)) &&
                             CELL_IS_EMPTY(sand_at(&s, 4, 2)),
        "fixture check: the pool's surface is open to the air between the "
        "two bulges - this is a basin, not a sealed pocket");

    int stone_before = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            stone_before += CELL_MATERIAL(sand_at(&s, x, y)) == MAT_STONE;
        }
    }

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "wall-notch impulse queue must fit in what the framebuffer leaves");
    sand_enable_impulses(&s, buf, W * H);
    sand_set_lava_burst(&s, 255);

    for (int i = 0; i < 500; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int stone_after = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            stone_after += CELL_MATERIAL(sand_at(&s, x, y)) == MAT_STONE;
        }
    }
    free(buf);
    TEST_ASSERT_EQUAL_INT_MESSAGE(stone_before, stone_after,
        "lava settled into a notch on a basin's inner wall - wall to its "
        "side, wall on the diagonal above that side, wall directly above, "
        "open air a cell over - must never burst, however high the chance "
        "is pinned: the two perpendiculars are not part of the lid, and a "
        "burst here is what blows the side out of every hand-drawn basin");
}

/* THE EXHAUSTIVE SHAPE TABLE. A rule that rotates with gravity and is
 * only ever exercised at one rotation is not really tested, so this
 * drives cover_mask()/covered_at() (sand_priv.h) directly, at each of the
 * 8 ring directions, over every one of the 256 ways to paint the full
 * 8-neighbour ring covered or open. Every combo first checks that
 * cover_mask() equals a mask built independently from that combo's own
 * bits at the 3 lid positions (ring_of()/ring_dir(), already covered by
 * their own tests elsewhere - not cover_mask()'s own logic checked
 * against itself), proving it genuinely ignores the 5 cells gravity
 * excludes - the two PERPENDICULARS included, the cells whose counting
 * blew the sides out of hand-drawn basins (see
 * test_lava_in_a_wall_notch_never_bursts above) - and not merely that it
 * is never asked about them. Then covered_at() must be true for exactly
 * the combos with all three lid cells painted and no other: 32 of the
 * 256, the same at every gravity direction, axis-aligned or diagonal.
 * The semi-disc rule this replaced had 7 valid shapes at axis-aligned
 * gravity and 6 at diagonal; the lid rule has one, everywhere. */
static void test_cover_primitive_matches_the_exhaustive_shape_table(void)
{
    fixture();
    sand_clear(&s);
    const int cx = W / 2, cy = H / 2;
    sand_set(&s, cx, cy, CELL_MAKE(MAT_LAVA, MASS_MAX));
    const uint8_t density = material_by_id((material_id_t)MAT_LAVA)->density;

    for (int g = 0; g < 8; g++) {
        const int *grav = ring_dir(g);
        s.last_load_dx = grav[0];
        s.last_load_dy = grav[1];
        const int anti = g + 4;   /* ring_of(grav) is just g */
        const unsigned lid_ring_bits = (1u << ((anti + 7) & 7)) |
                                       (1u << (anti & 7)) |
                                       (1u << ((anti + 1) & 7));

        int sealed = 0;
        for (unsigned combo = 0; combo < 256u; combo++) {
            for (int i = 0; i < 8; i++) {
                const int *d = ring_dir(i);
                sand_set(&s, cx + d[0], cy + d[1],
                        (combo & (1u << i)) ? STONE : CELL_EMPTY);
            }

            const unsigned mask = cover_mask(&s, cx, cy, W, H, density);
            unsigned expected = 0;
            for (int i = 0; i < 3; i++) {
                if (combo & (1u << ((anti - 1 + i) & 7))) {
                    expected |= 1u << i;
                }
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)expected, (int)mask,
                "cover_mask() must depend only on the 3 lid ring positions, "
                "read in ring order, and nothing else - not the "
                "perpendiculars, not anything gravity-ward");

            const bool lid_complete = (combo & lid_ring_bits) == lid_ring_bits;
            TEST_ASSERT_EQUAL_MESSAGE(lid_complete,
                covered_at(&s, cx, cy, W, H, density),
                "covered_at() must be true exactly when all three lid cells "
                "are covered, whatever the other five hold");
            sealed += lid_complete;
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(32, sealed,
            "exactly 32 of the 256 ring paintings carry a complete lid (the "
            "other five cells free to be anything), at every gravity "
            "direction alike - there is no axis-aligned/diagonal asymmetry "
            "left in this rule");
    }
}

/* THE HEADLINE CASE - bd esp32c6-a2j. An INTERIOR cell of a lava pool
 * wider than one cell and more than one cell deep, sealed only by a
 * crust directly above it, must still burst - the case cover_count()'s
 * cardinal rule could never fire for at all (see this file's own header
 * comment and bd esp32c6-a2j's own notes for why): the tested cell's
 * left, right AND below neighbours are all more lava - KIND_LIQUID,
 * which neighbor_smothers() never counts - so under the OLD screen-fixed
 * cardinal rule only "up" ever counted, one of the three it needed.
 * Under THIS rule up-left, up and up-right are the whole question, and
 * all three are crust - a complete lid, regardless of what the pool does
 * below or beside the tested cell. */
static void test_a_wide_pool_under_a_crust_bursts(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 1, STONE);   /* the crust: a wide, solid lid */
        sand_set(&s, x, 4, STONE);   /* floor, keeps the pool from draining */
    }
    for (int y = 2; y <= 3; y++) {
        for (int x = 1; x <= 6; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const int tx = 3, ty = 2;   /* interior: not touching the pool's own
                                  * left/right edge, so both diagonal
                                  * neighbours above it are genuine crust */
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx, ty)),
        "fixture check: lava at the cell under test");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx - 1, ty)),
        "fixture check: more lava to its left, not stone or open air - a "
        "genuine pool interior, not an isolated cell in a solid pocket");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx + 1, ty)),
        "fixture check: more lava to its right too");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx, ty + 1)),
        "fixture check: and more lava directly below it - this cell has "
        "no support of its own, only the crust above seals it in");

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "wide-pool-burst impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, buf, W * H);
    sand_set_lava_burst(&s, 255);

    bool burst = false;
    for (int i = 0; i < 10 && !burst; i++) {
        sand_step(&s, 0, 1000, 0);
        burst = CELL_MATERIAL(sand_at(&s, tx, ty)) != MAT_LAVA;
    }

    free(buf);
    TEST_ASSERT_TRUE_MESSAGE(burst,
        "an interior cell of a wide, deep lava pool, sealed only by a "
        "crust directly above it, must still burst within this budget - "
        "the case cover_count()'s screen-fixed cardinal rule could never "
        "fire for, because it counted 'down' (more lava, never counted) "
        "instead of the two upper diagonals (crust, which do)");
}

/* GRAVITY-RELATIVE, NOT SCREEN-RELATIVE - the risk covered_at() inherits
 * from cover_mask() (bd esp32c6-a2j): the wide-pool-under-a-crust test
 * just above only ever exercises this under ordinary downward gravity,
 * so a regression that quietly swapped s->last_load_dx/dy for a fixed
 * screen-up direction would still pass it. This is the direct
 * descendant of the now-removed test_sealed_lava_vents_toward_gravity_
 * relative_up (bd esp32c6-0f2 retired the vent mechanism it guarded;
 * the behaviour it stopped from regressing - gravity, not the screen,
 * decides "up" - survives here against the burst instead).
 *
 * The same wide-pool-under-a-crust shape, TRANSPOSED: a pool wide along
 * screen-y instead of screen-x, sealed by a crust to its LEFT instead of
 * above it, driven by genuine sideways gravity (gx=1000, gy=0) so
 * gravity-relative "up" is screen-LEFT. Under the correct rule the
 * interior test cell's three lid cells (left, up-left, down-left,
 * gravity-relative) are all crust - a complete lid. Under a regression
 * hardcoded to screen-up, the same cell's lid would be up-left, up and
 * up-right, of which only up-left is stone (the other two are more
 * lava) - never bursts. */
static void test_a_wide_pool_under_a_sideways_crust_bursts(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        sand_set(&s, 1, y, STONE);   /* crust: a tall wall to the left */
        sand_set(&s, 4, y, STONE);   /* wall, keeps the pool from draining */
    }
    for (int x = 2; x <= 3; x++) {
        for (int y = 1; y <= 6; y++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const int tx = 2, ty = 3;   /* interior: touching the crust directly
                                  * to its left, but not the pool's own
                                  * top/bottom edge, so both diagonal
                                  * neighbours toward the crust are
                                  * genuine crust too */
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx, ty)),
        "fixture check: lava at the cell under test");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx, ty - 1)),
        "fixture check: more lava above it - screen-up is NOT "
        "gravity-relative up in this scene");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA,
        CELL_MATERIAL(sand_at(&s, tx, ty + 1)),
        "fixture check: and more lava below it too");

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "sideways-pool-burst impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&s, buf, W * H);
    sand_set_lava_burst(&s, 255);

    bool burst = false;
    for (int i = 0; i < 10 && !burst; i++) {
        sand_step(&s, 1000, 0, 0);   /* gravity pulls RIGHT, not down */
        burst = CELL_MATERIAL(sand_at(&s, tx, ty)) != MAT_LAVA;
    }

    free(buf);
    TEST_ASSERT_TRUE_MESSAGE(burst,
        "under sideways gravity, a lava cell sealed by a crust to its "
        "gravity-relative up (screen-LEFT here) must still burst - if "
        "this fails while test_a_wide_pool_under_a_crust_bursts (its "
        "downward-gravity sibling) still passes, the cover gate "
        "regressed to a fixed screen direction instead of "
        "s->last_load_dx/dy");
}

/* THE NEGATIVE COUNTERPART: an ordinary, uncovered open pool - a floor
 * beneath it, open air above and to both sides - must never burst either,
 * however high the chance is pinned. Guards against this firing on scenes
 * it must not touch at all, not just against the threshold being wrong. */
static void test_an_open_lava_pool_never_bursts(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 2; x < W - 2; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }
    sand_set_lava_burst(&s, 255);
    const int before = liquid_mass_of(MAT_LAVA);
    TEST_ASSERT_TRUE_MESSAGE(before > 0,
        "fixture check: an open pool of lava sitting on a floor");

    for (int i = 0; i < 500; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, liquid_mass_of(MAT_LAVA),
        "an ordinary open lava pool - a floor beneath it, open air above "
        "and to both sides - must never burst, however high the chance "
        "is pinned");
}

/* sand_explode() is a documented no-op without sand_enable_impulses()
 * (its own first line, sand.c) - so with impulses never enabled, a burst
 * must still convert the cell to MAT_STONE (place_reacted() does not
 * touch s->impulse_buf at all) and simply throw nothing. Not gated on
 * impulses being enabled at all - see step_one_burning_cell()'s own new
 * block for why that is correct rather than a gap: no impulses means no
 * explosions anywhere else in the simulation either, and a bare
 * conversion to stone is not a wrong answer on its own. */
static void test_buried_lava_still_becomes_stone_with_impulses_off(void)
{
    fixture();
    sand_clear(&s);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    const int cx = W / 2, cy = H / 2;
    sand_set(&s, cx, cy, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set_lava_burst(&s, 255);

    bool converted = false;
    for (int i = 0; i < 10 && !converted; i++) {
        sand_step(&s, 0, 1000, 0);
        converted = CELL_MATERIAL(sand_at(&s, cx, cy)) != MAT_LAVA;
    }

    TEST_ASSERT_TRUE_MESSAGE(converted,
        "a covered lava cell must still convert away from lava within "
        "this budget even with impulses never enabled");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, cx, cy)),
        "with no impulse buffer, sand_explode() is a pure no-op - the "
        "centre cell must become stone and stay stone, not fire, since "
        "nothing was thrown to fill any core with");
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(MAT_FIRE,
                CELL_MATERIAL(sand_at(&s, cx + dx, cy + dy)),
                "no impulse buffer means no explosion at all - fire must "
                "not appear anywhere around the converted cell either");
        }
    }
}

/* Nor does conducted heat boil it away.
 *
 * conduct_heat() turns whatever the heat reaches into steam if that cell
 * is KIND_LIQUID, and lava is a liquid - so lava on the far side of a
 * conductor was boiled into steam by the heat of other lava. A plain pool
 * never showed it, because a pool has no conductor running through it. A
 * vessel with stone in it does, and that is what a drawn one looks like.
 *
 * The measurement that found it: a pillared vessel lost 83% of its lava in
 * 200 steps where a flat-floored one lost none, and water and oil in the
 * same pillared vessel lost nothing - which is what ruled out a bug in
 * liquid movement and pointed at the burning path. */
static void test_lava_is_not_boiled_by_its_own_conducted_heat(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Two pools of lava with a single conducting wall between them. */
    const int wall = W / 2;
    for (int y = H - 3; y < H - 1; y++) {
        sand_set(&s, wall, y, STONE);
        for (int x = 1; x < W - 1; x++) {
            if (x != wall) {
                sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
    }
    const int before = liquid_mass_of(MAT_LAVA);

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, liquid_mass_of(MAT_LAVA),
        "lava must not be boiled into steam by heat conducted from other "
        "lava - a liquid that BURNS is a heat source, and a heat source is "
        "not a kettle");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_STEAM),
        "and no steam at all should come off it: there is no water here, "
        "so steam is the visible symptom of lava being boiled");
}

void run_sand_lava_burial_suite(void)
{
    RUN_TEST(test_lava_buried_in_stone_is_not_deleted);
    RUN_TEST(test_buried_lava_bursts_into_stone_and_fire);
    RUN_TEST(test_lava_under_a_lid_with_gaps_never_bursts);
    RUN_TEST(test_lava_in_a_wall_notch_never_bursts);
    RUN_TEST(test_cover_primitive_matches_the_exhaustive_shape_table);
    RUN_TEST(test_a_wide_pool_under_a_crust_bursts);
    RUN_TEST(test_a_wide_pool_under_a_sideways_crust_bursts);
    RUN_TEST(test_an_open_lava_pool_never_bursts);
    RUN_TEST(test_buried_lava_still_becomes_stone_with_impulses_off);
    RUN_TEST(test_lava_is_not_boiled_by_its_own_conducted_heat);
}

SUITE_REGISTER(run_sand_lava_burial_suite);
