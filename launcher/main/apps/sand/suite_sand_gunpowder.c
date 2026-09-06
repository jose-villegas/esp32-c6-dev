/*=============================================================================
 * Portable suite: the falling-sand automaton - gunpowder - encoding,
 * movement, ignition, the fuse, and moisture.
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

/* ===================================================================
 * Gunpowder: REVISION 2, the fuse model.
 *
 * Ignition (fire contact, damped by moisture) and the heat path (lava
 * beside it, heat conducted through stone, once any wet stage has
 * steamed the moisture off) both write GUNPOWDER_LIT_CELL - a burning
 * STATE like wood's, not an immediate blast. A lit cell is a heat
 * source: it ignites flammable neighbours, and counts down on its own
 * burn_decay roll. Only at BURN-OUT does step_one_burning_cell() ask
 * find_lit_two_by_two() (sand_reactions.c) whether this cell is one
 * corner of a still-lit 2x2 - if impulses are enabled and it is, that is
 * a blast (sand_explode(), SAND_GUNPOWDER_BLAST_RADIUS); otherwise it is
 * plain fire, the ordinary way a flame guts out. See material.h's own
 * comment on reaction_t.explodes and material.c's GUNPOWDER_REACTION for
 * the full account this section tests against.
 * =================================================================== */

/* Cells whose CELL_MATERIAL reads MAT_EXTENDED are either a static or
 * gunpowder - count_cells_of() (this file, above) cannot tell the two
 * apart, so gunpowder-specific tests get their own counter through
 * cell_is_gunpowder() instead. */
static int count_cells_gunpowder(void)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, y))) {
                n++;
            }
        }
    }
    return n;
}

/* --- encoding and appearance --------------------------------------- */

/* A dry tone travels with the grain exactly the way a sand shade or a
 * dirt tone already does - see test_a_grain_keeps_its_shade_as_it_falls
 * and test_a_dry_dirt_grain_keeps_its_tone_as_it_falls, whose pattern
 * this repeats for gunpowder's own three-tone codec. */
static void test_a_gunpowder_grain_keeps_its_tone_as_it_falls(void)
{
    fixture();
    const cell_t grain = GUNPOWDER_CELL(2);
    sand_set(&s, 3, 0, grain);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(grain, sand_at(&s, 3, 3),
        "a gunpowder grain's dry tone must travel with it as it falls, "
        "the same as sand's shade and dirt's tone already do - see "
        "material_name()'s own comment on why every one of the eight "
        "codes still shares the single name \"Gunpowder\"");
}

/* material_name() decodes the WHOLE nibble through extended_names[] for
 * both halves of MAT_EXTENDED (material.c's own comment on why) - every
 * one of gunpowder's eight codes, dry, damp or lit, must answer the same
 * single name. */
static void test_material_name_says_gunpowder_for_every_code(void)
{
    for (int v = 0; v < 8; v++) {
        char why[48];
        snprintf(why, sizeof why, "gunpowder code %d", v);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Gunpowder",
            material_name(GUNPOWDER_CELL(v)), why);
    }
}

/* Three dry tones, distinct from one another, and moisture 1..moist_max
 * strictly DARKENING with level, never landing back on a dry tone's
 * exact colour - see material.c's own comment on the GUNPOWDER palette
 * block for the rule this pins. Code 7 (GUNPOWDER_LIT) is deliberately
 * left out of the ramp: it is a burning STATE, not a wetness level - see
 * reaction_t.lit_from's own comment (material.h) for why moisture_of()
 * reads it as dry (0), not saturated, and material.c's own comment on
 * why this suite's final report flags that entry's colour and comment as
 * stale rather than asserting anything about it here. */
static void test_gunpowder_palette_tones_are_distinct_and_moisture_darkens(void)
{
    const gfx_color_t *pal = material_palette();
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);

    for (int a = 0; a < r->tones; a++) {
        for (int b = a + 1; b < r->tones; b++) {
            char why[96];
            snprintf(why, sizeof why,
                "dry tones %d and %d must not share a colour", a, b);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pal[GUNPOWDER_CELL(a)],
                pal[GUNPOWDER_CELL(b)], why);
        }
    }

    int prev_lum = -1;
    for (int m = 1; m <= r->moist_max; m++) {
        const cell_t c = with_moisture(GUNPOWDER_CELL(0), (uint8_t)m, r);
        const int lum = panel_luminance(pal[c]);
        if (m > 1) {
            char why[96];
            snprintf(why, sizeof why,
                "moisture level %d must read darker than level %d", m, m - 1);
            TEST_ASSERT_LESS_THAN_INT_MESSAGE(prev_lum, lum, why);
        }
        prev_lum = lum;

        for (int tone = 0; tone < r->tones; tone++) {
            char why2[96];
            snprintf(why2, sizeof why2,
                "moisture %d must not repeat dry tone %d's exact colour",
                m, tone);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(pal[GUNPOWDER_CELL(tone)],
                pal[c], why2);
        }
    }
}

/* The whole codec, pinned by its documented shape (material.h's moisture
 * codec comment, REVISION 2's codes table): codes 0-2 are the three dry
 * tones (moisture 0), 3-6 are moisture 1-4, and 7 is GUNPOWDER_LIT, a
 * burning STATE that reads as dry (moisture 0), never as saturated - see
 * reaction_t.lit_from's own comment for why. with_moisture() must round
 * -trip every level moisture_of() can report, including 0 (a no-op call
 * that lands on a dry tone, not a real wet state, but must not crash or
 * mis-decode either). */
static void test_gunpowder_codes_decode_to_the_documented_moisture(void)
{
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    static const uint8_t expected_moisture[8] = { 0, 0, 0, 1, 2, 3, 4, 0 };

    for (int v = 0; v < 8; v++) {
        const cell_t c = GUNPOWDER_CELL(v);
        char why[48];
        snprintf(why, sizeof why, "gunpowder code %d", v);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected_moisture[v],
            moisture_of(c, r), why);
        TEST_ASSERT_EQUAL_MESSAGE(v == GUNPOWDER_LIT, cell_is_burning(c), why);
    }

    for (int m = 0; m <= r->moist_max; m++) {
        char why[48];
        snprintf(why, sizeof why, "moisture level %d", m);
        const cell_t c = with_moisture(GUNPOWDER_CELL(0), (uint8_t)m, r);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)m, moisture_of(c, r), why);
    }
}

/* --- movement -------------------------------------------------------- */

static void test_gunpowder_falls_and_piles_like_a_powder(void)
{
    fixture();
    sand_set(&s, 3, 0, GUNPOWDER_CELL(0));

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "a single grain of gunpowder must fall straight down onto the "
        "floor, the same as any other powder");

    /* And under a TILTED gravity, a small heap must migrate toward and
     * pile against whichever wall is down - the same claim
     * test_a_heap_settles_against_whichever_wall_is_down makes for sand. */
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 4; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    const int expected = count_cells_gunpowder();

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 1, 0, 0);
    }

    int touching_wall = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!cell_is_gunpowder(sand_at(&s, x, y))) {
                continue;
            }
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(W / 2, x,
                "every grain must migrate to the side gravity points at");
            if (x == W - 1) {
                touching_wall++;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, count_cells_gunpowder(),
        "piling under a sideways gravity must conserve every grain");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, touching_wall,
        "the heap must actually reach the wall gravity points at, not "
        "stall short of it");
}

/* Density only settles a contest between a POWDER and a LIQUID - a grain
 * displaces a liquid cell whenever it is denser, the ordinary sinking
 * every powder already does. Between two POWDERS AT REST, density never
 * decides anything at all: this simulation deliberately never lets one
 * powder sink through another sitting still (see sand.c's own comment,
 * around the try_fall_or_scatter() family, on why a grain has "no
 * business sinking through another powder" - weight is carried through
 * contact, not resolved by density, unless an IMPULSE is involved). So
 * gunpowder sinks through every liquid it is denser than, but a powder
 * poured onto a bed of gunpowder (or gunpowder poured onto a bed of a
 * powder) simply rests on top of it, whichever way around the pour
 * happened - acid is left out of the liquid half deliberately, since it
 * also DISSOLVES gunpowder (dissolvable 200,
 * test_acid_dissolves_gunpowder already covers that reaction) and would
 * confound a plain sinking check. */
