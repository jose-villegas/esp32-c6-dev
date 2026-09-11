/*=============================================================================
 * Portable suite: the falling-sand automaton - dirty-row conservation,
 * gravity in other directions, spawning, and emitters.
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

/* --- dirty rows: nothing changes without saying so ---------------------- */

/* The invariant the renderer depends on, asserted directly for every
 * material rather than inferred from the passes that maintain it.
 *
 * app_sand.c only repaints rows whose dirty_rows byte is set, so a cell
 * that changes on a row nobody marked is a pixel left stale on the panel
 * until something else happens to redraw that band. That failure is
 * invisible in every other test here - the grid is right, only the screen
 * is wrong - and it is exactly the kind of thing that gets noticed on
 * device and not before.
 *
 * TRANSIENT materials are the reason this is worth a test of its own.
 * A grain of sand changes when it moves, and a move is hard to forget
 * about. Fire, gas, steam and smoke also change when they merely AGE:
 * tick_decay() rewrites the variant nibble in place, the palette turns
 * that into a different colour, and nothing has moved at all. A pass that
 * remembered to mark its moves and forgot to mark its decay would look
 * perfectly correct right up until a flame stopped fading on screen. */
static void assert_every_change_is_marked(material_id_t m, int steps,
                                          const char *what)
{
    static uint8_t seen[W * H];

    fixture();
    sand_track_dirty_rows(&s, dirty);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 0; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    for (int y = 2; y <= 4; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(m, MATERIAL_VARIANTS - 1));
        }
    }

    for (int i = 0; i < steps; i++) {
        memcpy(seen, s.cells, sizeof seen);
        memset(dirty, 0, sizeof dirty);   /* the renderer clears as it draws */
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (seen[y * W + x] == s.cells[y * W + x]) {
                    continue;
                }
                /* The whole byte, not just the material nibble: a cell
                 * that only faded still has to be repainted. */
                TEST_ASSERT_TRUE_MESSAGE(dirty[y] != 0, what);
            }
        }
    }
}

static void test_every_cell_change_marks_its_row_dirty(void)
{
    assert_every_change_is_marked(MAT_SAND, 120,
        "a sand grain must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_WATER, 120,
        "a water cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_GAS, 240,
        "a gas cell must never change on a row left unmarked - it AGES as "
        "well as moving, and a fade is a repaint too");
    assert_every_change_is_marked(MAT_FIRE, 240,
        "a fire cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_SMOKE, 240,
        "a smoke cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_STEAM, 240,
        "a steam cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_OIL, 120,
        "an oil cell must never change on a row left unmarked");
    assert_every_change_is_marked(MAT_LAVA, 240,
        "a lava cell must never change on a row left unmarked");
}

/* --- conservation ------------------------------------------------------- */

static void test_grains_are_never_created_or_destroyed(void)
{
    fixture();

    /* A slab dropped into the middle, then shaken through every gravity
     * direction. Whatever the rules do, the count must not drift. */
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    const int expected = sand_count(&s);
    TEST_ASSERT_EQUAL_INT(15, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a step must conserve grains in every gravity direction");
        }
    }
}

static void test_a_grain_keeps_its_shade_as_it_falls(void)
{
    fixture();
    const uint8_t shade = SAND_LAST_SHADE;
    sand_set(&s, 3, 0, shade);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(shade, sand_at(&s, 3, 3),
        "shade travels with the grain, or a falling pile shimmers");
}

/* --- gravity in other directions ---------------------------------------- */

static void test_grains_fall_upward_when_the_board_is_inverted(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    sand_step(&s, 0, -1, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, H - 2),
        "gravity is whatever direction it is given, including up");
}

static void test_grains_fall_sideways_when_the_board_is_on_its_edge(void)
{
    fixture();
    sand_set(&s, 0, 3, SAND_FIRST_SHADE);

    sand_step(&s, 1, 0, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 1, 3),
        "with gravity to the right, the far wall is the floor");
}

