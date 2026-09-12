/*
 * Portable suite: the falling-sand automaton - gravity, dithering, falling,
 * dirty-row tracking, friction, and sleeping.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 */
#include <math.h> /* not every file in the split still needs atan2()/M_PI,
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

#include "suites.h"
#include "unity.h"

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "util/intmath.h"

static void
assert_looks_like(const char* rows[], int count, const char* why) {
    char expected[H][W + 1];
    char actual[H][W + 1];

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            expected[y][x] = (y < count && rows[y][x] == 'o') ? 'o' : '.';
            actual[y][x] = sand_at(&s, x, y) == SAND_EMPTY ? '.' : 'o';
        }
        expected[y][W] = '\0';
        actual[y][W] = '\0';
    }

    for (int y = 0; y < H; y++) {
        /* Row by row, so the failure names the row that differs rather than
         * printing one long unreadable string. */
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[y], actual[y], why);
    }
}

/* --- gravity quantisation ----------------------------------------------- */

static void
test_gravity_quantises_to_eight_directions(void) {
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

static void
test_a_slight_tilt_still_reads_as_straight_down(void) {
    fixture();
    int dx, dy;

    /* 10 degrees off vertical must not become a diagonal, or the sand would
     * visibly lean while the board looks level. The boundary is 22.5 deg. */
    sand_gravity_direction(18, 100, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dx, "a 10 degree tilt is still down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dy, "a 10 degree tilt is still down");
}

static void
test_no_gravity_has_no_direction(void) {
    fixture();
    int dx = 9, dy = 9;

    sand_gravity_direction(0, 0, &dx, &dy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dx, "a zero vector has no direction");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dy, "a zero vector has no direction");
}

/* --- dithering the direction -------------------------------------------- */

/* Runs `trials` steps at one gravity angle and reports how many of them chose
 * the diagonal. */
static int
diagonal_share(int gx, int gy, int trials) {
    fixture();
    int diagonals = 0;
    for (int i = 0; i < trials; i++) {
        int dx, dy;
        sand_gravity_direction_dithered(&s, gx, gy, &dx, &dy);
        if (dx != 0 && dy != 0) {
            diagonals++;
        }
    }
    return diagonals;
}

static void
test_an_exactly_aligned_direction_is_never_dithered(void) {
    /* The whole simulation would become non-deterministic if it were. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diagonal_share(0, 1000, 500), "straight down must always be straight down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diagonal_share(1000, 0, 500), "straight right must always be straight right");
    TEST_ASSERT_EQUAL_INT_MESSAGE(500, diagonal_share(1000, 1000, 500),
                                  "an exact 45 degrees must always be the diagonal");
}

static void
test_an_intermediate_angle_uses_both_neighbours(void) {
    /* This is the point of the whole exercise: 22.5 degrees is not down and is
     * not down-right, so it must be some of each. */
    const int share = diagonal_share(414, 1000, 1000);

    TEST_ASSERT_GREATER_THAN_MESSAGE(200, share,
                                     "a half-way tilt must use the diagonal a substantial fraction of the "
                                     "time, or it has snapped to the nearest of eight");
    TEST_ASSERT_LESS_THAN_MESSAGE(800, share, "and must still fall straight down a substantial fraction of the time");
}

static void
test_the_average_direction_tracks_the_true_angle(void) {
    /* The dithering is only worth anything if the proportion is right, not
     * merely non-zero. tan(22.5 deg) = 0.414, so this input is 22.5 degrees off
     * vertical - exactly half way to the diagonal, so about half the steps. */
    const int share = diagonal_share(414, 1000, 2000);

    TEST_ASSERT_INT_WITHIN_MESSAGE(160, 1000, share, "22.5 degrees must come out near a 50/50 mix");

    /* And a slight tilt must stay mostly vertical, or the sand leans when the
     * board looks level. tan(10 deg) = 0.176 -> about 22% diagonal. */
    const int slight = diagonal_share(176, 1000, 2000);
    TEST_ASSERT_INT_WITHIN_MESSAGE(160, 440, slight, "a 10 degree tilt must be mostly straight down");

    TEST_ASSERT_GREATER_THAN_MESSAGE(slight, share,
                                     "a larger tilt must use the diagonal more often than a smaller one");
}

static void
test_dithering_still_conserves_grains(void) {
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    const int expected = sand_count(&s);

    /* An awkward angle, so the direction changes from step to step. */
    for (int i = 0; i < 100; i++) {
        sand_step(&s, 300, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                                      "a dithered direction must not break the sweep-order guarantee");
    }
}

/* --- falling ------------------------------------------------------------ */

static void
test_a_grain_falls_one_cell_per_step(void) {
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0), "the grain must leave the cell it came from");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 1),
                                  "a grain in open space falls exactly one cell per step");

    sand_step(&s, 0, 1, 0);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 2), "and keeps falling on the next step");
}

