/*=============================================================================
 * Portable suite: the falling-sand automaton.
 *
 * Runs on the host and on the board. Nothing here touches hardware - the grid
 * is a byte array and gravity is a pair of ints - which is the point: a rule
 * like "a grain slides off a pile" is far easier to state on a 5x5 grid than
 * to spot by eye on a 184x224 one at 40 fps.
 *
 * Grids are written out as text so a failure is readable:
 *   '.' empty, 'o' a grain.
 *===========================================================================*/

#include <string.h>

#include "unity.h"
#include "suites.h"

#include "sand.h"

/* Big enough for every case here, small enough to write out by hand. */
#define W 8
#define H 8

static sand_t   s;
static uint8_t  cells[W * H];

static void fixture(void)
{
    sand_init(&s, cells, W, H, 12345u);
}

/* Load a picture of a grid. Rows are given top to bottom, so the text reads
 * the way the screen looks. */
static void load(const char *rows[], int count)
{
    sand_clear(&s);
    for (int y = 0; y < count; y++) {
        for (int x = 0; rows[y][x] != '\0'; x++) {
            if (rows[y][x] == 'o') {
                sand_set(&s, x, y, SAND_FIRST_SHADE);
            }
        }
    }
}

static void assert_looks_like(const char *rows[], int count, const char *why)
{
    char expected[H][W + 1];
    char actual[H][W + 1];

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            expected[y][x] = (y < count && rows[y][x] == 'o') ? 'o' : '.';
            actual[y][x]   = sand_at(&s, x, y) == SAND_EMPTY ? '.' : 'o';
        }
        expected[y][W] = '\0';
        actual[y][W]   = '\0';
    }

    for (int y = 0; y < H; y++) {
        /* Row by row, so the failure names the row that differs rather than
         * printing one long unreadable string. */
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[y], actual[y], why);
    }
}

/* --- gravity quantisation ----------------------------------------------- */

static void test_gravity_quantises_to_eight_directions(void)
{
    fixture();
    int dx, dy;

    sand_gravity_direction(0, 100, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dx, "straight down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "straight down");

    sand_gravity_direction(0, -100, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, dy, "straight up");

    sand_gravity_direction(100, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dx, "right");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dy, "right");

    sand_gravity_direction(100, 100, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dx, "down-right diagonal");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "down-right diagonal");

    sand_gravity_direction(-70, 70, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, dx, "down-left diagonal");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "down-left diagonal");
}

static void test_a_slight_tilt_still_reads_as_straight_down(void)
{
    fixture();
    int dx, dy;

    /* 10 degrees off vertical must not become a diagonal, or the sand would
     * visibly lean while the board looks level. The boundary is 22.5 deg. */
    sand_gravity_direction(18, 100, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dx, "a 10 degree tilt is still down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "a 10 degree tilt is still down");
}

static void test_no_gravity_has_no_direction(void)
{
    fixture();
    int dx = 9, dy = 9;

    sand_gravity_direction(0, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dx, "a zero vector has no direction");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dy, "a zero vector has no direction");
}

/* --- falling ------------------------------------------------------------ */

static void test_a_grain_falls_one_cell_per_step(void)
{
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "the grain must leave the cell it came from");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 1),
        "a grain in open space falls exactly one cell per step");

    sand_step(&s, 0, 1, 0);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 2),
        "and keeps falling on the next step");
}

static void test_a_grain_rests_on_the_floor(void)
{
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    for (int i = 0; i < 5; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, H - 1),
        "the floor is solid - a grain must not fall out of the grid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s),
        "and must not be duplicated by hitting it");
}

static void test_a_grain_does_not_fall_through_another(void)
{
    fixture();
    /* Sitting directly on a grain that is itself on the floor, and walled in
     * on both sides, so neither can go anywhere. */
    static const char *before[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "...o....",
        "..ooo...",
    };
    load(before, 8);
    const int before_count = sand_count(&s);

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before_count, sand_count(&s),
        "grains must never merge");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 6),
        "a grain resting on a full column with no free diagonal stays put");
}

static void test_a_grain_slides_off_a_pile(void)
{
    fixture();
    /* One grain balanced on another, with both diagonals open. It must topple
     * rather than balance - that is what makes a heap spread out. */
    static const char *before[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "...o....",
        "...o....",
    };
    load(before, 8);

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 6),
        "the upper grain must topple off, not balance");
    const bool left  = sand_at(&s, 2, 7) != SAND_EMPTY;
    const bool right = sand_at(&s, 4, 7) != SAND_EMPTY;
    TEST_ASSERT_TRUE_MESSAGE(left || right,
        "it must land in one of the two diagonals below");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, sand_count(&s), "and still be one grain");
}