static void test_a_heap_settles_against_whichever_wall_is_down(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 4; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    /* Long enough for everything to reach the right-hand wall and stop. */
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 1, 0, 0);
    }

    /* Note what is NOT asserted: that every grain ends up in the last column
     * or two. It does not, and should not - the heap forms a wedge with a 45
     * degree angle of repose, and grains on the slope are held up by their
     * neighbours rather than by the wall. Demanding they pack flat would be
     * asserting the absence of the very behaviour that makes it look like
     * sand. What matters is the side, the contact and the stability. */
    int touching_wall = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) == SAND_EMPTY) {
                continue;
            }
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(W / 2, x,
                "every grain must migrate to the side gravity points at");
            if (x == W - 1) {
                touching_wall++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, touching_wall,
        "the heap must actually reach the wall, not stall short of it");

    uint8_t settled[W * H];
    memcpy(settled, cells, sizeof(settled));
    sand_step(&s, 1, 0, 0);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
        "a settled heap must be completely stable, not creep for ever");
}

/* --- spawning ----------------------------------------------------------- */

static void test_spawn_fills_a_disc(void)
{
    fixture();

    const int filled = sand_spawn(&s, 4, 4, 2, MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, filled, "a spawn must place grains");
    TEST_ASSERT_EQUAL_INT_MESSAGE(filled, sand_count(&s),
        "the reported count must match what is actually on the grid");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "the centre is inside the disc");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 0, 0),
        "a corner far outside the radius must stay empty");
}

static void test_spawn_is_clipped_to_the_grid(void)
{
    fixture();

    /* Centred off the top-left corner: most of the disc is out of bounds. */
    const int filled = sand_spawn(&s, 0, 0, 3, MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, filled,
        "the part of the disc that is on the grid must still be placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(filled, sand_count(&s),
        "clipped cells must not be counted as filled");
}

static void test_spawning_onto_existing_grains_does_not_double_count(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 2, MAT_SAND);
    const int after_first = sand_count(&s);

    const int filled = sand_spawn(&s, 4, 4, 2, MAT_SAND);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, filled,
        "spawning onto a full disc fills nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_first, sand_count(&s),
        "and must not change the grid");
}

static void test_erase_removes_a_disc(void)
{
    fixture();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    const int removed = sand_erase(&s, 4, 4, 2);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, removed, "an erase must remove grains");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "the centre is inside the disc");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 0, 0),
        "a corner far outside the radius must be untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W * H - removed, sand_count(&s),
        "the reported count must match what actually left the grid");
}

static void test_erasing_empty_space_removes_nothing(void)
{
    fixture();

    const int removed = sand_erase(&s, 4, 4, 3);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, removed,
        "erasing an empty area must report nothing removed, or the count "
        "drifts the same way a double-counting spawn would");
}

static void test_erase_is_clipped_to_the_grid(void)
{
    fixture();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    const int removed = sand_erase(&s, 0, 0, 3);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, removed, "the on-grid part must go");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W * H - removed, sand_count(&s),
        "cells off the grid must not be counted as removed");
}

static void test_erase_marks_the_rows_it_emptied(void)
{
    dirty_fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 4, SAND_FIRST_SHADE);
    }
    memset(dirty, 0, sizeof(dirty));

    sand_erase(&s, 4, 4, 1);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "a row a grain was removed from has changed and must be redrawn - "
        "otherwise the erased sand stays visible on the panel");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0], "a distant row has not");
}

static void test_spawned_grains_use_the_full_range_of_shades(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 3, MAT_SAND);

    bool seen[SAND_LAST_SHADE + 1] = { false };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t c = sand_at(&s, x, y);
            if (c != SAND_EMPTY) {
                TEST_ASSERT_TRUE_MESSAGE(c >= SAND_FIRST_SHADE && c <= SAND_LAST_SHADE,
                    "every grain must carry a valid shade");
                seen[c] = true;
            }
        }
    }

    int distinct = 0;
    for (int i = SAND_FIRST_SHADE; i <= SAND_LAST_SHADE; i++) {
        distinct += seen[i] ? 1 : 0;
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct,
        "a flat-coloured pile looks like a solid block, not sand");
}

/* --- emitters ------------------------------------------------------------- */

/* A persistent point source - see sand_add_emitter() in sand.h. Unlike
 * sand_spawn()/sand_erase() above, an emitter is stepped by sand_step()
 * itself rather than acting the moment it is called, so most of these
 * tests run at least one step before looking at the grid. */