static void
test_a_grain_rests_on_the_floor(void) {
    fixture();
    sand_set(&s, 3, H - 1, SAND_FIRST_SHADE);

    for (int i = 0; i < 5; i++) {
        sand_step(&s, 0, 1, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, H - 1),
                                  "the floor is solid - a grain must not fall out of the grid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s), "and must not be duplicated by hitting it");
}

static void
test_a_grain_does_not_fall_through_another(void) {
    fixture();
    /* Sitting directly on a grain that is itself on the floor, and walled in
     * on both sides, so neither can go anywhere. */
    static const char* before[] = {
        "........", "........", "........", "........", "........", "........", "...o....", "..ooo...",
    };
    load(before, 8);
    const int before_count = sand_count(&s);

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before_count, sand_count(&s), "grains must never merge");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 6),
                                  "a grain resting on a full column with no free diagonal stays put");
}

static void
test_a_grain_slides_off_a_pile(void) {
    fixture();
    /* One grain balanced on another, with both diagonals open. It must topple
     * rather than balance - that is what makes a heap spread out. */
    static const char* before[] = {
        "........", "........", "........", "........", "........", "........", "...o....", "...o....",
    };
    load(before, 8);

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 6), "the upper grain must topple off, not balance");
    const bool left = sand_at(&s, 2, 7) != SAND_EMPTY;
    const bool right = sand_at(&s, 4, 7) != SAND_EMPTY;
    TEST_ASSERT_TRUE_MESSAGE(left || right, "it must land in one of the two diagonals below");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, sand_count(&s), "and still be one grain");
}

static void
test_a_grain_in_a_pit_stays_put(void) {
    fixture();
    /* Blocked below and on both diagonals: nowhere to go. */
    static const char* before[] = {
        "........", "........", "........", "........", "........", "........", "...o....", "..ooo...",
    };
    load(before, 8);

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1, 0);
    }

    static const char* after[] = {
        "........", "........", "........", "........", "........", "........", "...o....", "..ooo...",
    };
    assert_looks_like(after, 8, "a settled heap must stop moving entirely");
}

/* --- dirty row tracking -------------------------------------------------- */

/* Everything here guards the same property: a row reported CLEAN must be
 * genuinely unchanged. Getting that wrong does not crash - it leaves stale
 * pixels on the panel, which is a maddening bug to chase from a photograph.
 *
 * dirty[] and dirty_fixture() live in suite_sand_common.{c,h} - reused far
 * past this section, by tests throughout the split. */

static void
test_a_settled_grid_reports_nothing_dirty(void) {
    dirty_fixture();
    /* A grain in the corner, blocked on every side it could reach. */
    sand_set(&s, 0, H - 1, SAND_FIRST_SHADE);
    memset(dirty, 0, sizeof(dirty));

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1, 0);
    }

    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[y],
                                        "nothing moved, so nothing may be reported as needing a redraw - "
                                        "otherwise a still screen costs as much as a busy one");
    }
}

static void
test_a_falling_grain_marks_both_rows_it_touched(void) {
    dirty_fixture();
    sand_set(&s, 3, 2, SAND_FIRST_SHADE);
    memset(dirty, 0, sizeof(dirty));

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[2], "the row the grain left changed and must be redrawn");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[3], "so did the row it arrived in");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0], "a row nowhere near it did not");
}