static void test_gunpowder_sinks_through_liquids_and_rests_on_and_under_sand(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "gunpowder is denser than water, so it must sink all the way "
        "through the pool rather than float on it");

    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(sand_at(&s, 3, H - 1)),
        "and through oil, the same reasoning");

    /* Sand poured onto a bed of gunpowder must rest ON that bed, never
     * sink into or beneath it. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    sand_set(&s, 3, 3, SAND);
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    bool sand_above_bed = false;
    for (int y = 0; y < 4; y++) {
        if (CELL_MATERIAL(sand_at(&s, 3, y)) == MAT_SAND) {
            sand_above_bed = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sand_above_bed,
        "sand poured onto a bed of gunpowder must rest ON it, not sink "
        "beneath it - powders never displace each other at rest, "
        "regardless of density");

    /* And the reverse pour order: gunpowder onto a bed of sand must
     * rest ON that bed in turn - the same mutual-blocking rule either
     * way around. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_set(&s, 3, 3, GUNPOWDER_CELL(0));
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    bool gp_above_bed = false;
    for (int y = 0; y < 4; y++) {
        if (cell_is_gunpowder(sand_at(&s, 3, y))) {
            gp_above_bed = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(gp_above_bed,
        "and gunpowder poured onto a bed of sand must rest ON it too - "
        "the same rule, the other pour order");
}

static void test_gunpowder_is_conserved_under_every_gravity(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    const int expected = count_cells_gunpowder();
    TEST_ASSERT_EQUAL_INT(15, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, count_cells_gunpowder(),
                "a step must conserve gunpowder grains in every gravity "
                "direction");
        }
    }
}

/* --- ignition and the heat path -------------------------------------- */

/* NOT fire_room() - that boxes fire in on all four cardinal sides, which
 * works for the GAS neighbour test_fire_ignites_an_adjacent_flammable_
 * neighbour uses (density 10, lighter than fire's own 15, so at least
 * one neighbour always fails smothered()'s "denser" test) but not for
 * gunpowder (density 50): stone on three sides plus gunpowder on the
 * fourth makes every one of fire's cardinal neighbours denser than
 * fire, and smothered() reads that as buried - fire is extinguished on
 * the very first step, before its own ignite walk ever runs, which is
 * exactly what a fully-boxed room measured (confirmed by direct
 * instrumentation while writing this test: the fire cell read back
 * empty at step 0 in every run). Left open on three sides instead, with
 * sand_set_mobility(&s, 0) holding it in place without needing walls
 * to do it - smothered() needs an occupied AND denser neighbour on
 * every side, and an empty one fails that trivially. */
static void test_fire_beside_dry_gunpowder_lights_it(void)
{
    fixture();
    sand_set_mobility(&s, 0);
    /* A floor under both cells (spanning far enough either side to catch
     * the two diagonal-down destinations too) - gunpowder is KIND_POWDER
     * and falls with nothing under it, unlike the GAS neighbour the
     * original fire_room()-boxed test used. mobility 0 only holds GAS
     * still; it says nothing about a powder's own gravity. */
    sand_set(&s, 2, 4, STONE);
    sand_set(&s, 3, 4, STONE);
    sand_set(&s, 4, 4, STONE);
    sand_set(&s, 5, 4, STONE);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GUNPOWDER_CELL(0));

    /* Looped, not a single step: flammability 200/256 is likely, not
     * certain, on any one roll - a generous budget against a ~78%/step
     * chance rather than betting the test on this one seed's first draw. */
    cell_t c = sand_at(&s, 4, 3);
    for (int i = 0; i < 20 && cell_is_gunpowder(c) && cell_code(c) != GUNPOWDER_LIT; i++) {
        sand_step(&s, 0, 1000, 0);
        c = sand_at(&s, 4, 3);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "fire beside dry gunpowder must light the fuse, not skip past it "
        "to some other material entirely");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(GUNPOWDER_LIT, cell_code(c),
        "a fresh ignition must land on GUNPOWDER_LIT (code 7) - the "
        "burning STATE reaction_t.ignites_to now points at, not plain "
        "MAT_FIRE the way section 2's design once wrote here");
}

static void test_lava_beside_dry_gunpowder_lights_it_through_the_heat_path(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, GUNPOWDER_CELL(0));

    bool lit = false;
    for (int i = 0; i < 200 && !lit; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 4, H - 2);
        lit = cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT;
    }

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "heat alone - lava sitting beside dry gunpowder, no flame "
        "required - must light the fuse, the same way it always could "
        "for wood, at heat_chance 24/256 (generous 200-step budget - "
        "P(never once in 200) is astronomically small)");
}

static void test_heat_conducted_through_stone_lights_gunpowder(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "heat-through-stone-lights-gunpowder grid must fit in what the "
        "framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    const int wall_x0 = 2;
    const int wall_len = 5;
    const int gp_x = wall_x0 + wall_len;

    sand_set(&wide, gp_x - 1, y + 1, STONE);
    sand_set(&wide, gp_x,     y + 1, STONE);
    sand_set(&wide, gp_x + 1, y + 1, STONE);
    sand_set(&wide, 1, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&wide, wall_x0 + i, y, STONE);
    }
    sand_set(&wide, gp_x, y, GUNPOWDER_CELL(0));

    bool lit = false;
    for (int i = 0; i < 150 && !lit; i++) {
        sand_step(&wide, 0, 1000, 0);
        const cell_t c = sand_at(&wide, gp_x, y);
        lit = cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT;
    }

    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "heat conducted through a stone wall - no flame ever touching "
        "the gunpowder directly - must still light the fuse, the same "
        "conduct_heat() walk that already boils water or smelts dirt "
        "through a wall");
}

/* --- the fuse: trails, blasts, burial, quenching ---------------------- */

/* Ignition spreads cell to cell along a trail exactly like fire spreads
 * through wood - only ONE end is lit here, the rest start dry, so this
 * is a claim about PROPAGATION, not (yet) about the 2x2 blast rule; see
 * test_a_one_wide_lit_trail_never_detonates for that half, pinned
 * separately with the whole trail pre-lit at once. */
static void test_a_lit_gunpowder_trail_burns_along_itself(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - the trail must
                                 * actually burn down behind the fuse
                                 * front, not just light up and stay lit */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 1, H - 2, GUNPOWDER_LIT_CELL);
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "trail-propagation impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&s, buf, W * H);

    bool reached_far_end = false;
    bool all_gone = false;
    for (int i = 0; i < 1000 && !all_gone; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t far = sand_at(&s, 5, H - 2);
        if (!reached_far_end &&
            (!cell_is_gunpowder(far) || cell_code(far) == GUNPOWDER_LIT)) {
            reached_far_end = true;
        }
        all_gone = true;
        for (int x = 1; x <= 5; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, H - 2))) {
                all_gone = false;
            }
        }
    }
    const int impulses = s.impulse_count;
    free(buf);

    TEST_ASSERT_TRUE_MESSAGE(reached_far_end,
        "ignition lit at one end of a 1-wide trail must reach the far "
        "end within a generous budget - a fuse that only ever burns the "
        "cell it started at is not a trail catching, it is one grain "
        "catching");
    TEST_ASSERT_TRUE_MESSAGE(all_gone,
        "the whole trail must eventually burn through - nothing but "
        "fire or empty left, no dry gunpowder surviving untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, impulses,
        "a trail only one cell wide never has a third lit neighbour to "
        "complete a 2x2 with (find_lit_two_by_two(), sand_reactions.c), "
        "so it must never detonate while it burns along");
}