static void test_an_emitter_fills_its_own_cell_when_empty(void)
{
    fixture();
    /* The bottom row, so gravity cannot immediately carry the fresh grain
     * away - this test is about placement, not about liquid movement, and
     * placing it anywhere else would make it about both. */
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "an emitter must fill its own point once that point is empty");
}

static void test_an_emitter_does_not_overwrite_an_occupied_cell(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "setup: an emitter may be placed over an already-occupied cell");

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "an emitter must never overwrite whatever is already sitting on "
        "its own point - that is the entire rate control, and an emitter "
        "that ignored it would be a firehose");
}

/* The failure mode this test exists to catch: a write that places material
 * but forgets to wake the block it landed in. That failure would still
 * pass a test that only checked the cell was written - sand_set() writes
 * the byte regardless of block-sleeping state - so this one goes on to
 * demand that the material actually MOVES afterwards, which only happens
 * if the sweep is still visiting that block. */
static void test_an_emitter_wakes_a_sleeping_block(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);

    /* An empty grid settles on its very first step: nothing in the block
     * moved, and neither did any neighbour (there is only the one block on
     * this WxH fixture - see BLOCK_COLS/BLOCK_ROWS above). */
    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_TRUE_MESSAGE(sand_block_settled(&s, 0, 0),
        "setup: the block must actually be asleep, or this test proves "
        "nothing about waking one");

    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER),
        "setup: the emitter's own point is empty and in bounds");

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    bool moved_off_row_zero = false;
    for (int x = 0; x < W && !moved_off_row_zero; x++) {
        for (int y = 1; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                moved_off_row_zero = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(moved_off_row_zero,
        "emitted water must have moved somewhere below row 0 over 60 steps "
        "- if it is still confined to the row it was emitted on, the block "
        "that went to sleep before the emitter arrived was never woken, "
        "and the sweep has been skipping it ever since (it would still be "
        "written there every step, since sand_set() does not care whether "
        "the block sleeps - only whether it later MOVES proves the wake "
        "happened)");
}

static void test_emitted_water_produces_a_continuing_stream(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER),
        "setup: the emitter's own point is empty and in bounds");

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int water_cells = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                water_cells++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(3, water_cells,
        "a water emitter left running over many steps must read as a "
        "continuing stream, not a single cell - each step the source cell "
        "clears by flowing away and the emitter refills it, over and over "
        "for as long as the tap runs");
}

/* emit_from_emitters() must write the exact placeholder brushes[] hands
 * sand_add_emitter() (app_sand.c): CELL_MAKE(material, 0), whose variant 0
 * means fill-level zero for KIND_LIQUID (material.h) - not a real cell.
 * Tests below use that literal placeholder, not this file's WATER/LAVA/...
 * macros (variant 8), and step with gravity (0, 0, 0) so sand_step()
 * returns after emit_from_emitters() (its own comment), isolating what was
 * written from later movement or reactions. */

static void test_an_emitted_liquid_cell_is_full_not_the_placeholders_zero_mass(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_WATER, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX, CELL_VARIANT(c),
        "an emitted liquid must be a FULL cell, exactly as a pour is - "
        "random_cell() always hands a fresh liquid cell MASS_MAX, never a "
        "random amount and never the placeholder's zero - a water tap "
        "that instead wrote variant 0 raw would place a cell with "
        "MAT_WATER in it and no water");
}

static void test_an_emitted_lava_cell_is_full_not_the_placeholders_zero_mass(void)
{
    /* Lava specifically, because it is what was reported on hardware: a
     * lava source produced steam (the reactions pass still saw MAT_LAVA
     * and quenched it) but no lava - because the cell it quenched never
     * carried any mass to look like lava in the first place. */
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_LAVA, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX, CELL_VARIANT(c),
        "an emitted lava cell must be FULL, not the placeholder's zero "
        "mass - a zero-mass lava cell is exactly what let a lava tap on "
        "hardware produce steam (the reactions pass still saw MAT_LAVA "
        "and quenched it) but no lava anyone could see or that could "
        "flow");
}