static void
test_every_changed_row_is_reported(void) {
    /* The exhaustive version, and the one that actually protects against
     * stale pixels: run a busy grid, then check the report against a full
     * before-and-after comparison. */
    dirty_fixture();
    for (int y = 1; y < 5; y++) {
        for (int x = 1; x < 7; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    for (int i = 0; i < 90; i++) {
        uint8_t before[W * H];
        memcpy(before, cells, sizeof(before));
        memset(dirty, 0, sizeof(dirty));

        sand_step(&s, 300, 1000, 40);

        for (int y = 0; y < H; y++) {
            const bool changed = memcmp(&before[y * W], &cells[y * W], W) != 0;
            if (changed) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[y],
                                                "a row whose contents changed was reported clean, which "
                                                "would leave stale pixels on the panel");
            }
        }
    }
}

static void
test_spawning_marks_the_rows_it_filled(void) {
    dirty_fixture();

    sand_spawn(&s, 4, 4, 1, MAT_SAND);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4], "the centre row was filled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0], "a distant row was not");
}

static void
test_tracking_starts_by_assuming_everything_changed(void) {
    fixture();
    memset(dirty, 0, sizeof(dirty));

    sand_track_dirty_rows(&s, dirty);

    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[y],
                                        "nothing is known about what is already on screen, so the first "
                                        "frame must redraw all of it");
    }
}

/* --- friction ------------------------------------------------------------ */

/* The behaviour these exist for: a floor of sand that skated sideways on the
 * faintest tilt, because nothing in the rules knew the bottom layer was
 * carrying everything above it. */

static void
test_load_counts_the_grains_stacked_above(void) {
    fixture();
    for (int y = 3; y < H; y++) {
        sand_set(&s, 2, y, SAND_FIRST_SHADE);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(4, sand_load_above(&s, 2, H - 1, 0, 1),
                                  "the bottom of a five-grain column carries the other four");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 2, 3, 0, 1), "the top of it carries nothing");
}

static void
test_load_stops_at_a_gap(void) {
    fixture();
    sand_set(&s, 2, 7, SAND_FIRST_SHADE);
    sand_set(&s, 2, 6, SAND_FIRST_SHADE);
    /* row 5 left empty */
    sand_set(&s, 2, 4, SAND_FIRST_SHADE);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_load_above(&s, 2, 7, 0, 1),
                                  "weight is carried through contact - a grain across a gap rests on "
                                  "something else, not on this one");
}

static void
test_open_sky_is_not_load(void) {
    fixture();
    sand_set(&s, 2, 0, SAND_FIRST_SHADE);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 2, 0, 0, 1),
                                  "a grain against the top edge carries nothing - off the grid is open "
                                  "sky, even though sand_at reports it as solid so the walls work");
}

static void
test_load_is_measured_against_gravity(void) {
    fixture();
    for (int x = 0; x < 4; x++) {
        sand_set(&s, x, 3, SAND_FIRST_SHADE);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, sand_load_above(&s, 3, 3, 1, 0),
                                  "with gravity to the right, the grains to the LEFT are the ones "
                                  "bearing down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 3, 3, 0, 1), "and none of them are above it");
}

static void
test_a_buried_grain_will_not_slide(void) {
    /* One step at a time from a fresh grid, so the grain under test is always
     * fully buried when it is considered - the sweep reaches the bottom row
     * first, before anything above it has had a chance to erode.
     *
     * Repeated rather than run once because the decision is probabilistic:
     * a single pass could pass by luck. */
    static const char* bed[] = {
        "........", "........", "..o.....", "..o.....", "..o.....", "..o.....", "..o.....", "..o.....",
    };

    for (int trial = 0; trial < 200; trial++) {
        /* A DIFFERENT seed each trial. The dithered direction only lands on
         * the diagonal - the case that used to let the base walk sideways -
         * some steps, so 200 identically-seeded trials would all replay the
         * same one and prove nothing. */
        sand_init(&s, cells, W, H, 7000u + (uint32_t)trial);
        load(bed, 8);
        sand_step(&s, 300, 1000, 0);

        TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 2, 7),
                                      "the bottom of a loaded column must stay put - it is holding the "
                                      "rest of the column up, and a floor that slides on the faintest "
                                      "tilt is what a frictionless model looks like");
    }
}