/* The positive case REVISION 2 exists for: four cells, all lit at once,
 * is the smallest lit body where whichever corner burns out FIRST is
 * GUARANTEED to still find its other three corners lit (the rejected
 * first draft asked for a fully-lit 3x3, where eight of nine cells are
 * on the PERIMETER and structurally can never satisfy that - see
 * find_lit_two_by_two()'s own comment, sand_reactions.c, for the device
 * measurement that forced the narrowing). Folded into one test with the
 * lone-cell comparison, the same pairing
 * test_a_confined_gas_pocket_bursts_instead_of_just_catching draws
 * against test_an_open_gas_pocket_still_just_catches_fire. */
static void test_a_lit_two_by_two_of_gunpowder_detonates(void)
{
    /* Scene A: the 2x2, boxed in stone - two rows of fire_room()'s own
     * shape stacked - so nothing so much as slides before the reactions
     * pass gets a turn at it. sand_set_decay(&s, SAND_DECAY_PER_MATERIAL)
     * is not decoration: sand_init()'s own default is decay == 0
     * (immortal - see sand_set_decay()'s own comment, sand.h), exactly
     * what test_a_buried_lit_gunpowder_cell_is_not_smothered and
     * test_water_quenches_lit_gunpowder_to_soaked both WANT, but this
     * test needs the real per-material burn_decay figure so burn-out
     * actually happens at all. */
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 2, H - 3, STONE);
    sand_set(&s, 5, H - 3, STONE);
    sand_set(&s, 3, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);

    impulse_t *square_buf = malloc((size_t)(W * H) * sizeof *square_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(square_buf,
        "2x2-detonates impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, square_buf, W * H);

    bool square_burned = false;
    for (int i = 0; i < 200 && !square_burned; i++) {
        sand_step(&s, 0, 1000, 0);
        square_burned = !cell_is_gunpowder(sand_at(&s, 3, H - 3)) ||
                        !cell_is_gunpowder(sand_at(&s, 4, H - 3)) ||
                        !cell_is_gunpowder(sand_at(&s, 3, H - 2)) ||
                        !cell_is_gunpowder(sand_at(&s, 4, H - 2));
    }
    const int square_impulses = s.impulse_count;
    free(square_buf);

    TEST_ASSERT_TRUE_MESSAGE(square_burned,
        "setup: at least one corner of the 2x2 must burn out within the "
        "budget");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, square_impulses,
        "a lit 2x2 must detonate at its first burn-out - the corner "
        "that goes first always still has its other three corners lit, "
        "which is exactly what find_lit_two_by_two() asks for");

    /* Scene B: a lone lit cell, same box shape, same impulses enabled -
     * structurally it can never be a corner of a lit 2x2 at all (it has
     * no lit neighbour whatsoever), so it must always resolve to plain
     * fire. */
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);

    impulse_t *lone_buf = malloc((size_t)(W * H) * sizeof *lone_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(lone_buf,
        "lone-lit-cell impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, lone_buf, W * H);

    bool lone_resolved = false;
    for (int i = 0; i < 200 && !lone_resolved; i++) {
        sand_step(&s, 0, 1000, 0);
        lone_resolved = !cell_is_gunpowder(sand_at(&s, 3, H - 2));
    }
    const int lone_impulses = s.impulse_count;
    const uint8_t lone_material = CELL_MATERIAL(sand_at(&s, 3, H - 2));
    free(lone_buf);

    TEST_ASSERT_TRUE_MESSAGE(lone_resolved,
        "setup: the lone lit cell must burn out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lone_impulses,
        "a lone lit cell has no lit neighbour at all, so it can never "
        "be a corner of a lit 2x2 and must never queue a blast");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, lone_material,
        "and it must burn out to plain fire - the ordinary "
        "ember-guttering outcome, not vanish or leave something else "
        "behind");
}

/* A stronger, board-wide version of the corner checks
 * test_a_lit_two_by_two_of_gunpowder_detonates already makes:
 * sand_explode()'s unconditional core fill (radius /
 * SAND_EXPLODE_CORE_DIVISOR, sand.c) reaches every cell of a 2x2
 * whichever corner turns out to be its centre, so nothing needs scanning
 * only the four corners by name - the whole board must come up clean of
 * the lit code once the blast has actually happened. */
static void test_a_detonating_two_by_two_leaves_no_lit_gunpowder_behind(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "detonation board-wide grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 11u);
    sand_set_decay(&wide, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&wide, 0);   /* hold the 2x2 in place until burn-out -
                                    * see test_gunpowder_without_impulses_
                                    * burns_to_fire's own use of this */

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "detonation board-wide impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    const int cx = WIDE_W / 2, cy = WIDE_H / 2;
    sand_set(&wide, cx,     cy,     GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx + 1, cy,     GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx,     cy + 1, GUNPOWDER_LIT_CELL);
    sand_set(&wide, cx + 1, cy + 1, GUNPOWDER_LIT_CELL);

    bool detonated = false;
    for (int i = 0; i < 200 && !detonated; i++) {
        sand_step(&wide, 0, 1000, 0);
        detonated = wide.impulse_count > 0;
    }

    bool any_lit = false;
    for (int y = 0; y < WIDE_H && !any_lit; y++) {
        for (int x = 0; x < WIDE_W; x++) {
            const cell_t c = sand_at(&wide, x, y);
            if (cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT) {
                any_lit = true;
                break;
            }
        }
    }
    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(detonated,
        "setup: the 2x2 must actually detonate within the budget");
    TEST_ASSERT_FALSE_MESSAGE(any_lit,
        "a detonation must consume every lit gunpowder cell it touches - "
        "sand_explode()'s own core fill (radius / SAND_EXPLODE_CORE_DIVISOR) "
        "unconditionally covers the whole 2x2, so nothing lit may survive "
        "the blast step anywhere on the board");
}

/* SAND_GUNPOWDER_BLAST_COOLDOWN (sand_reactions.c) caps detonations at
 * one per step at its shipped value of 1, board-wide - a big pile goes off one blast at a time
 * rather than however many of its 2x2s happen to qualify together. Two
 * lit 2x2s, more than SAND_GUNPOWDER_BLAST_RADIUS apart (so neither
 * blast can physically reach the other's room - the only thing linking
 * them is the shared s->fuse_blast_wait cooldown), forced to burn
 * out on the SAME step (decay 255 - a near-certain single-step burn-out
 * for every one of the eight lit cells). Scan order (top-to-bottom,
 * left-to-right - this file's own top comment) reaches the first
 * group's qualifying corner before the second's, so the first one
 * detonates and the second, even though it structurally qualifies just
 * as much, finds the cap already spent and falls through to plain fire
 * instead. */