static void test_an_emitted_transient_cell_has_full_life_not_the_placeholders_zero(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, 0, CELL_MAKE(MAT_GAS, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1, CELL_VARIANT(c),
        "an emitted transient material must start at FULL LIFE, exactly "
        "as a pour does - random_cell() hands a fresh decaying cell "
        "MATERIAL_VARIANTS - 1, never the placeholder's variant 0, which "
        "decay reads as life already spent: a gas tap that wrote it raw "
        "would emit cells already dead");
}

static void test_an_emitted_powder_still_lands_in_a_valid_shade(void)
{
    /* Unlike the liquid and transient cases above, this one cannot
     * distinguish the fix from the bug by itself - variant 0 happens to
     * be a valid shade for a powder (see material.h's top comment), which
     * is exactly why sand and snow LOOKED fine on hardware while water
     * and lava did not. It is here anyway, as a regression guard: nothing
     * about routing the emitter through sand_spawn_cell() may push a
     * powder's variant outside the range a pour would ever produce. */
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, H - 1, CELL_MAKE(MAT_SAND, 0)),
        "setup: placing an emitter over empty, in-bounds ground must "
        "succeed");

    sand_step(&s, 0, 0, 0);

    const cell_t c = sand_at(&s, 3, H - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(c), "setup");
    TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) < SAND_DUNE_SHADES,
        "an emitted grain of sand must land within the dune shade band, "
        "same as a pour does - random_cell() never hands a freshly "
        "painted grain one of the shades reserved for cullet");
}

/* The same claim test_the_brush_and_the_setter_agree_about_every_material
 * makes about sand_set() versus the brush, but for the emitter versus
 * sand_spawn_cell() - and exhaustive over every material an emitter may
 * ever hold, rather than sampled to the handful above. The reason to walk
 * all of them rather than trust water/lava/gas/sand as representatives is
 * exactly what made this bug ship: variant 0 means something DIFFERENT for
 * each material kind - a liquid's fill level, a transient's life, glass's
 * temperature, soil's tone and moisture, a powder's shade - and picking
 * representatives only catches the kinds someone thought to check. A loop
 * over every emit-eligible material catches the next one added with a
 * variant meaning nobody anticipated, the same way this one got through. */
static void test_an_emitter_and_sand_spawn_cell_agree_about_every_material(void)
{
    const int x = 3, y = 3;

    for (int m = 1; m < MAT_COUNT; m++) {
        const cell_t placeholder = CELL_MAKE((material_id_t)m, 0);
        if (!material_can_emit(placeholder)) {
            continue;   /* material_can_emit() is the same gate
                         * sand_add_emitter()'s caller applies (see
                         * test_material_can_emit_matches_every_brush_by_kind)
                         * - a material that can never legally be an
                         * emitter has nothing to agree about here */
        }

        /* The emitter, given the exact placeholder the brush table would
         * hand it. Zero gravity, so this step does nothing but emit - see
         * this block's own top comment. */
        fixture();
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, x, y, placeholder),
            "setup");
        sand_step(&s, 0, 0, 0);
        const cell_t emitted = sand_at(&s, x, y);

        /* sand_spawn_cell(), given the very same placeholder, on an
         * identically fresh board - same seed, same RNG draw count so
         * far (zero), so any random pick inside random_cell() lands on
         * the same value in both. */
        fixture();
        sand_spawn_cell(&s, x, y, 0, placeholder);
        const cell_t spawned = sand_at(&s, x, y);

        char why[256];
        snprintf(why, sizeof why,
                 "an emitter of %s disagrees with sand_spawn_cell() about "
                 "what a fresh cell of it looks like - if the emitter's "
                 "byte is the unresolved placeholder (variant 0) rather "
                 "than spawned's, the emitter is writing the brush's raw "
                 "byte instead of resolving it",
                 material_by_id((material_id_t)m)->name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(spawned, emitted, why);
    }

    /* GUNPOWDER, again separately - material_can_emit(placeholder) is
     * already exercised for every ordinary id by the loop above; this is
     * the one emit-eligible material the loop's CELL_MAKE(id, 0)
     * placeholder can never construct at all, since gunpowder has no
     * material_id_t of its own. */
    {
        const cell_t placeholder = GUNPOWDER_CELL(0);

        fixture();
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, x, y, placeholder),
            "setup");
        sand_step(&s, 0, 0, 0);
        const cell_t emitted = sand_at(&s, x, y);

        fixture();
        sand_spawn_cell(&s, x, y, 0, placeholder);
        const cell_t spawned = sand_at(&s, x, y);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(spawned, emitted,
            "an emitter of gunpowder disagrees with sand_spawn_cell() "
            "about what a fresh cell of it looks like");
    }
}