static void
test_a_surface_grain_still_slides(void) {
    fixture();
    /* Same column, but this looks at the TOP grain, which carries nothing.
     * Friction must not turn the pile into a solid block. */
    static const char* before[] = {
        "........", "........", "........", "..o.....", "..o.....", "..o.....", "..o.....", "..o.....",
    };
    load(before, 8);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 300, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 2, 3),
                                    "an unloaded grain on top of a column must still topple off, or a "
                                    "pile can never reach its angle of repose");
}

static void
test_friction_never_stops_a_grain_falling(void) {
    fixture();
    /* Buried under four grains, but with nothing underneath. Whatever is on
     * top of it, an unsupported grain falls - that is what unsupported means. */
    for (int y = 0; y < 5; y++) {
        sand_set(&s, 3, y, SAND_FIRST_SHADE);
    }

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 5), "friction applies to sliding, never to falling");
}

static void
test_shaking_overcomes_friction(void) {
    fixture();
    static const char* before[] = {
        "........", "........", "........", "..o.....", "..o.....", "..o.....", "..o.....", "..o.....",
    };
    load(before, 8);
    const int before_count = sand_count(&s);

    /* Shaken hard, the same column that stays put above must spread. Shaking a
     * jar of sand levels it; that is friction being overcome, not absent. */
    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1, 255);
    }

    int bottom_row = 0;
    for (int x = 0; x < W; x++) {
        if (sand_at(&s, x, H - 1) != SAND_EMPTY) {
            bottom_row++;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, bottom_row, "shaking must fluidise the pile and spread it along the floor");
    TEST_ASSERT_EQUAL_INT_MESSAGE(before_count, sand_count(&s), "and must still conserve grains");
}

/* Where the sand is, left to right, summed - a single number that moves if the
 * bed migrates even by one grain. */
static long
centre_of_mass_x(void) {
    long total = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) != SAND_EMPTY) {
                total += x;
            }
        }
    }
    return total;
}

static void
test_a_flat_bed_does_not_slide_on_a_slight_tilt(void) {
    /* THE reported behaviour: a floor of sand skating sideways on gyro input.
     *
     * Deliberately ONE grain deep, so there is no load anywhere and burial
     * cannot be what holds it. Only the angle of repose can - a bed on a floor
     * tilted well below the friction angle must not move at all.
     *
     * Room on both sides to slide into, so staying put is a real result. */
    for (int trial = 0; trial < 20; trial++) {
        sand_init(&s, cells, W, H, 31u + (uint32_t)trial);
        for (int x = 2; x < 6; x++) {
            sand_set(&s, x, H - 1, SAND_FIRST_SHADE);
        }
        const long before = centre_of_mass_x();

        /* About 14 degrees - a tilt you would not expect to pour sand. */
        for (int i = 0; i < 200; i++) {
            sand_step(&s, 250, 1000, 0);
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE((int)before, (int)centre_of_mass_x(),
                                      "a flat bed must not migrate at a tilt below the angle of repose "
                                      "- sand tips, it does not get dragged");
    }
}

/* A bigger grid, because depth is the whole variable here and eight rows is
 * not enough to have a deep bed and somewhere for it to go. */
#define BIG_W 16
#define BIG_H 12

/* Pours a bed `rows` deep down a steep tilt and reports how far its trailing
 * edge ends up. */
static int
settled_base_x(int rows, uint32_t seed) {
    uint8_t* big_cells = malloc((size_t)BIG_W * BIG_H);
    sand_t* big = malloc(sizeof *big);
    if (!big_cells || !big) {
        free(big_cells);
        free(big);
        TEST_ASSERT_TRUE_MESSAGE(false, "the deep-bed grid must fit in what the framebuffer leaves");
    }

    sand_init(big, big_cells, BIG_W, BIG_H, seed);
    for (int y = BIG_H - rows; y < BIG_H; y++) {
        for (int x = 2; x < 8; x++) {
            sand_set(big, x, y, SAND_FIRST_SHADE);
        }
    }
    for (int i = 0; i < 120; i++) {
        sand_step(big, 1200, 1000, 0); /* well past the angle of repose */
    }
    int base = BIG_W;
    for (int x = 0; x < BIG_W; x++) {
        if (sand_at(big, x, BIG_H - 1) != SAND_EMPTY) {
            base = x;
            break;
        }
    }

    free(big);
    free(big_cells);
    return base;
}

static void
test_a_deep_bed_is_harder_to_move_than_a_thin_one(void) {
    /* Burial, as distinct from the angle of repose. Past the friction angle
     * both beds pour - but the deep one is carrying its own weight, so its
     * base should be left behind while the thin one slides away entirely.
     *
     * Averaged over several seeds: the decision is probabilistic, and a single
     * run of either could land anywhere. */
    const int trials = 12;
    long deep = 0;
    long thin = 0;

    for (int t = 0; t < trials; t++) {
        deep += settled_base_x(4, 101u + (uint32_t)t);
        thin += settled_base_x(1, 101u + (uint32_t)t);
    }

    const int deep_base = (int)(deep / trials);
    const int thin_base = (int)(thin / trials);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, thin_base - deep_base,
                                             "a bed four deep must leave its base far further back than a bed one "
                                             "deep - without load-based friction both slide away together, which "
                                             "is the difference between sand and a sheet of it");
}