static void test_fuse_blasts_are_capped_at_one_per_step(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "blast-cap grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
    sand_set_decay(&wide, 255);

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "blast-cap impulse queue must fit in what the framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, 4, STONE);
    }
    /* Group A: columns 2-3. */
    sand_set(&wide, 1, 2, STONE);
    sand_set(&wide, 1, 3, STONE);
    sand_set(&wide, 4, 2, STONE);
    sand_set(&wide, 4, 3, STONE);
    sand_set(&wide, 2, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 2, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 3, GUNPOWDER_LIT_CELL);
    /* Group B: columns 26-27 - 24 cells from group A, past
     * SAND_GUNPOWDER_BLAST_RADIUS (20), so neither blast can physically
     * touch the other's room. */
    sand_set(&wide, 25, 2, STONE);
    sand_set(&wide, 25, 3, STONE);
    sand_set(&wide, 28, 2, STONE);
    sand_set(&wide, 28, 3, STONE);
    sand_set(&wide, 26, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 26, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 3, GUNPOWDER_LIT_CELL);

    sand_step(&wide, 0, 1000, 0);

    const int impulses_after_one_step = wide.impulse_count;
    const bool a_all_gone = !cell_is_gunpowder(sand_at(&wide, 2, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 3, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 2, 3)) &&
                            !cell_is_gunpowder(sand_at(&wide, 3, 3));
    const bool b_all_gone = !cell_is_gunpowder(sand_at(&wide, 26, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 27, 2)) &&
                            !cell_is_gunpowder(sand_at(&wide, 26, 3)) &&
                            !cell_is_gunpowder(sand_at(&wide, 27, 3));
    /* A real blast's core (radius / SAND_EXPLODE_CORE_DIVISOR) fills
     * unconditionally, reaching the walls at distance 1; a CAPPED
     * burn-out only ever writes fire into its own single cell (or, if it
     * was the corner spend_lit_two_by_two() targeted, into the OTHER
     * cells of ITS OWN 2x2 - never into a wall a cell away), so the wall
     * is exactly what tells a real detonation apart from a capped one. */
    const uint8_t a_wall = CELL_MATERIAL(sand_at(&wide, 1, 2));
    const uint8_t b_wall = CELL_MATERIAL(sand_at(&wide, 25, 2));

    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(a_all_gone,
        "setup: group A must burn out within this one forced step");
    TEST_ASSERT_TRUE_MESSAGE(b_all_gone,
        "setup: group B must burn out within this one forced step too");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, impulses_after_one_step,
        "at least one of the two groups must have detonated");
    TEST_ASSERT_TRUE_MESSAGE(
        (a_wall != MAT_STONE) != (b_wall != MAT_STONE),
        "exactly ONE of the two rooms' walls must have been breached - "
        "the cap must have let exactly one of the two qualifying 2x2s "
        "through this step, not both and not neither");
}

/* And the cooldown is a WAIT, not merely a per-step cap: with
 * sand_set_fuse_cooldown() raised to 3, the second of two independent
 * lit 2x2s must still be waiting a step later, where at the shipped
 * value of 1 it would already have gone off. Same two-room scene as
 * above (24 cells apart, so neither blast can reach the other's room);
 * the only difference is the wait, which is the whole claim.
 *
 * Group B is left UNLIT for the first step and lit afterwards, so its
 * burn-out cannot race group A's inside the same pass - what is under
 * test is whether the board is still refusing on a LATER step, not the
 * within-step ordering test_fuse_blasts_are_capped_at_one_per_step
 * already pins. */
static void test_a_longer_fuse_cooldown_delays_the_next_blast(void)
{
    wide_cells = malloc((size_t)WIDE_W * WIDE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wide_cells,
        "cooldown grid must fit in what the framebuffer leaves");
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
    sand_set_decay(&wide, 255);
    sand_set_fuse_cooldown(&wide, 3);

    impulse_t *buf = malloc((size_t)(WIDE_W * WIDE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "cooldown impulse queue must fit in what the framebuffer leaves");
    sand_enable_impulses(&wide, buf, WIDE_W * WIDE_H);

    for (int x = 0; x < WIDE_W; x++) {
        sand_set(&wide, x, 4, STONE);
    }
    sand_set(&wide, 1, 2, STONE);
    sand_set(&wide, 1, 3, STONE);
    sand_set(&wide, 4, 2, STONE);
    sand_set(&wide, 4, 3, STONE);
    sand_set(&wide, 2, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 2, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 3, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 25, 2, STONE);
    sand_set(&wide, 25, 3, STONE);
    sand_set(&wide, 28, 2, STONE);
    sand_set(&wide, 28, 3, STONE);

    sand_step(&wide, 0, 1000, 0);   /* group A detonates, wait := 3 */

    const bool a_breached = CELL_MATERIAL(sand_at(&wide, 1, 2)) != MAT_STONE;

    sand_set(&wide, 26, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 2, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 26, 3, GUNPOWDER_LIT_CELL);
    sand_set(&wide, 27, 3, GUNPOWDER_LIT_CELL);

    sand_step(&wide, 0, 1000, 0);   /* wait ticks 3 -> 2: still refused */

    const bool b_gone = !cell_is_gunpowder(sand_at(&wide, 26, 2)) &&
                        !cell_is_gunpowder(sand_at(&wide, 27, 2)) &&
                        !cell_is_gunpowder(sand_at(&wide, 26, 3)) &&
                        !cell_is_gunpowder(sand_at(&wide, 27, 3));
    const uint8_t b_wall = CELL_MATERIAL(sand_at(&wide, 25, 2));

    free(buf);
    free(wide_cells);

    TEST_ASSERT_TRUE_MESSAGE(a_breached,
        "setup: the first 2x2 must detonate on its own step, or there is "
        "no cooldown running to test");
    TEST_ASSERT_TRUE_MESSAGE(b_gone,
        "setup: the second 2x2 must burn out on the step after, so the "
        "refusal below is the cooldown's doing and not a fuse still lit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, b_wall,
        "a burn-out inside the cooldown must fall through to plain fire, "
        "leaving its room's wall intact - with no cooldown at all this "
        "this same 2x2 would have breached it");
}

/* Replaces the old board-edge claim (an edge cell CAN now be the corner
 * of an INWARD 2x2, so "the board edge never detonates" is false on its
 * own) with the two claims that actually still hold: off-board counts as
 * not lit (find_lit_two_by_two()'s own comment, sand_reactions.c), and a
 * single LINE, however long, never completes a square. One end of this
 * trail sits AT the board's own edge (x == 0), the whole length pre-lit
 * at once - unlike test_a_lit_gunpowder_trail_burns_along_itself, which
 * is about propagation from one end, this is about the shape itself
 * never qualifying, so every cell starts lit and none of them are ever
 * given a chance to catch each other. */
static void test_a_one_wide_lit_trail_never_detonates(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - this test needs
                                 * the whole trail to actually burn out */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 6, H - 2, STONE);
    for (int x = 0; x <= 5; x++) {
        sand_set(&s, x, H - 2, GUNPOWDER_LIT_CELL);
    }

    impulse_t *buf = malloc((size_t)(W * H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "edge-trail impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, buf, W * H);

    bool all_gone = false;
    for (int i = 0; i < 300 && !all_gone; i++) {
        sand_step(&s, 0, 1000, 0);
        all_gone = true;
        for (int x = 0; x <= 5; x++) {
            if (cell_is_gunpowder(sand_at(&s, x, H - 2))) {
                all_gone = false;
            }
        }
    }
    const int impulses = s.impulse_count;
    free(buf);

    TEST_ASSERT_TRUE_MESSAGE(all_gone,
        "setup: the whole trail must have burned out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, impulses,
        "a lit cell whose only lit neighbours are off-board or in a "
        "single line - never a second one completing a square - must "
        "never detonate, whether it sits at the board's own edge or "
        "safely inside it");
}

/* Gunpowder carries its own oxidiser, unlike wood or a candle - a fuse
 * buried on every cardinal side by something denser (which WOULD
 * smother an ordinary flame - see smothered(), sand_reactions.c) must
 * keep burning regardless, or nothing inside a real pile could ever
 * reach burn-out at all. Decay forced to 0 (immortal) so this is purely
 * about the smothered() skip, not a race against burn-out timing. */
static void test_a_buried_lit_gunpowder_cell_is_not_smothered(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);

    /* The full 3x3 ring, all eight neighbours - not just the four
     * cardinals smothered() itself reads. Walling only the cardinals
     * left the two open diagonals as a slide route: a powder blocked
     * straight down but open diagonally still slides that way (the same
     * scatter behaviour any pile uses to find its angle of repose), and
     * confirmed by direct instrumentation while writing this test, the
     * grain walked diagonally for several steps and off this board
     * entirely rather than staying put to prove anything about
     * smothered() at all. The diagonals play no part in smothered()'s
     * own four-neighbour test - they are here only to pin the grain in
     * place so the four cardinals are the only thing left that could
     * move it. */
    const int x = W / 2, y = H / 2;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            sand_set(&s, x + dx, y + dy, STONE);
        }
    }
    sand_set(&s, x, y, GUNPOWDER_LIT_CELL);

    for (int i = 0; i < 100; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const cell_t c = sand_at(&s, x, y);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT,
        "a lit gunpowder cell walled in by stone on all four cardinal "
        "sides - which would smother an ordinary flame - must keep "
        "burning: reaction_t.explodes != 0 skips smothered() outright "
        "for gunpowder (its own comment, sand_reactions.c)");
}