/* The observable claim behind the byte-level tests above: a liquid emitter
 * left running has to make a POOL, not merely keep writing cells that
 * carry the right material and no water. Summing CELL_VARIANT (the fill
 * level) rather than counting cells is the point: a cell can be present
 * and correctly MAT_WATER while carrying zero mass, which a count-based
 * check (test_emitted_water_produces_a_continuing_stream above) would not
 * catch. */
static void test_a_running_water_emitter_accumulates_mass_on_the_floor(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(
        sand_add_emitter(&s, 3, 0, CELL_MAKE(MAT_WATER, 0)),
        "setup: the emitter's own point is empty and in bounds");

    /* Grid walls are solid past the last row (see sand_at()'s own
     * comment), so the bottom row is already a floor with no need to
     * paint one - the same shape test_an_emitter_fills_its_own_cell_when_
     * empty relies on. */
    for (int i = 0; i < 150; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    long total_mass = 0;
    bool water_on_the_floor = false;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_IS_EMPTY(c) || CELL_MATERIAL(c) != MAT_WATER) {
                continue;
            }
            total_mass += CELL_VARIANT(c);
            if (y == H - 1) {
                water_on_the_floor = true;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(3 * MASS_MAX, total_mass,
        "a water emitter left running above a floor must ACCUMULATE far "
        "more mass than one full cell's worth - this is the reported "
        "symptom itself: a source producing cells that carry MAT_WATER "
        "but no mass never pools no matter how long the tap runs, because "
        "there is never anything in any of the cells it writes");
    TEST_ASSERT_TRUE_MESSAGE(water_on_the_floor,
        "the accumulated water must be sitting on the floor, not stranded "
        "only at the emitter's own point - a source with no mass has "
        "nothing that could ever fall");
}

static void test_adding_an_emitter_over_an_occupied_cell_still_registers(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, H - 1, WATER),
        "an emitter may be placed over an already-occupied cell");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "and it must actually be registered, not silently dropped for "
        "landing on something");

    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "and it must not emit while the cell stays occupied");

    /* Cleared directly, not via sand_erase() - erase would also remove the
     * emitter itself (see test_erase_stops_an_emitter_from_emitting below)
     * and defeat the point of this test, which is only about the delay
     * between registering and first emitting. */
    sand_set(&s, 3, H - 1, SAND_EMPTY);
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "once the cell clears, the already-registered emitter must start "
        "emitting into it");
}

static void test_adding_at_an_existing_emitter_replaces_its_cell(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 4, WATER), "setup");
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 4, GAS),
        "re-adding at the same point must succeed, not fail because the "
        "point is already an emitter");

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "the second add must replace the first emitter's cell, not add a "
        "second emitter at the same point");

    int x, y;
    cell_t cell;
    TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, 0, &x, &y, &cell), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(cell),
        "the surviving emitter must emit the SECOND cell it was given, not "
        "the first");
}

static void test_the_emitter_cap_is_respected(void)
{
    fixture();
    /* W * H (64) is well over SAND_MAX_EMITTERS (16), so every one of these
     * has a distinct, in-bounds point and none of them collide with each
     * other. */
    for (int i = 0; i < SAND_MAX_EMITTERS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, i % W, i / W, WATER),
            "every emitter up to the cap must be accepted");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_MAX_EMITTERS, sand_emitter_count(&s),
        "setup");

    /* (0, 2) was not used by the loop above (it only reaches y = 1). */
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, 2, GAS),
        "one more than the cap must be refused");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_MAX_EMITTERS, sand_emitter_count(&s),
        "and the refusal must not have corrupted the list - the count must "
        "stay exactly at the cap");

    for (int i = 0; i < SAND_MAX_EMITTERS; i++) {
        int x, y;
        cell_t cell;
        TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, i, &x, &y, &cell),
            "every emitter that was already there must still be readable");
        TEST_ASSERT_EQUAL_INT_MESSAGE(i % W, x,
            "and unchanged by the refused add");
        TEST_ASSERT_EQUAL_INT_MESSAGE(i / W, y,
            "and unchanged by the refused add");
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(cell),
            "and unchanged by the refused add");
    }
}