static void
test_a_steep_tilt_does_pour_the_bed(void) {
    /* The other side of the same rule: past the friction angle it MUST move,
     * or the sand is glued down rather than resting. About 50 degrees. */
    fixture();
    for (int x = 2; x < 6; x++) {
        sand_set(&s, x, H - 1, SAND_FIRST_SHADE);
    }
    const long before = centre_of_mass_x();

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 1200, 1000, 0);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE((int)before, (int)centre_of_mass_x(),
                                     "past the angle of repose the bed must pour downhill");
}

/* --- sleeping ------------------------------------------------------------ */

/* A row wrongly left asleep leaves a grain hanging that should have fallen -
 * no crash, no corruption, it just quietly stops being sand. So the central
 * test checks not which rows were skipped but that nothing was left able to
 * move: settle with sleeping on, run again with sleeping OFF, and require
 * that nothing at all happens. */

/* BLOCK_COLS/BLOCK_ROWS/sleep_blocks/settle_with_sleeping()/
 * assert_nothing_left_to_do() live in suite_sand_common.{c,h} - reused far
 * past this section. */

static void
test_sleeping_leaves_nothing_able_to_move(void) {
    static const char* heap[] = {
        "...oo...", "...oo...", "...oo...", "...oo...", "........", "........", "........", "........",
    };
    settle_with_sleeping(heap, 8, 200, 0, 1000);
    assert_nothing_left_to_do(0, 1000);
}

static void
test_sleeping_leaves_nothing_able_to_move_on_a_slope(void) {
    /* A tilt exercises the diagonal moves and the dithered direction, which
     * is where a sweep-order or wake-up mistake would show. */
    static const char* heap[] = {
        "..oooo..", "..oooo..", "..oooo..", "........", "........", "........", "........", "........",
    };
    settle_with_sleeping(heap, 8, 300, 1200, 1000);
    assert_nothing_left_to_do(1200, 1000);
}

static void
test_sand_poured_onto_a_sleeping_pile_still_falls(void) {
    static const char* bed[] = {
        "........", "........", "........", "........", "........", "........", "........", "oooooooo",
    };
    settle_with_sleeping(bed, 8, 50, 0, 1000);

    /* The bed is now asleep. Drop a grain far above it. */
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 6),
                                  "a grain dropped onto a sleeping pile must fall and land on it - if "
                                  "adding sand does not wake the rows it passes through, it hangs");
}