/* Quenching a lit gunpowder cell must leave it SOAKED, not merely
 * unlit - a fuse doused mid-burn is wet, and landing back on the dry
 * code 0 would let it relight off the very neighbour that just wet it.
 * Decay forced to 0 so this is purely about the quench branch, not a
 * race against burn-out. */
static void test_water_quenches_lit_gunpowder_to_soaked(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, WATER);

    sand_step(&s, 0, 1000, 0);

    const cell_t c = sand_at(&s, 3, H - 2);
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "quenching a lit gunpowder cell must leave it as gunpowder, not "
        "vanish it the way quenching an ordinary flame does");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(GUNPOWDER_LIT, cell_code(c),
        "the fuse must actually be out - no longer the lit code");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r->moist_max, moisture_of(c, r),
        "a lit gunpowder cell doused by water must land SOAKED "
        "(moist_max), not merely at the unlit dry code 0");
}

/* Without an impulse buffer, sand_explode() is a documented no-op (see
 * its own first line, sand.h) - the same fallback
 * test_an_open_gas_pocket_still_just_catches_fire pins for a confined
 * gas pocket. Reuses the exact 2x2 shape that DOES detonate with
 * impulses enabled (test_a_lit_two_by_two_of_gunpowder_detonates), so
 * the only variable here is the missing buffer. */
static void test_gunpowder_without_impulses_burns_to_fire(void)
{
    fixture();
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);   /* sand_init()'s default
                                 * is immortal (decay 0) - every corner
                                 * has to actually burn out here */
    sand_set_mobility(&s, 0);   /* keep an earlier-resolved corner's
                                 * fresh fire from drifting off before
                                 * the last corner is checked */
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 2, STONE);
    sand_set(&s, 2, H - 3, STONE);
    sand_set(&s, 5, H - 3, STONE);
    sand_set(&s, 3, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);
    /* No sand_enable_impulses() - s->impulse_buf stays NULL, the default. */

    /* WHAT EACH CORNER BECOMES AT THE MOMENT IT BURNS OUT, not what sits
     * there once all four have. The four burn-outs are four independent
     * geometric waits (reaction_t.burn_decay), so they are spread over
     * many steps, and the fire the first one leaves is itself a transient
     * that decays - at a long enough fuse it is gone before the last
     * corner resolves, which read as "the corner left nothing" and made
     * this test fail on a pure tuning change. Sampling each cell the step
     * it stops being gunpowder pins the claim the fallback actually makes
     * and is indifferent to how long the fuse burns. */
    uint8_t became[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    const int cx[4] = {3, 4, 3, 4};
    const int cy[4] = {H - 3, H - 3, H - 2, H - 2};
    bool all_resolved = false;
    for (int i = 0; i < 400 && !all_resolved; i++) {
        sand_step(&s, 0, 1000, 0);
        all_resolved = true;
        for (int k = 0; k < 4; k++) {
            const cell_t c = sand_at(&s, cx[k], cy[k]);
            if (cell_is_gunpowder(c)) {
                all_resolved = false;
            } else if (became[k] == 0xFFu) {
                became[k] = CELL_MATERIAL(c);
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(all_resolved,
        "setup: every corner of the 2x2 must burn out within the budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[0],
        "without an impulse buffer, a corner that would otherwise "
        "detonate must fall through to plain fire instead");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[1],
        "same corner rule for the second cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[2],
        "same corner rule for the third cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, became[3],
        "same corner rule for the fourth cell of the 2x2");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "s->impulse_count must stay zero - there is no buffer for "
        "sand_explode() to have written into at all");
}

/* --- moisture chemistry ------------------------------------------------ */

/* moist_max (SOAKED) beside lava: the one moisture level where
 * reaction_t.soaked_to's own saturated-to-oil roll can fire
 * (step_one_soaking_cell(), sand_reactions.c) and the one level where
 * SAND_DAMP_IGNITION_SHIFT's damping (f >>= 2*m) actually reaches zero -
 * moisture 2 still lights at 12 in 256 a step by design, so only moist_max
 * is safe to assert "never" at. A soaked fuse must never reach the LIT
 * code while it is still soaked; if it stops being gunpowder before
 * drying below moist_max, oil is the only accepted exit (soaked_to), not
 * fire or anything else. */
static void test_soaked_gunpowder_never_lights_beside_lava(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));

    uint8_t last_moisture = r->moist_max;
    bool ignited_while_soaked = false;
    bool resolved = false;
    bool became_oil = false;
    for (int i = 0; i < 4000 && !resolved && !ignited_while_soaked; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 4, H - 2);
        if (!cell_is_gunpowder(c)) {
            became_oil = CELL_MATERIAL(c) == MAT_OIL;
            resolved = true;
            break;
        }
        if (cell_code(c) == GUNPOWDER_LIT) {
            if (last_moisture == r->moist_max) {
                ignited_while_soaked = true;
            }
            break;
        }
        last_moisture = moisture_of(c, r);
    }

    TEST_ASSERT_FALSE_MESSAGE(ignited_while_soaked,
        "a fuse still SOAKED (moist_max) on the step before must never "
        "reach the LIT code - heat has to dry it below saturation first");
    if (resolved) {
        TEST_ASSERT_TRUE_MESSAGE(became_oil,
            "a soaked cell that stops being gunpowder before drying below "
            "moist_max must have turned to oil (soaked_to) - its only "
            "other documented exit at this moisture level, never fire or "
            "anything else");
    }
}

/* moisture 2 - below moist_max, so reaction_t.soaked_to's oil roll
 * (held == moist_max only) can never fire and confound this test's one
 * claim: heat must drive a level of moisture off, visibly as steam,
 * generalising dirt's own wet-earth stage to gunpowder via
 * moisture_of()/with_moisture(). Tolerant of the cell going on to light
 * once it is genuinely dry - that is a different claim, not this one. */
static void test_heat_dries_wet_gunpowder_one_level_with_steam(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 3, H - 3, STONE);
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    const uint8_t start_moisture = 2;
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), start_moisture, r));

    uint8_t last_moisture = start_moisture;
    bool moisture_fell = false;
    bool steam_seen = false;
    for (int i = 0; i < 4000 && !(moisture_fell && steam_seen); i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(MAT_STEAM) > 0) {
            steam_seen = true;
        }
        const cell_t c = sand_at(&s, 4, H - 2);
        if (!cell_is_gunpowder(c)) {
            break;
        }
        const uint8_t m = moisture_of(c, r);
        if (m < last_moisture) {
            moisture_fell = true;
        }
        last_moisture = m;
    }

    TEST_ASSERT_TRUE_MESSAGE(moisture_fell,
        "heat must actually drive a level of moisture off within the "
        "budget, the same wet-earth stage dirt already goes through");
    TEST_ASSERT_TRUE_MESSAGE(steam_seen,
        "driving moisture off a heated cell must visibly puff steam, the "
        "same wet-earth branch dirt uses");
}

/* Statistical, over many independent single-cell trials rather than one:
 * SAND_DAMP_IGNITION_SHIFT (sand_reactions.c) damps gunpowder's 200/256
 * flammability to 50/256 at moisture 1, so dry cells must ignite far
 * more often than damp ones in the same single roll. Fresh moisture
 * codes are written directly at setup, not soaked up through
 * simulation, so this test never touches soil_set_moisture()/
 * soil_dry_out() at all. */
#define DAMP_TEST_TRIALS 150