static void test_out_of_bounds_emitters_are_rejected(void)
{
    fixture();
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, -1, 0, WATER),
        "negative x is off the grid");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, -1, WATER),
        "negative y is off the grid");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, W, 0, WATER),
        "x == W is one past the right edge");
    TEST_ASSERT_FALSE_MESSAGE(sand_add_emitter(&s, 0, H, WATER),
        "y == H is one past the bottom edge");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "none of the rejected adds may have registered anything");
}

static void test_remove_emitters_only_removes_those_in_radius(void)
{
    fixture();
    sand_add_emitter(&s, 4, 4, WATER);   /* the centre: distance 0 */
    sand_add_emitter(&s, 5, 4, WATER);   /* distance 1: inside a radius of 1 */
    sand_add_emitter(&s, 4, 6, GAS);     /* distance 2: outside it */
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, sand_emitter_count(&s), "setup");

    const int removed = sand_remove_emitters(&s, 4, 4, 1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, removed,
        "only the two emitters within the radius must be removed - the "
        "same disc test sand_erase() uses");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s),
        "and exactly one emitter must remain");

    int x, y;
    cell_t cell;
    TEST_ASSERT_TRUE_MESSAGE(sand_emitter_at(&s, 0, &x, &y, &cell), "setup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, x,
        "the survivor must be the one outside the radius");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, y,
        "the survivor must be the one outside the radius");
}

static void test_erase_stops_an_emitter_from_emitting(void)
{
    fixture();
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 0, WATER), "setup");

    sand_erase(&s, 3, 0, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "erasing over an emitter must remove it - without this a running "
        "tap could never be turned off, which would make the whole "
        "feature a trap");

    sand_step(&s, 0, 1000, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "and it must actually have stopped emitting, not merely been "
        "forgotten by sand_emitter_count() while still running");
}

static void test_erase_count_excludes_emitters(void)
{
    fixture();

    /* An emitter with nothing under it: erasing here changes no cell, so
     * the count sand_erase() returns must be zero even though an emitter
     * also went away. */
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 3, 3, WATER), "setup");
    const int removed_empty = sand_erase(&s, 3, 3, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, removed_empty,
        "removing an emitter over an already-empty cell must not count as "
        "a cell removed - sand_remove_emitters() is what counts emitters, "
        "not this return value");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "setup check: the emitter must actually be gone");

    /* An emitter WITH a grain under it: erasing here changes exactly one
     * cell, and the emitter leaving too must not make it look like two -
     * see sand_erase()'s own comment in sand.h on why that count must stay
     * exactly "cells changed", not "cells changed plus emitters removed". */
    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    TEST_ASSERT_TRUE_MESSAGE(sand_add_emitter(&s, 4, 4, WATER), "setup");
    const int removed_occupied = sand_erase(&s, 4, 4, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, removed_occupied,
        "the count must still mean exactly one cell changed, whether or "
        "not an emitter also happened to sit on it");
}

static void test_sand_init_clears_emitters_from_a_previous_use(void)
{
    fixture();
    sand_add_emitter(&s, 3, 3, WATER);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_emitter_count(&s), "setup");

    sand_init(&s, cells, W, H, 12345u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_emitter_count(&s),
        "sand_init() must clear emitters left over from a previous use of "
        "the struct, the same as it clears every other piece of per-run "
        "state - see the THIRD-COPY bug in sand_init()'s own comment for "
        "what carrying stale state across a reused sand_t already cost "
        "once");
}

/* Every entry the app's palette offers - see brushes[] in app_sand.c - by
 * KIND rather than by importing that table: this file cannot see
 * app_sand.c and should not start to. The four flowing kinds (powder,
 * liquid, gas) come out true and the two static ones false, which is
 * exactly the KIND_POWDER/LIQUID/GAS-may, KIND_STATIC-may-not rule
 * material_can_emit() implements - see its own comment in material.h, and
 * test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on
 * above for why the two extended entries both land on `false` through one
 * shared row rather than two independent answers. */