static void
test_undermining_a_sleeping_pile_collapses_it(void) {
    /* A bed spanning the full width, so the walls hold it and it is genuinely
     * settled. A bare column is not: its sides are open, so grains slide off
     * it as it stands there, and what the test found underneath depended on
     * the exact random sequence rather than on the rule being tested. */
    static const char* bed[] = {
        "........", "........", "........", "........", "........", "oooooooo", "oooooooo", "oooooooo",
    };
    settle_with_sleeping(bed, 8, 100, 0, 1000);

    /* Pull one grain out from underneath. What was resting on it must come
     * down, which means the erase has to have woken the rows around it. */
    sand_erase(&s, 3, 7, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 7), "the hole must actually have been made");

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 7),
                                  "removing a grain must wake what was resting on it, or the pile hangs "
                                  "in the air above a hole");
}

static void
test_turning_the_board_wakes_a_sleeping_pile(void) {
    static const char* bed[] = {
        "........", "........", "........", "........", "........", "........", "oooo....", "oooo....",
    };
    settle_with_sleeping(bed, 8, 100, 0, 1000);

    /* Now put the board on its side. Nothing has moved, so every row is
     * asleep - but every grain can now move, and only the change of direction
     * says so. */
    for (int i = 0; i < 100; i++) {
        sand_step(&s, 1000, 0, 0);
    }

    int at_right_wall = 0;
    for (int y = 0; y < H; y++) {
        if (sand_at(&s, W - 1, y) != SAND_EMPTY) {
            at_right_wall++;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, at_right_wall,
                                     "changing the gravity direction must wake everything - the pile was "
                                     "settled for the old direction, not the new one");
}

void
run_sand_motion_suite(void) {
    RUN_TEST(test_gravity_quantises_to_eight_directions);
    RUN_TEST(test_a_slight_tilt_still_reads_as_straight_down);
    RUN_TEST(test_no_gravity_has_no_direction);
    RUN_TEST(test_an_exactly_aligned_direction_is_never_dithered);
    RUN_TEST(test_an_intermediate_angle_uses_both_neighbours);
    RUN_TEST(test_the_average_direction_tracks_the_true_angle);
    RUN_TEST(test_dithering_still_conserves_grains);
    RUN_TEST(test_a_grain_falls_one_cell_per_step);
    RUN_TEST(test_a_grain_rests_on_the_floor);
    RUN_TEST(test_a_grain_does_not_fall_through_another);
    RUN_TEST(test_a_grain_slides_off_a_pile);
    RUN_TEST(test_a_grain_in_a_pit_stays_put);
    RUN_TEST(test_a_settled_grid_reports_nothing_dirty);
    RUN_TEST(test_a_falling_grain_marks_both_rows_it_touched);
    RUN_TEST(test_every_changed_row_is_reported);
    RUN_TEST(test_spawning_marks_the_rows_it_filled);
    RUN_TEST(test_tracking_starts_by_assuming_everything_changed);
    RUN_TEST(test_load_counts_the_grains_stacked_above);
    RUN_TEST(test_load_stops_at_a_gap);
    RUN_TEST(test_open_sky_is_not_load);
    RUN_TEST(test_load_is_measured_against_gravity);
    RUN_TEST(test_a_buried_grain_will_not_slide);
    RUN_TEST(test_a_surface_grain_still_slides);
    RUN_TEST(test_friction_never_stops_a_grain_falling);
    RUN_TEST(test_shaking_overcomes_friction);
    RUN_TEST(test_a_flat_bed_does_not_slide_on_a_slight_tilt);
    RUN_TEST(test_a_deep_bed_is_harder_to_move_than_a_thin_one);
    RUN_TEST(test_a_steep_tilt_does_pour_the_bed);
    RUN_TEST(test_sleeping_leaves_nothing_able_to_move);
    RUN_TEST(test_sleeping_leaves_nothing_able_to_move_on_a_slope);
    RUN_TEST(test_sand_poured_onto_a_sleeping_pile_still_falls);
    RUN_TEST(test_undermining_a_sleeping_pile_collapses_it);
    RUN_TEST(test_turning_the_board_wakes_a_sleeping_pile);
}

SUITE_REGISTER(run_sand_motion_suite);