/* One [FIRE][GUNPOWDER][STONE] triple per trial, all in a single row, so
 * every trial is fully isolated from every other: the STONE separator
 * blocks horizontal contact with the next trial's fire or gunpowder, and
 * the only row is floored by stone beneath it, so nothing falls before
 * reactions gets a turn. Earlier draft PACKED trials into a tall column
 * instead (fire/gunpowder pairs stacked with no separator) and measured
 * dry and damp both igniting at ~95-97% - because a freshly-lit
 * gunpowder cell is itself a heat source the SAME scan pass still
 * reaches, and stacked vertically its own ignite walk reached the NEXT
 * trial's still-dry cell directly, cascading down the column independent
 * of that trial's own moisture roll entirely. Isolating every trial is
 * what makes this a test of ONE roll's damping, not of a chain
 * reaction. */
static void ignite_trial_row(sand_t *g, uint8_t *cells, int w, int trials, cell_t gp_byte)
{
    sand_init(g, cells, w, 2, 7u);
    sand_set_mobility(g, 0);   /* keep fire from rising away before
                                * reactions gets a turn at it this same
                                * step - see test_an_open_gas_pocket_
                                * still_just_catches_fire's own use of
                                * this for the same reason */
    sand_set_conduction(g, 0);   /* the one-cell stone separator between
                                * trials is a real conductor at the
                                * table's own rate - left at the default,
                                * conduct_heat() carried a trial's own
                                * fire sideways through it into the NEXT
                                * trial's gunpowder, a second independent
                                * heat_chance roll neither isolated trial
                                * was supposed to get. Confirmed by direct
                                * instrumentation while writing this test:
                                * with conduction on, damp ignited at
                                * ~87% instead of the ~19.5% one damped
                                * roll predicts. Sealed shut here, since
                                * this test is about the DIRECT-contact
                                * ignite roll only. */
    for (int i = 0; i < trials; i++) {
        const int base = i * 3;
        sand_set(g, base + 0, 0, FIRE);
        sand_set(g, base + 1, 0, gp_byte);
        sand_set(g, base + 2, 0, STONE);
    }
    for (int x = 0; x < w; x++) {
        sand_set(g, x, 1, STONE);
    }
}

static void test_damp_gunpowder_ignites_less_readily_than_dry(void)
{
    const int w = DAMP_TEST_TRIALS * 3;
    uint8_t *dry_cells = malloc((size_t)w * 2);
    uint8_t *damp_cells = malloc((size_t)w * 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(dry_cells,
        "dry ignition-rate grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(damp_cells,
        "damp ignition-rate grid must fit in what the framebuffer leaves");

    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    const cell_t damp_byte = with_moisture(GUNPOWDER_CELL(0), 1, r);
    sand_t dry_g, damp_g;
    ignite_trial_row(&dry_g, dry_cells, w, DAMP_TEST_TRIALS, GUNPOWDER_CELL(0));
    ignite_trial_row(&damp_g, damp_cells, w, DAMP_TEST_TRIALS, damp_byte);

    /* ONE step only - a short exposure to a single roll, not a budget
     * long enough for the heat path to dry a damp cell off (dries != 0)
     * and then catch it on a LATER, undamped roll, which would erase the
     * very difference this test exists to measure. */
    sand_step(&dry_g, 0, 1000, 0);
    sand_step(&damp_g, 0, 1000, 0);

    int dry_lit = 0, damp_lit = 0;
    for (int i = 0; i < DAMP_TEST_TRIALS; i++) {
        const cell_t dry_c = sand_at(&dry_g, i * 3 + 1, 0);
        if (cell_is_gunpowder(dry_c) && cell_code(dry_c) == GUNPOWDER_LIT) {
            dry_lit++;
        }
        const cell_t damp_c = sand_at(&damp_g, i * 3 + 1, 0);
        if (cell_is_gunpowder(damp_c) && cell_code(damp_c) == GUNPOWDER_LIT) {
            damp_lit++;
        }
    }
    free(dry_cells);
    free(damp_cells);

    char why[288];
    snprintf(why, sizeof why,
             "dry gunpowder (flammability 200/256) must ignite far more "
             "readily in one roll than damp (moisture 1, damped to "
             "50/256 by SAND_DAMP_IGNITION_SHIFT) - dry lit %d of %d "
             "trials, damp lit %d of %d; a 2x margin is generous against "
             "both binomial spreads at n=%d",
             dry_lit, DAMP_TEST_TRIALS, damp_lit, DAMP_TEST_TRIALS,
             DAMP_TEST_TRIALS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2 * damp_lit, dry_lit, why);
}

/* Statistical comparison against dirt, over many ISOLATED cells (stone
 * to each side, stone floor beneath) so neither soaking nor drying ever
 * has a same-species neighbour to trade with - no diffusion, no
 * percolation. Dirt is unaffected either way (CELL_MATERIAL(dirt) indexes
 * its own real row). */
#define WET_DRY_TRIALS 40

static void test_water_wets_gunpowder_and_it_dries_out_slowly(void)
{
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);

    /* THE WETTING HALF: an isolated dry cell under a splash of water
     * must take on moisture - the ordinary soaking-up path
     * (step_one_soaking_cell()'s own held < moist_max branch), which
     * calls with_moisture(c, held + 1, r) with the row passed in
     * directly, never re-derived from CELL_MATERIAL(c). */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set(&s, 2, H / 2, STONE);
    sand_set(&s, 4, H / 2, STONE);
    sand_set(&s, 3, H / 2 + 1, STONE); /* a floor: gunpowder is a powder and
                                        * would otherwise fall out from under
                                        * the water before it could soak */
    sand_set(&s, 3, H / 2, GUNPOWDER_CELL(0));
    sand_set(&s, 3, H / 2 - 1, WATER);

    bool wetted = false;
    for (int i = 0; i < 400 && !wetted; i++) {
        sand_step(&s, 0, 1000, 0);
        wetted = moisture_of(sand_at(&s, 3, H / 2), gp_r) != 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(wetted,
        "gunpowder under water must take on moisture, the same as dirt "
        "already does");

    /* THE DRYING HALF: gunpowder dries at half dirt's rate
     * (reaction_t.dries 1 against dirt's 2, both material.c), so after
     * the SAME budget, starting from the SAME moisture 1, more of a
     * gunpowder row must still be wet than a dirt row. */
    uint8_t *gp_grid = malloc((size_t)3 * (size_t)WET_DRY_TRIALS);
    uint8_t *dirt_grid = malloc((size_t)3 * (size_t)WET_DRY_TRIALS);
    TEST_ASSERT_NOT_NULL_MESSAGE(gp_grid,
        "gunpowder dry-out comparison grid must fit in what the "
        "framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(dirt_grid,
        "dirt dry-out comparison grid must fit in what the framebuffer "
        "leaves");

    sand_t gp, dirt;
    sand_init(&gp, gp_grid, 3, WET_DRY_TRIALS, 21u);
    sand_init(&dirt, dirt_grid, 3, WET_DRY_TRIALS, 21u);

    for (int y = 0; y < WET_DRY_TRIALS; y++) {
        sand_set(&gp, 0, y, STONE);
        sand_set(&gp, 2, y, STONE);
        sand_set(&gp, 1, y, with_moisture(GUNPOWDER_CELL(0), 1, gp_r));

        sand_set(&dirt, 0, y, STONE);
        sand_set(&dirt, 2, y, STONE);
        sand_set(&dirt, 1, y, CELL_SOIL(MAT_DIRT, 0, 1));
    }

    const int budget = 150;
    for (int i = 0; i < budget; i++) {
        sand_step(&gp, 0, 1000, 0);
        sand_step(&dirt, 0, 1000, 0);
    }

    int gp_wet = 0, dirt_wet = 0;
    for (int y = 0; y < WET_DRY_TRIALS; y++) {
        if (moisture_of(sand_at(&gp, 1, y), gp_r) != 0) {
            gp_wet++;
        }
        if (CELL_MOISTURE(sand_at(&dirt, 1, y)) != 0) {
            dirt_wet++;
        }
    }
    free(gp_grid);
    free(dirt_grid);

    /* 224 measured the actual message, not what -Wformat-truncation proves:
     * five %d's, each assumed worst-case absent a range the compiler can
     * see, put its own bound at 241 - real ints here or not, this only
     * compiles clean on the device toolchain past that mark. */

    /* static, not a stack frame: the device's main task stack is 3584
     * bytes total, shared with Unity and printf, and check_stack_usage.py
     * gates any one frame at 1024 - this buffer has no reason to sit on
     * the stack at all. */
    static char why[320];
    snprintf(why, sizeof why,
             "gunpowder dries at half dirt's rate so after the SAME "
             "%d-step budget more of it must still be wet - gunpowder "
             "%d/%d wet, dirt %d/%d wet (roughly 56%% against 31%% "
             "expected, a wide margin against both binomial spreads at "
             "n=%d)",
             budget, gp_wet, WET_DRY_TRIALS, dirt_wet, WET_DRY_TRIALS,
             WET_DRY_TRIALS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(dirt_wet, gp_wet, why);
}

/* Total moisture across every gunpowder cell can only fall or move, never
 * rise - an upper bound, not full conservation (a lit cell reads
 * moisture_of() == 0, reaction_t.lit_from's own comment, which pulls the
 * total down without moving it anywhere) - adapted from the same claim
 * test_moisture_is_conserved_as_it_spreads makes for dirt, to gunpowder's
 * own same-species diffusion (soaks_to == 0, so there is no sand-like
 * neighbour for it to convert - spreading can only ever be gunpowder
 * handing a share to more gunpowder). */
static void test_gunpowder_moisture_never_multiplies_as_it_spreads(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 0, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));
    const int placed = (int)r->moist_max;

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int total = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (cell_is_gunpowder(c)) {
                    total += moisture_of(c, r);
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(total <= placed,
            "spreading moisture must move it, not multiply it - a front "
            "that gains on every hop is a watering can that fills "
            "itself");
    }
}