static void test_material_can_emit_matches_every_brush_by_kind(void)
{
    static const struct { cell_t cell; bool can_emit; const char *why; } cases[] = {
        { SAND,                    true,  "sand is a powder" },
        { WATER,                   true,  "water is a liquid" },
        { STONE,                   false, "stone is static" },
        { GAS,                     true,  "gas is a gas" },
        { FIRE,                    true,  "fire is a gas" },
        { WOOD,                    false, "wood is static" },
        { OIL,                     true,  "oil is a liquid" },
        { LAVA,                    true,  "lava is a liquid" },
        { CELL_MAKE(MAT_ACID, MASS_MAX), true,  "acid is a liquid" },
        { GLASS,                   false, "glass is static" },
        { SNOW,                    true,  "snow is a powder" },
        { CELL_MAKE(MAT_DIRT, 0),  true,  "dirt is a powder" },
        { MATX(MATX_ICE),          false, "ice shares the extended row's KIND_STATIC" },
        { MATX(MATX_PLANT),        false, "plant shares the extended row's KIND_STATIC" },
        { GUNPOWDER_CELL(0),       true,  "gunpowder is the one extended-range material that reads KIND_POWDER, not the statics' shared KIND_STATIC" },
    };

    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[k].can_emit,
            material_can_emit(cases[k].cell), cases[k].why);
    }
}

void run_sand_spawning_suite(void)
{
    RUN_TEST(test_every_cell_change_marks_its_row_dirty);
    RUN_TEST(test_grains_are_never_created_or_destroyed);
    RUN_TEST(test_a_grain_keeps_its_shade_as_it_falls);
    RUN_TEST(test_grains_fall_upward_when_the_board_is_inverted);
    RUN_TEST(test_grains_fall_sideways_when_the_board_is_on_its_edge);
    RUN_TEST(test_a_heap_settles_against_whichever_wall_is_down);
    RUN_TEST(test_spawn_fills_a_disc);
    RUN_TEST(test_spawn_is_clipped_to_the_grid);
    RUN_TEST(test_spawning_onto_existing_grains_does_not_double_count);
    RUN_TEST(test_erase_removes_a_disc);
    RUN_TEST(test_erasing_empty_space_removes_nothing);
    RUN_TEST(test_erase_is_clipped_to_the_grid);
    RUN_TEST(test_erase_marks_the_rows_it_emptied);
    RUN_TEST(test_spawned_grains_use_the_full_range_of_shades);
    RUN_TEST(test_an_emitter_fills_its_own_cell_when_empty);
    RUN_TEST(test_an_emitter_does_not_overwrite_an_occupied_cell);
    RUN_TEST(test_an_emitter_wakes_a_sleeping_block);
    RUN_TEST(test_emitted_water_produces_a_continuing_stream);
    RUN_TEST(test_an_emitted_liquid_cell_is_full_not_the_placeholders_zero_mass);
    RUN_TEST(test_an_emitted_lava_cell_is_full_not_the_placeholders_zero_mass);
    RUN_TEST(test_an_emitted_transient_cell_has_full_life_not_the_placeholders_zero);
    RUN_TEST(test_an_emitted_powder_still_lands_in_a_valid_shade);
    RUN_TEST(test_an_emitter_and_sand_spawn_cell_agree_about_every_material);
    RUN_TEST(test_a_running_water_emitter_accumulates_mass_on_the_floor);
    RUN_TEST(test_adding_an_emitter_over_an_occupied_cell_still_registers);
    RUN_TEST(test_adding_at_an_existing_emitter_replaces_its_cell);
    RUN_TEST(test_the_emitter_cap_is_respected);
    RUN_TEST(test_out_of_bounds_emitters_are_rejected);
    RUN_TEST(test_remove_emitters_only_removes_those_in_radius);
    RUN_TEST(test_erase_stops_an_emitter_from_emitting);
    RUN_TEST(test_erase_count_excludes_emitters);
    RUN_TEST(test_sand_init_clears_emitters_from_a_previous_use);
    RUN_TEST(test_material_can_emit_matches_every_brush_by_kind);
}

SUITE_REGISTER(run_sand_spawning_suite);