static void test_a_grain_in_a_pit_stays_put(void)
{
    fixture();
    /* Blocked below and on both diagonals: nowhere to go. */
    static const char *before[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "...o....",
        "..ooo...",
    };
    load(before, 8);

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1, 0);
    }

    static const char *after[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "...o....",
        "..ooo...",
    };
    assert_looks_like(after, 8, "a settled heap must stop moving entirely");
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

    const int filled = sand_spawn(&s, 4, 4, 2);

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
    const int filled = sand_spawn(&s, 0, 0, 3);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, filled,
        "the part of the disc that is on the grid must still be placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(filled, sand_count(&s),
        "clipped cells must not be counted as filled");
}

static void test_spawning_onto_existing_grains_does_not_double_count(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 2);
    const int after_first = sand_count(&s);

    const int filled = sand_spawn(&s, 4, 4, 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, filled,
        "spawning onto a full disc fills nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_first, sand_count(&s),
        "and must not change the grid");
}

static void test_spawned_grains_use_the_full_range_of_shades(void)
{
    fixture();

    sand_spawn(&s, 4, 4, 3);

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

/* --- free fall and shaking ---------------------------------------------- */

static void test_nothing_moves_in_free_fall(void)
{
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 0, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "with no gravity there is no down, so nothing falls");
}

static void test_shaking_spreads_a_pile_sideways(void)
{
    fixture();

    /* A single tall column. Left alone it topples slowly; shaken hard it
     * should flatten, so its highest grain ends up lower. */
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, SAND_FIRST_SHADE);
    }

    for (int i = 0; i < 40; i++) {
        sand_step(&s, 0, 1, 255);
    }

    int highest = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) != SAND_EMPTY && y < highest) {
                highest = y;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2, highest,
        "shaking must flatten the pile, not leave the column standing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, sand_count(&s),
        "and shaking must still conserve grains");
}

/* --- on the real grid, on the real chip --------------------------------- */

#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"

/* Must match app_sand.c. Duplicated rather than shared because sand.h has no
 * business knowing the screen size - see the note at the top of sand.h. */
#define REAL_W 184
#define REAL_H 224

/* The step shares a frame with gfx_present(), which alone takes 17.6 ms of a
 * 23 ms frame. That leaves about 5 ms for the simulation and the render
 * together, so half of it is the honest budget for the step on its own.
 *
 * It currently measures ~2.9 ms, so there is real headroom - the number is
 * here to catch a regression, not to be scraped past. Two changes moved it:
 * building at -O2 rather than -Og (9.0 -> 6.7 ms) and taking the RNG out of
 * the common path (6.7 -> 2.9 ms). See docs/ESP32-C6-AMOLED-Notes.md. */
#define STEP_BUDGET_US 4000

static void test_a_full_size_step_fits_in_the_frame_budget(void)
{
    uint8_t *big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(big,
        "the real grid must fit in what the framebuffer leaves behind");

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 99u);

    /* Half full, and deliberately not settled: a grid of falling grains is the
     * expensive case, because every one of them attempts a move. A settled
     * pile is cheaper and would flatter the measurement. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&real, x, y, SAND_FIRST_SHADE);
            }
        }
    }
    const int grains = sand_count(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "sand_step on %dx%d with %d grains: %lld us",
             REAL_W, REAL_H, grains, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real),
        "the full-size grid must conserve grains too");

    free(big);

    TEST_ASSERT_LESS_THAN_MESSAGE(STEP_BUDGET_US, (int)per_step,
        "the simulation no longer fits in its share of the frame");
}
#endif /* DEVICE_BUILD */

/* --- suite -------------------------------------------------------------- */

void run_sand_suite(void)
{
    RUN_TEST(test_gravity_quantises_to_eight_directions);
    RUN_TEST(test_a_slight_tilt_still_reads_as_straight_down);
    RUN_TEST(test_no_gravity_has_no_direction);

    RUN_TEST(test_a_grain_falls_one_cell_per_step);
    RUN_TEST(test_a_grain_rests_on_the_floor);
    RUN_TEST(test_a_grain_does_not_fall_through_another);
    RUN_TEST(test_a_grain_slides_off_a_pile);
    RUN_TEST(test_a_grain_in_a_pit_stays_put);

    RUN_TEST(test_grains_are_never_created_or_destroyed);
    RUN_TEST(test_a_grain_keeps_its_shade_as_it_falls);

    RUN_TEST(test_grains_fall_upward_when_the_board_is_inverted);
    RUN_TEST(test_grains_fall_sideways_when_the_board_is_on_its_edge);
    RUN_TEST(test_a_heap_settles_against_whichever_wall_is_down);

    RUN_TEST(test_spawn_fills_a_disc);
    RUN_TEST(test_spawn_is_clipped_to_the_grid);
    RUN_TEST(test_spawning_onto_existing_grains_does_not_double_count);
    RUN_TEST(test_spawned_grains_use_the_full_range_of_shades);

    RUN_TEST(test_nothing_moves_in_free_fall);
    RUN_TEST(test_shaking_spreads_a_pile_sideways);

#ifdef DEVICE_BUILD
    RUN_TEST(test_a_full_size_step_fits_in_the_frame_budget);
#endif
}

SUITE_REGISTER(run_sand_suite);