/* soaked_to/soaked_chance (material.c's GUNPOWDER_REACTION) place MAT_OIL
 * directly through place_reacted() once held reaches moist_max - a
 * different mechanism entirely from the moisture codec's own dry/wet
 * codes, so it is untouched by the drying-side bug documented above. A
 * bone-dry cell's held is always 0, so it can never roll this chance at
 * all. */
static void test_soaked_gunpowder_can_turn_into_oil_and_dry_never_does(void)
{
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));
    }

    bool saw_oil = false;
    for (int i = 0; i < 6000 && !saw_oil; i++) {
        sand_step(&s, 0, 1000, 0);
        saw_oil = count_cells_of(MAT_OIL) > 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_oil,
        "gunpowder saturated at moist_max must eventually turn into a "
        "full cell of oil - the one exit section 2's soaked_to design "
        "gives sitting-wet gunpowder besides drying back out");

    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GUNPOWDER_CELL(0));
    }
    for (int i = 0; i < 6000; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_OIL),
        "bone-dry gunpowder must never roll the saturated-to-oil chance "
        "- it only ever fires once held reaches moist_max, and a dry "
        "cell's held is always 0");
}

static void test_acid_dissolves_gunpowder(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int y = 1; y < H; y++) {
        sand_set(&s, 1, y, GLASS);
        sand_set(&s, W - 2, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, GUNPOWDER_CELL(0));
        }
    }
    for (int y = 1; y <= 2; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, count_cells_gunpowder(),
        "setup: there must be gunpowder to eat");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_gunpowder(),
        "acid must eat the gunpowder it settles onto, the same rate "
        "(dissolvable 200) it already eats sand and dirt at");
}

/* --- D1/E1/E2 regression: gunpowder is not soil, a lit fuse is not wet or
 * re-placed --------------------------------------------------------------
 *
 * D1 (the coordinator's own decision, GUNPOWDER_FIXES.md section 7): a new
 * reaction_t field `soil` (nonzero: plants may root in, sprout from,
 * drink from and conduct water into this material) replaces the old
 * `dries != 0` test at every plant/root site that meant "this is soil" -
 * dirt sets `.soil = 1`; nobody else. Moisture DIFFUSION between
 * same-species cells and percolation keep using `dries`, unchanged - only
 * the "is this ground a plant can use" question moves to `soil`. */

/* E2: a lit fuse must not be doused to an arbitrary level by a wet
 * same-species neighbour - soak diffusion's same_species() branch
 * (sand_reactions.c) is gated on !cell_is_burning(n), the same guard that
 * keeps percolation, find_water() and drinking off a burning cell. Decay
 * forced to 0 so this is purely about the diffusion gate, not a race
 * against burn-out. */
static void test_a_wet_neighbour_does_not_put_out_a_lit_fuse(void)
{
    fixture();
    sand_clear(&s);
    sand_set_decay(&s, 0);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    sand_set(&s, 3, H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 3, H - 2);
        char why[96];
        snprintf(why, sizeof why, "step %d", i);
        TEST_ASSERT_TRUE_MESSAGE(
            cell_is_gunpowder(c) && cell_code(c) == GUNPOWDER_LIT, why);
    }
}

/* D1: a rooted tree beside wet gunpowder must never convert it into more
 * root - mirrors test_a_root_never_eats_dry_dirt_sand_or_empty_space's
 * own pattern, with a moist candidate this time rather than a dry one,
 * since gunpowder is excluded by MATERIAL (reaction_t.soil == 0), not by
 * moisture level - step_one_rooting_cell()'s own candidate scan rejects
 * it before the eligibility roll is ever drawn, let alone spend_soil_
 * moisture() called. Moisture 2, not moist_max: at moist_max reaction_t.
 * soaked_to's own saturated-to-oil roll (step_one_soaking_cell()) can
 * fire all on its own over a budget this long, which is a real,
 * independent exit this test has nothing to do with - see test_soaked_
 * gunpowder_can_turn_into_oil_and_dry_never_does.
 *
 * DELIBERATELY NOT ASSERTED: that the gunpowder's moisture stays exactly
 * where it started. reaction_t.dries's own ambient-drying roll
 * (step_one_soaking_cell(), the ~1/256-a-step branch with nothing to
 * hand off to) is real, independent of any root, and fires on its own
 * over a budget this long regardless - dirt ambient-dries with nobody
 * watching it too. What D1 actually guarantees, and the only thing
 * checked below, is that gunpowder is never CONVERTED - the root's own
 * candidate scan never reaching it at all, so spend_soil_moisture() is
 * never even called on it, whatever ambient drying does to it in the
 * meantime. */
static void test_a_root_does_not_drink_from_or_eat_gunpowder(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    for (int x = cx - 2; x <= cx + 2; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy - 1, CELL_MAKE(MAT_WOOD, 0));   /* shelter, up */
    sand_set(&s, cx, cy, MATX(MATX_ROOT));
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);
    const uint8_t gp_moisture = 2;
    const cell_t soaked = with_moisture(GUNPOWDER_CELL(0), gp_moisture, gp_r);
    sand_set(&s, cx + 1, cy, soaked);   /* right: moist gunpowder, the one
                                         * candidate this test is about */

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const cell_t c = sand_at(&s, cx + 1, cy);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "a root must never convert gunpowder into a root cell, whatever "
        "moisture it holds - gunpowder is not soil (reaction_t.soil == 0)");
}

/* D1: a trunk beside wet gunpowder, with an empty cell free to seed a
 * leaf into, must never sprout - step_one_sprouting_cell()'s own
 * neighbour scan (reaction_of(n)->soil != 0) must reject gunpowder the
 * same way it already rejects dry dirt, sand and empty space. Moisture
 * 2, not moist_max, for the same reason the root test above picks it -
 * moist_max risks the independent soaked_to->oil roll over a budget this
 * long, which has nothing to do with sprouting. */
static void test_plants_do_not_sprout_in_gunpowder(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2, cy = 3;
    /* A floor two cells wider than the candidates themselves, not flush
     * with them - the same margin test_a_root_never_eats_dry_dirt_sand_
     * or_empty_space uses and for the same reason: a powder blocked
     * straight down still has an open diagonal-down to scatter into and
     * escape the very cell this test means to watch, which a floor flush
     * with the candidates does not close off. */
    for (int x = cx - 2; x <= cx + 1; x++) {
        sand_set(&s, x, cy + 1, STONE);
    }
    sand_set(&s, cx, cy, CELL_MAKE(MAT_WOOD, 0));
    const reaction_t *gp_r = reaction_of(GUNPOWDER_BASE);
    const uint8_t gp_moisture = 2;
    const cell_t soaked = with_moisture(GUNPOWDER_CELL(0), gp_moisture, gp_r);
    sand_set(&s, cx - 1, cy, soaked);
    /* (cx + 1, cy) stays SAND_EMPTY from sand_clear() above - the
     * candidate cell a real sprout would seed a leaf into. */

    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    bool any_leaf = false;
    for (int y = 0; y < H && !any_leaf; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                any_leaf = true;
                break;
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(any_leaf,
        "a trunk beside gunpowder, however wet, must never sprout a leaf "
        "- gunpowder is not soil (reaction_t.soil == 0), the same guard "
        "test_a_root_never_eats_dry_dirt_sand_or_empty_space pins for a "
        "root's own neighbour scan");

    /* Moisture is NOT asserted to stay exactly at gp_moisture here, the
     * same reason test_a_root_does_not_drink_from_or_eat_gunpowder's own
     * comment gives: reaction_t.dries's ambient-drying roll is real,
     * independent of any sprout, and can fire on its own over a budget
     * this long. Only the CONVERSION claim is checked. */
    const cell_t c = sand_at(&s, cx - 1, cy);
    TEST_ASSERT_TRUE_MESSAGE(cell_is_gunpowder(c),
        "and the gunpowder itself must still be gunpowder - never eaten "
        "or converted");
}

/* E1: a lit fuse must not be RE-PLACED by heat every single step it sits
 * beside a heat source - try_heat_transform_given()'s heat_chance roll,
 * run against a neighbour that is already burning, used to write the
 * IDENTICAL GUNPOWDER_LIT_CELL byte back onto itself whenever the roll
 * passed: same value, but still a write, so its row was marked dirty and
 * its block woken every step regardless (4 RNG draws + wakes per lit
 * cell, per the fix list this test pins). sand_track_dirty_rows() is the
 * observable chosen here - it is exactly the mechanism a real redraw
 * keys off, and a write that changes nothing must never trip it.
 *
 * Everything else sharing the fuse's row has to be provably inert, or a
 * legitimate, unrelated write there would look exactly like the bug this
 * pins. STONE was tried first and rejected: its own heat_ramp (32,
 * material.c) climbs a variant under conducted heat, which marks the row
 * dirty all by itself a few steps in - confirmed by direct instrumentation
 * while writing this test (dirty[row] flipped at step 6, STONE's own
 * variant climbing 0x33 -> 0x36 over the run, nothing to do with the
 * fuse). WOOD (KIND_STATIC, heat_ramp 0, no `heats_to`/`heat_chance` of
 * its own) walls lava in instead - immune to try_heat_transform_given()
 * outright (its first two checks both fail before either the ramp or the
 * heat_chance roll). Flammability forced to 0 board-wide so wood's own
 * small flammability (6) can never turn it to fire on contact with lava
 * either - this test is about the HEAT path only. */
static void test_a_lit_fuse_is_not_re_placed_by_heat(void)
{
    dirty_fixture();
    sand_set_decay(&s, 0);          /* immortal - stays lit for the whole
                                      * budget, itself a heat source too */
    sand_set_flammability(&s, 0);   /* isolate the heat path - see this
                                      * test's own top comment on wood */
    /* sand_set_mobility(0) only holds GAS still (see test_fire_beside_
     * dry_gunpowder_lights_it's own comment) - it says nothing about a
     * POWDER's own gravity or a LIQUID's own flow, so both still need a
     * real floor/wall to stay exactly in place for 50 steps. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);   /* floor - a different row entirely */
    }
    sand_set(&s, 2, H - 2, CELL_MAKE(MAT_WOOD, 0));   /* inert wall, not stone */
    sand_set(&s, 3, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    sand_set(&s, 4, H - 2, GUNPOWDER_LIT_CELL);
    const cell_t before = sand_at(&s, 4, H - 2);
    memset(dirty, 0, sizeof dirty);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(before, sand_at(&s, 4, H - 2),
        "a lit fuse beside a heat source must stay the identical byte - "
        "try_heat_transform_given() must reject an already-burning "
        "neighbour before its heat_chance roll, not merely happen to "
        "place the same value back");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[H - 2],
        "and it must never even be WRITTEN, identical value or not - a "
        "write that changes nothing still marks its row dirty and wakes "
        "its block every step, which is exactly the cost E1 exists to "
        "remove");
}

void run_sand_gunpowder_suite(void)
{
    RUN_TEST(test_a_gunpowder_grain_keeps_its_tone_as_it_falls);
    RUN_TEST(test_material_name_says_gunpowder_for_every_code);
    RUN_TEST(test_gunpowder_palette_tones_are_distinct_and_moisture_darkens);
    RUN_TEST(test_gunpowder_codes_decode_to_the_documented_moisture);
    RUN_TEST(test_gunpowder_falls_and_piles_like_a_powder);
    RUN_TEST(test_gunpowder_sinks_through_liquids_and_rests_on_and_under_sand);
    RUN_TEST(test_gunpowder_is_conserved_under_every_gravity);
    RUN_TEST(test_fire_beside_dry_gunpowder_lights_it);
    RUN_TEST(test_lava_beside_dry_gunpowder_lights_it_through_the_heat_path);
    RUN_TEST(test_heat_conducted_through_stone_lights_gunpowder);
    RUN_TEST(test_a_lit_gunpowder_trail_burns_along_itself);
    RUN_TEST(test_a_lit_two_by_two_of_gunpowder_detonates);
    RUN_TEST(test_a_detonating_two_by_two_leaves_no_lit_gunpowder_behind);
    RUN_TEST(test_fuse_blasts_are_capped_at_one_per_step);
    RUN_TEST(test_a_longer_fuse_cooldown_delays_the_next_blast);
    RUN_TEST(test_a_one_wide_lit_trail_never_detonates);
    RUN_TEST(test_a_buried_lit_gunpowder_cell_is_not_smothered);
    RUN_TEST(test_water_quenches_lit_gunpowder_to_soaked);
    RUN_TEST(test_gunpowder_without_impulses_burns_to_fire);
    RUN_TEST(test_soaked_gunpowder_never_lights_beside_lava);
    RUN_TEST(test_heat_dries_wet_gunpowder_one_level_with_steam);
    RUN_TEST(test_damp_gunpowder_ignites_less_readily_than_dry);
    RUN_TEST(test_water_wets_gunpowder_and_it_dries_out_slowly);
    RUN_TEST(test_gunpowder_moisture_never_multiplies_as_it_spreads);
    RUN_TEST(test_soaked_gunpowder_can_turn_into_oil_and_dry_never_does);
    RUN_TEST(test_acid_dissolves_gunpowder);
    RUN_TEST(test_a_wet_neighbour_does_not_put_out_a_lit_fuse);
    RUN_TEST(test_a_root_does_not_drink_from_or_eat_gunpowder);
    RUN_TEST(test_plants_do_not_sprout_in_gunpowder);
    RUN_TEST(test_a_lit_fuse_is_not_re_placed_by_heat);
}

SUITE_REGISTER(run_sand_gunpowder_suite);
