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

#include <stdlib.h>
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

/* --- dithering the direction -------------------------------------------- */

/* Runs `trials` steps at one gravity angle and reports how many of them chose
 * the diagonal. */
static int diagonal_share(int gx, int gy, int trials)
{
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

static void test_an_exactly_aligned_direction_is_never_dithered(void)
{
    /* The whole simulation would become non-deterministic if it were. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diagonal_share(0, 1000, 500),
        "straight down must always be straight down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diagonal_share(1000, 0, 500),
        "straight right must always be straight right");
    TEST_ASSERT_EQUAL_INT_MESSAGE(500, diagonal_share(1000, 1000, 500),
        "an exact 45 degrees must always be the diagonal");
}

static void test_an_intermediate_angle_uses_both_neighbours(void)
{
    /* This is the point of the whole exercise: 22.5 degrees is not down and is
     * not down-right, so it must be some of each. */
    const int share = diagonal_share(414, 1000, 1000);

    TEST_ASSERT_GREATER_THAN_MESSAGE(200, share,
        "a half-way tilt must use the diagonal a substantial fraction of the "
        "time, or it has snapped to the nearest of eight");
    TEST_ASSERT_LESS_THAN_MESSAGE(800, share,
        "and must still fall straight down a substantial fraction of the time");
}

static void test_the_average_direction_tracks_the_true_angle(void)
{
    /* The dithering is only worth anything if the proportion is right, not
     * merely non-zero. tan(22.5 deg) = 0.414, so this input is 22.5 degrees off
     * vertical - exactly half way to the diagonal, so about half the steps. */
    const int share = diagonal_share(414, 1000, 2000);

    TEST_ASSERT_INT_WITHIN_MESSAGE(160, 1000, share,
        "22.5 degrees must come out near a 50/50 mix");

    /* And a slight tilt must stay mostly vertical, or the sand leans when the
     * board looks level. tan(10 deg) = 0.176 -> about 22% diagonal. */
    const int slight = diagonal_share(176, 1000, 2000);
    TEST_ASSERT_INT_WITHIN_MESSAGE(160, 440, slight,
        "a 10 degree tilt must be mostly straight down");

    TEST_ASSERT_GREATER_THAN_MESSAGE(slight, share,
        "a larger tilt must use the diagonal more often than a smaller one");
}

static void test_dithering_still_conserves_grains(void)
{
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

/* --- dirty row tracking -------------------------------------------------- */

/* Everything here guards the same property: a row reported CLEAN must be
 * genuinely unchanged. Getting that wrong does not crash - it leaves stale
 * pixels on the panel, which is a maddening bug to chase from a photograph. */

static uint8_t dirty[H];

static void dirty_fixture(void)
{
    fixture();
    sand_track_dirty_rows(&s, dirty);
    memset(dirty, 0, sizeof(dirty));
}

static void test_a_settled_grid_reports_nothing_dirty(void)
{
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

static void test_a_falling_grain_marks_both_rows_it_touched(void)
{
    dirty_fixture();
    sand_set(&s, 3, 2, SAND_FIRST_SHADE);
    memset(dirty, 0, sizeof(dirty));

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[2],
        "the row the grain left changed and must be redrawn");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[3],
        "so did the row it arrived in");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0],
        "a row nowhere near it did not");
}

static void test_every_changed_row_is_reported(void)
{
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
            const bool changed =
                memcmp(&before[y * W], &cells[y * W], W) != 0;
            if (changed) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[y],
                    "a row whose contents changed was reported clean, which "
                    "would leave stale pixels on the panel");
            }
        }
    }
}

static void test_spawning_marks_the_rows_it_filled(void)
{
    dirty_fixture();

    sand_spawn(&s, 4, 4, 1, MAT_SAND);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4], "the centre row was filled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, dirty[0], "a distant row was not");
}

static void test_tracking_starts_by_assuming_everything_changed(void)
{
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

static void test_load_counts_the_grains_stacked_above(void)
{
    fixture();
    for (int y = 3; y < H; y++) {
        sand_set(&s, 2, y, SAND_FIRST_SHADE);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(4, sand_load_above(&s, 2, H - 1, 0, 1),
        "the bottom of a five-grain column carries the other four");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 2, 3, 0, 1),
        "the top of it carries nothing");
}

static void test_load_stops_at_a_gap(void)
{
    fixture();
    sand_set(&s, 2, 7, SAND_FIRST_SHADE);
    sand_set(&s, 2, 6, SAND_FIRST_SHADE);
    /* row 5 left empty */
    sand_set(&s, 2, 4, SAND_FIRST_SHADE);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_load_above(&s, 2, 7, 0, 1),
        "weight is carried through contact - a grain across a gap rests on "
        "something else, not on this one");
}

static void test_open_sky_is_not_load(void)
{
    fixture();
    sand_set(&s, 2, 0, SAND_FIRST_SHADE);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 2, 0, 0, 1),
        "a grain against the top edge carries nothing - off the grid is open "
        "sky, even though sand_at reports it as solid so the walls work");
}

static void test_load_is_measured_against_gravity(void)
{
    fixture();
    for (int x = 0; x < 4; x++) {
        sand_set(&s, x, 3, SAND_FIRST_SHADE);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, sand_load_above(&s, 3, 3, 1, 0),
        "with gravity to the right, the grains to the LEFT are the ones "
        "bearing down");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_load_above(&s, 3, 3, 0, 1),
        "and none of them are above it");
}

static void test_a_buried_grain_will_not_slide(void)
{
    /* One step at a time from a fresh grid, so the grain under test is always
     * fully buried when it is considered - the sweep reaches the bottom row
     * first, before anything above it has had a chance to erode.
     *
     * Repeated rather than run once because the decision is probabilistic:
     * a single pass could pass by luck. */
    static const char *bed[] = {
        "........",
        "........",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
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

static void test_a_surface_grain_still_slides(void)
{
    fixture();
    /* Same column, but this looks at the TOP grain, which carries nothing.
     * Friction must not turn the pile into a solid block. */
    static const char *before[] = {
        "........",
        "........",
        "........",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
    };
    load(before, 8);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 300, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 2, 3),
        "an unloaded grain on top of a column must still topple off, or a "
        "pile can never reach its angle of repose");
}

static void test_friction_never_stops_a_grain_falling(void)
{
    fixture();
    /* Buried under four grains, but with nothing underneath. Whatever is on
     * top of it, an unsupported grain falls - that is what unsupported means. */
    for (int y = 0; y < 5; y++) {
        sand_set(&s, 3, y, SAND_FIRST_SHADE);
    }

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 5),
        "friction applies to sliding, never to falling");
}

static void test_shaking_overcomes_friction(void)
{
    fixture();
    static const char *before[] = {
        "........",
        "........",
        "........",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
        "..o.....",
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

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, bottom_row,
        "shaking must fluidise the pile and spread it along the floor");
    TEST_ASSERT_EQUAL_INT_MESSAGE(before_count, sand_count(&s),
        "and must still conserve grains");
}

/* Where the sand is, left to right, summed - a single number that moves if the
 * bed migrates even by one grain. */
static long centre_of_mass_x(void)
{
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

static void test_a_flat_bed_does_not_slide_on_a_slight_tilt(void)
{
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
static uint8_t big_cells[BIG_W * BIG_H];
static sand_t  big;

/* Pours a bed `rows` deep down a steep tilt and reports how far its trailing
 * edge ends up. */
static int settled_base_x(int rows, uint32_t seed)
{
    sand_init(&big, big_cells, BIG_W, BIG_H, seed);
    for (int y = BIG_H - rows; y < BIG_H; y++) {
        for (int x = 2; x < 8; x++) {
            sand_set(&big, x, y, SAND_FIRST_SHADE);
        }
    }
    for (int i = 0; i < 120; i++) {
        sand_step(&big, 1200, 1000, 0);   /* well past the angle of repose */
    }
    for (int x = 0; x < BIG_W; x++) {
        if (sand_at(&big, x, BIG_H - 1) != SAND_EMPTY) {
            return x;
        }
    }
    return BIG_W;
}

static void test_a_deep_bed_is_harder_to_move_than_a_thin_one(void)
{
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

static void test_a_steep_tilt_does_pour_the_bed(void)
{
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

/* The hazard is specific: a row wrongly left asleep leaves a grain hanging
 * that should have fallen. It does not crash and it does not corrupt anything,
 * it just quietly stops being sand.
 *
 * So the central test does not check which rows were skipped - it checks the
 * only thing that matters, that nothing was left able to move. Settle the grid
 * with sleeping on, then run it again with sleeping OFF and require that
 * nothing at all happens. If sleeping froze something, the second pass frees
 * it and the grids differ. */

#define BLOCK_COLS ((W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS ((H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
static uint8_t sleep_blocks[BLOCK_COLS * BLOCK_ROWS];

static void settle_with_sleeping(const char *rows[], int count, int steps,
                                 int gx, int gy)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    load(rows, count);

    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }
}

static void assert_nothing_left_to_do(int gx, int gy)
{
    uint8_t settled[W * H];
    memcpy(settled, cells, sizeof(settled));

    /* Same grid, same rules, but every row examined every step. */
    sand_t awake;
    sand_init(&awake, cells, W, H, 999u);
    memcpy(cells, settled, sizeof(settled));

    for (int i = 0; i < 60; i++) {
        sand_step(&awake, gx, gy, 0);
    }

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
        "a fully awake simulation found something to move that the sleeping "
        "one had left alone - which means sleeping froze sand that should "
        "still have been falling");
}

static void test_sleeping_leaves_nothing_able_to_move(void)
{
    static const char *heap[] = {
        "...oo...",
        "...oo...",
        "...oo...",
        "...oo...",
        "........",
        "........",
        "........",
        "........",
    };
    settle_with_sleeping(heap, 8, 200, 0, 1000);
    assert_nothing_left_to_do(0, 1000);
}

static void test_sleeping_leaves_nothing_able_to_move_on_a_slope(void)
{
    /* A tilt exercises the diagonal moves and the dithered direction, which
     * is where a sweep-order or wake-up mistake would show. */
    static const char *heap[] = {
        "..oooo..",
        "..oooo..",
        "..oooo..",
        "........",
        "........",
        "........",
        "........",
        "........",
    };
    settle_with_sleeping(heap, 8, 300, 1200, 1000);
    assert_nothing_left_to_do(1200, 1000);
}

static void test_sand_poured_onto_a_sleeping_pile_still_falls(void)
{
    static const char *bed[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "oooooooo",
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

static void test_undermining_a_sleeping_pile_collapses_it(void)
{
    /* A bed spanning the full width, so the walls hold it and it is genuinely
     * settled. A bare column is not: its sides are open, so grains slide off
     * it as it stands there, and what the test found underneath depended on
     * the exact random sequence rather than on the rule being tested. */
    static const char *bed[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "oooooooo",
        "oooooooo",
        "oooooooo",
    };
    settle_with_sleeping(bed, 8, 100, 0, 1000);

    /* Pull one grain out from underneath. What was resting on it must come
     * down, which means the erase has to have woken the rows around it. */
    sand_erase(&s, 3, 7, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 7),
        "the hole must actually have been made");

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 7),
        "removing a grain must wake what was resting on it, or the pile hangs "
        "in the air above a hole");
}

static void test_turning_the_board_wakes_a_sleeping_pile(void)
{
    static const char *bed[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "oooo....",
        "oooo....",
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

/* --- 2D block locality -----------------------------------------------------
 *
 * Sleeping used to be row-shaped: one settled bit per whole row, so a
 * resting region only stayed asleep if the ENTIRE row it sat in was quiet.
 * These tests exercise what that shape could not: genuine locality in both
 * directions, not just in y, and regardless of which way gravity points -
 * the specific case measured on real hardware (a pour keeping a whole row
 * awake, even where most of it was long settled) and the specific reason a
 * row-shaped scheme cannot fix it even in principle (wake propagation only
 * ever reached vertically, which stops meaning much once gravity tilts
 * towards horizontal). A grid of its own: 3x3 SAND_BLOCK_W x SAND_BLOCK_H
 * blocks, enough room for "far apart" to mean something.
 *
 * Capped rather than a bare SAND_BLOCK_W/H*3, and malloc'd per test rather
 * than `static`: this file is compiled into the device build too, where a
 * `static` array is permanent BSS for the whole boot, not just while a
 * test is running. A block-size tuning experiment (SAND_BLOCK_H=64) once
 * grew a `static loc_cells` from 2304 to 9216 bytes that way, and the
 * already-tight device heap (the framebuffer alone claims 322 of ~424 KiB)
 * could not spare it - not just failing a test, but leaving less heap for
 * the REST of that same boot, including the real sand app a user might
 * open afterwards, long after the self-test suite had finished with it.
 * Freed at the end of every test that mallocs it, same as the existing
 * frame-budget tests below already do - the cap on top is extra headroom
 * for block sizes well past anything sane to ship, not the primary fix.
 *
 * The cap can't be a flat 128 independent of the block size, though:
 * test_a_block_wakes_when_disturbed_diagonally() plants its grain at the
 * last row of block-row 0 and boxes it in two rows further down (gy + 2,
 * gy = SAND_BLOCK_H - 1), which needs LOC_H >= SAND_BLOCK_H + 2 to even
 * exist on the grid. A flat 128 cap silently clipped that room away once
 * SAND_BLOCK_H passed ~42 (128/3), and sand_set() on an out-of-range cell
 * is a silent no-op (see sand.c), so the containing stones never landed
 * and the test failed on an assertion that had nothing to do with the
 * simulation. Same reasoning applies to LOC_W for the gx + 1 stone in that
 * same test. So the cap floats: never below what that test's geometry
 * needs, 128 otherwise. */
#define LOC_W_CAP (((SAND_BLOCK_W + 2) > 128) ? (SAND_BLOCK_W + 2) : 128)
#define LOC_H_CAP (((SAND_BLOCK_H + 2) > 128) ? (SAND_BLOCK_H + 2) : 128)
#define LOC_W (((SAND_BLOCK_W * 3) < LOC_W_CAP) ? (SAND_BLOCK_W * 3) : LOC_W_CAP)
#define LOC_H (((SAND_BLOCK_H * 3) < LOC_H_CAP) ? (SAND_BLOCK_H * 3) : LOC_H_CAP)
#define LOC_BLOCK_COLS ((LOC_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define LOC_BLOCK_ROWS ((LOC_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
static uint8_t *loc_cells;
static uint8_t *loc_sleep_blocks;
static sand_t   loc;

/* Mallocs the three buffers above, fresh, every call - loc_free() below
 * must be called before the test returns, or the next call here leaks the
 * previous allocation. */
static void loc_fixture(void)
{
    loc_cells        = malloc((size_t)LOC_W * LOC_H);
    loc_sleep_blocks = malloc((size_t)LOC_BLOCK_COLS * LOC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(loc_cells);
    TEST_ASSERT_NOT_NULL(loc_sleep_blocks);

    sand_init(&loc, loc_cells, LOC_W, LOC_H, 555u);
    sand_enable_sleeping(&loc, loc_sleep_blocks);
}

static void loc_free(void)
{
    free(loc_cells);
    free(loc_sleep_blocks);
}

static void test_two_separate_active_spots_in_the_same_block_row_do_not_wake_each_other(void)
{
    loc_fixture();

    /* A small heap, settled on the floor in the leftmost block-column. */
    for (int x = 0; x < SAND_BLOCK_W; x++) {
        sand_set(&loc, x, LOC_H - 1, SAND_FIRST_SHADE);
    }
    for (int i = 0; i < 100; i++) {
        sand_step(&loc, 0, 1000, 0);
    }

    /* Heap, not a stack array: at the shipped SAND_BLOCK_W (16) this was a
     * harmless 2 KB, but it scales with the tunable (see loc_fixture()'s
     * own comment on SAND_BLOCK_W being worth retuning) and the device's
     * main task stack is only 3.5 KB total (CONFIG_ESP_MAIN_TASK_STACK_SIZE)
     * - a wider block size alone was enough to blow it, with a real
     * stack-protection panic on device that a host run cannot reproduce
     * (the host stack is megabytes). Found via exactly that: a block-size
     * tuning sweep this session hit it at SAND_BLOCK_W=32. */
    uint8_t *left_before = malloc((size_t)SAND_BLOCK_W * LOC_H);
    TEST_ASSERT_NOT_NULL(left_before);
    for (int y = 0; y < LOC_H; y++) {
        for (int x = 0; x < SAND_BLOCK_W; x++) {
            left_before[y * SAND_BLOCK_W + x] = sand_at(&loc, x, y);
        }
    }

    /* A stream, falling in the rightmost block-column - two block-columns
     * away, but landing in the SAME row the heap rests in. A row-shaped
     * scheme could not tell these apart: the whole row would be forced
     * awake by the stream, heap included. LOC_H steps is enough for a
     * fresh grain to fall the full height of the grid, whatever
     * SAND_BLOCK_H currently is. */
    for (int i = 0; i < LOC_H; i++) {
        sand_set(&loc, LOC_W - 1, 0, SAND_FIRST_SHADE);
        sand_step(&loc, 0, 1000, 0);
    }

    bool left_unchanged = true;
    for (int y = 0; y < LOC_H && left_unchanged; y++) {
        for (int x = 0; x < SAND_BLOCK_W; x++) {
            if (sand_at(&loc, x, y) != left_before[y * SAND_BLOCK_W + x]) {
                left_unchanged = false;
                break;
            }
        }
    }
    const cell_t stream_landed = sand_at(&loc, LOC_W - 1, LOC_H - 1);

    free(left_before);
    loc_free();
    TEST_ASSERT_TRUE_MESSAGE(left_unchanged,
        "a settled heap must stay undisturbed while an unrelated stream "
        "falls far away in the same row band - block-shaped sleeping must "
        "keep the two apart, which is exactly what a row-shaped scheme "
        "could not do");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, stream_landed,
        "the stream itself must actually have landed, or this test would "
        "pass for the wrong reason - nothing moving at all");
}

static void test_a_block_wakes_when_disturbed_diagonally(void)
{
    loc_fixture();

    /* A grain at the last row of block-row 0, first column of
     * block-column 1 - a true block corner regardless of SAND_BLOCK_W/H -
     * fully boxed in on its only three legal moves: straight down and
     * both diagonals. */
    const int gx = SAND_BLOCK_W;
    const int gy = SAND_BLOCK_H - 1;
    sand_set(&loc, gx, gy, SAND_FIRST_SHADE);
    sand_set(&loc, gx,     gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks the fall */
    sand_set(&loc, gx + 1, gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks down-right */
    sand_set(&loc, gx - 1, gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks down-left */
    /* Once the down-left slide is freed and the grain lands there, it
     * must stop - otherwise it keeps sliding on its own three legal moves
     * from its new position, and the test would be checking the wrong
     * cell. */
    sand_set(&loc, gx - 1, gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, gx - 2, gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, gx,     gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    for (int i = 0; i < 100; i++) {
        sand_step(&loc, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_FIRST_SHADE, sand_at(&loc, gx, gy),
        "the grain must still be boxed in and asleep before the test begins,"
        " or freeing the down-left slide below proves nothing");

    /* Free the down-left slide - differs from the grain's own block in
     * BOTH x and y, a true diagonal neighbour, not the orthogonal case an
     * easy bug could special-case by mistake. */
    sand_erase(&loc, gx - 1, gy + 1, 0);

    for (int i = 0; i < 20; i++) {
        sand_step(&loc, 0, 1000, 0);
    }

    const cell_t old_cell = sand_at(&loc, gx, gy);
    const cell_t new_cell = sand_at(&loc, gx - 1, gy + 1);

    loc_free();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, old_cell,
        "the grain must have left its old cell");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, new_cell,
        "and taken the newly-freed diagonal slide - if the wake only "
        "reached orthogonal neighbours, the grain's own block would still "
        "be asleep and it would still be sitting where it started");
}

static void test_sideways_tilt_wakes_only_the_disturbed_column(void)
{
    loc_fixture();

    /* Two grains in the same row, far apart in x - which is now the
     * direction of travel, gravity pointing straight right - each boxed in
     * by three stone blockers covering its only three legal moves. */
    sand_set(&loc, 2, 10, SAND_FIRST_SHADE);
    sand_set(&loc, 3, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks the fall */
    sand_set(&loc, 3, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks down-right slide */
    sand_set(&loc, 3, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks up-right slide */
    /* Once (3,10) is freed and the grain moves there, it must stop, or it
     * keeps sliding diagonally from its new position and the test would
     * be checking the wrong cell. */
    sand_set(&loc, 4, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, 4, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, 4, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    sand_set(&loc, 18, 10, SAND_FIRST_SHADE);
    sand_set(&loc, 19, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, 19, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&loc, 19, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    for (int i = 0; i < 100; i++) {
        sand_step(&loc, 1000, 0, 0);
    }

    uint8_t right_before[8 * 4];   /* a small window around the right grain */
    for (int y = 8; y < 12; y++) {
        for (int x = 16; x < 24; x++) {
            right_before[(y - 8) * 8 + (x - 16)] = sand_at(&loc, x, y);
        }
    }

    /* Free only the left grain's fall. */
    sand_erase(&loc, 3, 10, 0);
    for (int i = 0; i < 20; i++) {
        sand_step(&loc, 1000, 0, 0);
    }

    const cell_t left_moved = sand_at(&loc, 3, 10);

    bool right_unchanged = true;
    for (int y = 8; y < 12 && right_unchanged; y++) {
        for (int x = 16; x < 24; x++) {
            if (sand_at(&loc, x, y) != right_before[(y - 8) * 8 + (x - 16)]) {
                right_unchanged = false;
                break;
            }
        }
    }

    loc_free();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, left_moved,
        "freeing the left grain's fall, under gravity now pointing along x, "
        "must let it actually move there - proving the wake reached along "
        "the now-primary direction of travel, not just vertically the way "
        "the old row-shaped scheme's wake_span() only ever did");
    TEST_ASSERT_TRUE_MESSAGE(right_unchanged,
        "a disturbance far along x must not reach a settled grain two "
        "block-columns away in the same row, even though x is now the "
        "primary direction of travel - locality must hold on that axis "
        "too, not only in y");
}

/* A basin wide enough to span several block-columns, so water poured in at
 * one end and the far wall it must reach are genuinely in different
 * blocks - the direct regression guard for equalise_one_row()'s touched-x
 * range accumulation (see the comment there): getting that range wrong by
 * under-waking would show up here as water that stops levelling partway
 * across.
 *
 * Both the pool and the water source it's fed from scale with SAND_BLOCK_W,
 * not just its width. A single 1-cell-wide column tall enough for the
 * default 16-wide block was tried first and does NOT generalise: at
 * SAND_BLOCK_W=32 (so a pool twice as wide) it still fails to reach the far
 * wall even with sleeping disabled entirely - that is insufficient water
 * mass to cross a wider floor within SAND_LIQUID_SIGHT's per-step reach
 * (see its comment), not a wake/sleep bug. So the source widens along with
 * the pool (POOL_WATER_COLS) as well as filling whatever vertical room the
 * pool has (POOL_WATER_H), instead of a fixed height. POOL_H itself grows
 * with POOL_W too, keeping the basin's proportions - and the vertical room
 * available to the source - the same at every block size. */
#define POOL_W (SAND_BLOCK_W * 2)
#define POOL_H (POOL_W / 2)
#define POOL_WALL_ROWS 3
#define POOL_WATER_COLS (POOL_W / 8)
#define POOL_WATER_H (POOL_H - POOL_WALL_ROWS - 1)
#define POOL_BLOCK_COLS ((POOL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define POOL_BLOCK_ROWS ((POOL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
/* malloc'd/freed per-test rather than static - a static array here is
 * permanent BSS for the whole device boot in any build that links this
 * suite (diagnostics), not just while this test runs. See loc_fixture()'s
 * comment above for the full story; this is the same bug class. */
static uint8_t *pool_cells;
static uint8_t *pool_sleep_blocks;
static sand_t   pool;

static void pool_fixture(void)
{
    pool_cells        = malloc((size_t)POOL_W * POOL_H);
    pool_sleep_blocks = malloc((size_t)POOL_BLOCK_COLS * POOL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(pool_cells);
    TEST_ASSERT_NOT_NULL(pool_sleep_blocks);

    sand_init(&pool, pool_cells, POOL_W, POOL_H, 77u);
    sand_enable_sleeping(&pool, pool_sleep_blocks);
}

static void pool_free(void)
{
    free(pool_cells);
    free(pool_sleep_blocks);
}

static void test_liquid_cross_flow_wakes_only_the_blocks_it_touches_by_range(void)
{
    /* End to end, same shape as test_water_poured_into_a_basin_reaches_
     * both_ends, just wide enough to span several block-columns and with
     * block-sleeping turned on - that test leaves it off. Walls at both
     * edges, water dropped in near the left one. */
    pool_fixture();

    for (int y = POOL_H - POOL_WALL_ROWS; y < POOL_H; y++) {
        sand_set(&pool, 0, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&pool, POOL_W - 1, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    /* POOL_WATER_COLS columns wide and POOL_WATER_H tall - see the comment
     * above POOL_W for why both need to scale with the pool rather than
     * staying a single fixed-height column. */
    for (int x = 1; x <= POOL_WATER_COLS; x++) {
        for (int y = 0; y < POOL_WATER_H; y++) {
            sand_set(&pool, x, y, CELL_MAKE(MAT_WATER, 8));
        }
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&pool, 0, 1000, 0);
    }

    const int far_end_material = CELL_MATERIAL(sand_at(&pool, POOL_W - 2, POOL_H - 1));

    pool_free();
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, far_end_material,
        "water poured in at one end must still reach the far end with "
        "block-sleeping on - if equalise_one_row()'s deferred wake under-"
        "ranges what it touches, the far blocks never wake back up and "
        "levelling silently stops partway across");
}

/* The one case where a move of something that is NOT a liquid still
 * relocates liquid: move_to() (sand_priv.h) is a SWAP, so sand entering a
 * water cell sends that water back UP into the row the sand came from - a
 * row that had no liquid in it at all a moment earlier. Whatever machinery
 * decides which rows the cross-flow pass looks at has to cope with liquid
 * arriving that way, and this is the test that says so.
 *
 * It was written against a specific such mechanism, ROW_NO_LIQUID - a
 * per-row "proved dry" cache that skipped rows, and that was cleared by
 * every move of every material. That cache has since been deleted outright
 * for costing more than it saved (see docs/Sand/Performance-Tuning-
 * Attempts.md's ninth attempt), and cross-flow now walks every row every
 * step, which passes this trivially. The test stays anyway: it was the
 * derivation that killed the cache's last proposed narrowing, and anything
 * that tries to skip rows again will need exactly this scenario to be
 * checked against. Verified to genuinely catch the failure when it was
 * written - with the clear suppressed, it failed with the water frozen in
 * the single cell the swap put it in.
 *
 * No test covered it before: the sleeping/liquid tests above use water
 * alone, and the sand-through-water tests (test_sand_sinks_through_water
 * and friends) run with sleeping off entirely.
 *
 * The setup is built so that ONLY cross-flow can spread the displaced
 * water, which is what makes the assertion sharp. The pool is full
 * (MASS_MAX), so once the water is pushed up it has no room below it and
 * no room down either slope - move_liquid_grain() inside the main sweep can
 * do nothing with it. If the row it landed in is examined, cross-flow
 * halves it into a neighbour on the very next pass and the row holds two
 * water cells; if that row is skipped, it holds exactly the one cell the
 * swap put there, for ever. The sand is dropped from the top row rather
 * than placed on the surface on purpose: an external sand_set() reports its
 * own rows, so a grain placed directly above the pool would itself announce
 * the landing row and mask the case under test. Falling from three rows up
 * makes the swap the only thing that reports it. */
static void test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    sand_clear(&s);

    /* A full pool, two rows deep. Full matters: nothing in it has anywhere
     * to flow, so it settles and every row above it is genuinely dry. */
    for (int y = H - 2; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Spelled out rather than the SAND shorthand, which this file does not
     * define until the material tests further down. */
    sand_set(&s, 1, 0, CELL_MAKE(MAT_SAND, 8));
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int water_cells_in_the_row_above_the_pool = 0;
    for (int x = 0; x < W; x++) {
        const cell_t c = sand_at(&s, x, H - 3);
        if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
            water_cells_in_the_row_above_the_pool++;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1,
        water_cells_in_the_row_above_the_pool,
        "sand sinking into a pool swaps water up into a row that held none "
        "a moment earlier - the cross-flow pass has to examine that row, or "
        "the displaced water sits in the single cell it was pushed into "
        "instead of levelling out along the row");
}


/* Block-shaped skipping in the cross-flow pass (BLOCK_LIQUID_NEAR, see
 * sand_priv.h) rests on one claim: every liquid cell sits in a block whose
 * NEAR bit is set. The interesting way for that to be false is liquid ARRIVING
 * in a block the sweep has already walked and found dry - which is why the bit
 * is expanded to a block's 8 neighbours rather than used raw.
 *
 * This is the fixture that makes the un-expanded version fail, and it was
 * verified to fail before it was kept: with the expansion removed it reports
 * exactly 1 water cell, frozen, against several when correct. Two block-ROWS
 * are the point - the sweep walks the lower one first, so a cell falling
 * across the boundary lands behind it - and a single cell of water is the
 * point too: it empties its source block completely as it goes, so the source
 * block's own bit does not accidentally cover the destination. Without the
 * expansion the pass then finds no liquid anywhere, concludes may_have_liquid
 * is false, and switches itself off for good with the water still on screen.
 */
#define CROSS_BLOCK_W 40
#define CROSS_BLOCK_H 80
#define CROSS_BLOCK_COLS ((CROSS_BLOCK_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define CROSS_BLOCK_ROWS ((CROSS_BLOCK_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

static void test_water_falling_into_the_next_block_down_still_spreads(void)
{
    uint8_t *cells  = malloc((size_t)CROSS_BLOCK_W * CROSS_BLOCK_H);
    uint8_t *blocks = malloc((size_t)CROSS_BLOCK_COLS * CROSS_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t g;
    sand_init(&g, cells, CROSS_BLOCK_W, CROSS_BLOCK_H, 3u);
    sand_enable_sleeping(&g, blocks);

    /* A stone shelf one row below the block boundary, so the water comes to
     * rest inside the LOWER block with nowhere gravity-ward left to go -
     * only cross-flow can move it after that. */
    for (int x = 0; x < CROSS_BLOCK_W; x++) {
        sand_set(&g, x, SAND_BLOCK_H + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    /* One full cell of water, in the last row of the UPPER block. */
    sand_set(&g, 5, SAND_BLOCK_H - 1, CELL_MAKE(MAT_WATER, MASS_MAX));

    for (int i = 0; i < 90; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int water_cells = 0;
    for (int x = 0; x < CROSS_BLOCK_W; x++) {
        const cell_t c = sand_at(&g, x, SAND_BLOCK_H);
        if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
            water_cells++;
        }
    }

    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, water_cells,
        "water that falls into a block the sweep had already walked must "
        "still be found by the cross-flow pass - the block-liquid bit has to "
        "be read expanded to a block's neighbours, or the pass skips the "
        "block the water landed in and then concludes there is no water on "
        "the grid at all");
}

/* At the real screen size, SAND_BLOCK_W/H (16x64) do NOT evenly divide
 * 184x224 - the last block-column is 8 cells wide instead of 16, and the
 * last block-row is 32 cells tall instead of 64. No other test in this
 * file uses a grid shaped like that - every other sleeping/locality test
 * picks dimensions that are exact multiples of the block size, on
 * purpose, to keep the ASCII fixtures small. That gap is real: it is
 * exactly what let a division-free block-index rewrite pass every other
 * test in this file while still producing bad indices (and a real-
 * device crash) once grains actually reached the screen's true edges.
 * This drives grains into every block edge - including the two partial
 * ones - under every gravity direction the dithering can produce. */
#define STRESS_W 184
#define STRESS_H 224
#define STRESS_BLOCK_COLS ((STRESS_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define STRESS_BLOCK_ROWS ((STRESS_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

static void test_block_indices_stay_in_range_at_the_real_screens_partial_edge_blocks(void)
{
    uint8_t *cells  = malloc((size_t)STRESS_W * STRESS_H);
    uint8_t *blocks = malloc((size_t)STRESS_BLOCK_COLS * STRESS_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t stress;
    sand_init(&stress, cells, STRESS_W, STRESS_H, 4242u);
    sand_enable_sleeping(&stress, blocks);

    /* A checkerboard over the whole grid, including the last column and
     * last row exactly - the two partial blocks - not just somewhere
     * comfortably inside a full one. */
    for (int y = 0; y < STRESS_H; y++) {
        for (int x = 0; x < STRESS_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&stress, x, y, SAND_FIRST_SHADE);
            }
        }
    }
    const int grains = sand_count(&stress);

    /* Every axis-aligned and diagonal direction in turn, a handful of
     * steps each - straight down first (settles against the real bottom
     * edge, y=223, the partial last block-row), then every other ring
     * direction, so both slide directions and the fall direction all
     * get to touch x=183 and y=223 (and 0/0) from every angle, not just
     * straight down. Gravity's own dithering already mixes two ring
     * directions per call; cycling the requested direction across all
     * eight on top of that is what reaches the corners specifically.
     * Kept short deliberately: a full checkerboard fill is the single
     * most expensive occupancy shape there is (see the frame-budget
     * tests), and this ran long enough at 30 steps/direction to trip the
     * device's 5s task watchdog - the point here is edge coverage, not
     * a long-running soak, so a handful of steps per direction is
     * plenty to touch every edge at least once. */
    const int gx[] = { 0, 100, 100, 100, 0, -100, -100, -100 };
    const int gy[] = { 100, 100, 0, -100, -100, -100, 0, 100 };
    for (int dir = 0; dir < 8; dir++) {
        for (int i = 0; i < 5; i++) {
            sand_step(&stress, gx[dir], gy[dir], 0);
        }
    }
    /* A hard shake at the end, the same jostle path the flip/undermining
     * tests use - it is what reaches try_slide()'s jostle-fall calls,
     * the two mark_move_in_block() sites the direction cycle above does
     * not otherwise exercise. */
    for (int i = 0; i < 3; i++) {
        sand_step(&stress, 0, 100, 200);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&stress),
        "grains must still be conserved after being driven through every "
        "block edge at the real screen's true, non-block-multiple size");

    free(cells);
    free(blocks);
}

/* A closer reproduction of the device test that actually crashed
 * (test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget,
 * DEVICE_BUILD-only so it never runs here) - a centred pile that reaches
 * the real bottom edge (y=223, the partial 32-tall last block-row) but
 * not either x edge, fully settled under pure-vertical gravity first,
 * then a single outright reversal - not the gradual eight-direction
 * cycle above, which never fully settles before changing direction. */
static void test_block_indices_stay_in_range_after_flipping_a_settled_pile_at_the_real_size(void)
{
    uint8_t *cells  = malloc((size_t)STRESS_W * STRESS_H);
    uint8_t *blocks = malloc((size_t)STRESS_BLOCK_COLS * STRESS_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, cells, STRESS_W, STRESS_H, 13u);
    sand_enable_sleeping(&real, blocks);

    for (int y = STRESS_H / 2; y < STRESS_H; y++) {
        for (int x = STRESS_W / 4; x < (STRESS_W * 3) / 4; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    const int grains = sand_count(&real);

    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, -1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real),
        "flipping gravity must conserve grains too");

    free(cells);
    free(blocks);
}

/* Same idea, reproducing test_a_screen_of_water_fits_in_the_frame_budget
 * instead (also DEVICE_BUILD-only) - liquid's move_liquid_grain()/
 * block_coord() path is untested by the two tests above, which only
 * ever place plain sand. */
static void test_block_indices_stay_in_range_for_a_falling_screen_of_water_at_the_real_size(void)
{
    uint8_t *cells  = malloc((size_t)STRESS_W * STRESS_H);
    uint8_t *blocks = malloc((size_t)STRESS_BLOCK_COLS * STRESS_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, cells, STRESS_W, STRESS_H, 11u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < STRESS_H / 2; y++) {
        for (int x = STRESS_W / 4; x < (STRESS_W * 3) / 4; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    for (int i = 0; i < 60; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    free(cells);
    free(blocks);
}

/* --- scatter -------------------------------------------------------------- */

/* Measures how wide a falling stream has become, in occupied columns. */
static int occupied_columns(void)
{
    int n = 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            if (sand_at(&s, x, y) != SAND_EMPTY) {
                n++;
                break;
            }
        }
    }
    return n;
}

static void test_falling_is_exact_when_scatter_is_off(void)
{
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 1, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 1),
        "with scatter off a grain in open air falls exactly one cell per step "
        "- the randomness is opt-in so that a test can say this and mean it");
}

static void test_scatter_spreads_a_falling_stream(void)
{
    /* The reported problem: a poured blob kept its shape all the way down,
     * because every grain in open air took the same move on the same step. */
    int narrow = 0;
    int spread = 0;

    for (int trial = 0; trial < 8; trial++) {
        sand_init(&s, cells, W, H, 400u + (uint32_t)trial);
        sand_set(&s, 3, 0, SAND_FIRST_SHADE);
        sand_set(&s, 3, 1, SAND_FIRST_SHADE);
        sand_set(&s, 3, 2, SAND_FIRST_SHADE);
        for (int i = 0; i < 4; i++) {
            sand_step(&s, 0, 1, 0);
        }
        narrow += occupied_columns();

        sand_init(&s, cells, W, H, 400u + (uint32_t)trial);
        sand_set_scatter(&s, 128);      /* exaggerated, so the effect is clear */
        sand_set(&s, 3, 0, SAND_FIRST_SHADE);
        sand_set(&s, 3, 1, SAND_FIRST_SHADE);
        sand_set(&s, 3, 2, SAND_FIRST_SHADE);
        for (int i = 0; i < 4; i++) {
            sand_step(&s, 0, 1, 0);
        }
        spread += occupied_columns();
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, narrow,
        "without scatter the stream stays exactly one column wide");
    TEST_ASSERT_GREATER_THAN_MESSAGE(narrow, spread,
        "with scatter it must disperse - that is the whole point");
}

static void test_scatter_conserves_grains(void)
{
    fixture();
    sand_set_scatter(&s, 128);
    for (int y = 0; y < 3; y++) {
        for (int x = 2; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    const int expected = sand_count(&s);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
            "scatter only ever chooses between moves that were already legal, "
            "so it cannot create or lose a grain");
    }
}

static void test_a_lagging_grain_is_not_left_asleep(void)
{
    /* The dangerous interaction. A scattered grain sometimes declines a move
     * it could have made - and a row where nothing moved looks exactly like a
     * settled row. Get this wrong and grains hang in mid-air for ever. */
    for (int trial = 0; trial < 24; trial++) {
        sand_init(&s, cells, W, H, 800u + (uint32_t)trial);
        sand_enable_sleeping(&s, sleep_blocks);
        sand_set_scatter(&s, 200);          /* lags constantly */
        sand_set(&s, 3, 0, SAND_FIRST_SHADE);

        for (int i = 0; i < 200; i++) {
            sand_step(&s, 0, 1, 0);
        }

        /* Scanned across the whole grid, not just the column it started in:
         * scatter drifts sideways as well as lagging, so where it lands is
         * not the question. Whether it landed at all is. */
        int lowest = -1;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) != SAND_EMPTY) {
                    lowest = y;
                }
            }
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, lowest,
            "a grain that chose to lag must still reach the floor - if a lag "
            "lets its row fall asleep, it hangs in the air for ever");
    }
}

/* --- materials ------------------------------------------------------------ */

/* Everything above this point is about sand. These are about the fact that a
 * cell is now a material, and that materials behave differently from each
 * other - which is the whole basis of the sandbox. */

#define WATER CELL_MAKE(MAT_WATER, 8)
/* Room temperature, not a shade: stone's variant is a TEMPERATURE
 * now, the same as glass's, so the 8 this used to carry placed every
 * floor and wall in the suite well above the shock threshold - hot
 * enough that a flake of snow landing on it turned it into sand. */
#define STONE CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT)
#define SAND  CELL_MAKE(MAT_SAND,  8)
#define GAS   CELL_MAKE(MAT_GAS,   8)
#define FIRE  CELL_MAKE(MAT_FIRE,  8)
/* UNLIT. Wood's variant is how much of it is left to burn, so the 8 this
 * used to carry now places a log that is already alight - which flares
 * fire, and turned a grain-conservation test into a count that grew.
 * Third macro in this file to be caught by a material spending its
 * variant on something: see GLASS and STONE above. */
#define WOOD  CELL_MAKE(MAT_WOOD,  0)
#define STEAM CELL_MAKE(MAT_STEAM, 8)
#define SMOKE CELL_MAKE(MAT_SMOKE, 8)
/* A LIT log. Wood's variant is how much of it is left to burn, so this is
 * what ember used to be - see reaction_t.burn_decay. */
#define EMBER CELL_MAKE(MAT_WOOD, MATERIAL_VARIANTS - 1)

/* Total AMOUNT of a material, not the number of cells holding it.
 *
 * For a liquid these are different questions, and only this one has a
 * conserved answer: two half-full cells can merge into one full cell without
 * a drop being lost. Counting cells and calling it conservation would report
 * a leak every time water settled. */
static long mass_of(const sand_t *g, int w, int h, material_id_t m)
{
    long total = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const cell_t c = sand_at(g, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == m) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

static int count_of(material_id_t m)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == m) {
                n++;
            }
        }
    }
    return n;
}

static void test_a_cell_carries_both_material_and_variant(void)
{
    const cell_t c = CELL_MAKE(MAT_WATER, 5);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(c),
        "the high nibble is the material");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, CELL_VARIANT(c),
        "the low nibble is the variant");
    TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(c), "and it is not empty");

    /* The trap this guards: a cell whose variant happens to be zero is still
     * occupied, so emptiness has to be read off the material nibble alone. */
    TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(CELL_MAKE(MAT_SAND, 0)),
        "a variant of zero is not an empty cell");
    TEST_ASSERT_TRUE(CELL_IS_EMPTY(CELL_EMPTY));
}

static void test_stone_never_moves(void)
{
    fixture();
    sand_set(&s, 3, 0, STONE);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, 3, 0),
        "a static material stays exactly where it is put, unsupported or not "
        "- that is what makes it something to build with");
}

static void test_nothing_displaces_stone(void)
{
    fixture();
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 3, 4, SAND);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, 3, 5),
        "sand must not sink through a stone floor however heavy it is");
}

static void test_sand_sinks_through_water(void)
{
    fixture();
    /* A pool with a grain of sand sitting on top of it. */
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than water, so it must sink all the way through the "
        "pool rather than float on it");
}

static void test_water_does_not_sink_through_sand(void)
{
    fixture();
    /* The mirror of the last one, and the half that a naive swap gets wrong:
     * displacement has to be one-way, or the two materials trade places back
     * and forth for ever. */
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_set(&s, 3, 3, WATER);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, 3)),
        "water is lighter than sand, so it must sit on top rather than sink "
        "into it");
}

static void test_displacement_conserves_both_materials(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    for (int x = 1; x < 5; x++) {
        sand_set(&s, x, 2, SAND);
    }
    const long water = mass_of(&s, W, H, MAT_WATER);
    const int  sand  = count_of(MAT_SAND);

    for (int i = 0; i < 80; i++) {
        sand_step(&s, 200, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(water, mass_of(&s, W, H, MAT_WATER),
            "displacing a liquid moves its mass about and destroys none of "
            "it - measured as an amount, since cells merge and split");
        TEST_ASSERT_EQUAL_INT_MESSAGE(sand, count_of(MAT_SAND),
            "and the powder doing the displacing is still whole cells");
    }
}

static void test_water_finds_its_own_level(void)
{
    fixture();
    /* A column of water in the middle of the floor. Sand would stand there as
     * a heap at its angle of repose; water must not. */
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, WATER);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int tallest = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!CELL_IS_EMPTY(sand_at(&s, x, y))) {
                tallest = y;
                y = H;
                break;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2, tallest,
        "a column of water must collapse and spread along the floor - having "
        "no angle of repose is the whole difference between a liquid and a "
        "powder");
}

static void test_a_powder_still_holds_a_heap(void)
{
    /* The other side of the same coin: making water spread must not have made
     * sand spread too. */
    fixture();
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, SAND);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int occupied_columns = 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            if (!CELL_IS_EMPTY(sand_at(&s, x, y))) {
                occupied_columns++;
                break;
            }
        }
    }

    TEST_ASSERT_LESS_THAN_MESSAGE(W, occupied_columns,
        "sand must still pile rather than level out - if it spread across the "
        "whole floor it has stopped being a powder");
}

static void test_water_can_be_held_by_a_stone_basin(void)
{
    /* What makes it a sandbox rather than a toy: build something, and it
     * holds. */
    fixture();
    sand_set(&s, 2, H - 1, STONE);
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 5, H - 1, STONE);
    sand_set(&s, 5, H - 2, STONE);
    for (int x = 3; x < 5; x++) {
        sand_set(&s, x, 4, WATER);
    }
    const int water = count_of(MAT_WATER);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int held = 0;
    for (int x = 3; x < 5; x++) {
        for (int y = H - 2; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                held++;
            }
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(water, held,
        "every drop must still be inside the basin - a wall that leaks is not "
        "a wall");
}

static void test_a_drop_resting_on_a_pool_comes_to_rest(void)
{
    /* The reported behaviour: settled water crept about like slime, a lump
     * sliding over the surface instead of becoming part of it.
     *
     * A drop with nothing on top of it has no weight pressing it anywhere, so
     * it must simply stop. Without the pressure test it slides sideways every
     * step - and since the sweep direction alternates, it slid left, right,
     * left, wandering the surface for ever. */
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    sand_set(&s, 3, 3, WATER);       /* one drop, on top, nothing above it */

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Checked one step at a time, not by comparing two distant snapshots.
     * The wandering is an OSCILLATION - the sweep direction alternates, so a
     * loose drop slides left, then right, and lands back where it started
     * every second step. Comparing step 40 against step 80 sees no difference
     * at all and passes a bug that is plainly visible on screen. */
    uint8_t settled[W * H];
    for (int i = 0; i < 5; i++) {
        memcpy(settled, cells, sizeof(settled));
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
            "water with nothing resting on it must come to a complete stop - "
            "sliding about on a level surface is what made it look like slime");
    }
}

static void test_water_under_weight_still_spreads(void)
{
    /* The other half, and the one a careless fix breaks: water DOES have to
     * spread, or it stands in a column on a flat floor and stops behaving
     * like a liquid at all. What makes it spread is the weight above it. */
    fixture();
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, WATER);   /* a column standing on the floor */
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int on_the_floor = 0;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, H - 1)) == MAT_WATER) {
            on_the_floor++;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, on_the_floor,
        "a column of water must collapse and run along the floor - the cells "
        "underneath have the whole column pressing on them");
}

static void test_water_poured_into_a_basin_reaches_both_ends(void)
{
    /* End to end: dropped in one corner, it must find the far side. */
    fixture();
    for (int y = H - 3; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    for (int y = 0; y < 6; y++) {
        sand_set(&s, 1, y, WATER);
    }

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, W - 2, H - 1)),
        "water poured in at one end must travel to the other - if it cannot, "
        "the pressure rule has been made too strict to flow at all");
}

/* A basin needs room around it, so these get a grid of their own. */
#define POUR_W 18
#define POUR_H 14
static uint8_t pour_cells[POUR_W * POUR_H];
static sand_t  pour;

/* An open-topped stone basin, filled with water and settled level. */
static void build_full_basin(void)
{
    sand_init(&pour, pour_cells, POUR_W, POUR_H, 9u);

    for (int y = POUR_H - 6; y < POUR_H; y++) {
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int y = POUR_H - 4; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            sand_set(&pour, x, y, CELL_MAKE(MAT_WATER, 8));
        }
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&pour, 0, 1000, 0);
    }
}

static int material_in_basin(material_id_t m)
{
    int n = 0;
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            if (CELL_MATERIAL(sand_at(&pour, x, y)) == m) {
                n++;
            }
        }
    }
    return n;
}

/* How much liquid is INSIDE the basin.
 *
 * Region-limited on purpose: whole-grid mass is conserved by definition, so
 * measuring that and calling it "the basin emptied" tests nothing at all. The
 * water is still on screen after it pours out - it is just somewhere else. */
static long mass_in_basin(void)
{
    long total = 0;
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            const cell_t c = sand_at(&pour, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

static void test_a_tipped_basin_pours_its_water_out(void)
{
    /* The reported behaviour: tilting the board poured some of the water and
     * then stopped, leaving a lump sitting in the basin like sand.
     *
     * The cause was that a liquid spread along a SCREEN row, which is only
     * "along the surface" while gravity points straight down. Tilted, water
     * could no longer level in its own frame, so it heaped against the low
     * wall instead of running over the lip. */
    build_full_basin();
    const long held = mass_in_basin();
    TEST_ASSERT_GREATER_THAN_MESSAGE(100, held,
        "the basin must actually be full to begin with");

    /* Tipped hard to the right - far past any angle of repose. */
    for (int i = 0; i < 600; i++) {
        sand_step(&pour, 1000, 300, 0);
    }

    /* Not held/10. That threshold encoded the OLD bug rather than the
     * physics: under this gravity the basin's right-hand wall (stone at
     * x=12, y=8..13, floor stone at y=13) is a FLOOR, and the corner at
     * (11,12) is a genuine pocket - the only way out is to climb to y<=7
     * and go over the wall's top corner at (12,7). The level plane through
     * that spill corner is (x-12)*1000 + (y-7)*300 > 0 for "below the
     * plane", and within the region mass_in_basin() measures (x 6..11,
     * y 8..12) only (11,11) and (11,12) satisfy it - two cells, 30 units of
     * the 144 this basin started with. No correct simulation can empty this
     * basin below that 30, so held/10 = 14 was never a reachable target; it
     * was only ever met while cross-flow treated a vertical ray as level and
     * let water climb the wall for nothing. The fix leaves 51: those 30
     * physically-trapped units plus about 1.4 cells of residual film, which
     * is the levelling rule's own dead band - a transfer moves whole mass
     * units, so up to one unit of level difference can persist at each hop
     * of a chain, and the escape route out of this corner is a four-hop
     * chain. 75 sits above the fix's 51 with room, and still below anything
     * that would call a heaped-up basin a pour. */
    const long left = mass_in_basin();
    TEST_ASSERT_LESS_THAN_MESSAGE(75, (int)left,
        "held on its side, a basin of water must pour rather than heap - 75 "
        "is set above the ~30 units this tilt physically traps in the low "
        "corner, not at the old held/10 threshold, which no correct "
        "simulation of this scene could ever meet");

    /* The property the count above is only a proxy for: what water is left
     * must sit in the LOW CORNER, not spread across the basin floor the way
     * a powder heaps. Verified on the fix: the water that remains is only at
     * x=10 and x=11, so x<=9 - more than half the basin floor's width - must
     * be completely dry. This is the stronger statement of "water has no
     * angle of repose": not just that little is left, but that what is left
     * has the SHAPE a puddle in a pocket has, not the shape a pile has. */
    for (int y = POUR_H - 6; y < POUR_H - 1; y++) {
        for (int x = 6; x <= 9; x++) {
            const cell_t c = sand_at(&pour, x, y);
            char why[128];
            snprintf(why, sizeof why,
                     "water heaped at (%d,%d), away from the low corner - "
                     "that is a powder's shape, not a liquid's", x, y);
            TEST_ASSERT_FALSE_MESSAGE(
                !CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER, why);
        }
    }
}

static void test_a_tipped_basin_keeps_its_sand(void)
{
    /* The control, and the reason the last test means anything: sand tipped
     * the same way must NOT all run out. If both emptied, the test above would
     * be measuring gravity rather than the difference between a liquid and a
     * powder. */
    sand_init(&pour, pour_cells, POUR_W, POUR_H, 9u);
    for (int y = POUR_H - 6; y < POUR_H; y++) {
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int y = POUR_H - 4; y < POUR_H - 1; y++) {
        for (int x = 6; x < 12; x++) {
            sand_set(&pour, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&pour, 0, 1000, 0);
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&pour, 1000, 300, 0);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, material_in_basin(MAT_SAND),
        "sand has friction and an angle of repose, so a tipped basin must "
        "keep some of it");
}

/* Wide enough that a puddle has somewhere to go. On a grid the pour can fill,
 * water and sand both end up "full" and the comparison measures nothing. */
#define WIDE_W 32
#define WIDE_H 20

/* A deliberately over-long grid for test_conduction_stops_at_the_reach_cap
 * alone - see that test for why it cannot share `wide`.
 * CONDUCT_REACH_TEST mirrors sand_reactions.c's own CONDUCT_REACH,
 * which is private to that file; if the two ever drift apart the
 * test stops proving anything, so keep them together. */
/* Half-full, matching WATER/STONE/GAS/... below rather than MASS_MAX -
 * a liquid's variant is a FILL LEVEL, so a grid laid out with these does
 * NOT fill its container, and a test that assumes a sealed box is brim
 * full because every cell in it was set will be wrong about where the
 * surface is. */
#define OIL   CELL_MAKE(MAT_OIL,  8)
#define LAVA  CELL_MAKE(MAT_LAVA, 8)
/* Room temperature, not a shade. Glass is the one material whose variant
 * is a TEMPERATURE (material.h's top comment), so the 8 this used to carry
 * silently placed every pane in these tests at half melt - and, worse,
 * placed it hot enough that a stray flake of snow would shatter it.
 *
 * SAND_AMBIENT_HEAT rather than 0, because 0 is no longer "at rest": it is
 * the bottom of the frost range, so a pane placed there would start out
 * looking chilled and spend the first steps of every test warming up. */
#define GLASS CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT)
#define SNOW  CELL_MAKE(MAT_SNOW,  8)

#define CONDUCT_REACH_TEST 32
#define CAP_W (CONDUCT_REACH_TEST + 16)
#define CAP_H 8
static uint8_t wide_cells[WIDE_W * WIDE_H];
static sand_t  wide;

/* How tall a heap the same pour leaves, in cells above the floor. */
static int poured_height(material_id_t m)
{
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    /* Measured shortly after the pour, not once everything has long since
     * settled. The mound is a TRANSIENT - water arriving faster than it can
     * flow away - and given hundreds of idle steps even a crawl levels out,
     * so a late measurement passes happily while the screen looks wrong. */
    for (int i = 0; i < 60; i++) {
        if (i < 20) {
            sand_spawn(&wide, WIDE_W / 2, 1, 2, m);
        }
        sand_step(&wide, 0, 1000, 0);
    }

    for (int y = 0; y < WIDE_H; y++) {
        for (int x = 0; x < WIDE_W; x++) {
            if (!CELL_IS_EMPTY(sand_at(&wide, x, y))) {
                return WIDE_H - y;
            }
        }
    }
    return 0;
}

static void test_water_puddles_where_sand_heaps(void)
{
    /* The reported behaviour: water poured from a finger built a mound
     * instead of spreading out.
     *
     * Two causes, and the second is the interesting one. Water had no way to
     * level in a tilted frame, and - even level - it spread only ONE cell per
     * step, which is far slower than a finger delivers. The mound was the
     * pour outrunning the flow.
     *
     * Sand is the control. If both heaped, or neither did, this would be
     * measuring the pour rather than the difference between a liquid and a
     * powder. */
    const int water = poured_height(MAT_WATER);
    const int sand  = poured_height(MAT_SAND);

    TEST_ASSERT_GREATER_THAN_MESSAGE(4, sand,
        "sand must build a real heap, or there is nothing to compare against");
    /* Measures 4 against sand's 8 - half the height, and about as flat as the
     * volume allows. Stated as a ratio rather than an absolute so the test
     * survives tuning the pour, and loose enough that it is guarding against a
     * mound rather than pinning an exact shape. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(sand / 2, water,
        "water poured from one spot must end up far flatter than the same "
        "sand - a liquid has no angle of repose and should not build a mound");
}

static void test_a_large_body_of_water_levels(void)
{
    /* Small puddles levelled while large ones froze into a staircase, because
     * a cell could only see eight columns and the terrace edges of a wide pool
     * are tens of columns apart. Nothing could see anywhere lower, so nothing
     * moved - and thousands of further steps changed nothing at all.
     *
     * The volume here is deliberately much wider than a cell can see, which is
     * the whole point: it must level by looking further than one step's worth
     * of travel. */
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    for (int i = 0; i < 90; i++) {
        sand_spawn(&wide, WIDE_W / 2, 1, 3, MAT_WATER);
        sand_step(&wide, 0, 1000, 0);
    }
    const long volume = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);
    for (int i = 0; i < 600; i++) {
        sand_step(&wide, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(volume, mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER),
        "and none of it may be lost on the way");

    /* Level means every column is the same depth, give or take the one row a
     * remainder has to sit somewhere. */
    /* Depth measured as AMOUNT per column, which is far finer than counting
     * cells: a column holding two full cells and a third of another is 35
     * units, not "two or three". Levelness can be asserted to a fraction of a
     * cell rather than rounded to whole ones. */
    int shallowest = 1 << 30;
    int deepest    = 0;
    for (int x = 0; x < WIDE_W; x++) {
        int depth = 0;
        for (int y = 0; y < WIDE_H; y++) {
            const cell_t c = sand_at(&wide, x, y);
            if (!CELL_IS_EMPTY(c)) {
                depth += CELL_VARIANT(c);
            }
        }
        if (depth < shallowest) {
            shallowest = depth;
        }
        if (depth > deepest) {
            deepest = depth;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2 * MASS_MAX, deepest,
        "there must actually be a pool here to level");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(MASS_MAX / 2, deepest - shallowest,
        "a settled pool must be level to within half a cell - a staircase "
        "means it stopped because it could not reach anywhere lower, not "
        "because it had finished");
}

static void test_a_settled_pool_does_not_flicker(void)
{
    /* The reported bug: water that looked finished settling kept visibly
     * flashing between shades and back - which is exactly what it looks
     * like when a large amount of mass swings from one cell to another and
     * back, since colour is read straight off fill level.
     *
     * Reproduced with gravity held slightly OFF axis, not straight down.
     * Exactly (0, 1000) never dithers at all - see
     * sand_gravity_direction_dithered() - so a test using it could never
     * catch a bug that only shows up once dithering is active, which real
     * handling almost always has: a hand is never perfectly level either.
     *
     * The cause was equalise_liquids()'s cross-flow axis being taken from
     * the DITHERED direction, which by design changes between two octants
     * almost every step once off axis - so the axis a "is this level"
     * search runs along changed out from under it constantly, and a pool
     * level along one axis can read as wildly unbalanced along the other. */
    const int gx = 60, gy = 1000;

    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    for (int i = 0; i < 90; i++) {
        sand_spawn(&wide, WIDE_W / 2, 1, 3, MAT_WATER);
        sand_step(&wide, gx, gy, 0);
    }
    for (int i = 0; i < 600; i++) {
        sand_step(&wide, gx, gy, 0);
    }

    /* Once settled, no single step may move much mass at all. The bug moved
     * roughly half the deepest column's worth in one step, then its
     * opposite the step after; this bounds it far below that, loose enough
     * to tolerate an unevenly-divisible remainder still finding its exact
     * rest spot at the surface. */
    uint8_t prev[WIDE_W * WIDE_H];
    memcpy(prev, wide_cells, sizeof(prev));

    long worst_churn = 0;
    for (int i = 0; i < 200; i++) {
        sand_step(&wide, gx, gy, 0);

        long churn = 0;
        for (int c = 0; c < WIDE_W * WIDE_H; c++) {
            const int a = CELL_IS_EMPTY(prev[c])       ? 0 : CELL_VARIANT(prev[c]);
            const int b = CELL_IS_EMPTY(wide_cells[c]) ? 0 : CELL_VARIANT(wide_cells[c]);
            churn += (a > b) ? (a - b) : (b - a);
        }
        if (churn > worst_churn) {
            worst_churn = churn;
        }
        memcpy(prev, wide_cells, sizeof(prev));
    }

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(4 * MASS_MAX, worst_churn,
        "a settled pool must not swing large amounts of mass around once "
        "level - the reported symptom was water that looked settled "
        "visibly changing colour and resettling, over and over");
}

/* Settles a w x h pool of water under (gx, gy) for `steps` steps, then
 * reports how much its surface tilts: the difference between the total
 * water mass held in the leftmost w/8 columns and the rightmost w/8
 * columns, in units of 1/1024 of a cell of depth per cell of x.
 *
 * Grid and block array are malloc()'d and freed here rather than static, so
 * this can be called at several different sizes - see the big-grid tests
 * above (e.g. test_the_four_liquid_scene_keeps_reacting_after_settling) for
 * the same shape. */
static int settled_surface_slope_q10(int w, int h, int gx, int gy, int steps)
{
    uint8_t *cells  = malloc((size_t)w * (size_t)h);
    uint8_t *blocks = malloc((size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    /* Free whatever succeeded BEFORE asserting, not after - see 565f72e.
     * TEST_ASSERT_NOT_NULL(blocks) alone would longjmp straight past both
     * frees and leak `cells` for the rest of that boot if the second
     * allocation ever came back NULL, and this helper is called eight
     * times in one test, so it would leak eight times over. */
    if (cells == NULL || blocks == NULL) {
        free(cells);
        free(blocks);
        TEST_FAIL_MESSAGE("need a grid and a block map to settle a pool "
                           "and measure its slope, and at least one of "
                           "the two failed to allocate");
    }

    sand_t s;
    sand_init(&s, cells, w, h, 41u);
    sand_enable_sleeping(&s, blocks);

    /* The bottom h/3 rows, full width, at MASS_MAX, and then ONE more row
     * above that at mass 7. The part-full row matters: a volume that
     * happens to divide exactly into full rows has no partially-filled cell
     * anywhere, and a partial cell is the only thing cross-flow can move -
     * such a pool sits dead still at any tilt, on HEAD as well as the fix,
     * because there is nothing for either ray to transfer. A real pool
     * always has a ragged surface, and this is what gives the fixture one. */
    const int full_rows = h / 3;
    for (int y = h - full_rows; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int x = 0; x < w; x++) {
        sand_set(&s, x, h - full_rows - 1, CELL_MAKE(MAT_WATER, 7));
    }

    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }

    const int q = w / 8;
    long lo = 0, hi = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < q; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                lo += CELL_VARIANT(c);
            }
        }
        for (int x = w - q; x < w; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                hi += CELL_VARIANT(c);
            }
        }
    }

    free(cells);
    free(blocks);

    /* An end-to-end difference of two eighths, deliberately, rather than a
     * regression fitted over every column: it is blind ON PURPOSE to the
     * surface's own +/-1 cell texture and to a few columns running dry at
     * the shallow end, both of which would swamp a per-column measure with
     * noise that has nothing to do with the angle. Multiplied out in
     * `long` - (hi-lo) runs to about 40000 on the biggest fixture here, and
     * *1024 does not overflow a long - then cast down to the int this
     * returns. */
    return (int)(((long)(hi - lo) * 1024) / ((long)q * (w - q) * MASS_MAX));
}

static void test_a_pool_settles_at_the_angle_it_is_tilted_to(void)
{
    /* The reported bug: a settled liquid surface only ever came out
     * perfectly flat or snapped to the 45-degree diagonal, never anywhere
     * in between - because equalise_liquids() levelled along a single ray
     * taken perpendicular to the NEAREST of the eight gravity directions,
     * so a settled surface could only ever be perpendicular to one of
     * those eight, and quantised to 0/45/90 degrees. It is worst at the
     * app's VERY LOW quality setting, whose grid is 368/6 x 448/6 = 61x74 -
     * which is why 61x74 is one of the fixtures below by name, not a round
     * number picked for convenience.
     *
     * Two tilts are needed, not one: 20 and 26 degrees sit either side of
     * the 22.5-degree boundary between two of the eight octants, and the
     * old behaviour failed them in OPPOSITE directions - flat below the
     * boundary (the nearest direction was still the axis) and snapped to
     * the diagonal above it (the nearest direction became the diagonal).
     * A fix that only moved the boundary rather than removing it could pass
     * one of these and fail the other.
     *
     * Measured on the host, all four fixtures, 2500 steps, this q10 slope
     * against the true one:
     *   on the fix:  20deg 0.262-0.369 (true 0.364); 26deg 0.394-0.479 (true 0.488)
     *   before it:   20deg 0.012-0.032 (dead flat);  26deg 0.645-0.941 (snapped
     *                                                 to the diagonal)
     * so the band asserted below, 0.15 to 0.58 (154 to 594 in q10), is green
     * on the fix at every fixture with at least 0.10 of margin, and red
     * before it at every fixture in both directions. */
    static const struct { int w, h; } fixtures[] = {
        { 32, 20 }, { 40, 28 }, { 61, 74 }, { 92, 56 },
    };
    const int steps = 2500;

    for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        const int w = fixtures[i].w, h = fixtures[i].h;
        char why[256];

        /* 20 degrees: true slope 0.364, 373 in q10. */
        const int slope20 = settled_surface_slope_q10(w, h, 342, 939, steps);
        snprintf(why, sizeof why,
                 "%dx%d fixture at 20 degrees: measured slope %d (q10), "
                 "expected roughly 373 (true slope 0.364) - flat near 0 "
                 "means the surface snapped to the axis, the old bug below "
                 "the octant boundary", w, h, slope20);
        TEST_ASSERT_GREATER_THAN_MESSAGE(154, slope20, why);
        TEST_ASSERT_LESS_THAN_MESSAGE(594, slope20, why);

        /* 26 degrees: true slope 0.488, 500 in q10. */
        const int slope26 = settled_surface_slope_q10(w, h, 422, 906, steps);
        snprintf(why, sizeof why,
                 "%dx%d fixture at 26 degrees: measured slope %d (q10), "
                 "expected roughly 500 (true slope 0.488) - close to 1024 "
                 "means the surface snapped to the diagonal, the old bug "
                 "above the octant boundary", w, h, slope26);
        TEST_ASSERT_GREATER_THAN_MESSAGE(154, slope26, why);
        TEST_ASSERT_LESS_THAN_MESSAGE(594, slope26, why);
    }
}

/* --- momentum: the wall-rebound splash ----------------------------------- */

#define REB_W 6
#define REB_H 4
static uint8_t reb_cells_a[REB_W * REB_H];
static uint8_t reb_cells_b[REB_W * REB_H];
static sand_t  reb_a, reb_b;

static long mass_in_column(const sand_t *g, int x, material_id_t m)
{
    long total = 0;
    for (int y = 0; y < REB_H; y++) {
        const cell_t c = sand_at(g, x, y);
        if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == (uint8_t)m) {
            total += CELL_VARIANT(c);
        }
    }
    return total;
}

/* Only the wall column, so the interior one stays empty - room the rebound
 * needs somewhere to deposit into. */
static void fill_left_wall_column(sand_t *g)
{
    for (int y = 0; y < REB_H; y++) {
        sand_set(g, 0, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
}

static void test_a_hard_flick_kicks_water_off_the_wall_it_just_hit(void)
{
    /* The reported wish: a wave that has just piled against a wall should
     * bounce off it a little, rather than simply sitting there the way a
     * steady tilt to the same angle leaves it.
     *
     * Both pools are primed pointing a different way, THEN filled with water
     * against the left wall, THEN stepped once more with gravity pointing
     * left - so the water itself never experiences its priming direction at
     * all, and the two setups differ in exactly one thing: which way
     * momentum was already pointing when that final step landed.
     *
     * Gravity is kept exactly horizontal throughout, which matters beyond
     * tidiness: it is what makes the comparison exact rather than merely
     * probable. Off-axis gravity dithers between two of the eight directions
     * at random (see sand_gravity_direction_dithered), and horizontal
     * gravity's own perpendicular is vertical, so ordinary cross-flow cannot
     * reach a neighbouring COLUMN either. With no randomness and no ordinary
     * path into column 1 at all, water arriving there can only be the
     * rebound - there is nothing else it could be.
     *
     * Both leave sand_set_flick() at its default of zero except on the one
     * step meant to be a flick: the gyroscope has nothing to say about a
     * turn that never happened, and the momentum arithmetic must not invent
     * one on its own just because the smoothed direction eventually moved -
     * see the comment above SAND_REBOUND_GAIN. */
    sand_init(&reb_a, reb_cells_a, REB_W, REB_H, 1u);
    sand_init(&reb_b, reb_cells_b, REB_W, REB_H, 1u);

    sand_step(&reb_a, -1000, 0, 0);   /* primed pointing left, same as below */
    sand_step(&reb_b,  1000, 0, 0);   /* primed pointing right - away from it */

    fill_left_wall_column(&reb_a);
    fill_left_wall_column(&reb_b);

    sand_step(&reb_a, -1000, 0, 0);   /* gradual: already pointed this way */

    sand_set_flick(&reb_b, 255);      /* the gyroscope says: a hard flick */
    sand_step(&reb_b, -1000, 0, 0);   /* flicked: a full reversal onto the wall */

    const long gradual_col1 = mass_in_column(&reb_a, 1, MAT_WATER);
    const long flicked_col1 = mass_in_column(&reb_b, 1, MAT_WATER);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gradual_col1,
        "sanity check on the test itself: gravity that was already pointing "
        "into the wall must produce no momentum and therefore no rebound");
    TEST_ASSERT_GREATER_THAN_MESSAGE(gradual_col1, flicked_col1,
        "a hard flick onto a wall the water is already resting against must "
        "kick some of it back into the grid");
}

static void test_a_reversal_without_a_flick_signal_still_does_not_rebound(void)
{
    /* The whole reason sand_set_flick() exists: (gx, gy) is smoothed, so a
     * caller could always manufacture a large frame-to-frame turn just by
     * reporting a sudden change of mind about where gravity points, with no
     * device motion behind it at all. If the turn's own size were what
     * triggered the rebound, this would be indistinguishable from a real
     * flick and the effect would fire on command rather than on speed.
     *
     * Same full reversal as the test above, but with nothing set into
     * sand_set_flick() - the caller reporting no motion. It must produce
     * exactly the same nothing that a steady gravity does. */
    sand_init(&reb_a, reb_cells_a, REB_W, REB_H, 1u);

    sand_step(&reb_a, 1000, 0, 0);     /* primed pointing right */
    fill_left_wall_column(&reb_a);
    sand_step(&reb_a, -1000, 0, 0);    /* the same reversal - but no flick set */

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mass_in_column(&reb_a, 1, MAT_WATER),
        "a direction change with no reported flick must not rebound, however "
        "large the change - only sand_set_flick() may trigger this");
}

/* --- gas ------------------------------------------------------------------ */

static void test_gas_rises_straight_up_under_ordinary_gravity(void)
{
    fixture();
    sand_set(&s, 3, H - 1, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, gas moves up - the opposite "
        "direction from every other material");
}

static void test_gas_falls_when_the_board_is_inverted(void)
{
    fixture();
    sand_set(&s, 3, 0, GAS);

    sand_step(&s, 0, -1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 3, 1)),
        "gas always moves AGAINST gravity, whatever direction that "
        "currently is - inverted gravity means gas falls, not a hardcoded "
        "upward move");
}

static void test_gas_rises_diagonally_under_tilted_gravity(void)
{
    fixture();
    sand_set(&s, 5, H - 1, GAS);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 1000, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 5, H - 1)),
        "the grain must have left its starting cell");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 2, H - 4)),
        "with gravity down-and-right, anti-gravity is up-and-left - three "
        "steps of (-1,-1) should land it exactly three columns left and "
        "three rows up from where it started");
}

static void test_gas_is_blocked_by_a_stone_ceiling(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
    }
    sand_set(&s, 3, H - 1, GAS);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Once blocked from rising further, a lone grain with open space on
     * both sides is free to drift sideways along the row (equalise_gas()
     * has no reason to keep it in its starting column - the same is true
     * of a single isolated liquid grain). So the real invariant to check
     * is not "still at column 3", it is "the ceiling was never displaced,
     * and the grain is still one row below it, not through it". */
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE,
            CELL_MATERIAL(sand_at(&s, x, 0)),
            "a solid ceiling with no gap must never be displaced");
    }

    bool found_gas_below_ceiling = false;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, 1)) == MAT_GAS) {
            found_gas_below_ceiling = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_gas_below_ceiling,
        "gas must stop right below a sealed ceiling, not pass through it");
}

static void test_gas_disperses_across_a_ceiling(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
    }
    for (int y = 0; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    /* Four grains stacked in one column - gas cannot rise through gas
     * (same density, can_enter() requires strictly denser), so once the
     * first reaches the ceiling the rest are blocked from stacking through
     * it too. Whether they disperse sideways instead, or just pile up
     * behind the leader, is exactly what this test checks. */
    for (int y = H - 4; y < H; y++) {
        sand_set(&s, W / 2, y, GAS);
    }

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int occupied_columns = 0;
    for (int x = 1; x < W - 1; x++) {
        for (int y = 1; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GAS) {
                occupied_columns++;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, occupied_columns,
        "four gas grains trapped under a ceiling must end up spread across "
        "more than the one column they rose in, or the perpendicular "
        "spread pass (equalise_gas()) is missing or broken");
}

static void test_sand_sinks_through_gas(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GAS);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than gas, so it must sink all the way through "
        "rather than float on it");
}

static void test_water_sinks_through_gas(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GAS);
        }
    }
    sand_set(&s, 3, 3, WATER);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "water is denser than gas too, so it must sink through it the same "
        "way sand does");
}

static void test_gas_grain_count_is_conserved(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, GAS);
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
                "a step must conserve gas grains in every gravity "
                "direction, the same as it does for sand");
        }
    }
}

static void test_rising_gas_wakes_the_blocks_it_passes_through(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    sand_set(&s, 3, H - 1, GAS);

    /* Straight, unchanging gravity - every step but the first is steady
     * state, so a block that stops earning BLOCK_ACTIVE from the gas pass
     * would start reporting settled almost immediately, well before the
     * grain actually runs out of places to rise to. */
    for (int i = 0; i < H - 1; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_FALSE_MESSAGE(sand_block_settled(&s, 0, 0),
            "the block must not be marked settled while a gas grain inside "
            "it is still actively rising step after step - that can only "
            "happen if the gas pass forgot to wake the block it just moved "
            "in");
    }
}

static void test_gas_scatter_can_be_disabled(void)
{
    fixture();
    sand_set_scatter(&s, 0);
    sand_set(&s, 3, H - 1, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with scatter off, gas rises exactly one cell per step in a "
        "straight line - the same guarantee sand's own fall makes");
}

static void test_gas_decays_and_disappears_over_time(void)
{
    fixture();
    /* Off by default (see test_gas_grain_count_is_conserved, which relies
     * on exactly that) - forced to 100% here for a fast, exact test rather
     * than waiting out the real material figure's low per-step odds. */
    sand_set_decay(&s, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, H - 1, 0, MAT_GAS),
        "setup: exactly one gas grain placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1,
        CELL_VARIANT(sand_at(&s, 3, H - 1)),
        "a freshly spawned gas grain must start at full life, not a random "
        "shade - random_cell() special-cases a decaying material the same "
        "way it already does a liquid's fill level");

    /* At a forced 100% chance, life ticks down by exactly one per step -
     * gone on the step that takes it from 1 to 0, so full life takes
     * exactly that many steps to clear. */
    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "gas must decay away to nothing given enough time - unlike every "
        "other material, whose grain count is conserved forever (see "
        "test_gas_grain_count_is_conserved)");
}

static void test_gas_decaying_away_marks_its_row_dirty(void)
{
    dirty_fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);   /* stay put, so the vanish lands at a
                                 * known row instead of wherever it drifted */

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 4, 0, MAT_GAS),
        "setup: exactly one gas grain placed");
    memset(dirty, 0, sizeof(dirty));

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "setup: the grain must have decayed away by now");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "the row a gas grain decayed away in must be marked dirty, or its "
        "last colour stays on the panel forever after the cell itself is "
        "already empty - tick_decay()'s vanish branch must call "
        "mark_rows() the same way its tick-down branch already does");
}

/* --- fire ------------------------------------------------------------- */

/* A stone box sealing columns x0..x1 of row 3 on all four sides with NO
 * spare cells inside, so nothing placed inside it can drift away via its
 * OWN movement pass (main sweep, sand_step_liquids(), or sand_step_gas()
 * - all of which run before reactions, in the same step) before the
 * reactions pass gets a chance to check adjacency: gas/fire rise against
 * gravity (blocked by the row 2 ceiling) and disperse sideways (blocked
 * by walls immediately at x0-1 and x1+1 - not just somewhere further
 * out, since ANY empty cell inside the box is still room for
 * equalise_gas() to hop into), water falls (blocked by the row 4
 * floor). Fire needing this too, not just gas, is new since fire became
 * kind = KIND_GAS - it is now just as capable of drifting off during the
 * SAME step it was placed as gas always was, and a room with even one
 * spare empty cell left room for exactly that (confirmed: the first
 * version of this room, walled only two columns further out than
 * strictly necessary, let both the fire and gas cells each hop one cell
 * sideways into the slack before reactions ever ran). The caller passes
 * the exact span it is about to fill (x0..x1 inclusive) - no slack, no
 * spare cells, by construction. */
static void fire_room(int x0, int x1)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 2, STONE);
        sand_set(&s, x, 4, STONE);
    }
    sand_set(&s, x0 - 1, 3, STONE);
    sand_set(&s, x1 + 1, 3, STONE);
}

static void test_fire_ignites_an_adjacent_flammable_neighbour(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "a flammable neighbour touching fire must ignite");
}

static void test_extinguishing_wins_over_igniting(void)
{
    fire_room(2, 4);
    sand_set(&s, 2, 3, WATER);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "fire touching water must be extinguished, and now becomes steam "
        "rather than simply vanishing - see reaction_t.quench_to");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "extinguishing must win outright over igniting - a gas neighbour "
        "must not catch fire in the same step the fire that would have "
        "lit it was put out");
}

static void test_fire_burns_out_and_disappears_over_time(void)
{
    fixture();
    sand_set_decay(&s, 255);   /* forced 100% chance, for a fast, exact
                                * test rather than waiting out the real
                                * material figure's low per-step odds */

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 3, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1,
        CELL_VARIANT(sand_at(&s, 3, 3)),
        "a freshly spawned fire cell must start at full life, not a "
        "random shade - random_cell() already generalises this for any "
        "decay != 0 material, gas included");

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "fire must burn out to nothing given enough time, the same "
        "decay mechanism gas already uses");
}

/* Supersedes the old test_fire_does_not_move_under_gravity: fire was
 * KIND_STATIC and this test asserted the opposite of what it asserts
 * now. Kept under the same topic heading rather than silently deleted,
 * mirroring how sand_reactions.c's own top comment narrates the
 * KIND_STATIC-era design as history worth keeping visible. Mirrors
 * test_gas_rises_straight_up_under_ordinary_gravity exactly - fire is
 * kind = KIND_GAS now, swept by the identical pass. */
static void test_fire_rises_and_disperses_like_gas(void)
{
    fixture();
    sand_set(&s, 3, H - 1, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, fire moves up - the same "
        "kind = KIND_GAS movement gas already has, replacing the "
        "immobile-ember behaviour this test used to assert");
}

/* Supersedes the old test_fire_is_not_displaced_by_falling_sand, for
 * the same reason as the test above - mirrors test_sand_sinks_through_gas
 * exactly, since fire's displacement rules are now identical to gas's
 * (density-based, not KIND_STATIC's blanket refusal). Burying it
 * completely is a different question - see
 * test_fire_is_smothered_when_fully_buried below, which is the actual
 * replacement for "sand can put fire out", just via smothering rather
 * than simple contact. */
static void test_sand_sinks_through_fire(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, FIRE);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than fire (60 > 15), so it must sink all the "
        "way through rather than be blocked by it - a single touch does "
        "not smother fire, it just passes through uneventfully");
}

static void test_fire_is_smothered_when_fully_buried(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);   /* above */
    sand_set(&s, 3, 4, STONE);   /* below */
    sand_set(&s, 2, 3, STONE);   /* left */
    sand_set(&s, 4, 3, STONE);   /* right */

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 3)),
        "fire buried on all four sides by something denser must smother "
        "out - the only way sand puts fire out, since a single touch "
        "just lets sand sink through uneventfully (see "
        "test_sand_sinks_through_fire above)");
}

static void test_fire_is_not_smothered_with_a_gap(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);   /* above */
    sand_set(&s, 2, 3, STONE);   /* left */
    sand_set(&s, 4, 3, STONE);   /* right */
    /* Diagonal-up neighbours also need blocking, not just the straight
     * cardinal ones - try_slide()'s own fallback would otherwise carry
     * fire diagonally out of (3,3) via the two open corners before
     * reactions ever ran, leaving the cell empty for a reason that has
     * nothing to do with smothering (confirmed: this is exactly what
     * happened the first version of this test wrote). */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    /* (3,4), below, deliberately left open - a KIND_GAS material only
     * rises and spreads sideways, never falls, so this gap is safe
     * from being closed by fire itself drifting into it before
     * reactions checks smothering; the test stays a clean check of the
     * smother predicate alone. */

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "one open side is enough for air to reach it - smothered() "
        "requires ALL four neighbours to be denser, not just three");
}

static void test_fire_is_not_smothered_by_gas(void)
{
    fixture();
    sand_set_mobility(&s, 0);   /* keep fire (and the surrounding gas)
                                 * from rising away before reactions
                                 * checks smothering this same step -
                                 * equalise_gas()'s own spread sub-pass
                                 * never touches these neighbours anyway
                                 * (different material, not empty), so
                                 * this alone is enough to pin fire */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, GAS);
    sand_set(&s, 3, 4, GAS);
    sand_set(&s, 2, 3, GAS);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "gas is not denser than fire (10 < 15), so a gas-only surround "
        "must not smother it - otherwise any sufficiently large, dense "
        "pocket of fire/gas would extinguish itself from the inside "
        "out");
}

static void test_liquid_wins_over_smothering(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 4, 3, STONE);
    /* The two upward diagonals also need blocking, not just the three
     * cardinal sides above - the straight-up cell being blocked leaves
     * try_slide()'s own diagonal fallback free to carry fire out to
     * (2,2) or (4,2) before reactions ever runs, now that the assertion
     * below checks WHAT fire became rather than merely that (3,3) ended
     * up empty (which a fire that drifted away would also satisfy,
     * masking exactly this - confirmed: this is what the first version
     * of this test, without these two lines, actually did). Mirrors
     * test_fire_is_not_smothered_with_a_gap's identical reasoning. */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 3, 4, WATER);
    /* Floor plus both down-diagonal neighbours, not the floor alone -
     * WATER here is CELL_MAKE(MAT_WATER, 8), not a full MASS_MAX cell,
     * and move_liquid_grain() can hand its entire mass to a single open
     * down-diagonal in one main-sweep call, draining (3,4) before
     * reactions ever gets a turn to see it - see
     * test_creating_steam_arms_the_gas_pass's identical fix for the
     * full reasoning. */
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 2, 5, STONE);
    sand_set(&s, 4, 5, STONE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "fire touching water on even one side must extinguish via the "
        "liquid rule (becoming steam, not simply vanishing - see "
        "reaction_t.quench_to) - smothered() itself would have said no "
        "here too (it explicitly excludes liquid neighbours from "
        "counting towards a smother, even though water is denser than "
        "fire), so this confirms that exclusion does not accidentally "
        "block the liquid path from still working");
}

static void test_igniting_a_neighbour_marks_its_row_dirty(void)
{
    dirty_fixture();
    /* Box gas in on every side except where it touches fire below, so it
     * cannot drift away via its OWN rise or spread pass (both run before
     * reactions, in the same step) before reactions gets to check it. A
     * straight-up ceiling alone blocks the plain rise but leaves the
     * diagonal slide fallback free to move it up-left/up-right instead
     * (which is exactly what happened the first time this test was
     * written with only the straight-up cell blocked) - and a ceiling
     * with no side walls at all leaves the perpendicular spread pass
     * free to walk it sideways out of column 3 entirely (the failure
     * before that). All three of straight-up, both diagonals-up, and
     * both sideways neighbours need blocking - everywhere except where
     * fire sits, directly below. */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 4, 3, STONE);
    sand_set(&s, 3, 4, FIRE);
    sand_set(&s, 3, 3, GAS);        /* directly above fire - a DIFFERENT
                                      * row from fire's own, so this test
                                      * can tell whether mark_rows()
                                      * targeted the ignited neighbour's
                                      * row specifically, not just the
                                      * fire cell's */
    memset(dirty, 0, sizeof(dirty));

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "setup: the gas neighbour must have ignited");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[3],
        "the row the newly-ignited neighbour is in must be marked dirty, "
        "or its cell changes colour on the panel without ever being "
        "redrawn - ignition's mark_rows() call must target the "
        "neighbour's row, not just the fire cell's own");
}

static void test_fire_burning_out_marks_its_row_dirty(void)
{
    dirty_fixture();
    sand_set_decay(&s, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 4, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    memset(dirty, 0, sizeof(dirty));

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "setup: the fire cell must have burned out by now");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "the row a fire cell burned out in must be marked dirty, or its "
        "last colour stays on the panel forever after the cell itself is "
        "already empty - tick_decay()'s vanish branch already does this "
        "correctly (shared with gas), this pins it down for fire too");
}

static void test_fire_spreads_through_a_connected_pocket_in_one_step(void)
{
    fixture();
    sand_set(&s, 0, 0, FIRE);
    for (int x = 1; x < W; x++) {
        sand_set(&s, x, 0, GAS);
    }

    sand_step(&s, 0, 1000, 0);

    for (int x = 1; x < W; x++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE,
            CELL_MATERIAL(sand_at(&s, x, 0)),
            "a straight line of gas laid out AHEAD of the reactions "
            "pass's own fixed scan direction (row-major, left to right) "
            "must ignite all the way through in a single step - the "
            "confirmed explosion-like cascade, not creeping spread. A "
            "line laid out BEHIND the scan direction would need several "
            "steps instead - a documented, accepted scan-order artifact, "
            "not a bug (see sand_reactions.c's own top comment)");
    }
}

static void test_pouring_stone_never_arms_the_reactions_pass(void)
{
    fixture();

    /* No public getter for may_have_burning - reading the field directly
     * is intentional here. The bug this guards (gating on kind ==
     * KIND_STATIC instead of the material ID, in sand_set()/
     * try_spawn_one()) is only externally visible as a silent
     * performance regression on device - a host test can only catch it
     * by checking the bookkeeping directly, there is no behavioural
     * difference in simulation output to assert on instead. */
    for (int i = 0; i < 20; i++) {
        sand_spawn(&s, 3, 3, 2, MAT_STONE);
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_FALSE_MESSAGE(s.may_have_burning,
            "placing plain stone must never set may_have_burning - stone "
            "shares KIND_STATIC with fire and ember, so gating on kind "
            "instead of the material ID would silently re-arm a "
            "full-grid reactions scan on every stone touch");
    }
}

static void test_placing_fire_arms_both_gas_and_fire_passes(void)
{
    fixture();

    /* Guards the else-if ordering bug found before this shipped: fire is
     * BOTH kind == KIND_GAS (needs sand_step_gas() to rise/disperse) AND
     * reactions[].burns (needs sand_step_reactions() to ignite/
     * extinguish/burn out) at once. sand_set()/try_spawn_one() used to
     * test these as an else-if chain with the kind check first, which
     * would shadow the burns check for every fire cell the moment fire's
     * kind became KIND_GAS - may_have_burning would silently never get
     * set, and a freshly painted fire spark would rise correctly
     * (kind-generic, unaffected) but never ignite, extinguish, or burn
     * out. No public getter for either flag - reading them directly is
     * intentional, mirroring test_pouring_stone_never_arms_the_reactions_pass's
     * own justified exception. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 3, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_gas,
        "a directly-placed fire cell must arm may_have_gas - it needs "
        "sand_step_gas() to rise and disperse");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_burning,
        "a directly-placed fire cell must ALSO arm may_have_burning - it "
        "needs sand_step_reactions() to ignite/extinguish/burn out, and "
        "an else-if chain that checks kind == KIND_GAS first would "
        "shadow this branch entirely");
}

/* --- wood, embers and steam ---------------------------------------------- */

/* Sets up a fire cell at (3,3) beside a wood cell at (4,3), boxed just
 * enough to keep the fire from smothering itself or drifting away
 * before reactions gets a turn.
 *
 * fire_room() (used by the plain-fire tests above) will not do here: it
 * seals a fire cell in on all four sides with stone, which works for a
 * GAS neighbour (density 10, lighter than fire's 15, so it never counts
 * towards smothered()'s ALL-of-4 test) but not for a WOOD one - wood's
 * density (150) is, unlike gas's, denser than fire, so a wood neighbour
 * on the one remaining open side completes the smother all by itself,
 * clearing the fire before it ever reaches the ignite loop (confirmed:
 * this is exactly what the first version of these two tests did). The
 * cell below fire is deliberately left open instead, mirroring
 * test_fire_is_not_smothered_with_a_gap's identical reasoning: gas only
 * ever rises or spreads sideways, never falls, so leaving it empty
 * cannot let fire drift away before this step's reactions pass runs,
 * and it is one fewer denser neighbour, so smothered() reads false. */
static void wood_ignition_room(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, WOOD);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
}

static void test_wood_does_not_catch_instantly(void)
{
    wood_ignition_room();

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WOOD, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "at the real per-material flammability (6 in 256), wood touching "
        "fire for a single step must almost always still be wood - a fire "
        "that catches instantly defeats the whole point of a slow-burning "
        "fuel");
}

static void test_wood_eventually_catches_and_becomes_an_ember(void)
{
    wood_ignition_room();
    sand_set_flammability(&s, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&s, 4, 3)),
        "wood forced to catch (flammability=255) must char into an "
        "ember, not flash straight to MAT_FIRE - fire is KIND_GAS and "
        "would float away on the very next gas pass, dissolving the log "
        "instead of letting it burn in place (see sand_reactions.c's top "
        "comment for the full reasoning)");
}

static void test_an_ember_does_not_rise(void)
{
    fixture();
    sand_set(&s, 3, H - 1, EMBER);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&s, 3, H - 1)),
        "a burning log is KIND_STATIC, unlike fire - it must stay exactly "
        "where it was placed rather than rising the way fire (KIND_GAS) "
        "does");
}

static void test_an_ember_burns_out_over_time(void)
{
    fixture();
    sand_set_decay(&s, 255);

    sand_set(&s, 3, 3, EMBER);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s),
        "setup: exactly one burning log placed");

    /* Twice an ember's own full life, not once: reaction_t.flare (no test
     * override exists for it - see the note on why smoke/flare get none)
     * can spawn a fresh MAT_FIRE cell on any step the ember is still
     * alive, and that fire cell gets its own full MATERIAL_VARIANTS-1
     * decay budget starting from whenever it was born - worst case, on
     * the ember's very last living step. This gives both budgets room to
     * run out even then, so the test is not at the mercy of exactly when
     * (if ever) the flare roll happens to hit. */
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1); i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "an ember, and anything it flared into fire along the way, must "
        "burn out to nothing given enough time - the same decay "
        "mechanism fire and gas already use");
}

static void test_an_ember_flares_fire_into_an_empty_neighbour(void)
{
    fixture();
    /* Decay left off (the default) - see material.c's own comment on
     * wood's burn_decay figure and test_an_ember_burns_out_over_time
     * above; this test wants an ember that lives long enough to get many
     * tries at the flare roll, not one racing its own burn-out. */
    sand_set(&s, 3, 3, EMBER);

    bool found_fire = false;
    for (int i = 0; i < 200 && !found_fire; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_fire; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_FIRE) {
                    found_fire = true;
                }
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_fire,
        "an ember must eventually flare a MAT_FIRE cell into an empty "
        "neighbour - at 48 in 256 per step, 200 steps is comfortably "
        "enough that never seeing one means try_flare() is broken, not "
        "unlucky");
}

static void test_quenching_costs_the_water_a_unit_of_mass(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, CELL_MAKE(MAT_WATER, MASS_MAX));

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX - 1,
        CELL_VARIANT(sand_at(&s, 4, 3)),
        "the liquid neighbour that quenches a burning cell must lose "
        "exactly one unit of its own mass, not the whole cell - a fire "
        "should cost a pot a sip of water per step boiled, not a gulp");
}

static void test_steam_rises_and_disperses(void)
{
    fixture();
    sand_set(&s, 3, H - 1, STEAM);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM,
        CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, steam rises - the same "
        "KIND_GAS movement gas and fire already have");
}

static void test_creating_steam_arms_the_gas_pass(void)
{
    fixture();
    sand_set_mobility(&s, 0);   /* keep fire from rising away before
                                 * reactions quenches it this same step -
                                 * mirrors test_fire_is_not_smothered_by_gas's
                                 * own use of this technique */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 4, WATER);
    /* Floor plus both down-diagonal neighbours, not the floor alone:
     * move_liquid_grain() tries down, then down-the-slope both ways, and
     * a mostly-empty cell (WATER here is CELL_MAKE(MAT_WATER, 8), not a
     * full MASS_MAX) can hand its ENTIRE mass to a single open diagonal
     * in one main-sweep call, draining (3,4) completely before
     * reactions ever gets a turn to check it for a liquid neighbour
     * (confirmed: this is exactly what the first version of this test,
     * with only the floor blocked, actually did). */
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 2, 5, STONE);
    sand_set(&s, 4, 5, STONE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "setup: quenching must have produced steam, with no other gas "
        "anywhere on the grid that could accidentally arm may_have_gas "
        "some OTHER way and mask the bug this test exists to catch");

    sand_set_mobility(&s, 255);   /* steam's own turn to rise, forced
                                   * deterministic the same way every
                                   * other single-step gas-movement test
                                   * in this suite is */
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 2)),
        "steam created by place_reacted() must actually be able to rise "
        "on its very next chance - if may_have_gas was not latched for "
        "it, sand_step_gas() early-returns and the cell sits frozen on "
        "the grid forever, a bug this test's empty-grid setup is built "
        "specifically to catch (see place_reacted()'s own comment in "
        "sand_reactions.c)");
}

static void test_burnt_out_fire_can_leave_smoke(void)
{
    fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);   /* keep every fire cell pinned in place
                                 * rather than rising or spreading into
                                 * whatever gaps open up as neighbours
                                 * burn out around it - not required for
                                 * correctness (a drifting fire cell
                                 * still burns out and can still leave
                                 * smoke wherever it ends up), but it
                                 * keeps this deterministic rather than
                                 * merely probable */

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, FIRE);
        }
    }

    /* Checked after EVERY step, not just once at the end, and for a
     * reason worth spelling out: s->decay is a single override that
     * applies to every material at once (see sand_set_decay()), not
     * just fire, so a freshly created steam cell is JUST as forced to
     * decay away as the fire that made it - it does not get to sit
     * still and wait to be inspected. sand_set(FIRE) here (unlike
     * sand_spawn()) also skips random_cell()'s "fresh transient starts
     * at full life" rule, so these cells start at CELL_MAKE(..., 8), not
     * 15 - they burn out around step 8, not step 15. A loop that ran a
     * comfortable margin PAST that (as
     * test_an_ember_burns_out_over_time's does, deliberately, to give a
     * late flare room to also finish decaying) would just as
     * deliberately give any steam created around step 8 enough of that
     * same margin to fully decay away AGAIN before the check ever runs -
     * the opposite of comfortable here. Checking every step catches
     * steam the moment it exists, however long it goes on to live. */
    bool found_steam = false;
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1) && !found_steam; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_steam; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SMOKE) {
                    found_steam = true;
                    break;
                }
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_steam,
        "a whole grid of fire burning out at once (40 in 256 smoke "
        "chance per cell) must leave at least one MAT_SMOKE cell behind "
        "- not seeing a single one across 64 cells means smoke is "
        "broken, not unlucky");
}

/* The two byproducts must not be the same material, asserted directly
 * rather than left implied by the two tests either side of it.
 *
 * They WERE one material - MAT_STEAM did both jobs - and sharing the row
 * was defensible right up until you watched it: a fire dying in mid-air
 * with no water within reach puffing bright white kettle-steam reads as
 * a bug, because the player can see there was nothing there to boil.
 * This pins the split so nobody re-merges them on the entirely correct
 * observation that the two materials[] rows are nearly identical - the
 * difference that matters is the palette, not the physics. */
/* Relative luminance of a rendered cell, 0-255. The palette stores
 * panel-ready (byte-swapped) RGB565 - see gfx_color.h - so this undoes
 * both before weighting the channels the way an eye does. */
static int cell_luminance(cell_t c)
{
    const gfx_color_t p = material_palette()[c];
    const unsigned v = (unsigned)(((p & 0xFF) << 8) | ((p >> 8) & 0xFF));
    const int r = (int)((v >> 11) & 0x1F) * 255 / 31;
    const int g = (int)((v >> 5)  & 0x3F) * 255 / 63;
    const int b = (int)(v         & 0x1F) * 255 / 31;
    return (r * 30 + g * 59 + b * 11) / 100;
}

/* The palette test the two-material split exists for.
 *
 * MAT_STEAM and MAT_SMOKE are near-identical rows in materials[] - the
 * ONLY thing that makes them worth being two materials is that a player
 * can tell them apart on sight. That makes their palettes load-bearing
 * rather than decorative, which is unusual enough here to be worth
 * asserting: everything else in this suite tests behaviour, and a future
 * palette tweak that quietly collapsed these two back into the same
 * range would break the feature while passing every other test in the
 * file.
 *
 * Two separate properties, and the second is the one that is easy to
 * lose. Equal-life brightness ordering is the obvious one. Non-overlap
 * of the whole RANGES is the subtle one: a puff of smoke is caught at
 * whatever point in its life you happen to look at it, so "fresh smoke
 * is dimmer than dying steam" is what actually guarantees no cell is
 * ever ambiguous. The first draft of this palette had the first
 * property and not the second. */
static void test_steam_and_smoke_are_told_apart_by_brightness(void)
{
    int smallest_gap = 255;
    for (int life = 0; life < MATERIAL_VARIANTS; life++) {
        const int steam = cell_luminance(CELL_MAKE(MAT_STEAM, life));
        const int smoke = cell_luminance(CELL_MAKE(MAT_SMOKE, life));
        if (steam - smoke < smallest_gap) {
            smallest_gap = steam - smoke;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(60, smallest_gap,
        "at equal life, steam must be clearly brighter than smoke at "
        "every one of the sixteen variants - measured at 89 when this "
        "was written, held to a looser 60 so ordinary palette tuning "
        "does not trip it but a collapse of the two ranges does");

    const int freshest_smoke = cell_luminance(CELL_MAKE(MAT_SMOKE,
                                                        MATERIAL_VARIANTS - 1));
    const int dying_steam    = cell_luminance(CELL_MAKE(MAT_STEAM, 1));

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(dying_steam, freshest_smoke,
        "and the two ranges must not overlap AT ALL: the brightest smoke "
        "there is must still be dimmer than the faintest steam, or a puff "
        "caught at the wrong moment of its life is ambiguous - which "
        "defeats the entire reason these are two materials rather than "
        "one");
}

/* --- bubbles: gas rising through standing liquid ---------------------- */

/* A sealed column of full-mass water in columns 2..5, rows 2..6, with a
 * stone floor and stone walls, so the water can neither drain nor spread
 * and a gas cell placed inside it has nowhere to go except up THROUGH the
 * water. Without that seal a gas cell just slips out sideways and the
 * test proves nothing about bubbling. */
static void water_column(void)
{
    fixture();
    sand_set_decay(&s, 0);   /* immortal: these tests measure movement,
                              * and a decaying cell that vanished
                              * mid-rise would read as "never escaped" */
    for (int y = 1; y <= 7; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
    }
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, 7, STONE);
    }
    for (int y = 2; y <= 6; y++) {
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

static int first_row_holding(uint8_t id)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == id) {
                return y;
            }
        }
    }
    return -1;
}

static long mass_held_by(uint8_t id)
{
    long m = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == id) {
                m += CELL_VARIANT(c);
            }
        }
    }
    return m;
}

/* The behaviour try_bubble() exists for, and the one this simulation could
 * not do at all before it.
 *
 * can_enter() only lets a DENSER mover displace a lighter target, and a
 * liquid never consults it anyway (room_in() refuses a cell holding any
 * other material). Between them, a steam cell under standing water had no
 * legal move in EITHER direction and sat frozen there forever - which is
 * exactly what it looked like on the device: a boiler that made steam and
 * then held onto it. */
/* --- oil and lava ----------------------------------------------------- */

/* A stone basin holding a pool of oil `depth` cells deep in columns
 * 2..5, with open air above it, and returns the row the surface sits on.
 * Open above on purpose, unlike fire_room(): these tests need a flame to
 * be able to sit ON the pool. */
static int oil_pool(int depth)
{
    fixture();
    sand_set_decay(&s, 0);
    const int floor = 6;
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, floor, STONE);
    }
    for (int y = floor - depth; y < floor; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    return floor - depth;
}

/* The rule that makes a slick burn instead of detonate.
 *
 * Without reaction_t.needs_air, one spark lights every cell of a
 * connected pool inside a single pass - this file's reactions scan
 * propagates ignition through a whole pocket in one step (see
 * sand_reactions.c's top comment), which is a fuel-air bomb, not a
 * slick. With it, only cells touching air can catch, so the interior of
 * the pool is untouchable until the layer above it has burned off. */
static void test_only_the_exposed_surface_of_an_oil_pool_can_ignite(void)
{
    const int surface = oil_pool(4);
    sand_set(&s, 3, surface - 1, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_OIL,
        CELL_MATERIAL(sand_at(&s, 3, surface + 2)),
        "an oil cell buried under more oil must NOT ignite, however "
        "much of the surface is alight - a pool burns off its top, it "
        "does not go up all at once");
}

/* The other half of needs_air, and the bug that shipped in the first
 * draft of it.
 *
 * "Exposed" originally meant "has an EMPTY cardinal neighbour", which is
 * wrong in the one situation that matters: a flame sitting on the pool is
 * not empty space, so the surface stopped counting as exposed at exactly
 * the moment it caught fire, and a slick with a fire blob parked on it
 * burned for thirty steps without ever lighting. Air has to include
 * gases. */
static void test_oil_ignites_with_a_flame_sitting_directly_on_it(void)
{
    const int surface = oil_pool(3);
    /* Cover the whole surface with fire, so no oil cell has any EMPTY
     * neighbour left at all - the exact case the first version failed. */
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, surface - 1, FIRE);
    }

    /* Re-laid every step, because a single flame does not stay put long
     * enough to react: fire is KIND_GAS, so sand_step_gas() lifts it away
     * during the SAME step it was placed, before sand_step_reactions()
     * ever runs. Holding the fire brush down is exactly this, and it is
     * how the app produces the situation in the first place. */
    bool caught = false;
    for (int i = 0; i < 40 && !caught; i++) {
        for (int x = 2; x <= 5; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, surface - 1))) {
                sand_set(&s, x, surface - 1, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        caught = CELL_MATERIAL(sand_at(&s, 3, surface)) != MAT_OIL;
    }

    TEST_ASSERT_TRUE_MESSAGE(caught,
        "oil with a flame resting on it must catch - a fire neighbour "
        "is a KIND_GAS cell, not an empty one, and touches_air() has to "
        "count gases as air or the surface is declared unexposed "
        "precisely when it is on fire");
}

/* Oil is fuel, so it must not also be an extinguisher. neighbor_quenches()
 * runs before ignition and returns outright, so getting this wrong does
 * not merely weaken the effect - it inverts it, and oil becomes the best
 * fire suppressant in the simulation. */
static void test_oil_does_not_put_fire_out()
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, OIL);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_EMPTY, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a fire touching OIL must not be extinguished - only liquids "
        "that are neither fuel nor a heat source quench");
}

/* And water must still work exactly as it did, which is the thing the
 * new rule could most easily have broken. */
static void test_water_still_puts_fire_out(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "water is neither fuel nor a heat source, so it must still "
        "quench on one touch - and still turn the fire to steam");
}

/* Two liquids of different densities have to sort themselves out, which
 * nothing before oil required. room_in() refuses a cell holding another
 * material and a liquid never consults can_enter(), so without
 * sink_through_lighter_liquid() the two simply block each other and oil
 * trapped under water stays there forever. */
/* Viscosity: liquids used to read no rate field at all, so every liquid
 * flowed at exactly the same speed and oil behaved like coloured water.
 * material.h's `mobility` is that rate - read by a gas as buoyancy and by
 * a liquid as viscosity inverted.
 *
 * Measured as steps for a tall column to reach the far wall: water 8,
 * oil 28 when this was written. Held to "oil takes at least twice as
 * long" so ordinary tuning does not trip it. */
/* Every liquid must DECLARE a mobility, checked in the table rather than
 * in motion.
 *
 * This exists because one did not. `mobility` arrived as a gas-only field
 * and only grew a liquid reader later, so lava's row - written before
 * that - left it unset, and an unset byte is zero.
 *
 * The obvious test is behavioural: place a column and assert it spreads.
 * That was written first and it does not work, which is worth recording.
 * A mobility of zero does not actually freeze a liquid, because the
 * wall-rebound splash moves liquid without consulting the gate - lava at
 * zero still crossed the same distance, in 249 steps against 20. Any
 * budget loose enough not to be flaky is loose enough to let that pass,
 * and any budget tight enough to catch it is pinning a performance figure
 * rather than an invariant.
 *
 * So this checks the table instead. Nonzero is not a claim about the
 * right value - only that somebody chose one, which is precisely the step
 * that got skipped. */
/* --- acid ---------------------------------------------------------------- */

/* Cells, not mass: a powder's variant is a shade, so summing it would be
 * meaningless for sand. */
static int count_cells_of(uint8_t id)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == id) {
                n++;
            }
        }
    }
    return n;
}

/* A sealed GLASS tank with `sand_rows` of sand in the bottom and
 * `acid_rows` of acid above it. Returns the acid mass placed.
 *
 * Glass, not stone. Stone used to be immune to acid and was therefore the
 * only thing acid could be kept in; it dissolves like everything else now
 * and glass is the sole exception. These fixtures were stone until that
 * changed, and both tests below started failing the moment it did - which
 * is the feature working, not a break. */
static long acid_tank(int sand_rows, int acid_rows)
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
    for (int y = H - 1 - sand_rows; y < H - 1; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    for (int y = 1; y <= acid_rows; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
    return mass_held_by(MAT_ACID);
}

static void test_acid_dissolves_sand(void)
{
    acid_tank(2, 2);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, count_cells_of(MAT_SAND),
        "setup: there must be sand to eat");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SAND),
        "acid must eat the sand it settles onto");
}

/* Glass is what acid can be kept in, and the only thing left that is.
 *
 * Stone held that role until acid learned to eat it. Moving the job to a
 * material you have to MAKE - sand plus sustained heat - is the point of
 * the change: acid is now dangerous to everything the level is built out
 * of, and a container is something you earn rather than something you
 * already had. */
static void test_acid_does_not_dissolve_its_container(void)
{
    acid_tank(2, 2);
    const int walls = count_cells_of(MAT_GLASS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, walls,
        "setup: the tank must actually be made of glass");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(walls, count_cells_of(MAT_GLASS),
        "acid must not touch glass - it is the one material that resists, "
        "and therefore the only thing acid can be held in");
}

/* The other half, and the reason glass has a job at all. */
static void test_acid_eats_through_stone(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, STONE);
    }
    const int before = count_cells_of(MAT_STONE);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, before, "setup: a stone floor");
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_ACID, MASS_MAX));
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(before, count_cells_of(MAT_STONE),
        "acid must eat into stone - stone stopped being the acid-proof "
        "material when glass took that role, and a stone wall that still "
        "held would leave glass with nothing to do");
}

/* ===================================================================
 * Glass as a material with a temperature: the heat ramp, cooling, and
 * thermal shock against snow.
 * =================================================================== */

/* The hottest glass cell anywhere, or 0 if there is no glass at all. */
static int hottest_glass(void)
{
    int hot = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) > hot) {
                hot = CELL_VARIANT(c);
            }
        }
    }
    return hot;
}

/* A pane with lava held against its underside, for `steps` steps. */
static void hold_lava_under_a_pane(int steps)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);
    }
    for (int i = 0; i < steps; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
    }
}

/* One more step of the scene hold_lava_under_a_pane() built, lava topped
 * back up. Separate so a caller can soak to a condition instead of to a
 * step count. */
static void hold_lava_under_a_pane_again(void)
{
    for (int x = 1; x < W - 1; x++) {
        if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
            sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }
    sand_step(&s, 0, 1000, 0);
}

/* Heat ACCUMULATES in the pane rather than transforming it on contact.
 *
 * This is the difference between `heat_ramp` and the `heat_chance` sand
 * uses, and it is the whole reason glass melting can mean "long exposure"
 * at all: a per-step roll has no memory, so under it a brief fierce flame
 * and a long slow one are the same event with different luck. Banking the
 * exposure in the cell is what lets duration be a real requirement.
 *
 * Asserted as "still glass, but changed" rather than on a specific level,
 * because the level is a race between the ramp and cooling and pinning it
 * would make this a test of the RNG. */
static void test_glass_banks_heat_rather_than_melting_on_contact(void)
{
    hold_lava_under_a_pane(60);

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_GLASS) > 0,
        "a brief touch of lava must not melt glass outright - if it does, "
        "the ramp is not being consulted and heat is transforming on "
        "contact the way it does for sand");
    TEST_ASSERT_TRUE_MESSAGE(hottest_glass() > 0,
        "and it must have GAINED heat while being touched, or nothing is "
        "accumulating and the pane is simply immune");
}

/* Held long enough, the same fire wins and the pane runs. */
static void test_a_fire_held_long_enough_melts_glass_to_lava(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);
    }

    int melted = 0;
    for (int i = 0; i < 4000 && !melted; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
        melted = count_cells_of(MAT_GLASS) == 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(melted,
        "lava held against a pane must eventually melt it. Cooling faster "
        "than the ramp climbs would make this never happen for any fire of "
        "any size, which is an easy thing to do by accident with two "
        "constants that pull opposite ways");
}

/* Take the fire away and the heat drains back out.
 *
 * This is the half of the mechanism that makes the ramp mean DURATION
 * rather than lifetime total. Without it a pane remembers every flame it
 * ever met, so a candle lit for one step a day melts it just as surely as
 * a furnace - the exposure simply accumulates forever. */
static void test_glass_forgets_a_fire_that_went_out(void)
{
    /* Soaked in short bursts up to a CONDITION rather than for a fixed
     * count: the ramp is fast enough now that a constant long enough to
     * heat the pane on a slow build melts it outright on this one, and a
     * fixture that destroys its own subject reports on nothing. */
    hold_lava_under_a_pane(1);
    for (int i = 0; i < 400 && hottest_glass() <= SAND_AMBIENT_HEAT + 2; i++) {
        hold_lava_under_a_pane_again();
    }
    const int peak = hottest_glass();
    TEST_ASSERT_TRUE_MESSAGE(peak > 0,
        "fixture check: the pane has to be hot before cooling it means "
        "anything");

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_LAVA) {
                sand_set(&s, x, y, 0);
            }
        }
    }
    for (int i = 0; i < 3000 && hottest_glass() > SAND_AMBIENT_HEAT; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "with the fire gone the pane must drain all the way back to room "
        "temperature - which is SAND_AMBIENT_HEAT, not 0, since 0 now means "
        "frosted");
}

/* And it drains on a board where nothing is burning and nothing dissolves.
 *
 * Separate from the test above because it fails differently: the reactions
 * pass returns early unless something wants it, and hanging heat off the
 * fire flag would leave a pane frozen at whatever level the fire left it -
 * cooling forever pending a fire that is, by definition, already out. */
static void test_glass_cools_on_a_board_with_no_fire_at_all(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1, hottest_glass(),
        "fixture check: the pane starts at the top of its ramp");

    for (int i = 0; i < 3000 && hottest_glass() > SAND_AMBIENT_HEAT; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "hot glass must cool with no fire and no acid anywhere on the "
        "board - temperature is its own reason to run the reactions pass");
}

/* Glass made by heat arrives COLD.
 *
 * place_reacted() gives a new cell a full variant, which is right for a
 * liquid (a full one) and for a transient (a fresh one) and would be
 * catastrophic here: sand fusing under a flame would produce a pane
 * already at the top of its melt ramp, and the next step would run it to
 * lava. Sand under a steady fire would reach lava in two ticks and the
 * duration this whole mechanism exists to express would be unreachable. */
static void test_freshly_fused_glass_starts_cold(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }

    int made = 0;
    for (int i = 0; i < 2000 && !made; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        made = count_cells_of(MAT_GLASS) > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(made,
        "fixture check: fire over sand has to make some glass");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest_glass(),
        "glass fused out of sand must start at room temperature - starting "
        "full would melt it to lava on the following step, and starting at "
        "0 would hand the player a pane that begins life frosted");
}

/* Snow on a glowing pane cracks it, and it goes back to being sand.
 *
 * Thermal shock needs a gradient, and a gradient needs something the
 * player can SEE is cold. An earlier draft used water for the cold side,
 * which works as a rule and fails as a design: nothing in this simulation
 * says water is cold, so a pane cracking beside it reads as "glass breaks
 * near water" rather than as a temperature difference. */
static void test_snow_shatters_a_glowing_pane_into_sand(void)
{
    fixture();
    const int panes = W - 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, SNOW);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* ALL of them, and it takes one step. This asked for "most" while
     * shock had to win a cooling roll first: the roll that decided whether
     * to crack was the same roll that cooled the pane, so a drift often
     * talked a pane down below the threshold instead of breaking it, and a
     * bank eating itself from its own meltwater could run a cell short.
     * Contact alone is enough now. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "snow touching panes above the shock threshold must shatter every "
        "one of them");
    TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_SAND),
        "and shattered glass must come back as SAND - that is what closes "
        "the loop heat opened, so the player can un-make the material");
}

/* The threshold is sharp, and it is the ONLY thing that decides.
 *
 * One level below it a pane is untouchable and one level above it breaks
 * on contact, which is a strange rule to have unless the player can see
 * which side of the line a pane is on - which is what the colour test
 * below is for. The two belong together: this one fixes the behaviour to
 * SAND_SHOCK_HEAT, that one fixes the appearance to the same number. */
static void test_the_shock_threshold_is_exact(void)
{
    const int panes = W - 2;

    for (int heat = SAND_SHOCK_HEAT - 1; heat <= SAND_SHOCK_HEAT; heat++) {
        fixture();
        sand_clear(&s);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int x = 1; x < W - 1; x++) {
            sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, (uint8_t)heat));
            sand_set(&s, x, H - 3, SNOW);
        }

        for (int i = 0; i < 60; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        if (heat < SAND_SHOCK_HEAT) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_GLASS),
                "a pane one level BELOW the shock threshold must survive "
                "snow - it only cools, which is what makes the threshold "
                "mean something");
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
                "and a pane exactly AT the threshold must break");
        }
    }
}

/* And the biggest colour change in glass's ramp is at that same level.
 *
 * Glass is the one material whose variant the player has to be able to
 * read, because it is the only one where the variant changes what the
 * material DOES rather than how it looks. A smooth ramp hid that: a pane
 * at 5 and a pane at 6 behave completely differently and looked nearly
 * identical, so pouring snow on a basin that was not quite hot enough
 * produced no reaction and no explanation for it.
 *
 * Asserted as "the largest step in the ramp", not "these two colours
 * differ", because any two entries of a gradient differ. The claim worth
 * defending is that this break is the one you notice. */
static void test_glass_looks_different_at_the_shock_threshold(void)
{
    const gfx_color_t *pal = material_palette();

    int gap[MATERIAL_VARIANTS] = { 0 };
    for (int v = 1; v < MATERIAL_VARIANTS; v++) {
        const gfx_color_t a = pal[MAT_GLASS * MATERIAL_VARIANTS + v - 1];
        const gfx_color_t b = pal[MAT_GLASS * MATERIAL_VARIANTS + v];
        /* Stored byte-swapped for the panel - see GFX_RGB in gfx_color.h. */
        const uint16_t ua = (uint16_t)((a >> 8) | (a << 8));
        const uint16_t ub = (uint16_t)((b >> 8) | (b << 8));
        const int dr = ((ua >> 11) & 0x1F) - ((ub >> 11) & 0x1F);
        const int dg = ((ua >>  5) & 0x3F) - ((ub >>  5) & 0x3F);
        const int db = ( ua        & 0x1F) - ( ub        & 0x1F);
        gap[v] = (dr < 0 ? -dr : dr) * 2 +
                 (dg < 0 ? -dg : dg) +
                 (db < 0 ? -db : db) * 2;
    }

    int widest = 1;
    for (int v = 2; v < MATERIAL_VARIANTS; v++) {
        if (gap[v] > gap[widest]) {
            widest = v;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_SHOCK_HEAT, widest,
        "the biggest colour change along glass's ramp has to land exactly "
        "where its behaviour changes - a pane that snow will shatter must "
        "not look like one that snow will merely cool");
}

/* A resting pane beside snow gets COLDER, and shows it.
 *
 * The report that produced this: "I still don't see any colour change of
 * glass when near snow." There was none to see, and it was not a rendering
 * problem. Ambient used to be 0, the bottom of the variant range, so a
 * pane at rest had nothing to lose - chilling it changed no number, so it
 * changed no colour, and snow beside glass was indistinguishable from snow
 * beside nothing.
 *
 * Two things had to change for this to be observable, and both are
 * asserted here: room temperature had to move off the floor so cold has
 * somewhere to go, and chilling had to be driven from the SNOW, because a
 * pane at rest never gets a turn of its own and so never looked at what
 * was sitting on it. */
static void test_snow_frosts_a_resting_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, GLASS);      /* at rest, not heated */
        sand_set(&s, x, H - 3, SNOW);
    }

    int coldest = MATERIAL_VARIANTS;
    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 1; x < W - 1; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) < coldest) {
                coldest = CELL_VARIANT(c);
            }
        }
    }

    TEST_ASSERT_LESS_THAN_MESSAGE(SAND_AMBIENT_HEAT, coldest,
        "snow resting on a pane at room temperature must pull it BELOW "
        "room temperature - if ambient is the bottom of the scale there is "
        "nothing to see, and the player gets no sign that snow and glass "
        "interact at all");

    /* And the palette has to disagree about the two, or the number moving
     * is still invisible. */
    const gfx_color_t *pal = material_palette();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        pal[MAT_GLASS * MATERIAL_VARIANTS + SAND_AMBIENT_HEAT],
        pal[MAT_GLASS * MATERIAL_VARIANTS + coldest],
        "and a frosted pane must not be drawn in the same colour as a "
        "resting one");
}

/* Cold spreads THROUGH the glass, past the cells the snow is touching.
 *
 * Without this the effect was real and nearly invisible: only the single
 * cell under a flake ever changed, and it barely changed, because snow
 * melts after a chill or two and `cools` drags the cell straight back
 * towards ambient. One cell one level off ambient is not something anyone
 * spots on a 184x224 board.
 *
 * Spreading it makes a patch of frost that creeps outward from where the
 * snow landed, which is both what frost looks like and what makes the
 * state readable. It is the same `conducts` that carries a fire's heat
 * through a wall, applied within the material - scaled down hard, because
 * at its own value a pane goes isothermal in a step or two and a wall that
 * is all one temperature cannot be hot inside and cold outside. */
static void test_frost_spreads_beyond_the_snow_touching_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);
    }
    /* Snow on the middle two cells only. */
    const int lo = W / 2 - 1, hi = W / 2;
    for (int x = lo; x <= hi; x++) {
        sand_set(&s, x, H - 3, SNOW);
    }

    int reached = 0;
    for (int i = 0; i < 600 && !reached; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            /* Strictly outside the snow's own footprint and its two
             * immediate sides, so this cannot pass on direct contact. */
            if (x >= lo - 1 && x <= hi + 1) {
                continue;
            }
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS &&
                CELL_VARIANT(c) < SAND_AMBIENT_HEAT) {
                reached = 1;
                break;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(reached,
        "cold must travel along the pane to cells no snow ever touched - a "
        "single chilled cell one level off ambient is a state change nobody "
        "can see");
}

/* Snow keeps on ordinary cold glass.
 *
 * It did not, briefly, and the report was exact: snow turned to water on
 * contact with glass when it never used to. Chilling had just been moved
 * so that it reaches panes at rest, and chilling costs the cold material
 * its own `heats_to` - so snow started paying the price tuned for standing
 * beside a FIRE in exchange for pushing a resting pane one level cooler.
 *
 * The cost now tracks what was actually absorbed: taking heat out of
 * something above room temperature melts snow, pushing cold into something
 * at or below it does not. Otherwise a snowbank cannot be kept anywhere
 * near the one building material it is meant to be used against. */
static void test_snow_keeps_on_ordinary_cold_glass(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, SNOW);
    }
    const int flakes = count_cells_of(MAT_SNOW);

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(flakes, count_cells_of(MAT_SNOW),
        "snow resting on glass at room temperature must not melt - it pays "
        "for heat it takes, and there was none to take");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "and it must leave no water behind, which is how the melting showed "
        "up on the board");
}

/* Shock works HOT ONTO COLD as well.
 *
 * Thermal shock is a large temperature CHANGE, not a high temperature, and
 * for a while only half of it existed: cold arriving at hot glass broke it,
 * heat arriving at frosted glass did not. Nobody could have explained that
 * asymmetry to a player, and the obvious experiment - chill a vessel, then
 * pour something hot into it - quietly did nothing.
 *
 * Kept as its own test rather than folded into the cold-onto-hot one
 * because the two run through completely different code: this direction
 * lives in try_heat_transform(), driven by the heat source, and the other
 * in step_one_cold_cell(), driven by the cold cell. They can break
 * independently and have. */
static void test_heat_arriving_at_frosted_glass_cracks_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }
    const int panes = W;
    const int sand_before = count_cells_of(MAT_SAND);

    int cracked = 0;
    for (int i = 0; i < 60 && !cracked; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
        cracked = count_cells_of(MAT_SAND) > sand_before;
    }

    TEST_ASSERT_TRUE_MESSAGE(cracked,
        "lava arriving at a frosted pane must crack it, not warm it "
        "through - shock is about the size of the change, and it has to "
        "work in both directions or it is not that");
    TEST_ASSERT_LESS_THAN_MESSAGE(panes, count_cells_of(MAT_GLASS),
        "and the pane must actually be gone, not merely warmed");
}

/* A pane at room temperature is not cracked by heat arriving.
 *
 * The guard on the test above, and the reason SAND_SHOCK_COLD is not
 * simply "below ambient": ordinary glass meeting fire has to warm up
 * through the ramp the way it always did, or every pane in the game breaks
 * the first time anyone lights something next to it. */
static void test_heat_arriving_at_resting_glass_only_warms_it(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, GLASS);            /* at rest */
    }
    const int sand_before = count_cells_of(MAT_SAND);

    for (int i = 0; i < 12; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(sand_before, count_cells_of(MAT_SAND),
        "glass at room temperature must warm up when fire reaches it, not "
        "shatter - only glass that was already COLD is shocked by heat");
}

/* Frost fades. It is a state, not a scar.
 *
 * Ambient is a resting point that gets approached from BOTH sides, which
 * is what makes putting cold below it work at all - otherwise the first
 * snowfall would leave every pane it touched permanently pale. */
static void test_a_frosted_pane_warms_back_to_room_temperature(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }

    int coldest = 0;
    for (int i = 0; i < 3000; i++) {
        sand_step(&s, 0, 1000, 0);
        coldest = MATERIAL_VARIANTS;
        for (int x = 1; x < W - 1; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_GLASS && CELL_VARIANT(c) < coldest) {
                coldest = CELL_VARIANT(c);
            }
        }
        if (coldest >= SAND_AMBIENT_HEAT) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_AMBIENT_HEAT, coldest,
        "a frosted pane left alone must warm back to room temperature - "
        "the same `cools` drift that brings a hot one down, running the "
        "other way");
}

/* Lava on one side of a wall, snow on the other: it cracks.
 *
 * The scenario the mechanism exists for, end to end and with nothing
 * pre-set - hold lava against a glass wall until it glows, then bank snow
 * on the far face. Every other shock test places a pane at a chosen
 * temperature, which tests the rule but assumes the pane can get hot at
 * all; here the heat has to arrive from a real source, through the
 * material, and reach the same cell the snow is touching.
 *
 * A vertical wall, deliberately. Laid flat with lava on top there is
 * nowhere for the snow to be except on top of the lava, where it flashes
 * off without ever meeting the hot cell - which is exactly what happens on
 * the board if you pour snow into an open basin rather than banking it
 * against the outside.
 *
 * The ORDER is the other half and the second block asserts it. Snow
 * present from the start fights the ramp instead of exploiting it:
 * chilling is faster than heating, so the wall never arrives at the
 * threshold and just sits there, warm on one face and frosted on the
 * other. Heat first, chill second. */
static void test_lava_one_side_snow_the_other_cracks_the_wall(void)
{
    const int wall = W / 2;

    /* --- heat it first ---------------------------------------------- */
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 1; y < H - 1; y++) {
        sand_set(&s, wall, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 1; x < wall; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    /* Soak until THE CELL THE SNOW WILL TOUCH is hot, not until any glass
     * anywhere is. Waiting on hottest_glass() passes as soon as some cell
     * up the wall gets there, which need not be the one being chilled a
     * moment later - a fixture that tests the wrong cell reports on the
     * wrong thing. */
    const int face = H - 2;
    int i;
    for (i = 0; i < 4000; i++) {
        const cell_t c = sand_at(&s, wall, face);
        if (CELL_MATERIAL(c) == MAT_GLASS &&
            CELL_VARIANT(c) >= SAND_SHOCK_HEAT) {
            break;
        }
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GLASS,
        CELL_MATERIAL(sand_at(&s, wall, face)),
        "fixture check: the wall cell being tested must survive the soak - "
        "if lava melted it there is nothing left to shatter");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_SHOCK_HEAT,
        CELL_VARIANT(sand_at(&s, wall, face)),
        "fixture check: lava held against a glass wall has to drive THAT "
        "cell past the shock threshold on its own, or the rest proves "
        "nothing about heat arriving from a real source");

    const int sand_before = count_cells_of(MAT_SAND);

    /* Topped up every step, the way a player pours rather than places.
     * Snow has scatter 90 - it drifts as it settles - so a single flake
     * set against the wall can wander off the one cell being tested
     * before the reactions pass ever looks at it, which makes a
     * single-placement version of this test a coin flip on the movement
     * RNG rather than a test of shock. */
    int cracked = 0;
    for (int k = 0; k < 60 && !cracked; k++) {
        for (int y = face - 1; y <= face; y++) {
            if (CELL_IS_EMPTY(sand_at(&s, wall + 1, y))) {
                sand_set(&s, wall + 1, y, SNOW);
            }
        }
        sand_step(&s, 0, 1000, 0);
        cracked = count_cells_of(MAT_SAND) > sand_before;
    }

    TEST_ASSERT_TRUE_MESSAGE(cracked,
        "snow banked against a wall that lava has heated from the far side "
        "must crack it - the gradient works whichever side the heat came "
        "from, which is the whole point of it being a gradient");

    /* --- and without snow it never cracks at all --------------------- */
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 1; y < H - 1; y++) {
        sand_set(&s, wall, y, GLASS);
    }
    for (int y = H - 3; y < H - 1; y++) {
        for (int x = 1; x < wall; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const int dry_sand_before = count_cells_of(MAT_SAND);
    for (int k = 0; k < 900; k++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(dry_sand_before, count_cells_of(MAT_SAND),
        "the identical wall with no snow must never shatter, however long "
        "the lava works on it - lava MELTS glass and only cold SHATTERS it, "
        "and a test that cannot tell those apart would pass on a board "
        "where snow did nothing at all");
}



/* One shock takes the whole pane, not one cell of it.
 *
 * Shattering used to convert a single cell, so breaking a pane needed as
 * many separate successful shocks as it had cells - and each one needs
 * something cold touching glass that is still hot, at the moment it
 * touches. Getting that to happen once is the interesting part; needing it
 * sixty times in the same place is attrition, and on the board it read as
 * thermal shock barely working.
 *
 * It is also what glass does. A pane does not crumble cell by cell as each
 * part independently decides to - a crack starts somewhere and travels. */
static void test_one_shock_cracks_the_whole_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* A pane the full width, hot, with a single flake at one END. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
    }
    sand_set(&s, 0, H - 3, SNOW);

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "a crack started at one end of a pane must run the length of it - "
        "the far end going too is the whole point, and it is why one "
        "successful shock is enough to matter");
}

/* But it does not jump a gap.
 *
 * The crack follows the material, so two panes that are not touching are
 * two panes. Without this the previous test passes just as well against
 * "any shock shatters all glass on the board", which would make glass
 * unusable anywhere near anything cold. */
static void test_a_crack_does_not_jump_to_a_separate_pane(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Two panes with a clear gap between them. */
    const int gap = W / 2;
    int far_side = 0;
    for (int x = 0; x < W; x++) {
        if (x == gap || x == gap + 1) {
            continue;
        }
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        if (x > gap) {
            far_side++;
        }
    }
    sand_set(&s, 0, H - 3, SNOW);

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(far_side, count_cells_of(MAT_GLASS),
        "the pane on the other side of the gap must be untouched - a crack "
        "runs through the material it is in, not through the air beside it");
}

/* The same snow on a cold pane does nothing at all.
 *
 * Which is what makes the rule about a GRADIENT rather than about snow
 * being corrosive to glass. Without this the previous test passes just as
 * happily against "snow destroys glass", a far worse rule that would make
 * the only acid-proof container in the game vulnerable to weather. */
static void test_cold_glass_is_unharmed_by_snow(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    const int panes = W - 2;
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, GLASS);
        sand_set(&s, x, H - 3, SNOW);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(panes, count_cells_of(MAT_GLASS),
        "snow on COLD glass must leave every pane intact - shock is about "
        "the temperature difference, not about snow being bad for glass");
}

/* Chilling costs the snow. It melts doing it.
 *
 * Snow that drained a glowing pane for free would be an unlimited heat
 * sink made of a material that arrives in a drift, so any pane anywhere
 * near weather would be unusable. Paying for the exchange is what keeps a
 * glass vessel over a fire a thing you can actually build. */
static void test_snow_melts_where_it_chills(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, SAND_SHOCK_HEAT - 1));
        sand_set(&s, x, H - 3, SNOW);
    }
    const int flakes = count_cells_of(MAT_SNOW);

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SNOW) < flakes,
        "snow that cools a hot pane must be spent doing it, or it is a "
        "free and permanent heat sink");
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_WATER) > 0,
        "and what it turns into is water, not nothing");
}

/* A re-initialised simulation remembers nothing about the old board.
 *
 * The may_have_* flags are an optimisation - they let whole passes be
 * skipped - so a stale one is not a wrong answer, it is a pass running
 * when it need not. A stale FALSE is the dangerous direction, and that is
 * what a missing reset produces on a fresh board.
 *
 * This is here because the omission hid a second bug rather than causing
 * one directly. may_have_temperature was not reset, the suite reuses one
 * static sand_t, so the flag arrived already true from whichever test ran
 * before - and the two tests written to prove the brush latched it could
 * not fail, because the thing they checked was true either way. Both bugs
 * were mine, in the same change, and the second made the first
 * untestable. */
static void test_reinitialising_forgets_the_old_board(void)
{
    fixture();
    sand_clear(&s);
    sand_set(&s, 1, 1, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_set(&s, 2, 1, GAS);
    sand_set(&s, 3, 1, FIRE);
    sand_set(&s, 4, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
    sand_set(&s, 5, 1, SNOW);

    TEST_ASSERT_TRUE_MESSAGE(s.may_have_liquid && s.may_have_gas &&
                             s.may_have_burning && s.may_have_dissolver &&
                             s.may_have_temperature,
        "fixture check: this board has one of everything, so every flag "
        "should be set before we throw it away");

    fixture();

    TEST_ASSERT_FALSE_MESSAGE(s.may_have_liquid,      "liquid flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_gas,         "gas flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_burning,     "burning flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_dissolver,   "dissolver flag leaked");
    TEST_ASSERT_FALSE_MESSAGE(s.may_have_temperature, "temperature flag leaked");
}

/* The brush and the setter must agree about what a cell implies.
 *
 * They did not, and the way they failed is the point. sand_set() and
 * try_spawn_one() each carried their own copy of the may_have_* latch
 * list, both with a comment noting the other one. A fifth flag was added
 * to one of them. Snow PLACED by sand_set() melted in water; snow PAINTED
 * with the brush never woke the reactions pass at all, so on a board with
 * no fire and no acid it sat in a pond forever and chilled nothing.
 *
 * Every test at the time used sand_set(), so every test passed and the
 * only way to find it was to draw snow into water by hand. This one walks
 * every material through both doors and compares what each one latched,
 * so a sixth flag cannot repeat it. */
static void test_the_brush_and_the_setter_agree_about_every_material(void)
{
    for (int m = 1; m < MAT_COUNT; m++) {
        const int cx = W / 2, cy = H / 2;

        /* The brush. */
        fixture();
        sand_clear(&s);
        sand_spawn(&s, cx, cy, 1, (material_id_t)m);
        const cell_t painted = sand_at(&s, cx, cy);
        const bool b_liquid = s.may_have_liquid;
        const bool b_gas    = s.may_have_gas;
        const bool b_burn   = s.may_have_burning;
        const bool b_diss   = s.may_have_dissolver;
        const bool b_temp   = s.may_have_temperature;

        TEST_ASSERT_FALSE_MESSAGE(CELL_IS_EMPTY(painted),
            "fixture check: the brush has to actually paint the centre "
            "cell, or this compares two empty boards");

        /* The setter, given the very cell the brush produced. */
        fixture();
        sand_clear(&s);
        sand_set(&s, cx, cy, painted);

        char why[96];
        snprintf(why, sizeof why,
                 "brush and setter disagree about %s", materials[m].name);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_liquid,      b_liquid, why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_gas,         b_gas,    why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_burning,     b_burn,   why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_dissolver,   b_diss,   why);
        TEST_ASSERT_EQUAL_MESSAGE(s.may_have_temperature, b_temp,   why);
    }
}

/* And the same thing end to end, through the door the player uses.
 *
 * The test above compares flags; this one says what the flags were for.
 * Snow painted into a pool on a board where nothing is burning has to
 * melt, and it is worth stating separately because the flag is only
 * machinery - the observable claim is that a drift does not sit in water
 * forever. */
static void test_snow_painted_into_water_melts(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_spawn(&s, W / 2, 1, 2, MAT_SNOW);
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SNOW) > 0,
        "fixture check: the brush has to put some snow on the board");

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SNOW),
        "snow PAINTED into water must melt, on a board with no fire and "
        "no acid anywhere - nothing else is running the reactions pass, "
        "so this is the whole of what wakes it");
}

/* Snow melts in liquid, not only near fire.
 *
 * Water is not a heat source in this simulation, so before `thaws` a
 * drift would ride on a pond forever - which looked wrong in the one
 * place snow is most likely to land. Nothing here has a temperature
 * except glass, so "liquid" stands in for "warm and touching you on
 * every side"; the alternative was giving water a temperature, which
 * means giving everything one, which means a second byte per cell that
 * this grid does not have. */
static void test_snow_melts_in_any_liquid(void)
{
    static const uint8_t liquids[] = { MAT_WATER, MAT_OIL, MAT_ACID };

    for (unsigned k = 0; k < sizeof liquids / sizeof liquids[0]; k++) {
        fixture();
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 4; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, 0, SNOW);
        }

        for (int i = 0; i < 600; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SNOW),
            "snow left sitting in a liquid must melt - any liquid, not "
            "water alone, since nothing here is at a temperature and a "
            "drift floating forever on oil needs more explaining than one "
            "that melts");
    }
}

/* And what it melts INTO is water, whatever melted it.
 *
 * Snow is frozen water and turns back into water; it does not become
 * more of whatever it touched. That would be an exploit rather than a
 * flourish - acid is spent as it dissolves, so snow that melted into
 * acid would be a bucket that refills itself, and snow melting into oil
 * would be a fuel printer. */
static void test_melting_snow_makes_water_not_more_of_the_liquid(void)
{
    static const uint8_t liquids[] = { MAT_OIL, MAT_ACID };

    for (unsigned k = 0; k < sizeof liquids / sizeof liquids[0]; k++) {
        fixture();
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 3; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, 0, SNOW);
        }

        for (int i = 0; i < 600; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_WATER) > 0,
            "snow melted by oil or acid has to leave WATER behind - it is "
            "frozen water, and turning into whatever dissolved it would "
            "make a snowbank a factory for that liquid");
    }
}

/* On dry ground it does not melt at all.
 *
 * Which is what stops the rule above from being "snow evaporates". A
 * drift has to keep on a bare floor, or it cannot be stockpiled and
 * carried to the pane it is meant to crack. */
static void test_snow_keeps_on_dry_ground(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, 0, SNOW);
    }
    const int fell = W - 2;

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(fell, count_cells_of(MAT_SNOW),
        "snow on bare stone must not melt - it melts in liquid and near "
        "heat, and a floor is neither");
}

/* Total mass of one liquid on the board - fill levels, not cell count.
 * A liquid that spreads occupies more cells holding less each, so cells
 * are the wrong unit for asking whether any of it was destroyed. */
static int liquid_mass_of(uint8_t id)
{
    int m = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == id) {
                m += CELL_VARIANT(c);
            }
        }
    }
    return m;
}

/* Stone carries a temperature, the same as glass. */
static void test_stone_heats_up_next_to_lava(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }

    int hottest = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_STONE && CELL_VARIANT(c) > hottest) {
                hottest = CELL_VARIANT(c);
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest,
        "stone under lava must warm up - it reads the same temperature "
        "scale glass does, so a player who has learned one wall can read "
        "the other");
}

/* But it never melts, however hot it gets, and that is the point of it.
 *
 * Glass names MAT_LAVA in `heats_to` and stone names nothing. If both
 * melted there would be no vessel that holds lava indefinitely and the
 * choice between the two materials would collapse into "glass, but it
 * dies". As it stands stone survives any heat and acid eats it, glass is
 * immune to acid and heat melts it, and both crack when chilled hot. */
static void test_stone_never_melts_however_hot(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reactions[MAT_STONE].heats_to,
        "stone must name nothing in heats_to - surviving heat is the "
        "reason to build out of it");

    fixture();
    sand_clear(&s);
    const int walls = W;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, CELL_MAKE(MAT_STONE, MATERIAL_VARIANTS - 1));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }

    for (int i = 0; i < 800; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(walls, count_cells_of(MAT_STONE),
        "stone held at the top of its ramp under lava must still be stone "
        "after 800 steps");
}

/* A hot stone wall does NOT crack when it is chilled. Glass, right beside
 * it in the same scene, does.
 *
 * Stone reads the temperature and does nothing else with it. Rock quenched
 * from hot spalls and cracks; it does not turn into sand, and there is no
 * honest byproduct to name for it - so `shatters_to` is left off, and the
 * absence is the decision.
 *
 * Both halves in one test on one board, because the claim is a CONTRAST.
 * "Stone survives" passes just as well on a board where nothing shocks at
 * all, which would hide the feature breaking rather than show it. */
static void test_snow_cracks_glass_but_not_stone(void)
{
    fixture();
    sand_clear(&s);
    const int mid = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        /* left half stone, right half glass - both at the top of the ramp */
        sand_set(&s, x, H - 2,
                 CELL_MAKE(x < mid ? MAT_STONE : MAT_GLASS,
                           MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, SNOW);
    }
    const int stone_before = count_cells_of(MAT_STONE);
    const int glass_before = count_cells_of(MAT_GLASS);

    for (int i = 0; i < 80; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reactions[MAT_STONE].shatters_to,
        "stone must name nothing in shatters_to - rock does not thermally "
        "shock into anything this simulation has a material for");
    TEST_ASSERT_EQUAL_INT_MESSAGE(stone_before, count_cells_of(MAT_STONE),
        "and a glowing stone wall packed with snow must survive it");
    TEST_ASSERT_LESS_THAN_MESSAGE(glass_before, count_cells_of(MAT_GLASS),
        "while glass on the same board, at the same temperature, under the "
        "same snow, must crack - otherwise this passes on a board where "
        "shock is simply broken");
}



/* Rough perceptual distance between two panel colours - both are RGB565
 * with the bytes swapped for the panel (GFX_RGB in gfx_color.h), so they
 * have to be unswapped before the channels mean anything. Green counts
 * once and red and blue twice, which is only a rule of thumb; nothing here
 * needs better than "clearly further apart". */
static int colour_gap(gfx_color_t x, gfx_color_t y)
{
    const uint16_t a = (uint16_t)((x >> 8) | (x << 8));
    const uint16_t b = (uint16_t)((y >> 8) | (y << 8));
    const int dr = ((a >> 11) & 0x1F) - ((b >> 11) & 0x1F);
    const int dg = ((a >>  5) & 0x3F) - ((b >>  5) & 0x3F);
    const int db = ( a        & 0x1F) - ( b        & 0x1F);
    return (dr < 0 ? -dr : dr) * 2 + (dg < 0 ? -dg : dg) +
           (db < 0 ? -db : db) * 2;
}

/* An outline moves less with temperature than the body it encloses.
 *
 * A wall going from grey to glowing changed its whole silhouette, so the
 * shape stopped being readable at exactly the moment it mattered. Cells
 * touching empty space are drawn much nearer their own resting colour, so
 * the outline stays put and the heat is shown by the inside.
 *
 * Asserted as a comparison rather than against fixed colours: the claim is
 * that the edge moves LESS than the body, which stays true if the ramps
 * are ever retuned. */
static void test_an_edge_shows_less_temperature_than_the_body(void)
{
    static const uint8_t tempered[] = { MAT_GLASS, MAT_STONE };

    for (unsigned k = 0; k < sizeof tempered / sizeof tempered[0]; k++) {
        const uint8_t m = tempered[k];
        gfx_color_t body[3], ed[3], hot[3], hot_ed[3];

        material_colours(CELL_MAKE(m, SAND_AMBIENT_HEAT), 0u, false, body);
        material_colours(CELL_MAKE(m, SAND_AMBIENT_HEAT), 0u, true,  ed);
        material_colours(CELL_MAKE(m, MATERIAL_VARIANTS - 1), 0u, false, hot);
        material_colours(CELL_MAKE(m, MATERIAL_VARIANTS - 1), 0u, true, hot_ed);

        const gfx_color_t rest_body = body[0], rest_edge = ed[0];
        const gfx_color_t hot_body = hot[0], hot_edge = hot_ed[0];

        char why[224];
        snprintf(why, sizeof why,
                 "%s: an edge must travel less than the body between rest "
                 "and full heat, or the outline changes with the "
                 "temperature and the shape stops reading",
                 materials[m].name);

        TEST_ASSERT_TRUE_MESSAGE(colour_gap(rest_edge, hot_edge) <
                                 colour_gap(rest_body, hot_body), why);
        TEST_ASSERT_TRUE_MESSAGE(colour_gap(rest_edge, hot_edge) > 0,
            "but it must still travel some - an outline frozen at ambient "
            "would hide heat entirely at the boundary");
    }
}

/* Every material is painted the way it is meant to be, and no other.
 *
 * material_colours() is consulted for every cell the renderer paints, so a
 * material picking up a pattern by accident is a visible change nobody
 * asked for - on a path the host tests otherwise never reach, because
 * app_sand.c does not compile here.
 *
 * PURELY VISUAL, all of it: nothing the simulation reads comes from this,
 * which is exactly why it needs its own guard. A wrong colour breaks no
 * behaviour and no other test would notice. */
static void test_each_material_is_painted_the_way_it_should_be(void)
{
    const gfx_color_t *pal = material_palette();

    for (int m = 1; m < MAT_COUNT; m++) {
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            const cell_t c = CELL_MAKE(m, v);
            gfx_color_t col[3] = { 0, 0, 0 };
            const material_pattern_t pat = material_colours(c, 0u, false, col);

            char why[128];
            snprintf(why, sizeof why, "%s variant %d", materials[m].name, v);

            if (m == MAT_GLASS) {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_HATCHED, pat, why);
                TEST_ASSERT_TRUE_MESSAGE(col[0] != col[1] && col[1] != col[2],
                    "glass is hatched, so its body, its lines and their "
                    "crossings must all differ - equal ones paint a flat "
                    "pane and the shine vanishes");
            } else if (m == MAT_STONE) {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_SPECKLED, pat, why);
            } else if (m == MAT_WOOD) {
                /* Speckled only while UNLIT. A burning log is a glow, and
                 * a glow that varies cell to cell reads as dirty rather
                 * than as fire. */
                TEST_ASSERT_EQUAL_MESSAGE(
                    v == 0 ? MATERIAL_SPECKLED : MATERIAL_FLAT, pat, why);
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(MATERIAL_FLAT, pat, why);
                TEST_ASSERT_EQUAL_MESSAGE(pal[c], col[0], why);
                TEST_ASSERT_EQUAL_MESSAGE(col[0], col[2], why);
            }
        }
    }
}

/* Glass has a grain too, and it is quieter than stone's.
 *
 * Stone is rock and wants visible speckle; glass is smooth and wants only
 * enough variation that a wall of it stops reading as one flat fill. Both
 * halves are asserted because both can fail alone - no variation is the
 * flat fill this exists to undo, and variation as loud as stone's would
 * make a pane look like gravel.
 *
 * Compared against stone rather than against a fixed number, so it stays
 * meaningful if either ramp is retuned. */
static void test_glass_grain_is_quieter_than_stone(void)
{
    int glass_spread = 0, stone_spread = 0;

    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        gfx_color_t g0[3], g1[3], s0[3], s1[3];
        material_colours(CELL_MAKE(MAT_GLASS, v), 0u, false, g0);
        material_colours(CELL_MAKE(MAT_GLASS, v), 3u, false, g1);
        material_colours(CELL_MAKE(MAT_STONE, v), 0u, false, s0);
        material_colours(CELL_MAKE(MAT_STONE, v), 7u, false, s1);

        glass_spread += colour_gap(g0[0], g1[0]);
        stone_spread += colour_gap(s0[0], s1[0]);
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, glass_spread,
        "glass must vary from cell to cell at all - without it a pane is "
        "one flat fill, which is what the grain exists to undo");
    TEST_ASSERT_TRUE_MESSAGE(glass_spread < stone_spread,
        "but it must vary LESS than stone does - glass is smooth and rock "
        "is not, and a pane speckled as hard as a wall reads as gravel");
}

/* The lines and the shine do NOT vary from cell to cell.
 *
 * They are light landing on the surface, not the surface itself. Letting
 * them wobble per cell makes a highlight look chewed instead of
 * reflective, so only the pane underneath carries the grain. */
static void test_the_shine_does_not_vary_between_cells(void)
{
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        gfx_color_t a[3], b[3];
        material_colours(CELL_MAKE(MAT_GLASS, v), 0u, false, a);
        material_colours(CELL_MAKE(MAT_GLASS, v), 2u, false, b);

        char why[128];
        snprintf(why, sizeof why,
                 "glass at temperature %d: the %%s must be identical in "
                 "every cell", v);
        TEST_ASSERT_EQUAL_MESSAGE(a[1], b[1], why);
        TEST_ASSERT_EQUAL_MESSAGE(a[2], b[2], why);
    }
}

/* Stone's speckle comes from the cell's POSITION, not from its variant.
 *
 * Stone used to carry a random shade and a wall looked like rock because
 * of it. Spending the variant on temperature took that away; deriving the
 * shade from a per-cell hash instead puts it back without the variant
 * having to mean two things at once.
 *
 * Both halves are asserted because both can fail on their own: a speckle
 * that does not vary is a flat slab again, and one that varies with
 * anything unstable would crawl and shimmer from frame to frame. */
static void test_stone_speckles_by_position_at_every_temperature(void)
{
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        const cell_t c = CELL_MAKE(MAT_STONE, v);
        gfx_color_t seen[8];
        int distinct = 0;

        for (unsigned h = 0; h < 8u; h++) {
            gfx_color_t col[3] = { 0, 0, 0 };
            material_colours(c, h, false, col);
            const gfx_color_t a = col[0];
            TEST_ASSERT_EQUAL_MESSAGE(col[0], col[1],
                "a speckled cell is one flat colour - the variation is "
                "BETWEEN cells, not inside one");
            int fresh = 1;
            for (int k = 0; k < distinct; k++) {
                if (seen[k] == a) {
                    fresh = 0;
                }
            }
            if (fresh) {
                seen[distinct++] = a;
            }
        }

        char why[160];
        snprintf(why, sizeof why,
                 "stone at temperature %d must offer more than one shade - "
                 "one shade is the flat slab this exists to undo", v);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, distinct, why);
    }

    /* Stable: the same cell asked twice gets the same answer. */
    gfx_color_t one[3], two[3];
    const cell_t c = CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT);
    material_colours(c, 12345u, false, one);
    material_colours(c, 12345u, false, two);
    TEST_ASSERT_EQUAL_MESSAGE(one[0], two[0],
        "the same cell must speckle the same way every time it is asked, "
        "or a stone wall shimmers");
}



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

/* ===================================================================
 * The extended range: sixteen materials behind the last slot.
 * =================================================================== */

/* Heat through a wall LIGHTS oil. It does not boil it away.
 *
 * conduct_heat() turned anything on the far side into steam if it was a
 * liquid and did not itself burn. Water is a liquid and does not burn, so
 * that was right when water was the only liquid that could be there - and
 * it has been wrong for every liquid added since. Lava was caught first,
 * because lava boiling itself is spectacular. Oil is quieter: it just
 * disappears.
 *
 * Measured before the fix: 180 units of oil in a stone pan over a fire
 * went to zero in sixty steps, leaving fourteen cells of steam. Steam is
 * the tell - oil has no business producing any at all, which is what makes
 * it a sharper assertion than the oil count. */
static void test_heat_through_a_pan_lights_oil_rather_than_boiling_it(void)
{
    fixture();
    sand_clear(&s);
    sand_set_flammability(&s, SAND_FLAMMABILITY_PER_MATERIAL);
    sand_set_conduction(&s, SAND_CONDUCTION_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 3, STONE);            /* the pan */
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));
    }

    bool lit = false;
    for (int i = 0; i < 300; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_STEAM),
            "oil must never make steam - it is fuel, not a kettle, and "
            "steam here means heat is evaporating it instead of lighting "
            "it");
        if (count_cells_of(MAT_OIL) < W - 2) {
            lit = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(lit,
        "and the heat must actually reach it - oil in a pan over a held "
        "fire has to catch, or this passes on a board where nothing "
        "happened at all");
}

/* A powder lands ON another powder, and still sinks through a liquid.
 *
 * Displacement used to be "anything not static, if you are heavier", so a
 * heavier powder fell straight through a lighter one - dirt through snow,
 * sand through snow, dirt through sand. Reported as dirt passing through a
 * snowbank instead of landing on it, which is what it looked like.
 *
 * A grain can push its way down through water or through smoke and cannot
 * push its way through packed grains however heavy it is. Density still
 * decides fluids and stops deciding anything between solids.
 *
 * Both halves in one test, because "powders stack" passes just as well on
 * a board where nothing displaces anything at all - which would leave sand
 * sitting on top of water. */
static void test_a_powder_lands_on_a_powder_but_sinks_in_a_liquid(void)
{
    static const struct { uint8_t bed, dropped; bool sinks; } cases[] = {
        { MAT_SNOW,  MAT_DIRT,  false },
        { MAT_SNOW,  MAT_SAND,  false },
        { MAT_SAND,  MAT_DIRT,  false },
        { MAT_WATER, MAT_SAND,  true  },
        { MAT_WATER, MAT_DIRT,  true  },
    };

    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const uint8_t bed = cases[k].bed, dropped = cases[k].dropped;
        fixture();
        sand_clear(&s);
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = H - 4; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y,
                         materials[bed].kind == KIND_LIQUID
                             ? CELL_MAKE(bed, MASS_MAX) : CELL_MAKE(bed, 4));
            }
        }
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, 1, CELL_MAKE(dropped, 4));
        }

        for (int i = 0; i < 300; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        int lowest = -1, highest_bed = H;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
                if (m == dropped && y > lowest) {
                    lowest = y;
                }
                if (m == bed && y < highest_bed) {
                    highest_bed = y;
                }
            }
        }

        char why[160];
        snprintf(why, sizeof why,
                 "%s dropped on %s should %s", materials[dropped].name,
                 materials[bed].name,
                 cases[k].sinks ? "sink through it - density decides fluids"
                                : "land on top of it - grains do not pass "
                                  "through grains");
        if (cases[k].sinks) {
            TEST_ASSERT_TRUE_MESSAGE(lowest > highest_bed, why);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(lowest <= highest_bed, why);
        }
    }
}

/* Hot gas warms what it touches - convection.
 *
 * It is here for what it makes VISIBLE rather than for what it achieves.
 * Measured three times against whether it helps shatter glass, it does
 * not: warmer air costs snow its life, because a pane above room
 * temperature charges snow for touching it, and snow is the scarce thing.
 *
 * What it does do is make heat reach where conduction cannot. Measured on
 * a stone flue with a wood fire at the bottom, the top of the flue sits at
 * ambient without it and one to two levels above with - the difference
 * between a chimney that is stone cold at the top and one that is warm,
 * which is only worth anything now that stone shows its temperature. */
static void test_hot_gas_warms_what_it_touches(void)
{
    fixture();
    sand_clear(&s);

    /* A stone slab with nothing but steam against it - no fire, no
     * conduction path, nothing else that could account for the heat. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, STONE);
    }
    for (int i = 0; i < 300; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, STEAM);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    int hottest = 0;
    for (int x = 0; x < W; x++) {
        const cell_t c = sand_at(&s, x, H - 2);
        if (CELL_MATERIAL(c) == MAT_STONE && CELL_VARIANT(c) > hottest) {
            hottest = CELL_VARIANT(c);
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SAND_AMBIENT_HEAT, hottest,
        "steam resting against stone must warm it - there is no fire here "
        "and nothing to conduct through, so the gas is the only thing that "
        "could have");
}

/* But it is not a fire: it lights nothing.
 *
 * The cheap way to get this heating was to give smoke `burns`, which would
 * have made a chimney full of smoke set light to a wooden roof. Convection
 * is a separate field for that reason, and the difference is worth a test
 * rather than a comment. */
static void test_hot_gas_does_not_set_fire_to_anything(void)
{
    fixture();
    sand_clear(&s);
    sand_set_flammability(&s, SAND_FLAMMABILITY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 2, WOOD);
    }
    for (int i = 0; i < 400; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, SMOKE);
            }
        }
        sand_step(&s, 0, 1000, 0);
    }

    for (int x = 0; x < W; x++) {
        TEST_ASSERT_FALSE_MESSAGE(cell_is_burning(sand_at(&s, x, H - 2)),
            "smoke must not light wood - it warms things that hold a "
            "temperature and does nothing else, which is the whole reason "
            "it is not simply `burns`");
    }
}

/* ===================================================================
 * Dirt: soaking, drying, and sand turning into soil.
 * =================================================================== */

/* Wet sand slowly becomes soil, and the water is SPENT doing it.
 *
 * Consuming the liquid is what separates soaking from `thaws`, which snow
 * uses to melt in one - snow survives on what the liquid gives it for
 * free. A shoreline that turned to soil without the sea getting any
 * shallower would be making matter out of nothing. */
static void test_wet_sand_becomes_dirt_and_spends_the_water(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    const int water_before = liquid_mass_of(MAT_WATER);

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_DIRT) > 0,
        "sand left sitting under water must turn into dirt - slowly, but "
        "it must happen");
    TEST_ASSERT_TRUE_MESSAGE(liquid_mass_of(MAT_WATER) < water_before,
        "and the water must be spent doing it, or soil is being made out "
        "of nothing");
}

/* Dirt holds moisture in its variant, and gives it back up.
 *
 * Both halves matter: without soaking there is no wet soil for anything to
 * grow in, and without drying a single watering would make a patch fertile
 * forever, which turns watering from something you DO into something you
 * did once. */
static void test_dirt_takes_on_moisture_and_dries_out_again(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_DIRT, 0));
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    int wettest = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) > wettest) {
                wettest = CELL_VARIANT(c);
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(wettest > 0,
        "dirt under water must take moisture on - its variant is how wet "
        "it is");

    /* Take the water away and let it dry. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) {
                sand_set(&s, x, y, 0);
            }
        }
    }
    int still_wet = 1;
    for (int i = 0; i < 4000 && still_wet; i++) {
        sand_step(&s, 0, 1000, 0);
        still_wet = 0;
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT && CELL_MOISTURE(c) != 0) {
                still_wet = 1;
            }
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(still_wet,
        "and with the water gone it must dry back out, or one watering "
        "makes a patch fertile for good");
}

/* Freshly drawn dirt is dry, and freshly made dirt is barely wet.
 *
 * The variant is moisture, so a random shade would hand the player soil
 * that arrives already watered - the fourth meaning this variant can carry
 * and the fourth time a random shade would have been wrong. */
static void test_new_dirt_starts_dry_in_a_random_tone(void)
{
    fixture();
    sand_clear(&s);
    sand_spawn(&s, W / 2, H / 2, 2, MAT_DIRT);

    int seen[SOIL_TONES];
    memset(seen, 0, sizeof seen);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_DIRT) {
                continue;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(c),
                "painted dirt must arrive bone dry - the low bits of its "
                "variant are moisture, and soil that arrives watered is "
                "fertile ground for free");
            seen[CELL_SOIL_TONE(c)] = 1;
        }
    }

    /* The other half of the variant is a carried tone, and unlike the
     * moisture it IS random - it is what makes a poured bank keep the
     * pattern it was poured with instead of sliding under a texture
     * pinned to the screen. */
    for (int t = 0; t < SOIL_TONES; t++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[t],
            "a brushful of dirt must use every tone - one tone is a flat "
            "fill, which is what the screen-space grain was there to hide");
    }
}


/* Soil's tone is CARRIED, and wetting it must not repaint it.
 *
 * This is the whole reason the tone is in the cell rather than hashed from
 * screen position: it has to travel with the grain, so a poured bank keeps
 * the pattern it was poured with and the strata show the shape of the
 * pile. A tone that got recomputed - from position, or from the moisture,
 * or clobbered by a soak that wrote the whole variant - would put the
 * texture back on the screen where it started.
 *
 * The moisture assert is half the test: without it, code that never
 * touched the variant at all would pass. */
static void test_soil_keeps_its_tone_through_wetting_and_drying(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    uint8_t tone[W];
    for (int x = 0; x < W; x++) {
        tone[x] = (uint8_t)(x % SOIL_TONES);
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, tone[x], 0));
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    int ever_wet = 0;
    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) != MAT_DIRT) {
                continue;
            }
            if (CELL_MOISTURE(c) != 0) {
                ever_wet = 1;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(tone[x], CELL_SOIL_TONE(c),
                "soil must keep the tone it was laid down with, however "
                "wet it gets - a tone that is recomputed or overwritten is "
                "a texture pinned to the screen again");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(ever_wet,
        "the soil has to have actually got wet, or this passes on soil "
        "nothing ever happened to");
}

/* And the two tones have to LOOK different.
 *
 * Everything above is bookkeeping if the palette paints them the same, and
 * nothing else would notice - the whole point of the bit is visual. Checked
 * at every wetness, because the ramps are built per tone and a mistake in
 * one end of one of them would otherwise hide. */
static void test_the_two_soil_tones_are_different_colours(void)
{
    const gfx_color_t *pal = material_palette();

    for (unsigned m = 0; m <= SOIL_MOISTURE_MAX; m++) {
        const cell_t lo = CELL_SOIL(MAT_DIRT, 0, m);
        const cell_t hi = CELL_SOIL(MAT_DIRT, 1, m);
        char why[96];
        snprintf(why, sizeof why, "soil tones at moisture %u", m);
        TEST_ASSERT_TRUE_MESSAGE(pal[lo] != pal[hi], why);
    }

    /* And wetness still has to read, or the stain a watering leaves is
     * invisible and the tone has eaten the ramp it was added beside. */
    TEST_ASSERT_TRUE_MESSAGE(
        pal[CELL_SOIL(MAT_DIRT, 0, 0)] !=
        pal[CELL_SOIL(MAT_DIRT, 0, SOIL_MOISTURE_MAX)],
        "dry soil and saturated soil of the SAME tone must differ - the "
        "wetness ramp is how a watered patch shows at all");
}


/* Only WATER wets what it touches.
 *
 * `soaks` belongs to sand and soil, and the obvious way to write it - take
 * a unit of any adjacent KIND_LIQUID - reads perfectly and is wrong for
 * three of the four liquids on this board. Measured before the fix, a bank
 * of sand under oil turned entirely into saturated soil; so did one under
 * LAVA. Reported as oil soaking, which it was, along with everything else.
 *
 * Wetness is not the same question as fluidity, and only the liquid knows
 * the answer, so it is the liquid that carries the flag.
 *
 * Oil is the liquid to test with. Acid dissolves sand and lava fuses it,
 * so with either of those "the sand is gone" proves nothing about
 * soaking; oil leaves it alone entirely, which is the point. */
static void test_only_water_wets_what_it_touches(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 6));
        sand_set(&s, x, H - 3, CELL_SOIL(MAT_DIRT, 1, 0));
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));
    }

    for (int i = 0; i < 800; i++) {
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) != MAT_DIRT) {
                    continue;
                }
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_MOISTURE(c),
                    "soil under OIL must stay bone dry - oil is a liquid "
                    "and is not wet, and the absorbing side cannot tell "
                    "the difference on its own");
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SAND) > 0,
        "and sand under oil must still be sand - it turned into a bank of "
        "saturated soil, which is the same bug seen from the other end");
}

/* Nothing soaks unless the simulation is told to let it.
 *
 * The override exists because soaking is a property of sand, and half the
 * suite puts sand in water to check that sand SINKS. Those tests are about
 * density and have no opinion about chemistry; they all broke the moment
 * soaking arrived switched on. */
static void test_soaking_is_off_unless_asked_for(void)
{
    fixture();
    sand_clear(&s);
    /* deliberately NOT calling sand_set_soak */

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
        sand_set(&s, x, H - 3, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_DIRT),
        "with soaking off, sand under water must stay sand - a mechanic "
        "that arrives switched on rewrites every scene that already "
        "existed");
}


/* A wetting front has to REACH.
 *
 * Moisture used to move one level per hop and only downhill by two or
 * more, and a grain converted by wet soil was born holding exactly 1 - one
 * short of the 2 it needed to pass anything on. So the front died at the
 * first ring of new soil, every time, however much water was behind it.
 * Reported as "the diffusion of wet sand to dirt is either too slow or
 * reaches a range limit now", which is precisely what it was.
 *
 * Moving half the difference instead is the ordinary way a diffusion
 * settles, and it is what this test pins: soil several cells away from
 * anything the water touched must still end up wet. */
static void test_a_wetting_front_spreads_past_the_cells_it_touched(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    /* One saturated grain of soil at one end, and no water anywhere: what
     * spreads has to be what that one cell is holding. */
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int reach = -1;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, H - 2)) == MAT_DIRT) {
            reach = x;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(reach >= 3,
        "one saturated cell of soil must wet sand several cells away, not "
        "just the grain it touches - a front that hands over one level and "
        "leaves the receiver below the threshold to pass it on stops dead "
        "at the first ring");
}

/* And it must reach WITHOUT inventing water.
 *
 * The obvious way to make a front travel further is to give the receiver
 * more than the donor gives up, which spreads beautifully and quietly
 * creates moisture out of nothing - and moisture is what a plant spends,
 * so it would mean one watering could grow a forest. Half the difference
 * is exactly conservative; this is what says so. */
static void test_moisture_is_conserved_as_it_spreads(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    const int placed = (int)SOIL_MOISTURE_MAX;

    /* Drying only ever removes moisture, so the total can fall but must
     * never rise above what was put in. */
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int total = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) == MAT_DIRT) {
                    total += CELL_MOISTURE(c);
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(total <= placed,
            "spreading moisture must move it, not multiply it - a front "
            "that gains on every hop is a watering can that fills itself");
    }
}


/* And what it hands over is a SHARE, not a token.
 *
 * This is the half of the front that reach alone does not pin down. Soil
 * converted by a passing wetting front used to be born holding exactly 1,
 * which is one short of the 2 it needs to wet anything itself - so every
 * new grain was a dead end, and the patch only ever crept outward as fast
 * as the original wet cell could top up the grain next to it. Half the
 * difference means a new grain arrives able to carry the front on. */
static void test_soil_a_wetting_front_converts_is_handed_a_real_share(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }
    sand_set(&s, 0, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));

    /* Caught the step it happens, before drying has taken any of it. */
    int handed = -1;
    for (int i = 0; i < 400 && handed < 0; i++) {
        sand_step(&s, 0, 1000, 0);
        const cell_t c = sand_at(&s, 1, H - 2);
        if (CELL_MATERIAL(c) == MAT_DIRT) {
            handed = CELL_MOISTURE(c);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(handed >= 0,
        "the grain beside saturated soil must become soil at all");
    TEST_ASSERT_TRUE_MESSAGE(handed >= (int)SOIL_MOISTURE_MAX / 2,
        "and must be handed a real share of what wet it - a grain born "
        "holding 1 is below the level it needs to wet anything itself, "
        "which makes every cell the front converts a dead end");
}


/* A shattered pane comes back as CULLET, not as beach.
 *
 * Sand's variant is a shade, so recording that a grain used to be glass
 * costs nothing but four of the sixteen shades it could have had. What it
 * buys is that the wreckage of a window stays visibly the wreckage of a
 * window - sand's shade never changes, so a heap of it keeps the memory
 * indefinitely and mixes into an ordinary dune without becoming it.
 *
 * The second assert is the one that matters for how it LOOKS. Shattered
 * glass was already landing at the top of sand's ramp, because that is
 * what the general placement helper hands a new cell - so it was already
 * the brightest sand there is, in one flat value across the whole pane. A
 * band that is not varied inside itself is a slab of colour, which is the
 * thing this is meant to stop being. */
static void test_a_shattered_pane_comes_back_as_cullet(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, 0));   /* fully frosted */
    }

    for (int i = 0; i < 60 && count_cells_of(MAT_SAND) == 0; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, CELL_MAKE(MAT_LAVA, MASS_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_SAND) > 0,
        "the pane has to have actually shattered");

    int shades[MATERIAL_VARIANTS] = { 0 };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_SAND) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) >= SAND_CULLET_BASE,
                "sand from a shattered pane must land in the cullet band - "
                "the top of the dune ramp is still the dune ramp, and pale "
                "tan reads as sand rather than as broken glass");
            shades[CELL_VARIANT(c)]++;
        }
    }

    int distinct = 0;
    for (int v = SAND_CULLET_BASE; v < MATERIAL_VARIANTS; v++) {
        distinct += shades[v] ? 1 : 0;
    }
    /* Three of the four, not merely "more than one". The band's top value
     * is exactly what the general placement helper hands a new cell, so a
     * crack that forgot to vary its shade still lands IN the band - a
     * mutation reverting the spread to place_reacted() passed a
     * two-distinct check on the strength of the one cell that starts the
     * crack. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(2, distinct,
        "and must vary across the band - one flat value over a whole pane "
        "is a slab of colour, which is what this replaced");
}

/* And painted sand must never land in that band.
 *
 * A reserved band only means anything if it is reserved. Without this the
 * ordinary brush would scatter grains claiming to be broken glass through
 * every dune on the board, at a quarter of them. */
static void test_painted_sand_stays_out_of_the_cullet_band(void)
{
    fixture();
    sand_clear(&s);
    sand_spawn(&s, W / 2, H / 2, 3, MAT_SAND);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) != MAT_SAND) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) < SAND_CULLET_BASE,
                "a painted grain must be a dune shade - the cullet band is "
                "reserved, and a brush that reaches into it makes every "
                "pile look like it has broken glass mixed through it");
        }
    }
}

/* The two bands have to be different colours, and not merely different
 * ends of one ramp.
 *
 * This is the whole point and nothing else would catch it: cullet built
 * from sand's own colours would pass every test above while looking
 * exactly like pale sand, which is what it looked like before. */
static void test_cullet_does_not_look_like_sand(void)
{
    const gfx_color_t *pal = material_palette();

    for (int c = SAND_CULLET_BASE; c < MATERIAL_VARIANTS; c++) {
        for (int d = 0; d < SAND_CULLET_BASE; d++) {
            char why[96];
            snprintf(why, sizeof why, "cullet %d against dune shade %d", c, d);
            TEST_ASSERT_TRUE_MESSAGE(
                pal[CELL_MAKE(MAT_SAND, c)] != pal[CELL_MAKE(MAT_SAND, d)],
                why);
        }
    }
}


/* Water reaches the BOTTOM of a submerged pile.
 *
 * Diffusion alone cannot do this and the shape of its failure is
 * distinctive: half-the-difference settles into a gradient of one level
 * per cell and then stops, because half of a gap of one is zero. So a pile
 * held under water wet its top few rows into a perfect ramp and froze,
 * with dry sand underneath it for ever, and the depth it reached was set
 * by the size of the moisture range rather than by how much water there
 * was. Reported as dirt not wetting a whole pile "even fully submerged".
 *
 * What fixes it is gravity: percolation needs no gradient, only room in
 * the cell it is going to, so it does not stall. */
static void test_water_percolates_to_the_bottom_of_a_submerged_pile(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        for (int y = 2; y < H - 1; y++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 6));
        }
    }

    /* Saturated, not merely damp - and that is the whole point of the
     * assert. This grid is shallower than the moisture range, so a pile
     * of it can be wet to the floor by diffusion alone; what diffusion
     * cannot do is SATURATE the bottom, because it settles at one level
     * per cell and the surface only ever holds the maximum. A full bottom
     * row is a water table, and only something that runs downhill without
     * needing a gradient builds one. */
    int wet_floor = 0;
    for (int i = 0; i < 1200 && !wet_floor; i++) {
        /* Held under: the pile has standing water on it throughout. */
        for (int x = 0; x < W; x++) {
            for (int y = 0; y < 2; y++) {
                if (CELL_IS_EMPTY(sand_at(&s, x, y))) {
                    sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
                }
            }
        }
        sand_step(&s, 0, 1000, 0);

        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, H - 2);
            if (CELL_MATERIAL(c) == MAT_DIRT &&
                CELL_MOISTURE(c) == SOIL_MOISTURE_MAX) {
                wet_floor = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(wet_floor,
        "the deepest row of a submerged pile must SATURATE - diffusion "
        "settles into a gradient of one level per cell and stops there, "
        "which leaves the bottom of a pile drier than the top for ever, "
        "however much water is standing on it");
}


/* Percolation goes down and SIDEWAYS-down, not straight down.
 *
 * That is what makes it look like water finding its way into sand -
 * fingers that wander, split where a wet cell sends half one way and half
 * the other, and join where two meet - rather than a flat sheet of damp
 * descending one row at a time. It is also the difference between water
 * getting past an obstacle and water stopping at one.
 *
 * The scene is built so that nothing else can be responsible. The wet cell
 * is walled in on both sides, so the sideways diffusion cannot reach the
 * grains below; and it is walled in directly beneath, so straight-down
 * percolation cannot either. The only way out is diagonal. */
static void test_water_percolates_diagonally_as_well_as_straight_down(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* One row above the floor, so the grains the water has to reach
     * are resting on it - a grain with empty space under it falls
     * out of the scene before any of this gets a turn. */
    const int cx = W / 2, cy = H - 3;

    sand_set(&s, cx, cy, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    sand_set(&s, cx - 1, cy, STONE);          /* no way out sideways */
    sand_set(&s, cx + 1, cy, STONE);
    sand_set(&s, cx, cy + 1, STONE);          /* nor straight down */
    sand_set(&s, cx - 1, cy + 1, CELL_MAKE(MAT_SAND, 6));   /* only these */
    sand_set(&s, cx + 1, cy + 1, CELL_MAKE(MAT_SAND, 6));

    int reached = 0;
    for (int i = 0; i < 600 && !reached; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int d = -1; d <= 1; d += 2) {
            if (CELL_MATERIAL(sand_at(&s, cx + d, cy + 1)) == MAT_DIRT) {
                reached = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(reached,
        "water walled in on both sides and underneath must still get out "
        "diagonally - percolation that only goes straight down is a "
        "rising damp, not water soaking into sand");
}

/* --- seeds ---------------------------------------------------------------- */

/* A seed painted in mid-air falls.
 *
 * It cannot fall the ordinary way. `kind` lives in materials[], and every
 * extended material shares one row of it, so making the plant a
 * KIND_POWDER would make ICE one too - and would break the plant itself,
 * since a grown stem is the same material as the seed and a column of
 * powder six tall would slump the moment it existed. So it falls in the
 * cold pass instead, into empty space and nowhere else. */
static void test_a_seed_falls_until_it_lands(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, W / 2, 0, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. A seed on bare stone can neither drink nor lean on a
     * trunk, so it withers - correctly - and a test that leaves one lying
     * there for hundreds of steps is timing the withering rate rather
     * than the falling. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, W / 2, H - 2),
        "a seed dropped from the top must come to rest on the floor - it "
        "is poured like a grain, and a grain that hangs where the brush "
        "left it is not one");
}

/* Two of them falling side by side must not hold each other up.
 *
 * The rule that keeps a branch attached to its tree is "touching more of
 * yourself", and on its own it is wrong in exactly this way: a pair of
 * seeds falling together each counted the other and the pair stopped dead
 * in mid-air, hanging off nothing at all. An anchor has to be something
 * that is itself standing on something. */
static void test_two_falling_seeds_do_not_hold_each_other_up(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, W / 2, 1, MATX(MATX_PLANT));
    sand_set(&s, W / 2 + 1, 1, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. A seed on bare stone can neither drink nor lean on a
     * trunk, so it withers - correctly - and a test that leaves one lying
     * there for hundreds of steps is timing the withering rate rather
     * than the falling. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Not "both on the floor". Once one of them is down, the other may
     * legitimately come to rest on its shoulder - a seed perched on a
     * grounded seed is a heap, which is what a handful of them poured out
     * should look like. What must not happen is the pair stopping where
     * the brush left them. */
    for (int x = 0; x < W; x++) {
        for (int y = 0; y <= 1; y++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT), sand_at(&s, x, y),
                "neither seed may still be at the height it was painted - "
                "two of them touching are two unsupported things, not one "
                "supported thing");
        }
    }
    int landed = 0;
    for (int x = 0; x < W; x++) {
        if (sand_at(&s, x, H - 2) == MATX(MATX_PLANT)) {
            landed = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(landed,
        "and at least one of them has to reach the floor");
}


/* A whole BRUSHFUL of seeds falls, not just one or two.
 *
 * The brush paints a disc, and a disc is the shape that breaks a careless
 * attachment rule. Two seeds side by side were already covered - each is
 * an anchor for the other only if it is standing on something, and
 * neither is. Two seeds STACKED were not, and they are worse: the upper
 * one qualifies as an anchor because it has something under it, and the
 * something is the very cell asking whether it may fall. The pair holds
 * itself up, and so does everything painted around it.
 *
 * Reported as the plant only falling when water was poured over it, which
 * is the reactions pass being woken for another reason and finding the
 * pile exactly where the brush left it. */
static void test_a_brushful_of_seeds_does_not_hang_in_the_air(void)
{
    fixture();
    sand_clear(&s);

    sand_spawn_cell(&s, W / 2, 2, 2, MATX(MATX_PLANT));
    TEST_ASSERT_TRUE_MESSAGE(count_cells_of(MAT_EXTENDED) > 4,
        "the brush has to have painted a disc, not a single cell - one "
        "seed on its own cannot show this at all");

    /* Watched for, not sampled at some fixed step. A disc on a bare board
     * can neither drink nor lean on a trunk, so it withers as it goes -
     * correctly - and at a leaf's falling pace the two rates are close
     * enough that "is it there at step N" is a coin toss. What the test
     * means is that it got to the bottom, so that is what it waits for. */
    int landed = 0;
    for (int i = 0; i < 300 && !landed; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, H - 1) == MATX(MATX_PLANT)) {
                landed = 1;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(landed,
        "a painted disc of seeds must reach the floor - a cell ABOVE "
        "another cannot be holding it up, and counting it as an anchor "
        "makes the whole pile support itself in mid-air");

    /* No second assert about the top of the disc being empty by then. It
     * is five cells tall on an eight-cell board, so when the lowest one
     * touches the floor the highest is still near where it started - and
     * "it reached the floor at all" is already the whole claim. */
}


/* A seed in a narrow shaft falls down it, rather than sticking to a wall.
 *
 * Only what is GRAVITY-WARD holds a body up. Counting a neighbour beside
 * it as support is an easy thing to write - it is a neighbour, it is
 * solid - and it wedges anything against any vertical surface, which on
 * this board means every seed poured next to a stone wall stops at the
 * height it was poured. */
static void test_a_seed_in_a_shaft_does_not_stick_to_the_walls(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int y = 0; y < H; y++) {
        sand_set(&s, cx - 1, y, STONE);
        sand_set(&s, cx + 1, y, STONE);
    }
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, cx, 0, MATX(MATX_PLANT));

    /* Long enough to fall the height of the board at a leaf's pace, and
     * no longer. A seed on bare stone can neither drink nor lean on a
     * trunk, so it withers - correctly - and a test that leaves one lying
     * there for hundreds of steps is timing the withering rate rather
     * than the falling. */
    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT), sand_at(&s, cx, H - 2),
        "a seed between two walls must fall to the bottom of the shaft - "
        "what is beside a thing does not hold it up, and treating it as "
        "support wedges everything against every wall on the board");
}


/* A settled faller keeps the pass armed.
 *
 * may_have_faller gates the whole reactions pass, and it was being set by
 * a cell MOVING rather than by one existing. So a board holding one
 * settled plant cleared the flag on the first step - and then nothing
 * could set it again, because latching happens when a cell is created and
 * a plant that is already there is not created twice.
 *
 * Everything downstream of that is quietly dead: dissolve the ground out
 * from under a plant with acid and it hangs in the air; the greenery never
 * withers. The cold pass documents the same shape of bug for snow sitting
 * on dry ground, and for the same reason: a cell with nothing to do NOW is
 * not a cell with nothing to do EVER.
 *
 * Asserting on the flag rather than on a scene, deliberately. Every scene
 * that shows the consequence is a race between falling and withering, and
 * the flag is the actual invariant. */
static void test_a_settled_plant_keeps_the_reaction_pass_armed(void)
{
    fixture();
    sand_clear(&s);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Resting on the floor, so it never moves, and beside wood, so it
     * never withers - it just sits there being a plant. */
    sand_set(&s, W / 2, H - 2, MATX(MATX_PLANT));
    sand_set(&s, W / 2 + 1, H - 2, CELL_MAKE(MAT_WOOD, 0));

    for (int i = 0; i < 100; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, W / 2, H - 2), "the plant has to still be there");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_faller,
        "a plant on the board must keep the faller flag armed even when it "
        "has not moved for a hundred steps - the flag says what is PRESENT, "
        "and once it clears nothing can arm it again");
}

/* And what a tree grows must NOT fall.
 *
 * The other half of the same rule, and the reason it cannot simply be
 * "fall when there is nothing underneath": a branch grows out sideways
 * over thin air, and without attachment every limb would snap off on the
 * step it appeared. */
static void test_a_growing_tree_does_not_shed_what_it_grows(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int grown = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        /* Nothing may ever be sitting on empty space unattached. */
        for (int y = 0; y < H - 1; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) != MATX(MATX_PLANT)) {
                    continue;
                }
                grown++;
                /* All EIGHT, because a branch grows out at an angle -
                 * that is what makes it a branch - so the cell it grew
                 * from is diagonally below it and orthogonally it may be
                 * touching nothing at all. Checking four was checking the
                 * shape a tree does not have. */
                int touching = 0;
                for (int d = 0; d < 8; d++) {
                    static const int ox[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
                    static const int oy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
                    const int nx = x + ox[d];
                    const int ny = y + oy[d];
                    if ((unsigned)nx >= (unsigned)W ||
                        (unsigned)ny >= (unsigned)H) {
                        continue;
                    }
                    if (!CELL_IS_EMPTY(sand_at(&s, nx, ny))) {
                        touching = 1;
                    }
                }
                TEST_ASSERT_TRUE_MESSAGE(touching,
                    "no part of a tree may be floating free - a limb that "
                    "detaches from what grew it is a bug in the falling "
                    "rule, not weather");
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(grown > 0, "the tree has to have grown at all");
}


/* A BURIED seed comes up through the soil.
 *
 * Which is how you plant one - drop a seed, cover it over, water it - and
 * it was the one arrangement guaranteed to do nothing at all. Growth put
 * its new cell in empty space, and a buried seed has none: the cell it
 * wanted was occupied, and occupied was the end of the matter.
 *
 * A shoot shoves instead. The run of loose material above it shifts up one
 * and the shoot takes the space, which is why the soil count is checked as
 * carefully as the emergence - a shoot that ATE its way out would pass the
 * first assert perfectly and quietly hollow out every bank on the board. */
static void test_a_buried_seed_comes_up_through_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        for (int y = H - 5; y < H - 1; y++) {
            sand_set(&s, x, y, CELL_SOIL(MAT_DIRT, x & 1,
                                         y >= H - 3 ? SOIL_MOISTURE_MAX : 0));
        }
    }
    /* Two rows of soil on top of it. */
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));
    const int soil = count_cells_of(MAT_DIRT);

    int up = 0;
    for (int i = 0; i < 1500 && !up; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int x = 0; x < W; x++) {
            for (int y = 0; y <= H - 6; y++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT) ||
                    CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WOOD) {
                    up = 1;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(up,
        "a seed under two rows of watered soil must reach daylight - "
        "burying a seed and watering it is how you plant one, and it was "
        "the one way to guarantee nothing happened");
    TEST_ASSERT_EQUAL_INT_MESSAGE(soil, count_cells_of(MAT_DIRT),
        "and it must SHOVE the soil aside, not eat it - a shoot that "
        "consumed its cover would tunnel every bank on the board hollow "
        "while passing every other test here");
}

/* But it will not push through stone.
 *
 * The other half of the same rule, and what keeps shoving from being a
 * licence to go anywhere: a shoot displaces loose things - powders,
 * liquids, gases - and stops dead at anything STATIC. Without that, a seed
 * under a flagstone lifts it. */
static void test_a_seed_under_stone_stays_put(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        sand_set(&s, x, H - 4, STONE);          /* a lid */
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    /* Kept watered. Soil dries, and a seed that cannot drink withers - so
     * without this the seed does eventually disappear, which is correct
     * behaviour and nothing to do with what this test is about. */
    for (int i = 0; i < 1500; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "a seed with a stone lid over it must stay one seed - a shoot "
        "shoves what is loose and stops at what is not, or it lifts "
        "flagstones");
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(STONE, sand_at(&s, x, H - 4),
            "and the lid must still be where it was put");
    }
}


/* A limb attached to a TRUNK stays on it.
 *
 * A branch grows out at an angle, so the cell it came from is diagonally
 * below it; orthogonally it is often touching nothing. Checking four
 * neighbours for an anchor therefore snapped off every limb whose trunk
 * did not happen to continue past it - and on a tilt, where the wood stays
 * put while gravity swings round underneath, whole crowns detached at once
 * and dropped a cell a step. Reported as the plant appearing to teleport,
 * and as the falling looking harsh: it was not the speed, it was how much
 * of the tree was falling.
 *
 * Hardening is what makes this bite, which is why the trunk here is wood:
 * a limb has to recognise what its own material turns into as something
 * to hold on to. */
static void test_a_limb_hangs_on_to_a_wooden_trunk(void)
{
    fixture();
    sand_clear(&s);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    /* Diagonally off the top of it, touching nothing else. */
    sand_set(&s, cx + 1, H - 5, MATX(MATX_PLANT));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_PLANT),
        sand_at(&s, cx + 1, H - 5),
        "a limb growing diagonally off a trunk must stay where it grew - "
        "it is touching the tree, just not squarely, and a tree that sheds "
        "every branch it grows is not one");
}


/* Loose greenery withers; greenery on a tree does not.
 *
 * Growth is the only thing on this board that makes cells, and until now
 * nothing took them away again except fire and acid. So every fragment a
 * tree shed - a limb broken off by a tilt, a seed poured onto bare stone -
 * was permanent, and the board silted up with green litter that could
 * neither do anything nor go anywhere.
 *
 * All three cases are checked together because the rule is only useful if
 * it can tell them apart. Withering everything that is thirsty would strip
 * a grown tree the moment its soil dried out. */
static void test_loose_greenery_withers_but_a_tree_keeps_its_leaves(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* left: a scrap on bare stone, nothing to drink and nothing to hold */
    sand_set(&s, 1, H - 2, MATX(MATX_PLANT));
    /* middle: a trunk with a LEAF on it and a stem beside it. The leaf
     * is sheltered by the wood; the stem is not, and that difference is
     * the point - foliage in a drought is a tree keeping its leaves,
     * green growth in a drought is growth that failed and should go. */
    sand_set(&s, 4, H - 2, CELL_MAKE(MAT_WOOD, 0));
    sand_set(&s, 5, H - 2, MATX(MATX_LEAF));
    sand_set(&s, 3, H - 2, MATX(MATX_PLANT));
    /* right: on watered soil */
    sand_set(&s, W - 1, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    sand_set(&s, W - 1, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 2000; i++) {
        /* The right-hand patch stays watered, so its plant can always
         * drink; the other two never can. */
        sand_set(&s, W - 1, H - 2,
                 CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 1, H - 2)),
        "a scrap of green on bare stone must eventually go - it can "
        "neither drink nor lean on a trunk, and nothing else on the board "
        "would ever have cleared it away");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_LEAF), sand_at(&s, 5, H - 2),
        "but FOLIAGE touching wood must stay, however dry it gets - a tree "
        "in a drought keeps its leaves, and a leaf cannot fall, so nothing "
        "else would ever clear a crown whose trunk had burned away");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, H - 2)),
        "while green GROWTH touching wood must not - sheltered growth "
        "never dies, so every stem that failed to finish its run stayed on "
        "the tree for ever, which is what the stacking was");
    /* Still plant, or already grown into wood - either way it is alive
     * and still there, which is the claim. Two thousand steps on watered
     * soil is plenty of time for a seedling to become a trunk. */
    const cell_t fed = sand_at(&s, W - 1, H - 3);
    TEST_ASSERT_TRUE_MESSAGE(fed == MATX(MATX_PLANT) ||
                             CELL_MATERIAL(fed) == MAT_WOOD,
        "and a plant that can still reach water must survive - withering "
        "is for what has nothing behind it, not for anything thirsty");
}

/* A tree is not a stick.
 *
 * Growth used to go straight up from the tip, every time, which grew a
 * one-cell column and nothing else - reported as growing "mostly one
 * side". Reaching the tip is still the common case; what makes it a tree
 * is that it sometimes leans, sometimes starts a limb further down, and
 * sometimes thickens the trunk instead. The last of those is what turns a
 * sapling into wood, because hardening counts a straight run along gravity
 * and a second column beside the first is a second run of its own. */
static void test_a_tree_grows_wider_than_one_column(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int columns = 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H - 2; y++) {
            const cell_t c = sand_at(&s, x, y);
            if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
                columns++;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, columns,
        "a tree must occupy more than the one column it was sown in - "
        "growing only from the tip and only straight up is a stick");
}


/* Water sitting on a plant goes into the ground.
 *
 * Every extended material shares one physics row, and that row is
 * `KIND_STATIC` at stone's density - so water cannot fall through foliage
 * and nothing about foliage can soak it up. Fill a bowl of leaves and the
 * water stays there for ever, which is exactly what it looked like.
 *
 * A plant conducts it instead: a unit of the liquid for a level of
 * moisture in the soil its roots reach. It cannot use the ordinary
 * `soaks` path to do it, because that raises the cell's own variant to
 * hold what it took, and a plant's variant is WHICH EXTENDED MATERIAL IT
 * IS - one soak would turn it into the next entry in the table.
 *
 * The scene walls the water in so the plant is the only way out. Water
 * that simply drained round the side would pass an assert about the water
 * going away while proving nothing at all. */
static void test_a_plant_drains_standing_water_into_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry */
    }
    const int cx = W / 2;
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));               /* the plug */
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    /* Sampled as it goes, not at the end. Soil dries, so by the time the
     * water is gone the moisture it turned into has gone too - asserting
     * on the final state tests the drying rate, not the drinking. */
    int wet = 0;
    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
        int now = 0;
        for (int x = 0; x < W; x++) {
            now += CELL_MOISTURE(sand_at(&s, x, H - 2));
        }
        if (now > wet) {
            wet = now;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "water walled in above a plant must drain through it - foliage is "
        "solid and undrainable otherwise, so a bowl of leaves holds a pond "
        "for ever");
    TEST_ASSERT_TRUE_MESSAGE(wet > 0,
        "and it must come out at the ROOTS - water that merely vanished "
        "would pass the assert above and quietly delete itself");
}

/* A plant with nowhere to put it does not drink.
 *
 * The guard on the test above, and the thing that stops this being a
 * disposal chute for water: what a plant does with a drink is hand it to
 * the soil. Rooted on bare stone there is no soil, so the water stays
 * where it is. */
static void test_a_plant_rooted_on_stone_does_not_drink(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, STONE);          /* no soil anywhere */
    }
    const int cx = W / 2;
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_WATER),
        "a plant standing on bare stone has nowhere to put a drink, so "
        "the water must stay - drinking is conduction into the ground, "
        "not a way of making water disappear");

    /* Nor does it drink OIL, however good the ground beneath it is. The
     * same rule as soaking and the same reason: only the liquid knows
     * whether it is wet, and a plant asking for "any adjacent liquid"
     * would siphon a slick into the soil as moisture. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_PLANT));
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_OIL, MASS_MAX));

    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_OIL),
        "oil walled in above a plant must stay there - a plant drinks "
        "water, and a rule that takes any liquid pipes a slick into the "
        "ground as moisture");
}



/* Hardening leaves a CROWN behind.
 *
 * Hardening is the only moment that holds a whole run at once, and after it
 * the tree has no green left low down - everything that grew is timber, and
 * growth is the only thing that makes cells. So a canopy has to be hung
 * here or nowhere; anything trying to leaf a tree through ordinary growth
 * is working on a part of it that no longer exists.
 *
 * Reported as branches spawning "and many times just be wood". */
static void test_a_hardened_trunk_is_left_with_foliage(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int leafed = 0;
    for (int i = 0; i < 3000 && !leafed; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);

        for (int y = 0; y < H && !leafed; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                    leafed = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(leafed,
        "a tree that hardens into wood must be left with foliage on it - "
        "nothing else can put any there, because hardening consumes every "
        "green cell it walks and wood does not grow");
}

/* And a hardened run is WIDER at its foot than at its top.
 *
 * "Plant should also widen as it grows, so it is interesting that wood is
 * just a stick." It was: thickening happened during growth, gated on how
 * far the growing cell was from the ground, and after the first hardening
 * every green cell sits on a wood column - so its lift is at least that
 * column's height and the allowance is zero for the rest of the tree's
 * life. Thickening was structurally dead the moment a tree first became a
 * tree.
 *
 * The taper is the half that is easy to lose. A trunk of uniform width is
 * a pillar, passes any "is it thick" assert, and looks nothing like a
 * tree - so this measures both ends and compares them, rather than
 * measuring one and hoping. */
static void test_a_hardened_trunk_is_thicker_at_the_foot(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    int foot = 0, top = 0;
    for (int i = 0; i < 3000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);

        /* Widest wood row anywhere, against the widest in the top half. */
        for (int y = 0; y < H - 2; y++) {
            int wide = 0;
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WOOD) {
                    wide++;
                }
            }
            if (wide > foot) {
                foot = wide;
            }
            if (y < (H - 2) / 2 && wide > top) {
                top = wide;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, foot,
        "a hardened trunk must be more than one cell wide somewhere - "
        "thickening during growth cannot reach a tree that has already "
        "hardened, so it has to happen as the wood is laid");
    TEST_ASSERT_GREATER_THAN_MESSAGE(top, foot,
        "and it must be WIDER at the foot than up in the branches - a "
        "trunk of uniform width is a pillar, and passes every assert that "
        "only asks whether it is thick");
}

/* Hardening takes the WHOLE run, tip and all.
 *
 * The reverse of what this file asserted until now, deliberately. A green
 * tip was the only way a tree could get taller, so hardening stopped one
 * cell short - and that is why a tree carried green around for ever.
 * Growth comes from crowned wood now (reaction_t.buds), so a run can turn
 * to timber entire and the tree still has a future.
 *
 * Asserted as "a tree reaches a state with no plant on it at all", which
 * is the claim that matters: plant is a phase a cell passes through, not a
 * tissue a tree keeps. */
static void test_a_finished_tree_carries_no_green(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    /* Watered while it grows, then left alone. Kept watered for ever it
     * simply keeps budding, which is the model working; the claim here is
     * about what a tree looks like once it is FINISHED.
     *
     * And leaving it dry is what makes the test discriminate: a green tip
     * clings to wood, so on a build that leaves one it survives drought
     * indefinitely and this never reaches zero. */
    int wooded = 0, bare = 0;
    for (int i = 0; i < 4000 && !bare; i++) {
        if (i < 1500) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, H - 2,
                         CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
        }
        sand_step(&s, 0, 1000, 0);

        if (count_cells_of(MAT_WOOD) > 0) {
            wooded = 1;
        }
        if (!wooded) {
            continue;
        }
        int green = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT)) {
                    green++;
                }
            }
        }
        if (green == 0) {
            bare = 1;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(wooded, "the tree has to have hardened at all");
    TEST_ASSERT_TRUE_MESSAGE(bare,
        "a tree must reach a state with no plant cells on it - hardening "
        "leaves no tip behind, so green is a phase a cell passes through "
        "on its way to timber rather than something a tree keeps");
}


/* A limb TRAVELS. It does not go out one cell and then climb.
 *
 * Growth used to reckon every direction from gravity, which meant a branch
 * could never get anywhere: one cell out makes a run of ONE, and a run
 * under three trips the gate that forces the straight-up arm - on every
 * attempt, for ever. So limbs went out a single cell and then grew
 * vertically alongside the trunk, which is the same thin-thread shape that
 * made basal suckers look like floating debris.
 *
 * A run's direction is not stored anywhere; it is read back off the grid,
 * from where the run has been over its last few cells. `holds_line` is the
 * chance of using it, and zero restores the old behaviour exactly - which
 * is what this is really pinned against.
 *
 * On a grid of its own, because the shared fixture is eight by eight and a
 * diagonal limb runs out of ceiling in three cells - far too soon to tell
 * travelling from the sideways cell an occasional LEAN produces anyway.
 * The limb is pre-built with a heading already established, because what
 * is being tested is what a run does once it HAS a direction. */
#define LIMB_W 30
#define LIMB_H 26
static uint8_t limb_cells[LIMB_W * LIMB_H];

static void test_a_limb_travels_outward_instead_of_climbing(void)
{
    /* Summed over eight seeds, and that is not laziness about a flaky
     * test - it is what the measurement actually supports. On any single
     * seed the two builds overlap: with the heading, one limb reached 7
     * and another 13; without it, the range is 6 to 7. A threshold picked
     * to split one seed would be fitted to that seed and would say
     * nothing. Totalled, the gap is unmistakable - 70 against 52 - and
     * the assert sits in the middle of it. */
    int total = 0;
    for (unsigned seed = 1; seed <= 8; seed++) {
        sand_t t;
        memset(limb_cells, 0, sizeof limb_cells);
        sand_init(&t, limb_cells, LIMB_W, LIMB_H, seed * 7919u);
        sand_set_soak(&t, SAND_SOAK_PER_MATERIAL);
        sand_set_decay(&t, SAND_DECAY_PER_MATERIAL);

        const int bx = 3;
        for (int x = 0; x < LIMB_W; x++) {
            sand_set(&t, x, LIMB_H - 1, STONE);
            sand_set(&t, x, LIMB_H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        for (int y = LIMB_H - 8; y < LIMB_H - 2; y++) {
            sand_set(&t, bx, y, CELL_MAKE(MAT_WOOD, 0));
        }
        sand_set(&t, bx + 1, LIMB_H - 9,  MATX(MATX_PLANT));
        sand_set(&t, bx + 2, LIMB_H - 10, MATX(MATX_PLANT));
        sand_set(&t, bx + 3, LIMB_H - 11, MATX(MATX_PLANT));

        int reach = bx + 3;
        for (int i = 0; i < 4000; i++) {
            for (int x = 0; x < LIMB_W; x++) {
                sand_set(&t, x, LIMB_H - 2,
                         CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
            }
            sand_step(&t, 0, 1000, 0);

            for (int y = 0; y < LIMB_H - 2; y++) {
                for (int x = 0; x < LIMB_W; x++) {
                    const cell_t c = sand_at(&t, x, y);
                    if ((c == MATX(MATX_PLANT) ||
                         CELL_MATERIAL(c) == MAT_WOOD) && x > reach) {
                        reach = x;
                    }
                }
            }
        }
        total += reach;
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(60, total,
        "limbs with a heading must carry on in that direction - reckoning "
        "every growth from gravity instead sends one straight up the side "
        "of its own trunk, and no tree ever puts out a bough");
}


/* A crowned trunk puts out new growth; a bare one does not.
 *
 * This is where a tree's growth comes from now. It used to come from a
 * green tip that hardening deliberately spared - which meant every tree
 * carried green permanently, and growth scaled with how much of it there
 * was, because every green cell rolled every step.
 *
 * The "already in leaf" half is not decoration, it is the bound. A canopy
 * touches a dozen cells of wood; if bare wood could bud, the rate would
 * scale with the trunk and the forest would run away exactly as it did
 * when growth scaled with green. Crowned wood at the head of its trunk is
 * a handful of cells per tree however fat it gets. */
static void test_a_crowned_trunk_buds_and_a_bare_one_does_not(void)
{
    const int cx = W / 2;

    /* Crowned: wood, a leaf on it, wet ground under it. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 5, MATX(MATX_LEAF));

    int budded = 0;
    for (int i = 0; i < 4000 && !budded; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !budded; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT)) {
                    budded = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(budded,
        "a trunk in leaf and in reach of water must put out new growth - "
        "hardening leaves no tip behind, so this is the only way a tree "
        "gets any taller");

    /* Bare: the same trunk, same water, no leaf on it. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }

    for (int i = 0; i < 4000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT),
                    sand_at(&s, x, y),
                    "bare wood must NOT bud - a canopy touches a dozen "
                    "cells of trunk, and if every one of them could bud, "
                    "the rate would scale with the tree all over again");
            }
        }
    }

    /* And crowned, but on ground too thin to pay for a limb. A bud costs
     * BUD_COST levels of moisture, not one - because a bud is the only
     * thing here that COMPOUNDS, so what has to bound it is the scarce
     * thing rather than a probability. Priced at one level, buds simply
     * drank the pour and the forest ran away. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 5, MATX(MATX_LEAF));

    for (int i = 0; i < 4000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));
        }
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(MATX(MATX_PLANT),
                    sand_at(&s, x, y),
                    "a crowned trunk on barely damp ground must NOT bud - "
                    "a limb has to be paid for, or the only thing bounding "
                    "the one mechanism that compounds is a dice roll");
            }
        }
    }
}

/* --- foliage -------------------------------------------------------------- */

/* A leaf on a tree never multiplies, and never moves.
 *
 * This is the entire reason foliage is its own material rather than more
 * plant. Every cell of a PLANT is a grower - foliage touches wood so it
 * never withers, and find_water() walks down through wood so it can always
 * drink - which means a canopy made of plant would feed the growth loop
 * with every leaf it put out, and that loop has run away once already.
 * Charging moisture per leaf makes it expensive; having no `grows` field
 * makes it impossible.
 *
 * Two scenes, because one cannot show both halves without confusing them.
 * A trunk standing in wet soil BUDS leaves of its own (see wood's
 * `sprouts`), so a scene with both a trunk and watered ground cannot tell
 * "the leaf spread" from "the tree put out another one" - which is exactly
 * how this test first failed when budding changed from plant to foliage. */
static void test_a_leaf_neither_spreads_nor_falls(void)
{
    /* One: on watered soil with NO wood anywhere. It can drink, so it will
     * not wither, and nothing else in the scene can produce foliage - so
     * any second leaf would have to have come from the first. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_LEAF));

    for (int i = 0; i < 2000; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2,
                     CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "a leaf on watered ground must stay exactly one leaf - foliage "
        "that can grow is a canopy that feeds the growth loop, which is "
        "the whole reason it is not made of plant");

    /* Two: hanging off the side of a trunk, over nothing, on DRY ground -
     * dry so the trunk cannot bud, which would put leaves in the scene
     * that this half is not about. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        sand_set(&s, cx, y, CELL_MAKE(MAT_WOOD, 0));
    }
    sand_set(&s, cx + 1, H - 4, MATX(MATX_LEAF));

    for (int i = 0; i < 2000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_LEAF),
        sand_at(&s, cx + 1, H - 4),
        "and it must not have moved - it has no `falls`, so a limb of "
        "foliage hangs off its trunk over thin air and stays there");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count_cells_of(MAT_EXTENDED),
        "nor multiplied, on dry ground where nothing can bud");
}

/* A leaf with no tree behind it goes.
 *
 * The other half of having no `falls`: nothing else would ever clear it.
 * Burn a trunk out from under a crown and, without this, the crown hangs
 * in the air permanently. `clings_to` is what tells the two apart - wood
 * beside it means it is still part of something. */
static void test_a_leaf_with_no_tree_withers_away(void)
{
    fixture();
    sand_clear(&s);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 4, MATX(MATX_LEAF));          /* alone, in mid-air */
    sand_set(&s, W - 3, H - 2, CELL_MAKE(MAT_WOOD, 0));
    sand_set(&s, W - 2, H - 2, MATX(MATX_LEAF));      /* beside a trunk */

    int gone = 0;
    for (int i = 0; i < 4000 && !gone; i++) {
        sand_step(&s, 0, 1000, 0);
        gone = CELL_IS_EMPTY(sand_at(&s, 2, H - 4));
    }

    TEST_ASSERT_TRUE_MESSAGE(gone,
        "a leaf with no wood beside it must wither - it cannot fall, so "
        "nothing else on the board would ever take it away, and a crown "
        "whose trunk burned out would hang there for good");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MATX(MATX_LEAF), sand_at(&s, W - 2, H - 2),
        "while one still touching a trunk stays, however dry it gets");
}

/* And water gets through a canopy.
 *
 * The field most likely to be left off, because it sounds like the
 * opposite of everything else about this material. Every extended material
 * shares one physics row - KIND_STATIC at stone's density - so water can
 * neither fall through foliage nor soak into it, and a bowl of leaves
 * holds a pond indefinitely. That was a real bug on the plant already, and
 * a leaf is the surface rain actually lands on. */
static void test_a_leaf_drains_standing_water_into_the_soil(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    const int cx = W / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));   /* bone dry */
    }
    sand_set(&s, cx - 1, H - 3, STONE);
    sand_set(&s, cx + 1, H - 3, STONE);
    sand_set(&s, cx - 1, H - 4, STONE);
    sand_set(&s, cx + 1, H - 4, STONE);
    sand_set(&s, cx, H - 3, MATX(MATX_LEAF));            /* the only way out */
    sand_set(&s, cx, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));

    int wet = 0;
    for (int i = 0; i < 900; i++) {
        sand_step(&s, 0, 1000, 0);
        int now = 0;
        for (int x = 0; x < W; x++) {
            now += CELL_MOISTURE(sand_at(&s, x, H - 2));
        }
        if (now > wet) {
            wet = now;
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_WATER),
        "water walled in above a leaf must drain through it - foliage is "
        "solid and undrainable otherwise, and a canopy would hold a pond");
    TEST_ASSERT_TRUE_MESSAGE(wet > 0,
        "and it must come out in the soil, not simply vanish");
}


/* A leaf that stops being fed dries by STAGES rather than blinking out.
 *
 * An extended material has no variant to age in - its low nibble is which
 * one it is - so a stage can only be held by being a different material.
 * Green, dry, dead, gone: three rows in the cold table and three palette
 * entries, and nothing at all added to the sweep.
 *
 * The later stages are deliberately SLOWER than the first. The obvious way
 * round - each step quicker than the last, a leaf accelerating towards
 * death - was measured and was over before you could see it: a crown of
 * eight showed at most one yellow cell at any sample. A leaf spends most
 * of its dying being visibly dead. */
static void test_a_dying_leaf_yellows_before_it_goes(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* A crown with no tree and no water under it: nothing feeds it, and
     * nothing shelters it. */
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, 2, MATX(MATX_LEAF));
    }

    int saw_dry = 0, saw_dead = 0, all_gone = 0;
    for (int i = 0; i < 6000 && !all_gone; i++) {
        sand_step(&s, 0, 1000, 0);

        int green = 0, dry = 0, dead = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                green += (c == MATX(MATX_LEAF));
                dry   += (c == MATX(MATX_LEAF_DRY));
                dead  += (c == MATX(MATX_LEAF_DEAD));
            }
        }
        saw_dry  |= (dry  > 0);
        saw_dead |= (dead > 0);
        all_gone  = (green + dry + dead) == 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_dry,
        "a leaf losing its tree must go YELLOW on the way out - dying in "
        "one step is a cell being deleted, not a leaf drying");
    TEST_ASSERT_TRUE_MESSAGE(saw_dead,
        "and brown after that");
    TEST_ASSERT_TRUE_MESSAGE(all_gone,
        "and be gone in the end - the whole reason foliage withers is that "
        "it cannot fall, so nothing else would ever clear it");
}

/* --- growing ------------------------------------------------------------- */

/* A plant on wet soil climbs.
 *
 * It has no variant to grow WITH - the low nibble is which extended
 * material it is - so it grows by occupying cells, walking to the tip of
 * its own column and putting the new one beyond that. This is the test
 * that the walk happens at all: without it a plant could only ever be one
 * cell tall, because the moment it grew, the new cell would be out of
 * reach of the ground and nothing further could happen. */
static void test_a_plant_on_wet_soil_grows_upward(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int tall = 0;
    for (int y = 0; y < H; y++) {
        const cell_t c = sand_at(&s, W / 2, y);
        if (c == MATX(MATX_PLANT) || CELL_MATERIAL(c) == MAT_WOOD) {
            tall++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(tall >= 3,
        "a plant standing on watered soil must climb away from the ground "
        "- growth is spatial for a material with no variant to spend");
}

/* And on dry soil it does not.
 *
 * Water is the whole limit on how far a tree gets, so soil with nothing in
 * it has to stop one. Without this the plant is not a plant, it is a
 * self-replicating material that fills the screen. */
static void test_a_plant_on_dry_soil_stays_where_it_is(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);

    /* Wet soil on the far side of a stone divider, so the board as a
     * whole HAS moisture on it - otherwise may_have_moisture is clear, the
     * growth branch is never reached, and the test would pass on the
     * strength of a pass gate rather than on the plant looking at the
     * ground it is actually standing on. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, x == W / 2 + 1 ? STONE
                             : x > W / 2 + 1
                                 ? CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX)
                                 : CELL_SOIL(MAT_DIRT, 0, 0));
    }
    sand_set(&s, 1, H - 3, MATX(MATX_PLANT));

    /* The most it is ever seen to be, not what is left at the end. A
     * plant that cannot drink withers, so a seedling on dry ground is
     * gone long before step 400 - which is right, and would make an
     * assert on the final count pass for the wrong reason. What this is
     * about is that it never GREW. */
    int tall = 0;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);

        int now = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_PLANT)) {
                    now++;
                }
            }
        }
        if (now > tall) {
            tall = now;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tall,
        "a plant on bone-dry soil must stay a seedling - moisture is the "
        "only thing limiting how tall a tree gets");
}

/* A tall enough plant becomes a trunk - and the trunk is not on fire.
 *
 * Wood's variant is its BURN PROGRESS, and the general placement helper
 * hands a new cell MATERIAL_VARIANTS - 1, which is right for a fill level
 * or a life counter and means "well alight" for wood. That is deliberate
 * where the reaction is fire making an ember of a log; it is catastrophic
 * here. Every tree that reached this height burned to nothing over the
 * next couple of hundred steps, on a board with no flame anywhere on it,
 * so the assert on the variant matters as much as the one on the
 * material. */
static void test_a_tall_plant_hardens_into_wood_that_is_not_alight(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    sand_set(&s, W / 2, H - 3, MATX(MATX_PLANT));

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int wood = 0;
    for (int y = 0; y < H; y++) {
        const cell_t c = sand_at(&s, W / 2, y);
        if (CELL_MATERIAL(c) != MAT_WOOD) {
            continue;
        }
        wood++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, CELL_VARIANT(c),
            "a trunk is wood that GREW, not wood that caught - wood's "
            "variant is burn progress, and a log placed at the top of it "
            "burns away on a board with no fire on it");
    }
    TEST_ASSERT_TRUE_MESSAGE(wood > 0,
        "a plant that grows a full run tall must harden into wood");
}


/* The grain hash must not stripe.
 *
 * Stone and wood take their speckle from this and nothing else, so if its
 * low bits do not vary they are not speckled - and for a long time they
 * were not. The low three bits came out very nearly constant along a row,
 * which drew both materials as flat horizontal bands, one shade each.
 * Nothing caught it because the hash lived in app_sand.c, which does not
 * compile on a host; moving it next to the tables that use it is half the
 * fix and this is the other half.
 *
 * Both halves of the assert matter. Even spread alone is satisfied by a
 * hash that stripes, as long as it stripes in equal proportions - it is
 * the ADJACENCY that says the variation is per cell rather than per row. */
static void test_the_grain_hash_does_not_stripe(void)
{
    enum { N = 64, BUCKETS = 8 };

    int same_row = 0, same_col = 0, seen[BUCKETS] = { 0 };
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const unsigned h = material_grain_hash(x, y) & (BUCKETS - 1);
            seen[h]++;
            if (x > 0 &&
                h == (material_grain_hash(x - 1, y) & (BUCKETS - 1))) {
                same_row++;
            }
            if (y > 0 &&
                h == (material_grain_hash(x, y - 1) & (BUCKETS - 1))) {
                same_col++;
            }
        }
    }

    /* One in eight neighbours will match by chance. Twice that is still
     * comfortably clear of the near-100% a striping hash produces. */
    const int pairs = N * (N - 1);
    const int limit = pairs / BUCKETS * 2;
    TEST_ASSERT_LESS_THAN_MESSAGE(limit, same_row,
        "side-by-side cells must mostly differ - a hash whose low bits are "
        "constant along a row paints stone and wood in flat stripes");
    TEST_ASSERT_LESS_THAN_MESSAGE(limit, same_col,
        "and so must cells one above another, for the same reason with the "
        "board turned ninety degrees");

    for (int i = 0; i < BUCKETS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[i] > N * N / BUCKETS / 2,
            "every shade must get used, and roughly evenly - a grain that "
            "reaches two of its eight values is two-tone noise");
    }
}


/* The dithered direction is recorded, and it is not the nearest one.
 *
 * Two different questions about gravity, and the simulation has always
 * needed both. "Which way is down, near enough" has to be steady, or a
 * resting pool judged against a constantly-changing axis reads as
 * unbalanced when it is not. "Which way is down THIS step" has to wobble
 * between the two eighths a tilt falls between, in proportion, or
 * everything flows at one of eight fixed angles instead of at the angle
 * the board is really at.
 *
 * The sweep and the liquid pass have used the dithered one all along.
 * Growth was reading the steady one, which is what made a stem a rigid
 * straight line - reported as the vertical growth being "too strict or
 * rigid whereas we have smoothing in the way we map tilt/gravity", and
 * exactly right. This is the plumbing that fixes it.
 *
 * Both asserts are needed: a build that recorded the nearest direction in
 * both fields passes the second on its own. */
static void test_a_tilt_between_two_directions_is_dithered_not_snapped(void)
{
    fixture();
    sand_clear(&s);

    /* Well off any of the eight axes, so the two it falls between should
     * both come up. */
    int steps_seen = 0, load_seen = 0;
    int step_dx[8] = { 0 }, load_dx[8] = { 0 };

    for (int i = 0; i < 200; i++) {
        sand_step(&s, 400, 1000, 0);

        int found = 0;
        for (int k = 0; k < steps_seen; k++) {
            if (step_dx[k] == s.last_step_dx) {
                found = 1;
            }
        }
        if (!found && steps_seen < 8) {
            step_dx[steps_seen++] = s.last_step_dx;
        }

        found = 0;
        for (int k = 0; k < load_seen; k++) {
            if (load_dx[k] == s.last_load_dx) {
                found = 1;
            }
        }
        if (!found && load_seen < 8) {
            load_dx[load_seen++] = s.last_load_dx;
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(1, steps_seen,
        "a tilt between two of the eight directions must spend steps on "
        "both - one value means everything on the board flows at a snapped "
        "angle rather than at the angle the board is at");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, load_seen,
        "while the NEAREST direction stays put, which is what anything "
        "measuring a resting state has to be judged against");
}


/* A stem that WANDERS is still one stem.
 *
 * Growth points along the dithered gravity direction, which spends steps
 * on each of the two eighths a tilt falls between - so a trunk climbs with
 * a kink in it rather than in a dead straight line. Every walk over a
 * plant has to tolerate that: the walk to the tip, the walk back to a
 * branch site, and the run that decides whether it has grown tall enough
 * to be wood.
 *
 * A staircase is the cheapest way to say so. Six cells, each one up or up
 * and across from the last, standing on wet soil: a walk that insists on a
 * straight line sees a run of two and this never becomes a trunk. */
static void test_a_stem_that_wanders_still_hardens(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    /* up, up, up-left, up, up-left, up */
    static const int stem[6][2] = {
        { 4, 5 }, { 4, 4 }, { 3, 3 }, { 3, 2 }, { 2, 1 }, { 2, 0 },
    };
    for (int i = 0; i < 6; i++) {
        sand_set(&s, stem[i][0], stem[i][1], MATX(MATX_PLANT));
    }

    /* Kept watered throughout. Soil dries, growth costs moisture, and
     * hardening only happens on a growth - so a single saturation turns
     * this into a race against the drying rate rather than a test of the
     * walk. */
    int hardened = 0;
    for (int i = 0; i < 4000 && !hardened; i++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
        }
        sand_step(&s, 0, 1000, 0);
        for (int k = 0; k < 6; k++) {
            if (CELL_MATERIAL(sand_at(&s, stem[k][0], stem[k][1])) ==
                MAT_WOOD) {
                hardened = 1;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(hardened,
        "a stem six cells long must harden into wood even though it is not "
        "straight - a walk that only steps in one direction measures a run "
        "of two and no tree on a tilted board would ever become a trunk");
}


/* A bare trunk in wet ground buds again.
 *
 * The loop this closes: growth hardens a plant into wood, and hardening
 * consumes the very cells that could grow. So a tree that reached its full
 * height was finished for good, and one that lost its foliage - to fire,
 * to acid, to a landslide - stayed a bare post for ever. The scene here is
 * the worst case on purpose: wood only, no plant anywhere on the board, so
 * nothing but the trunk itself can be responsible for what appears.
 *
 * The dry half of the test is the half that matters. Budding out of
 * nothing would mean a wooden wall sprouted a hedge, and it is the
 * moisture that has to be doing the work. */
static void test_a_bare_trunk_in_wet_ground_buds_again(void)
{
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, SOIL_MOISTURE_MAX));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }

    /* Specifically FOLIAGE. It used to bud a plant, and a plant at the
     * foot of a trunk is a sucker - a grower, which climbed the outside
     * of the trunk as a wandering one-cell thread that never got thick
     * enough to harden and stop. Asserting on MAT_EXTENDED alone would
     * pass on either, which is what it did while the bug was there. */
    int budded = 0;
    for (int i = 0; i < 1500 && !budded; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !budded; y++) {
            for (int x = 0; x < W; x++) {
                if (sand_at(&s, x, y) == MATX(MATX_LEAF)) {
                    budded = 1;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(budded,
        "wood standing in watered soil must put out new growth - without "
        "it a tree is a thing that happens once, and anything that takes "
        "its foliage leaves a post that can never recover");

    /* And on dry ground, nothing. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }
    for (int i = 0; i < 1500; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_EXTENDED),
        "and on dry ground it must not - budding out of nothing is a "
        "wooden wall growing a hedge, and water is what pays for growth");

    /* A trunk in barely damp ground stays a trunk.
     *
     * Deliberately NOT claimed as a test that budding spends the water it
     * uses. It is not: budding puts its cell in an empty space beside the
     * trunk, and on a grid this size those run out long before the
     * moisture does, so a build that budded for free passes this
     * unchanged - checked, by mutation. The moisture cost is real and
     * bounds the total on a full board, and nothing here can see it.
     *
     * What this does pin is that one level of moisture is not a licence
     * to keep budding, which is the failure that would be visible. */
    fixture();
    sand_clear(&s);
    sand_set_soak(&s, SAND_SOAK_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, CELL_SOIL(MAT_DIRT, 1, 0));
    }
    sand_set(&s, W / 2, H - 2, CELL_SOIL(MAT_DIRT, 1, 1));   /* one level */
    for (int y = H - 5; y < H - 2; y++) {
        sand_set(&s, W / 2, y, CELL_MAKE(MAT_WOOD, 0));
    }
    for (int i = 0; i < 1500; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(2, count_cells_of(MAT_EXTENDED),
        "a trunk in barely damp ground must put out a bud or two, not a "
        "thicket - budding is the one part of growth with no lift to "
        "limit it, since it happens at the foot of the trunk");
}


/* Plant, leaf and ice are speckled, and everything else extended is not.
 *
 * An extended material's variant IS which one it is, so neither can carry
 * a shade and the position hash is the only variation available - the same
 * tool stone and wood use, and right here for the same reason it was wrong
 * for dirt: neither a wall of ice nor a grown tree moves.
 *
 * The negative half matters as much. The switch is on the low nibble, and
 * a mistake there does not fail to build - it paints some other extended
 * material in leaf green, which is the sort of thing nobody notices until
 * a fourteenth material arrives and comes out looking like a hedge. */
static void test_the_right_extended_materials_are_speckled(void)
{
    gfx_color_t col[3] = { 0, 0, 0 };

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const cell_t c = MATX(k);
        const bool grained = (k == MATX_PLANT || k == MATX_LEAF ||
                              k == MATX_LEAF_DRY || k == MATX_LEAF_DEAD ||
                              k == MATX_ICE);

        int distinct = 0;
        gfx_color_t seen[8];
        for (unsigned hash = 0; hash < 8u; hash++) {
            const material_pattern_t pat = material_colours(c, hash, false,
                                                            col);
            char why[96];
            snprintf(why, sizeof why, "extended material %d", k);
            TEST_ASSERT_EQUAL_MESSAGE(
                grained ? MATERIAL_SPECKLED : MATERIAL_FLAT, pat, why);

            bool known = false;
            for (int i = 0; i < distinct; i++) {
                known = known || (seen[i] == col[0]);
            }
            if (!known) {
                seen[distinct++] = col[0];
            }
        }

        if (grained) {
            TEST_ASSERT_GREATER_THAN_MESSAGE(4, distinct,
                "a speckled material must actually use its grain - a table "
                "of eight identical colours is a flat fill with extra steps");
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, distinct,
                "and one without a grain must not vary with position - it "
                "would be the palette entry of some other material leaking "
                "through the wrong branch");
        }
    }
}


/* The three airborne materials agree with themselves about weight, speed
 * and lifetime.
 *
 * Steam is the lightest and quickest and should be the first to go; a
 * heavy flammable gas should pool and wait. For a long time it was the
 * other way round on the last of those three - gas faded in about 120
 * steps, steam in 160, smoke in 240 - so the heaviest, slowest thing in
 * the air was also the first to disappear, and a pocket of gas could not
 * be built with.
 *
 * Asserted on the TABLE rather than by watching cells fade. Three
 * populations decaying past each other is a slow and noisy way to check a
 * fact that is written down in one place, and this is the same reasoning
 * as the acid test above: nonzero is not a claim about the right value,
 * only that somebody chose one and that the three still agree. */
static void test_the_air_agrees_about_weight_speed_and_lifetime(void)
{
    /* Lighter rises faster. */
    TEST_ASSERT_LESS_THAN_MESSAGE(materials[MAT_SMOKE].density,
        materials[MAT_STEAM].density, "steam must be lighter than smoke");
    TEST_ASSERT_LESS_THAN_MESSAGE(materials[MAT_GAS].density,
        materials[MAT_SMOKE].density, "smoke must be lighter than gas");

    TEST_ASSERT_GREATER_THAN_MESSAGE(materials[MAT_SMOKE].mobility,
        materials[MAT_STEAM].mobility, "steam must move faster than smoke");
    TEST_ASSERT_GREATER_THAN_MESSAGE(materials[MAT_GAS].mobility,
        materials[MAT_SMOKE].mobility, "smoke must move faster than gas");

    /* And the lighter it is, the sooner it is gone: decay is a chance to
     * tick DOWN, so a bigger figure is a shorter life. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(materials[MAT_SMOKE].decay,
        materials[MAT_STEAM].decay,
        "steam must fade sooner than smoke - it condenses, it does not "
        "linger");
    TEST_ASSERT_GREATER_THAN_MESSAGE(materials[MAT_GAS].decay,
        materials[MAT_SMOKE].decay,
        "and smoke sooner than gas - the heaviest, slowest thing in the "
        "air must be the last to go, or a pocket of it cannot be built "
        "with");
}

/* Every material has a colour, and every extended material has one too.
 *
 * The palette is one flat array of 256 entries indexed by the whole cell
 * byte, and C zero-fills whatever an initialiser does not reach. So a
 * material whose block is missing does not fail to build - it renders
 * BLACK, which looks like a styling choice rather than a bug.
 *
 * That has happened twice. Both times a block was added or removed
 * somewhere in the middle and every block after it shifted by sixteen: the
 * first time the extended range landed on the wrong id, the second time
 * folding ember out left ice reading from the zero-filled tail. Reported
 * as "ice look is pretty bad, just black", which is exactly what an unset
 * palette entry looks like.
 *
 * The blocks carry explicit `[MAT_X * MATERIAL_VARIANTS] =` designators
 * now, so removing a material cannot shift the ones after it. This checks
 * the result rather than the mechanism, because the failure is silent
 * either way. */
static void test_every_material_has_a_palette_block(void)
{
    const gfx_color_t *pal = material_palette();

    for (int m = 0; m < MAT_COUNT; m++) {
        int set = 0;
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            if (pal[m * MATERIAL_VARIANTS + v] != 0) {
                set++;
            }
        }
        /* 192, not 144: the device build's -Werror=format-truncation
         * proves 144 can clip the tail of this message for a long
         * material name, and a clipped diagnostic is exactly the kind
         * of silent failure this test exists to make loud. */
        char why[192];
        snprintf(why, sizeof why,
                 "%s (id %d) has %d of %d palette entries set - a block "
                 "that is missing or misaligned renders black, and black "
                 "is not an error anyone sees as one",
                 materials[m].name, m, set, MATERIAL_VARIANTS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS, set, why);
    }

    /* And the extended range, whose entries are one per material rather
     * than a block each - the same failure, one level down. */
    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        char why[128];
        snprintf(why, sizeof why,
                 "extended material %d (cell 0x%02X) has no colour", k,
                 (unsigned)MATX(k));
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, pal[MATX(k)], why);
    }
}

/* Ice is the colour it is meant to be, not merely some colour.
 *
 * The check above catches a block that is missing. This catches one that
 * is present but WRONG - reading a neighbouring material's entry, which is
 * what a shifted palette produces and what the count test cannot see. */
static void test_ice_is_its_own_colour(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t ice = pal[MATX(MATX_ICE)];

    TEST_ASSERT_EQUAL_MESSAGE(GFX_RGB(0xB6E4F2), ice,
        "ice must be the pale blue its own palette entry names - anything "
        "else means the entry is being read from somewhere other than "
        "where it was written");

    for (int m = 0; m < MAT_COUNT; m++) {
        for (int v = 0; v < MATERIAL_VARIANTS; v++) {
            if (pal[m * MATERIAL_VARIANTS + v] == ice) {
                char why[128];
                snprintf(why, sizeof why,
                         "ice shares a colour with %s variant %d",
                         materials[m].name, v);
                TEST_FAIL_MESSAGE(why);
            }
        }
    }
}

/* An extended material keeps its identity through a paint.
 *
 * Its low nibble IS which material it is, so anything that treats the
 * variant as a shade to randomise - which is what happens to every
 * ordinary static material - would silently repaint it as a different
 * extended material. That is the one way this scheme can go wrong
 * quietly. */
static void test_an_extended_material_survives_being_painted(void)
{
    fixture();
    sand_clear(&s);

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        sand_clear(&s);
        sand_spawn_cell(&s, W / 2, H / 2, 0, MATX(k));
        const cell_t got = sand_at(&s, W / 2, H / 2);

        char why[96];
        snprintf(why, sizeof why,
                 "extended material %d came back as %d", k, CELL_VARIANT(got));
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_EXTENDED, CELL_MATERIAL(got), why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k, CELL_VARIANT(got), why);
    }
}

/* They share one physics row, and that is the deal.
 *
 * material_of() is read per cell per step by the sweep, so it must not
 * decode anything. Every extended material therefore moves - or rather
 * does not move - identically. Asserting it keeps someone from quietly
 * adding a row that expects otherwise. */
static void test_every_extended_material_shares_one_physics_row(void)
{
    const material_t *first = material_of(MATX(0));

    for (int k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        char why[96];
        snprintf(why, sizeof why, "extended material %d", k);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(first, material_of(MATX(k)), why);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_STATIC, first->kind,
        "the shared row has to be an inert solid - anything that moves "
        "needs its own physics, which is exactly what the extended range "
        "cannot give it");
}

/* But they get their own reactions, which is the point of the range. */
static void test_extended_materials_get_their_own_reactions(void)
{
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, reaction_of(MATX(MATX_ICE))->chills,
        "ice must chill - an extended material with no reactions of its "
        "own would just be a coloured block");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reaction_of(MATX(1))->chills,
        "and an extended material that has not been defined must not "
        "inherit the reactions of one that has - they are separate rows, "
        "not one shared row like the physics");
}

/* Ice does what it exists for: it cracks hot glass, and it stays put.
 *
 * Snow already chills, but snow is a powder - it drifts as it falls,
 * floats on water, and melts in any liquid, so aiming it at one face of a
 * hot vessel is most of the difficulty of using it. Ice is the same cold
 * in a form that can be BUILT with. */
static void test_ice_cracks_hot_glass_and_stays_where_it_is_put(void)
{
    fixture();
    sand_clear(&s);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_GLASS, MATERIAL_VARIANTS - 1));
        sand_set(&s, x, H - 3, MATX(MATX_ICE));
    }
    const int ice_x = 1, ice_y = H - 3;

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "ice against glass at the top of its ramp must crack it, the same "
        "as snow does - that is what makes it worth having as a solid");

    /* It sat on glass that has now become sand, so it may have settled a
     * row; what matters is that it did not drift sideways the way a
     * powder does. */
    bool still_there = false;
    for (int y = ice_y; y < H; y++) {
        if (CELL_MATERIAL(sand_at(&s, ice_x, y)) == MAT_EXTENDED) {
            still_there = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(still_there,
        "and it must still be in the column it was placed in - a solid "
        "block is the whole difference from snow");
}

/* Snow floats, because it is lighter than what it lands on.
 *
 * Not decoration: floating is what puts snow ON TOP of a pool rather than
 * under it, which is where it has to be to reach anything. A snowfall that
 * sank would be a snowfall the player could not aim. */
static void test_snow_floats_on_water(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = H - 4; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int x = 2; x < W - 2; x++) {
        sand_set(&s, x, 0, SNOW);
    }

    /* Twenty steps: long enough for the drift to fall and settle, short
     * enough that it has not melted yet. Snow in water is on a clock now
     * (see test_snow_melts_in_any_liquid), so a longer run would measure
     * an empty board and pass for the wrong reason - which is exactly
     * what it did when thawing was added under it. */
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int lowest_snow = -1, highest_water = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_SNOW && y > lowest_snow) {
                lowest_snow = y;
            }
            if (m == MAT_WATER && y < highest_water) {
                highest_water = y;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(lowest_snow >= 0,
        "fixture check: some snow has to survive the fall to say anything "
        "about where it ended up");
    TEST_ASSERT_TRUE_MESSAGE(lowest_snow <= highest_water,
        "snow must come to rest on top of the water, not under it - it is "
        "lighter than water and can_enter() is what makes that true");
}

/* Glass conducts heat, the same as stone.
 *
 * It did not, and the omission was invisible: glass had no reactions[] row
 * at all, so `conducts` defaulted to 0 and heat stopped dead at it. A
 * stone vessel over a flame boiled its contents and a glass one did
 * not - backwards, given glass is the vessel you have to MAKE and the only
 * one acid cannot eat.
 *
 * An absent row reads as "this material has no reactions", which is
 * correct for most materials and was wrong for this one. Nothing warns
 * about it, which is what this test is for.
 *
 * Asserted against stone rather than as an absolute figure: the two are
 * meant to be interchangeable thermally, so that choosing between them is
 * a decision about acid and nothing else. */
static void test_glass_conducts_heat_like_stone(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(reactions[MAT_STONE].conducts,
                                  reactions[MAT_GLASS].conducts,
        "glass must conduct heat as well as stone - a glass vessel over a "
        "flame has to boil what is in it, and glass differing from stone "
        "on any axis but acid makes the choice between them a guess");

    /* And in the simulation, not only in the table: a sealed vessel with
     * water in it and fire underneath. */
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 3, GLASS);          /* the vessel's base */
    }
    for (int x = 2; x < W - 2; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    bool boiled = false;
    for (int i = 0; i < 400 && !boiled; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 2))) {
                sand_set(&s, x, H - 2, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        boiled = count_cells_of(MAT_STEAM) > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "fire under a glass base must boil the water above it, exactly as "
        "it does under a stone one");
}

/* Sand plus sustained heat makes glass: reaction_t.heats_to, which is a
 * phase change rather than combustion and so is kept apart from
 * flammability/ignites_to. Sand does not catch fire, and calling the field
 * that would send the next reader looking for a flame. */
static void test_sand_turns_to_glass_under_sustained_heat(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, CELL_MAKE(MAT_SAND, 8));
    }

    /* A flame held against it, re-laid each step: fire is KIND_GAS and
     * rises away during the same step it is placed, so a single spark
     * never gets a turn to react - the same reason the wood-ignition
     * fixtures hold their spark down. */
    bool made = false;
    for (int i = 0; i < 600 && !made; i++) {
        for (int x = 1; x < W - 1; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, H - 3))) {
                sand_set(&s, x, H - 3, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        made = count_cells_of(MAT_GLASS) > W;   /* more than the floor */
    }

    TEST_ASSERT_TRUE_MESSAGE(made,
        "sand held against a flame must eventually become glass - slowly, "
        "slowly, so it is something you set up and wait for "
        "rather than something a stray spark does to a dune");
}

/* Dissolving is a TRANSFER, like quenching a fire or soaking a grain: the
 * acid is consumed by the work it does. Asserted as an exact ratio, since
 * it is exactly one unit of acid mass per cell removed. */
static void test_acid_spends_a_unit_of_itself_per_cell_dissolved(void)
{
    const long acid_before = acid_tank(2, 2);
    const int sand_before = count_cells_of(MAT_SAND);

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int eaten = sand_before - count_cells_of(MAT_SAND);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, eaten, "setup: something eaten");
    TEST_ASSERT_EQUAL_INT_MESSAGE(eaten, acid_before - mass_held_by(MAT_ACID),
        "every cell dissolved must cost the acid exactly one unit of its "
        "own mass - without that a single drop eats an unbounded amount "
        "and remains a single drop");
}

/* The consequence of that, and the reason it is worth paying for: a
 * finite amount of acid can only eat a finite amount. A drop lands on a
 * deep pile and stops partway rather than boring through the floor. */
/* Acid fizzes: a dissolve sometimes leaves smoke where the cell was.
 *
 * Without it acid worked in complete silence - cells simply stopped
 * existing, with nothing on screen to say what had happened or that the
 * acid was doing anything at all. reaction_t.fizz puts a wisp in the cell
 * that was just eaten, which is about to be empty anyway.
 *
 * MAT_SMOKE and not MAT_STEAM: steam here means water that got hot, and
 * acid fumes are not that - the same distinction that made smoke its own
 * material in the first place. */
static void test_acid_fizzes_while_it_eats(void)
{
    acid_tank(2, 2);

    bool fizzed = false;
    for (int i = 0; i < 400 && !fizzed; i++) {
        sand_step(&s, 0, 1000, 0);
        fizzed = count_cells_of(MAT_SMOKE) > 0;
    }

    TEST_ASSERT_TRUE_MESSAGE(fizzed,
        "acid eating a pile of sand must leave some smoke behind - it is "
        "the only sign on screen that the acid is working");
}

/* And the fizz has to be able to get OUT of the acid, which it does for
 * free: smoke is lighter than every liquid, so try_bubble() (sand_gas.c)
 * swaps it up through the pool. Worth asserting, because a byproduct that
 * cannot leave the liquid that made it would just accumulate at the
 * bottom, invisible under the acid. */
static void test_the_fizz_rises_out_of_the_acid(void)
{
    const int surface = 1;      /* acid_tank() fills from row 1 down */
    acid_tank(2, 2);

    int highest = H;
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SMOKE && y < highest) {
                    highest = y;
                }
            }
        }
    }

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(surface, highest,
        "smoke made at the bottom of an acid pool must reach the top of "
        "it - a gas is lighter than any liquid, and try_bubble() is what "
        "lets it climb out instead of being trapped underneath");
}

static void test_a_little_acid_cannot_eat_an_unlimited_amount(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    for (int y = 2; y < H - 1; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    const int sand_before = count_cells_of(MAT_SAND);
    /* One cell of acid: MASS_MAX units, so at one unit a cell it can
     * account for at most MASS_MAX cells however long it is left. */
    sand_set(&s, W / 2, 1, CELL_MAKE(MAT_ACID, MASS_MAX));

    for (int i = 0; i < 2000; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int eaten = sand_before - count_cells_of(MAT_SAND);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(MASS_MAX, eaten,
        "a single cell of acid holds MASS_MAX units and spends one per "
        "cell, so it can never dissolve more than that many - if it can, "
        "the bite has stopped costing anything");
}

static void test_every_liquid_declares_a_mobility(void)
{
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (materials[m].kind != KIND_LIQUID) {
            continue;
        }
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s is a liquid and must set its own `mobility` - leaving "
                 "it unset does not read as an error, it reads as a very "
                 "viscous liquid, which is how it lasts",
                 materials[m].name);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, materials[m].mobility, msg);
    }
}

/* Interfacial drag: a liquid pushing into another gets less willing the
 * further in it already is.
 *
 * Two liquids exchange by swapping whole cells gravity-ward, and under tilt
 * that direction is DITHERED between two octants step by step. Ungated,
 * every water cell with oil below it swaps every step, so water drills into
 * the oil along alternating diagonals and the boundary becomes a mixed
 * band - which on screen reads as straight lines running through what
 * should be a smooth surface.
 *
 * What the drag actually buys is not a smaller number, it is a BOUNDED one.
 * Measured across grid widths 24 to 40, eight seeds each, counting water
 * cells left sitting inside the oil after a tilt:
 *
 *     width      24    26    28    30    32    34    36    40
 *     ungated  17.4  18.9  20.5  21.4  14.3  16.9  22.4  29.8
 *     drag      8.1  12.5  12.1   8.9  12.5  12.1  12.0  12.3
 *
 * Ungated it grows with the length of the interface; with drag it sits flat
 * near twelve however wide the board gets. That is the property worth
 * having and the one worth testing, so this uses a deliberately WIDE grid -
 * on the 32-wide shared fixture the two are 14.3 against 12.5, close enough
 * that the test passed either way and proved nothing. It did, for one
 * round, before the sweep above was run.
 *
 * Averaged over seeds for the same reason: a single run of a chaotic scene
 * is not evidence. */
#define DRAG_W 40
#define DRAG_H 20

static int water_inside_oil(sand_t *g)
{
    static const int d[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    int inside = 0;
    for (int y = 1; y < DRAG_H - 1; y++) {
        for (int x = 1; x < DRAG_W - 1; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != MAT_WATER) {
                continue;
            }
            int oil = 0;
            for (int k = 0; k < 4; k++) {
                if (CELL_MATERIAL(sand_at(g, x + d[k][0], y + d[k][1]))
                        == MAT_OIL) {
                    oil++;
                }
            }
            if (oil >= 3) {
                inside++;
            }
        }
    }
    return inside;
}

static void test_water_does_not_drill_into_oil_when_tilted(void)
{
    static uint8_t drag_cells[DRAG_W * DRAG_H];
    sand_t g;
    const int seeds = 4;
    int total = 0;

    for (int k = 0; k < seeds; k++) {
        memset(drag_cells, 0, sizeof drag_cells);
        sand_init(&g, drag_cells, DRAG_W, DRAG_H, (uint32_t)(11 + k));
        sand_set_mobility(&g, SAND_MOBILITY_PER_MATERIAL);

        for (int x = 0; x < DRAG_W; x++) {
            sand_set(&g, x, 0, STONE);
            sand_set(&g, x, DRAG_H - 1, STONE);
        }
        for (int y = 0; y < DRAG_H; y++) {
            sand_set(&g, 0, y, STONE);
            sand_set(&g, DRAG_W - 1, y, STONE);
        }
        /* A thin slick of oil on the floor with water resting on it - the
         * unstable order, since water is the denser of the two. */
        for (int x = 1; x < DRAG_W - 1; x++) {
            sand_set(&g, x, DRAG_H - 2, CELL_MAKE(MAT_OIL, MASS_MAX));
            sand_set(&g, x, DRAG_H - 3, CELL_MAKE(MAT_OIL, MASS_MAX));
        }
        for (int y = DRAG_H - 7; y <= DRAG_H - 4; y++) {
            for (int x = 1; x < DRAG_W - 1; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
            }
        }

        for (int i = 0; i < 60; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        for (int i = 0; i < 300; i++) {
            sand_step(&g, 700, 700, 0);
        }
        total += water_inside_oil(&g);
    }

    /* 20 sits between the two measured means at this width (29.8 ungated,
     * 12.3 with drag) with room either side for ordinary variation. */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(20 * seeds, total,
        "water must not end up riddled through the oil body after a tilt - "
        "without interfacial drag the gravity-ward swap fires on every "
        "interface cell every step, drilling into the oil along the "
        "dithered diagonals, and the intrusion grows with the width of the "
        "board instead of staying bounded");
}

static void test_oil_flows_more_slowly_than_water(void)
{
    int steps[2];
    const material_id_t liquids[2] = { MAT_WATER, MAT_OIL };

    /* On `wide` (32 cells across), not the 8-wide default fixture. Over
     * six cells of travel both liquids arrive in the same four steps and
     * the difference is pure quantisation; the ratio only means anything
     * across a real distance. */
    for (int k = 0; k < 2; k++) {
        sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 5u);
        sand_set_mobility(&wide, SAND_MOBILITY_PER_MATERIAL);
        for (int x = 0; x < WIDE_W; x++) {
            sand_set(&wide, x, WIDE_H - 1, STONE);
        }
        for (int y = 1; y <= WIDE_H - 2; y++) {
            for (int x = 1; x <= 4; x++) {
                sand_set(&wide, x, y, CELL_MAKE(liquids[k], MASS_MAX));
            }
        }

        steps[k] = -1;
        for (int i = 1; i <= 3000 && steps[k] < 0; i++) {
            sand_step(&wide, 0, 1000, 0);
            if (!CELL_IS_EMPTY(sand_at(&wide, WIDE_W - 2, WIDE_H - 2))) {
                steps[k] = i;
            }
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, steps[k],
            "setup: both liquids must eventually reach the far wall");
    }

    /* Half again, not double: oil is 140 against water's 255 and lands
     * around twice the time, but this is a "they are distinguishable"
     * assertion rather than a pin on the current figures, and oil is
     * deliberately tunable towards water. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE((steps[0] * 3) / 2, steps[1],
        "oil must take clearly longer than water to spread the same "
        "distance - before `mobility` had a liquid reader the two were "
        "indistinguishable");
}

static void test_oil_trapped_under_water_floats_to_the_surface(void)
{
    fixture();
    sand_set_decay(&s, 0);
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, 7, STONE);
    }
    for (int y = 2; y <= 6; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
    }
    for (int y = 2; y <= 5; y++) {
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, WATER);
        }
    }
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, 6, OIL);      /* underneath the whole column */
    }

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Asserted as an ORDERING rather than "oil is at row 2", because
     * these cells are half full (see the shorthand macros at the top of
     * this file) so the column does not reach the brim and the surface
     * is not where counting rows would suggest. What matters is that
     * every oil cell ends up above every water cell. */
    /* Asserted as "which liquid is on top", not as a strict row
     * ordering of every cell, and not as "oil is at row 2".
     *
     * Two things make the tempting stronger assertions wrong. These
     * cells are half full (see the shorthand macros at the top of this
     * file), so the column never reaches the brim and the surface is not
     * where counting rows would put it. And the two liquids cannot mix
     * within a cell, so the interface between them is ragged - one row
     * genuinely holds some oil and some water at the same time, which
     * makes "every oil cell is above every water cell" false even when
     * the separation is perfect. */
    int top = -1, bottom = -1;
    for (int y = 0; y < H && top < 0; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID) {
                top = CELL_MATERIAL(c);
                break;
            }
        }
    }
    for (int y = H - 1; y >= 0 && bottom < 0; y--) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_LIQUID) {
                bottom = CELL_MATERIAL(c);
                break;
            }
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_OIL, top,
        "the topmost liquid must be OIL - it started underneath the "
        "whole column and has to have risen through it, which only "
        "happens because the denser water sinks into it, a "
        "gravity-ward move riding the main sweep's own no-double-move "
        "guarantee");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, bottom,
        "and the bottom-most liquid must be WATER, for the same reason "
        "from the other end");
}

/* Lava is the first material that is a liquid AND a heat source, so it
 * is the first place the variant nibble's two meanings could collide.
 * decay != 0 would make tick_decay() read a lava cell's MASS as a
 * lifespan and eat it. */
static void test_lava_does_not_decay_away(void)
{
    fixture();
    /* Deliberately NO sand_set_decay() here, and that is the point of
     * the test rather than an omission.
     *
     * The obvious version of this forces sand_set_decay(&s, 255) to make
     * any decay show up immediately - but that override replaces the
     * per-material figure for EVERY material at once (see tick_decay()),
     * lava included, so it forces lava to decay no matter what its own
     * row says and destroys the cell every time. It tests the override,
     * not the table.
     *
     * Running on the per-material defaults instead is what actually pins
     * the thing worth pinning: lava's own decay must be 0, so that
     * tick_decay() never reads its variant nibble - which for a liquid
     * is FILL LEVEL, not life - and never eats the cell's mass. Set
     * lava's decay to anything nonzero and this fails.
     *
     * A one-cell-wide well. Lava is a liquid, so a lone cell on an open
     * floor does not stay put - equalise_liquids() spreads it sideways
     * and thins it to nothing worth measuring. Penning it in is what
     * makes "is it still here, and still full" a question about DECAY
     * rather than about flow. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    sand_set(&s, 2, H - 2, STONE);
    sand_set(&s, 4, H - 2, STONE);
    sand_set(&s, 3, H - 2, LAVA);

    for (int i = 0; i < 4 * MATERIAL_VARIANTS; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_LAVA, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "lava must be immortal - its variant nibble is a FILL LEVEL, "
        "not life remaining, so any decay at all would consume the "
        "cell's own mass");
    TEST_ASSERT_EQUAL_INT_MESSAGE(CELL_VARIANT(LAVA),
        CELL_VARIANT(sand_at(&s, 3, H - 2)),
        "and at exactly the mass it was placed with, not merely present "
        "- a decay tick reads the variant nibble as life and would show "
        "up here first");
}

/* The reaction that makes lava worth having, and a straight reuse of
 * quench_to: what water does to a burning cell is put it out, and what
 * putting lava out means is rock. */
static void test_water_freezes_lava_into_stone(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, LAVA);
    sand_set(&s, 4, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "lava quenched by water must become stone, not vanish - "
        "reaction_t.quench_to, the same field that turns a quenched "
        "fire into steam");
}

/* Guards may_have_heat_holder's arm-only design (sand.h/sand_priv.h): the
 * flag is armed the moment a heat_ramp cell exists on the grid and is
 * deliberately never cleared, because a heat_ramp cell can be CREATED
 * mid-pass, behind step_one_reacting_row()'s own scan pointer, rather
 * than painted onto the board before the step runs. Lava quenching into
 * stone is exactly that: place_reacted() writes the new MAT_STONE cell
 * from inside the very row walk that is checking may_have_heat_holder's
 * sibling flags, so the row that already passed this cell never reports
 * it, and an end-of-pass clear - "tidy it up like the other five flags" -
 * would erase what latch_content_flags() had just armed one line
 * earlier. That is the regression this test exists to catch: anyone who
 * adds `if (!(found & FOUND_HEAT_HOLDER)) s->may_have_heat_holder =
 * false;` next to the other five in sand_step_reactions() will fail here,
 * because the very first quenched cell has no way left to ever re-arm it.
 *
 * Deliberately no stone or glass is painted anywhere in this scene - not
 * even as a floor or walls - specifically so may_have_heat_holder starts
 * false and the only heat_ramp cell that ever exists is the one born from
 * the quench itself. Lava and water sit on the bottom row instead of
 * needing a floor: dest_row() returns NULL past the last row, which is
 * enough to stop either of them falling out from under themselves without
 * painting a single cell that would pre-arm the flag.
 *
 * Also worth noting because it is the whole reason this flag has to be
 * independent of may_have_temperature: place_reacted() gives a freshly
 * created heat-ramp cell SAND_AMBIENT_HEAT, not a hot variant, so the new
 * stone does NOT arm may_have_temperature (see latch_content_flags()'s
 * ambient-exception). It must still arm may_have_heat_holder, because
 * that is exactly what a later convecting gas cell needs to find it. */
static void test_lava_quenched_into_stone_mid_pass_arms_the_heat_holder_flag(void)
{
    fixture();
    sand_clear(&s);

    TEST_ASSERT_FALSE_MESSAGE(s.may_have_heat_holder,
        "setup: a freshly cleared grid holds nothing with a heat_ramp, so "
        "the flag must start false");

    /* Lava IS the heat rather than holding one (no heat_ramp of its own);
     * water quenches it into stone, which does. Resting on the bottom row
     * so neither needs a floor cell to stay put. */
    sand_set(&s, 3, H - 1, LAVA);
    sand_set(&s, 4, H - 1, WATER);

    bool saw_heat_holder = false;
    for (int i = 0; i < 5; i++) {
        sand_step(&s, 0, 1000, 0);

        bool any_heat_holder = false;
        for (int y = 0; y < H && !any_heat_holder; y++) {
            for (int x = 0; x < W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (!CELL_IS_EMPTY(c) && reaction_of(c)->heat_ramp != 0) {
                    any_heat_holder = true;
                    break;
                }
            }
        }
        if (any_heat_holder) {
            saw_heat_holder = true;
        }

        TEST_ASSERT_TRUE_MESSAGE(!any_heat_holder || s.may_have_heat_holder,
            "a cell with a non-zero heat_ramp exists on the grid, so "
            "may_have_heat_holder must be armed - even though this cell "
            "(lava quenched to stone) was created mid-pass, behind the "
            "scan pointer, rather than painted onto the board before the "
            "step ran");
    }

    /* If this never went true, the loop above proved nothing - it never
     * saw a heat holder and passed for the wrong reason. */
    TEST_ASSERT_TRUE_MESSAGE(saw_heat_holder,
        "setup: lava next to water must have quenched into stone within "
        "5 steps, or this test never exercised the mid-pass creation case "
        "it exists to guard");
}

/* Lava burns, so it must not quench - the same rule oil needs, arrived
 * at from the other side (oil is fuel, lava is a heat source, and
 * neighbor_quenches() excludes both). */
static void test_lava_does_not_put_fire_out(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, LAVA);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_EMPTY, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a fire touching LAVA must not be extinguished by it - lava is "
        "a heat source, and a liquid only quenches if it is neither "
        "fuel nor burning itself");
}

static void test_steam_bubbles_up_through_standing_water(void)
{
    water_column();
    sand_set(&s, 3, 6, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    TEST_ASSERT_EQUAL_INT_MESSAGE(6, first_row_holding(MAT_STEAM),
        "setup: the steam must start at the BOTTOM of the water column, "
        "with the full depth of it to climb through");

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    const int reached = first_row_holding(MAT_STEAM);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, reached,
        "the steam must still exist - decay is off for this test, so "
        "losing it means it was destroyed rather than moved");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(2, reached,
        "steam under standing water must rise all the way OUT of the "
        "column, not sit where it was made - a gas lighter than the "
        "liquid above it displaces that liquid downward one cell at a "
        "time");
}

/* A bubble swaps two whole cells, so the liquid it shoves aside has to
 * arrive intact - same material, same amount. Nothing here splits mass,
 * which is what makes that guarantee exact rather than approximate, and
 * this pins it: a bubble that quietly rounded a partial cell away would
 * drain a boiler every time one rose. */
static void test_bubbling_conserves_the_water_it_displaces(void)
{
    water_column();
    /* One deliberately PARTIAL cell in the bubble's path, so this would
     * catch a swap that rebuilt the liquid at full mass instead of
     * carrying its own variant nibble across. */
    sand_set(&s, 3, 4, CELL_MAKE(MAT_WATER, 7));
    sand_set(&s, 3, 6, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    const long before = mass_held_by(MAT_WATER);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, mass_held_by(MAT_WATER),
            "a bubble must never create or destroy water mass on any "
            "step - it is a swap of two whole cells, and the liquid "
            "keeps its own variant nibble as it moves");
    }
}

/* Bubbling is keyed on KIND_GAS and a density comparison, not on steam
 * specifically, so plain gas gets it too - asserted directly so nobody
 * "fixes" try_bubble() into a steam special case later. */
static void test_plain_gas_bubbles_up_through_water_too(void)
{
    water_column();
    sand_set(&s, 3, 6, GAS);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(2, first_row_holding(MAT_GAS),
        "gas is lighter than water too, so it must bubble out of a "
        "column of it exactly the way steam does - try_bubble() tests "
        "kind and density, not material identity");
}

/* The other half of the rule: only LIQUIDS get shoved aside. A gas capped
 * by something solid stays put, or "bubbling" would quietly become a
 * licence to walk through walls. */
static void test_a_bubble_does_not_push_through_a_solid(void)
{
    fixture();
    sand_set_decay(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 3, STONE);
        sand_set(&s, x, 5, STONE);
    }
    sand_set(&s, 3, 4, CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1));

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "the stone ceiling must still be stone - a bubble displaces "
        "liquid only, never a solid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 4)),
        "and the steam must still be under it, not through it");
}

static void test_quenching_makes_steam_but_burning_out_makes_smoke(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, WATER);
    sand_set(&s, 4, 3, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "a fire put out by water must leave STEAM - water that got hot, "
        "which is exactly what happened");

    /* Same fire, no water anywhere, forced to burn out and forced to
     * smoke: the residue must be the OTHER material. sand_set_decay()
     * at 255 burns it out on the first step it gets; smoke's own 40 in
     * 256 chance is not forced, so this loops until it fires rather
     * than asserting on a single roll. */
    fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, FIRE);
    }

    bool found_smoke = false, found_steam = false;
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1); i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const uint8_t m = CELL_MATERIAL(sand_at(&s, x, y));
                if (m == MAT_SMOKE) found_smoke = true;
                if (m == MAT_STEAM) found_steam = true;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_smoke,
        "fire burning out with no water in the scene must leave SMOKE");
    TEST_ASSERT_FALSE_MESSAGE(found_steam,
        "and must never leave STEAM - there is no water anywhere on this "
        "grid, so a steam cell here would mean the two byproducts have "
        "been collapsed back into one material");
}

static void test_stone_conducts_heat_into_water_beyond_it(void)
{
    fixture();
    sand_set_mobility(&s, 0);   /* keep fire from rising away before it
                                 * gets a turn to conduct - same
                                 * technique used throughout this
                                 * section */
    sand_set_conduction(&s, 255);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, STONE);
    /* Floor plus both down-diagonals under the water, not the floor
     * alone - see test_creating_steam_arms_the_gas_pass's own comment
     * for why a partial-mass WATER cell needs all three blocked. */
    sand_set(&s, 4, 4, STONE);
    sand_set(&s, 5, 4, STONE);
    sand_set(&s, 6, 4, STONE);
    sand_set(&s, 5, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 5, 3)),
        "a fire beside a single cell of stone must boil water sitting "
        "on the OTHER side of that stone - the whole boiler mechanism - "
        "without fire ever crossing the stone itself");
}

static void test_stone_does_not_conduct_fire_into_empty_space(void)
{
    fixture();
    sand_set_mobility(&s, 0);
    sand_set_conduction(&s, 255);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, STONE);
    /* (5, 3) deliberately left empty. */

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 5, 3)),
        "conduction must never create fire in empty space on the far "
        "side of a conductor, even with the roll forced to succeed "
        "every time it is tried - a sealed stone container must stay "
        "sealed");
}

/* Wide enough for an eleven-cell-thick stone wall (matching
 * app_sand.c's own pour brush, POUR_RADIUS 5 with no size control - see
 * conduct_heat()'s own top-of-file comment for why that thickness
 * specifically) plus fire, water and margin, all in one row of `wide`
 * (WIDE_W/WIDE_H, declared above). Builds fire at column 1, a stone
 * wall `wall_len` cells thick starting at column 2, and one water cell
 * just past it - boxes the water (floor plus both down-diagonals, see
 * test_creating_steam_arms_the_gas_pass) so it cannot drain away before
 * conduction gets a look at it, and pins fire with mobility rather than
 * physically boxing it in, since a physical box dense enough to also
 * stop the diagonal slides would make every side of fire denser than
 * fire itself and smother it outright (confirmed: this is what the
 * first version of this helper, walled on every side, actually did).
 * Returns the column the water cell sits at. */
static int build_boiler_room(int wall_len)
{
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_mobility(&wide, 0);

    const int y = 2;
    const int fire_x  = 1;
    const int wall_x0 = fire_x + 1;
    const int water_x = wall_x0 + wall_len;

    sand_set(&wide, water_x - 1, y + 1, STONE);
    sand_set(&wide, water_x,     y + 1, STONE);
    sand_set(&wide, water_x + 1, y + 1, STONE);

    sand_set(&wide, fire_x, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&wide, wall_x0 + i, y, STONE);
    }
    sand_set(&wide, water_x, y, WATER);

    return water_x;
}

static void test_a_thick_wall_still_conducts(void)
{
    /* AFTER build_boiler_room(), not before - it calls sand_init()
     * internally, which would otherwise wipe this override right back
     * to its default. */
    const int water_x = build_boiler_room(11);
    sand_set_conduction(&wide, 255);

    bool boiled = false;
    for (int i = 0; i < 10 && !boiled; i++) {
        sand_step(&wide, 0, 1000, 0);
        boiled = CELL_MATERIAL(sand_at(&wide, water_x, 2)) == MAT_STEAM;
    }

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "an eleven-cell-thick stone wall - what app_sand.c's own pour "
        "brush actually draws, not a one-cell idealisation - must still "
        "conduct and eventually boil the water beyond it. A reach of "
        "exactly one cell (an earlier version of this feature) would "
        "make this fail forever: that is the whole reason the walk "
        "attenuates with depth instead of stopping cold at one cell");
}

static void test_conduction_stops_at_the_reach_cap(void)
{
    /* Its own grid, not the shared `wide` one: this test needs a
     * conductor run longer than CONDUCT_REACH, and the cap is now 32,
     * which does not fit across WIDE_W (32). Widening the shared grid
     * instead would have changed the cell count every other test using
     * it draws random numbers over, so this one test gets its own.
     *
     * The wall length here tracks CONDUCT_REACH and has to stay ahead
     * of it: this test asserts the cap EXISTS, not that it sits at any
     * particular depth, so raising the cap means raising this too.
     *
     * Forcing conduction to 255 (AFTER sand_init(), which would
     * otherwise wipe the override) makes every ROLL along the walk
     * succeed, so the only thing left that can stop it is the reach cap
     * itself, which is exactly what this pins down. */
    static uint8_t cap_cells[CAP_W * CAP_H];
    sand_t cap;
    sand_init(&cap, cap_cells, CAP_W, CAP_H, 3u);
    sand_set_mobility(&cap, 0);
    sand_set_conduction(&cap, 255);

    const int y = 2;
    const int wall_x0 = 2;
    const int wall_len = CONDUCT_REACH_TEST + 8;
    const int water_x = wall_x0 + wall_len;

    sand_set(&cap, water_x - 1, y + 1, STONE);
    sand_set(&cap, water_x,     y + 1, STONE);
    sand_set(&cap, water_x + 1, y + 1, STONE);
    sand_set(&cap, 1, y, FIRE);
    for (int i = 0; i < wall_len; i++) {
        sand_set(&cap, wall_x0 + i, y, STONE);
    }
    sand_set(&cap, water_x, y, WATER);

    for (int i = 0; i < 50; i++) {
        sand_step(&cap, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&cap, water_x, 2)),
        "a conductor run longer than CONDUCT_REACH must never conduct "
        "at all, even with every per-cell roll forced to succeed - the "
        "walk has to actually stop at the cap, not merely be unlikely "
        "to reach that far");
}

/* Steps until MAT_STEAM appears past a stone wall `wall_len` cells
 * thick, using the REAL per-material conduction figure (material.c's
 * stone row) rather than an override - or `budget` if it never appears
 * within that many steps. */
static int steps_to_boil(int wall_len, int budget)
{
    const int water_x = build_boiler_room(wall_len);
    for (int i = 0; i < budget; i++) {
        sand_step(&wide, 0, 1000, 0);
        if (CELL_MATERIAL(sand_at(&wide, water_x, 2)) == MAT_STEAM) {
            return i + 1;
        }
    }
    return budget;
}

static void test_a_thick_wall_conducts_more_slowly_than_a_thin_one(void)
{
    /* At the real figure (176 in 256, ~0.69/step), a thin wall's
     * cumulative miss probability is negligible within a handful of
     * steps (0.31^10 =~ 9e-6); a thick eleven-cell one needs roughly 41
     * steps on average (0.69^11 =~ 0.024/step) and has real spread
     * around that - so this compares actual step counts rather than
     * asserting a fixed pass/fail line either wall would sometimes
     * cross the wrong way on an unlucky seed. */
    const int budget = 200;
    const int thin  = steps_to_boil(1, budget);
    const int thick = steps_to_boil(11, budget);

    TEST_ASSERT_LESS_THAN_MESSAGE(thick, thin,
        "at the real per-material conduction figure, a thin (1-cell) "
        "stone wall must boil the water beyond it sooner than an "
        "eleven-cell one - thermal resistance falling out of the "
        "attenuating walk itself, not a second constant");
}

/* Boiling happens where the HEAT is, not where the steam wants to end up.
 *
 * This test asserted the exact opposite until bubbling existed, and the
 * reversal is worth keeping visible rather than quietly rewriting.
 * conduct_heat() reaches the bottom cell of the column - the one touching
 * the hot stone - and used to hand it to a boil_surface() walk that
 * climbed against gravity to convert the TOP cell instead. That walk was
 * not decoration: steam made at the bottom of a pool was permanently
 * stuck there (can_enter() only lets a denser mover displace a lighter
 * target, and room_in() will not let water fall into a steam cell
 * either), so boiling anywhere but the surface produced nothing anyone
 * could see.
 *
 * try_bubble() (sand_gas.c) removed that constraint, and with it the
 * only reason to boil anywhere other than the heat source. Boiling the
 * bottom cell now reads the way a real pot does - a column of bubbles
 * climbing from the hot base - instead of steam appearing at the surface
 * from nowhere.
 *
 * sand_set_mobility(&wide, 0) keeps the newly made steam still for the
 * duration, so this measures WHERE the boil happened rather than where
 * the bubble had got to by the time it was inspected. */
static void test_boiling_converts_the_cell_nearest_the_heat(void)
{
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);
    sand_set_conduction(&wide, 255);
    sand_set_mobility(&wide, 0);

    const int x = 5;
    const int fire_y = 6, stone_y = 5, water_top = 1, water_bottom = 4;

    sand_set(&wide, x, fire_y, FIRE);
    sand_set(&wide, x, stone_y, STONE);
    for (int y = water_top; y <= water_bottom; y++) {
        sand_set(&wide, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    /* Side walls the height of the column, so cross-flow has nowhere to
     * send any mass and the column stays exactly this shape while
     * conduction does its work. */
    for (int y = water_top; y <= fire_y; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    bool boiled = false;
    for (int i = 0; i < 10 && !boiled; i++) {
        sand_step(&wide, 0, 1000, 0);
        boiled = CELL_MATERIAL(sand_at(&wide, x, water_bottom)) == MAT_STEAM;
    }

    TEST_ASSERT_TRUE_MESSAGE(boiled,
        "the cell nearest the stone - the one conduct_heat() actually "
        "reaches - must be the one that boils");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&wide, x, water_top)),
        "and the surface must still be plain water at that moment - "
        "boiling happens at the heat source now, and the steam makes "
        "its own way up by bubbling rather than being conjured at the "
        "top of the column. A surface cell boiling first would mean the "
        "old against-gravity walk had come back");
}

static void test_the_boiler_end_to_end(void)
{
    sand_init(&wide, wide_cells, WIDE_W, WIDE_H, 3u);

    const int x = 10;
    const int wood_y    = 19;
    const int slab_top  = 8, slab_bottom = 18;   /* 11 rows thick,
                                                  * matching the pour
                                                  * brush's real
                                                  * thickness */
    const int water_top = 5, water_bottom = 7;   /* 3 cells of water */

    sand_set(&wide, x, wood_y, WOOD);
    for (int y = slab_top; y <= slab_bottom; y++) {
        sand_set(&wide, x, y, STONE);
    }
    for (int y = water_top; y <= water_bottom; y++) {
        sand_set(&wide, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    /* Side walls the height of the water and the slab (not wood's own
     * row - the ignition spark below needs an open cell beside the
     * wood, and wood is KIND_STATIC so it needs no walls to stay put
     * regardless), so nothing drains or drifts sideways out of the
     * column while the boiler does its work. */
    for (int y = water_top; y <= slab_bottom; y++) {
        sand_set(&wide, x - 1, y, STONE);
        sand_set(&wide, x + 1, y, STONE);
    }

    const long water_before = mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER);

    /* Light it: a forced-certain spark, pinned in place just long
     * enough to catch - mirrors wood_ignition_room()'s reasoning above,
     * except the fuel here is KIND_STATIC and never drifts itself, only
     * the spark that lights it could.
     *
     * sand_set_mobility(&s, 0) alone is NOT enough here, and it is worth
     * knowing why: it only blocks sub-pass 1 of sand_step_gas() (the
     * rise/diagonal-slide attempt), not sub-pass 2 (equalise_gas()'s
     * sideways spread), which is gated on has_room_above() - "is
     * there room to rise" - not on mobility at all. The spark's own row
     * sits one below the stone slab's bottom, so the cell directly
     * above it is real stone, not open sky: has_room_above() correctly
     * reports false, so equalise_gas() does NOT defer to sub-pass 1 the
     * way it would with open sky above (see
     * test_creating_steam_arms_the_gas_pass, where that deferral is
     * exactly what pins fire) - it goes ahead and looks sideways
     * instead, and an open cell beside the spark is exactly what it
     * would use to drift away before ever touching the wood (confirmed:
     * this is what the first version of this test, without the block
     * below, actually did). Blocking that one remaining open side is
     * what actually pins it: every neighbour is then either denser
     * stone, wood, or off the grid (which never counts, win or lose -
     * see neighbor_smothers()), so neither sub-pass has anywhere left
     * to send it. */
    sand_set(&wide, x - 2, wood_y, STONE);
    sand_set_flammability(&wide, 255);
    sand_set_mobility(&wide, 0);
    sand_set(&wide, x - 1, wood_y, FIRE);
    sand_step(&wide, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&wide, x, wood_y)),
        "setup: the wood must have caught and charred into an ember");

    /* Back to realistic behaviour for the boiler itself - the whole
     * point of this test is the REAL per-material conduction figure
     * (material.c's stone row) working through a slab as thick as the
     * app's own pour brush actually draws, not a forced one. */
    sand_set_flammability(&wide, SAND_FLAMMABILITY_PER_MATERIAL);
    sand_set_mobility(&wide, SAND_MOBILITY_PER_MATERIAL);

    for (int i = 0; i < 300; i++) {
        sand_step(&wide, 0, 1000, 0);
    }

    bool steam_above_basin = false;
    for (int y = 0; y < water_top && !steam_above_basin; y++) {
        for (int x2 = 0; x2 < WIDE_W; x2++) {
            if (CELL_MATERIAL(sand_at(&wide, x2, y)) == MAT_STEAM) {
                steam_above_basin = true;
                break;
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(steam_above_basin,
        "the boiler must eventually produce steam that rises above "
        "where the water started - wood catching, charring into an "
        "ember, conducting heat through an eleven-cell stone slab, and "
        "boiling the water on the other side, all through the real "
        "per-material figures with nothing forced. This is the "
        "feature; if it does not pass, nothing else in this section "
        "matters");
    TEST_ASSERT_LESS_THAN_MESSAGE(water_before,
        mass_of(&wide, WIDE_W, WIDE_H, MAT_WATER),
        "the water level must have dropped - the boiler consumed at "
        "least one cell's worth of it");
}

static void test_wood_and_steam_grain_count_is_conserved(void)
{
    fixture();
    /* No reaction side-effects to worry about here, unlike ember: wood
     * never burns on its own (it has no burning neighbour in this
     * scene) and steam does not react at all (reactions[MAT_STEAM] is
     * all-zero), so may_have_burning never even arms and
     * sand_step_reactions() early-returns every step - this is purely
     * a movement (or, for wood, non-movement) conservation check.
     * Ember is deliberately NOT covered here: its flare
     * (reaction_t.flare) can spawn a brand new MAT_FIRE cell out of an
     * empty neighbour, which is the feature working as designed
     * (test_an_ember_flares_fire_into_an_empty_neighbour), not a
     * conservation violation - but it does mean a strict grain-count
     * invariant, the kind this test checks, does not apply to ember the
     * way it does to every other material here. */
    for (int y = 1; y <= 2; y++) {
        for (int x = 1; x <= 3; x++) {
            sand_set(&s, x, y, WOOD);
        }
    }
    for (int y = 4; y <= 5; y++) {
        for (int x = 1; x <= 3; x++) {
            sand_set(&s, x, y, STEAM);
        }
    }
    const int expected = sand_count(&s);
    TEST_ASSERT_EQUAL_INT(12, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a step must conserve wood and steam grains in every "
                "gravity direction, the same as every other material");
        }
    }
}

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

    for (int i = 0; i < 90; i++) {
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

/* Must match app_sand.c. Duplicated rather than shared because sand.h has no
 * business knowing the screen size - see the note at the top of sand.h.
 *
 * Outside the DEVICE_BUILD block below, along with the tiling that uses it,
 * so the host can check the SHAPE of the mixed-material scene even though
 * only the device can time it. */
#define REAL_W 184
#define REAL_H 224

/* How much of the mixed-material scene is left empty, so a gravity flip has
 * somewhere to launch into. */
#define EMPTY_SHARE_PERCENT 33

/* Which material lands on cell (x, y) in the all-pairs tiling.
 *
 * Bands were the first attempt and covered far less than they looked like
 * they did: stacking materials in horizontal strips puts only the
 * vertically-adjacent pairs in contact, and measured, just 20 of the 66
 * possible pairs ever met. Two thirds of the reactions this simulation can
 * perform never fired in the scene whose whole purpose is to fire all of
 * them.
 *
 * This tiles instead. Horizontal neighbours in row y differ by `stride`,
 * and successive rows step through every possible difference, so every pair
 * of materials ends up adjacent somewhere and the pattern wraps without a
 * seam.
 *
 * One copy, called by both the device test that times the scene and the
 * host test that checks its coverage. Written out twice they could drift,
 * and the host check would then be verifying a pattern nobody runs. */
static inline int all_pairs_material_at(int x, int y, int first, int n_mats)
{
    const int stride = (y % (n_mats - 1)) + 1;
    return first + ((x * stride + y) % n_mats);
}

/* Every pair of materials really is adjacent somewhere in that scene.
 *
 * The property the scene exists for, and until now it was a number somebody
 * measured by hand once and wrote in a comment - "66 of 66" - taken on
 * faith through two subsequent materials. It is derived from MAT_COUNT, so
 * it should survive adding one, but "should" is what a test is for: a
 * tiling that quietly lost coverage would leave the worst case measuring
 * less than it claims while still passing its own budget.
 *
 * Host-side, because coverage is a property of the pattern and needs no
 * clock. Only the timing has to happen on the chip. */
static void test_the_mixed_scene_puts_every_material_pair_in_contact(void)
{
    const int first  = MAT_EMPTY + 1;
    const int n_mats = MAT_COUNT - first;
    const int top    = (REAL_H * EMPTY_SHARE_PERCENT) / 100;
    const int want   = (n_mats * (n_mats - 1)) / 2;

    /* One bit per unordered pair. */
    static bool seen[MATERIAL_MAX][MATERIAL_MAX];
    memset(seen, 0, sizeof seen);

    int found = 0;
    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = all_pairs_material_at(x, y, first, n_mats);
            const int nb[2] = {
                x + 1 < REAL_W ? all_pairs_material_at(x + 1, y, first, n_mats) : m,
                y + 1 < REAL_H ? all_pairs_material_at(x, y + 1, first, n_mats) : m,
            };
            for (int k = 0; k < 2; k++) {
                const int a = m < nb[k] ? m : nb[k];
                const int b = m < nb[k] ? nb[k] : m;
                if (a != b && !seen[a][b]) {
                    seen[a][b] = true;
                    found++;
                }
            }
        }
    }

    char why[160];
    snprintf(why, sizeof why,
             "the mixed-material scene must put all %d pairs of %d "
             "materials in contact - it reached %d, so some reaction it "
             "claims to exercise never fires in it",
             want, n_mats, found);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, found, why);
}

/* --- three more scenes, built once and shared with a device benchmark --- */

/* A previous round of this project spent three device rounds optimising a
 * function a failing benchmark never actually called - the benchmark timed
 * a scene that did not exercise the code it claimed to. The rule that came
 * out of it: a benchmark must be proven to run the reactions it claims to
 * measure, and "proven" means a host test that builds the SAME scene
 * through the SAME builder and checks the reactions really fired, not a
 * comment asserting they do. The three scenes below follow that shape -
 * see all_pairs_material_at() above for where the pattern started. */

/* Four liquids of different density, painted upside down. Left alone in
 * their own settled order - lava at the bottom, oil on top, water and acid
 * between - the four of them stratify within a few steps and the scene
 * goes quiet: each layer finds its level and the interfaces that were
 * doing the reacting stop touching. Painted INVERTED instead - lava on
 * top, then acid, then water, then oil at the bottom - every layer has to
 * migrate through every other layer to reach where density wants it, so
 * the interfaces stay in contact and reacting for the whole measured
 * window instead of resolving into inert bands.
 *
 * One copy, called by both the device test that times it and the host test
 * below that checks the reactions it claims to keep alive actually are. */
static void build_four_liquid_scene(sand_t *s)
{
    const int top = REAL_H / 6;                 /* headroom above the pour */
    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int band = ((y - top) * 4) / (REAL_H - top);
            const material_id_t m = (band == 0) ? MAT_LAVA
                                   : (band == 1) ? MAT_ACID
                                   : (band == 2) ? MAT_WATER : MAT_OIL;
            sand_set(s, x, y, CELL_MAKE(m, MASS_MAX));
        }
    }
}

/* The property the scene above exists for: that inverting the density
 * order really does keep the reactions running instead of merely moving
 * where they happen. Host-side, same reasoning as
 * test_the_mixed_scene_puts_every_material_pair_in_contact - coverage is a
 * property of the scene and needs no clock, only the timing needs the
 * chip.
 *
 * Runs with the app's own per-material scatter, decay and mobility rather
 * than the defaults, because app_sand.c does too - see the device test
 * below for why that setting matters here specifically. */
static void test_the_four_liquid_scene_keeps_reacting_after_settling(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_four_liquid_scene(&s);

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int stone = 0, steam = 0, fire = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_STONE)      stone++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_FIRE)  fire++;
        }
    }

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, stone,
        "lava quenched by water should still be leaving a good showing of "
        "stone at the end of the window - if it isn't, the scene has gone "
        "quiet and the device test beside it is measuring almost nothing");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, steam,
        "water boiled and fire quenched should still be leaving a good "
        "showing of steam at the end of the window - if it isn't, the "
        "scene has gone quiet and the device test beside it is measuring "
        "almost nothing");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(50, fire,
        "oil ignited by lava should still be leaving a good showing of "
        "fire at the end of the window - if it isn't, the scene has gone "
        "quiet and the device test beside it is measuring almost nothing");
}

/* A lava reservoir on the floor, a water slab on the roof, and between them
 * repeating six-cell columns of sand, wood and oil with every fourth
 * column left empty as a chute. Lava is the reaction-richest material in
 * the simulation - it is a heat source, it quenches to stone in water, it
 * boils water to steam, it turns sand to glass by heat, it ignites both
 * wood and oil, and it flares - so this puts all six of those in one scene
 * instead of spending one test per reaction.
 *
 * The empty column matters more than it looks. Without it, the roof water
 * perches on top of the columns and takes most of a minute to reach the
 * lava, so the quench and boil reactions - two of the six this scene
 * exists to exercise - never fire inside the measured window at all. With
 * it, water has somewhere to fall straight through, and reaches the lava
 * while the scene is still burning. */
static void build_lava_stress_scene(sand_t *s)
{
    /* floor: a lava reservoir */
    for (int y = (REAL_H * 3) / 4; y < REAL_H; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));

    /* middle: repeating columns six cells wide - sand, wood, oil, then a
     * gap - deliberately, not an oversight, see the comment above. */
    for (int y = REAL_H / 3; y < (REAL_H * 3) / 4; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int col = (x / 6) % 4;
            if (col == 0)      sand_set(s, x, y, SAND_FIRST_SHADE);
            else if (col == 1) sand_set(s, x, y, CELL_MAKE(MAT_WOOD, 0));
            else if (col == 2) sand_set(s, x, y, CELL_MAKE(MAT_OIL, MASS_MAX));
            /* col == 3 is the chute - left empty on purpose */
        }
    }

    /* roof: a water slab */
    for (int y = 0; y < REAL_H / 6; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
}

/* All six reactions the scene above exists to cover really do fire in it,
 * checked the same way test_the_four_liquid_scene_keeps_reacting_after_-
 * settling checks its own scene: build it through the same function the
 * device test uses, step it the same number of times, and count. */
static void test_the_lava_stress_scene_reaches_every_reaction_it_claims(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_lava_stress_scene(&s);

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int glass = 0, fire = 0, steam = 0, stone = 0, extended = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            if (m == MAT_GLASS)      glass++;
            else if (m == MAT_FIRE)  fire++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_STONE) stone++;
            if (cell_is_extended(c)) extended++;
        }
    }

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(400, glass,
        "sand converting under sustained heat should have left a good "
        "showing of glass");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(400, fire,
        "wood and oil igniting against the lava should have left a good "
        "showing of fire");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(20, steam,
        "water reaching the lava through the chute should have boiled some "
        "of it to steam - a low count here means the chute let the water "
        "perch instead of falling through");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5, stone,
        "water reaching the lava through the chute should have quenched "
        "some of it to stone - a low count here means the chute let the "
        "water perch instead of falling through");

    /* This scene already has both ingredients a plant needs sitting in it -
     * wood, and sand that could take up water and become soil - and yet it
     * never grows one: the roof water reaches the lava through the chute
     * and flashes straight to steam before it ever gets to wet the sand,
     * so no dirt is ever made and the wood stays dry for the whole run.
     * That is an accident of how this scene happens to be tuned, not a
     * property anyone has checked - and the plant materials are under
     * active development, so pin it here instead of leaving it to keep
     * holding by luck. The device test beside this one gets its frame
     * budget pegged from a hardware capture of this same scene; if plant
     * growth ever starts happening inside that measured window, the
     * number being pegged would quietly stop describing what the test
     * claims to measure. This assertion is what makes that change
     * announce itself instead of passing silently. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended,
        "the lava stress scene should not be growing any plants - if it "
        "is, the device test's frame budget is no longer measuring the "
        "scene it claims to");
}

/* An edge-to-edge checkerboard of smoke and steam, with one spark of fire
 * in a bottom corner. The same "deliberately synthetic worst case, not
 * something the pour brush can produce" framing as the two full-screen
 * fire tests below already use for an edge-to-edge screen of fire: no
 * scene a user can actually paint packs the whole grid with gas, but the
 * reactions pass has to survive the case where one does.
 *
 * What this catches that neither of those two does: fire and plain gas
 * have no convection behaviour, but smoke and steam do - they warm what
 * they touch - and no benchmark in this suite has ever put either of them
 * on screen in quantity before. This is the scene where the reactions
 * pass's per-cell neighbour work for gases actually runs.
 *
 * Left at the DEFAULT scatter, decay and mobility - deliberately NOT the
 * per-material settings build_four_liquid_scene() uses above. At
 * per-material decay the smoke and steam fade away within the measured
 * window, and the scene stops being the steady worst case it exists to
 * be. */
static void build_smoke_and_steam_scene(sand_t *s)
{
    for (int y = 0; y < REAL_H; y++)
        for (int x = 0; x < REAL_W; x++)
            sand_set(s, x, y, ((x + y) & 1) ? CELL_MAKE(MAT_SMOKE, 8)
                                             : CELL_MAKE(MAT_STEAM, 8));
    sand_set(s, REAL_W / 2, REAL_H - 1, CELL_MAKE(MAT_FIRE, 8));
}

/* The scene above really is still a gas screen at the end of the measured
 * window, not one that quietly emptied itself into something else - and
 * cells are conserved throughout, the same setup check the two full-screen
 * fire tests below make of their own scenes. */
static void test_the_smoke_and_steam_scene_stays_a_gas_screen(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&s, blocks);

    build_smoke_and_steam_scene(&s);
    const int total = REAL_W * REAL_H;

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int smoke = 0, steam = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_SMOKE)      smoke++;
            else if (m == MAT_STEAM) steam++;
        }
    }
    const int count = sand_count(&s);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, count,
        "cells must only ever convert material, never appear or vanish, "
        "across a screen of smoke and steam");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5000, smoke,
        "the scene should still be mostly smoke and steam at the end of "
        "the window - a low count means this has decayed into something "
        "the device test beside it no longer measures");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(5000, steam,
        "the scene should still be mostly smoke and steam at the end of "
        "the window - a low count means this has decayed into something "
        "the device test beside it no longer measures");
}

/* A lattice of glass-walled compartments - 20 columns by 24 rows, 480 in
 * all - each one a ring of glass around a single payload, with a shatter
 * trigger sitting just outside the ring rather than inside it. Every other
 * thermal-shock test in this file places one pane at a chosen temperature
 * and drops one cold or hot thing next to it; this scene exists to ask
 * what the mechanism does at the scale the pour brush can actually
 * produce, with hundreds of panes cracking, draining and re-heating at
 * once instead of one.
 *
 * THE INVARIANT THAT MAKES THE SCENE HONEST: every ring is painted at
 * variant 2, 3 or 4 - strictly between SAND_SHOCK_COLD (1) and
 * SAND_SHOCK_HEAT (5) - so no compartment is born already qualifying for
 * a crack. An earlier draft of this scene used an asymmetric range that
 * reached down to 0 and 1, and it was a real dead end: those rings
 * shattered on step 1, through whichever shock direction their family was
 * NOT meant to be exercising, before the outside trigger had ramped
 * anything at all - the scene was testing its own setup rather than the
 * mechanism. Starting strictly inside the gap is also the stagger lever:
 * step_one_cold_cell() moves a pane one level per successful roll, so a
 * ring at 2 is one chill from the cold threshold and a ring at 4 is
 * three - and the same distances the other way round for the climb to
 * SAND_SHOCK_HEAT - so the 480 compartments do not all cross at once
 * even though they are all built from the same two triggers.
 *
 * WHY THE COMPARTMENTS ARE SEPARATED: each ring is 20 cells, far under
 * crack_run()'s CRACK_MAX of 256, and what keeps one shock from reaching
 * a neighbouring compartment's glass at all is the tile's own layout, not
 * the grid's leftover margin: lx 0 and ly 0-1 are left empty and the
 * trigger takes lx 1, lx 8 and ly 8, so the nearest glass in the next
 * tile is three cells away with a trigger and empty space in between.
 * (The four spare columns and eight spare rows - 20x9 is 180 of 184 and
 * 24x9 is 216 of 224 - are unused margin along the right and bottom
 * edges, and separate nothing.) See test_a_crack_does_not_jump_to_a_-
 * separate_pane, which is the same guarantee this scene leans on at 480x
 * the scale.
 *
 * WHY LAVA IS A PAYLOAD AND NEVER A TRIGGER: lava is a liquid, and an
 * outside trigger sits in a bare one-cell-wide U with nothing under it
 * from below the grid - a liquid there would simply drain away before it
 * ever got to test anything. The two outside triggers are instead the
 * materials that hold still on their own: burning wood (KIND_STATIC) and
 * ice (KIND_STATIC). Lava only ever appears as a payload, sitting inside
 * a box that can actually hold it. This is a deliberate departure from
 * the original sketch for this scene, which asked for lava as an outside
 * trigger too - it does not survive contact with how liquids move.
 *
 * WHAT THE FAMILY SPLIT DOES AND DOES NOT DO: the left ten columns
 * (family C) pair a burning-wood trigger with a cold payload - ice or
 * snow - so they are BUILT to favour the cold-onto-hot direction, and the
 * right ten columns (family H) pair an ice trigger with a hot payload -
 * wood or lava - to favour hot-onto-cold. MEASURED, the split is not
 * pure: family C's own cold payload chills its ring past SAND_SHOCK_COLD
 * from the inside, so hot-onto-cold fires there too, and family H's own
 * hot payload pushes its ring past SAND_SHOCK_HEAT from the inside, so
 * cold-onto-hot fires there as well. Both directions run in both halves
 * from step 1. The split earns its place as the payload/trigger MATRIX -
 * four combinations of {cold, hot} outside x {cold, hot} inside, laid out
 * so every compartment has an outside push and an inside push in the same
 * or opposite sense - not as proof that either half exercises only one
 * direction. What actually proves each direction fires is the pair of
 * counters in the host test below, which look at the mechanism's own
 * precondition directly rather than trusting the geometry to imply it.
 *
 * What this measures that nothing else in this file does: heat_ramp
 * climbing through hundreds of independent panes at once, in-glass
 * conduction along each ring, crack_run() firing under sustained load
 * instead of once, and the mixed aftermath of that all at once -
 * meltwater, steam, escaping fire and falling cullet sharing the same
 * screen.
 *
 * Runs at the app's own per-material scatter, decay and mobility, the
 * same choice build_lava_stress_scene() makes above and for the same
 * reason: app_sand.c does too. */
static void build_thermal_shock_scene(sand_t *s)
{
    for (int tr = 0; tr < 24; tr++) {
        for (int tc = 0; tc < 20; tc++) {
            const int ox = tc * 9, oy = tr * 9;
            const bool family_c = (tc < 10);
            const int ring_temp = 2 + (tc % 3);   /* {2,3,4} */

            /* glass ring: perimeter of lx in [2,7], ly in [2,7] */
            for (int ly = 2; ly <= 7; ly++) {
                for (int lx = 2; lx <= 7; lx++) {
                    if (lx != 2 && lx != 7 && ly != 2 && ly != 7) {
                        continue;
                    }
                    sand_set(s, ox + lx, oy + ly,
                             CELL_MAKE(MAT_GLASS, (uint8_t)ring_temp));
                }
            }

            /* the trigger, outside the box: a U under and beside it */
            const cell_t trigger = family_c ? CELL_MAKE(MAT_WOOD, MASS_MAX)
                                            : MATX(MATX_ICE);
            for (int ly = 2; ly <= 8; ly++) {
                sand_set(s, ox + 1, oy + ly, trigger);
                sand_set(s, ox + 8, oy + ly, trigger);
            }
            for (int lx = 2; lx <= 7; lx++) {
                sand_set(s, ox + lx, oy + 8, trigger);
            }

            /* the payload, inside */
            const bool low = (tr & 1) != 0;
            const cell_t payload = family_c
                ? (low ? MATX(MATX_ICE) : CELL_MAKE(MAT_SNOW, MASS_MAX))
                : (low ? CELL_MAKE(MAT_WOOD, MASS_MAX)
                       : CELL_MAKE(MAT_LAVA, MASS_MAX));
            const int ly0 = low ? 5 : 3;
            for (int ly = ly0; ly <= ly0 + 1; ly++) {
                for (int lx = 3; lx <= 6; lx++) {
                    sand_set(s, ox + lx, oy + ly, payload);
                }
            }
        }
    }
}

/* The two shock directions - cold arriving at hot glass in
 * step_one_cold_cell(), heat arriving at cold glass in try_heat_-
 * transform() - are different code paths that can break independently
 * and have (see test_heat_arriving_at_frosted_glass_cracks_it, which
 * exists for exactly that reason). This scene claims to exercise both at
 * once across the whole lattice, and the two counters below check that
 * claim directly rather than trusting the payload/trigger matrix to
 * imply it - see build_thermal_shock_scene()'s comment for why the
 * matrix alone is not that proof.
 *
 * d1_ready counts MAT_GLASS cells at variant >= SAND_SHOCK_HEAT with a
 * cardinal neighbour whose reaction_of() has chills != 0 - exactly
 * step_one_cold_cell()'s shock precondition, which takes no roll once it
 * holds. d2_ready is its mirror: MAT_GLASS cells at variant <=
 * SAND_SHOCK_COLD with a cardinal neighbour that cell_is_burning(), which
 * is try_heat_transform()'s precondition, also roll-free. Roll-free is
 * the whole reason to count preconditions rather than cracks: a standing
 * precondition is a fact about the board, not a probability, so a
 * non-zero count is real evidence that direction is live. It is not quite
 * a promise that those exact panes break next step - the movement passes
 * run first, and a drift or a melting block can leave the pane before the
 * reactions pass reaches it - which is why the assertions below grade
 * these on HOW MANY STEPS the precondition stands, not on a count.
 *
 * Both are counted after every one of the 10 measured steps, because the
 * claim is that each direction keeps firing across the window, not just
 * once at the start.
 *
 * WHY TEN STEPS, AND WHY THIRDS OF (step - 1) / 3: the window is graded
 * by charging each step's new cullet to one third of it - steps 1-3, 4-6,
 * 7-10 - and ten is the shortest window where the LAST third still earns
 * a real share of the total. Measured, ten steps split 54.1 / 28.9 /
 * 17.0 percent; twelve, fifteen and twenty all push the tail under 15 as
 * the early cracking dominates more and more of the run (11.8, 11.5 and
 * 13.6 percent), and eight steps split honestly into thirds leaves the
 * last one at 11.0. The divisor and the step count are one decision:
 * change the window without changing /3 and the buckets stop being
 * thirds at all, which is exactly how an earlier draft came to grade a
 * 2/2/4 split as if it were 3/3/4.
 *
 * The cullet tally needs a STICKY mask - a cell that was ever cullet,
 * tracked separately from what is cullet right now - and that is a real
 * finding rather than a stylistic choice. Cullet is MAT_SAND at a variant
 * SAND_CULLET_BASE or higher, and it is not inert: sand.heats_to is
 * MAT_GLASS, so a fallen shard sitting near a hot payload can re-fuse
 * into glass and later crack again. A naive per-step delta on a live
 * MAT_SAND count goes negative the moment that happens, undercounting
 * exactly the churn this scene exists to show. The sticky mask only ever
 * grows, so "new cullet this third" stays a meaningful, non-negative
 * quantity even while individual cells are cycling glass -> cullet ->
 * glass under the payload's heat.
 *
 * The mask is a BITSET, not a byte per cell. This is the only test in
 * the file that needs a second full-grid buffer alongside `big`, and the
 * device has only about 68 KB of heap left once the display framebuffer
 * is carved out of it - two 41,216-byte grids do not fit in that, one
 * byte per cell does. The first device run of this test with a byte mask
 * failed AND leaked 41,240 bytes for the rest of boot, because the null
 * check on the third malloc aborted the test before the frees at its end
 * ever ran. One bit per cell brings the mask down to 5,152 bytes, which
 * fits comfortably. */
#define EVER_CULLET_BYTES \
    (((size_t)REAL_W * (size_t)REAL_H + 7) / 8)

static inline bool ever_cullet_get(const uint8_t *mask, size_t idx)
{
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

/* Sets the bit for `idx` and reports whether it was actually clear
 * beforehand, so callers can count new cullet without a second pass over
 * the mask. */
static inline bool ever_cullet_set(uint8_t *mask, size_t idx)
{
    const uint8_t bit = (uint8_t)(1u << (idx & 7));
    const bool was_clear = (mask[idx >> 3] & bit) == 0;
    mask[idx >> 3] |= bit;
    return was_clear;
}

static void test_the_thermal_shock_scene_shatters_in_both_directions(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t *ever_cullet = malloc(EVER_CULLET_BYTES);
    const bool have_all = (big != NULL && blocks != NULL &&
                            ever_cullet != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(ever_cullet);
        TEST_FAIL_MESSAGE("need a grid, a block map and a one-bit-per-cell "
                           "cullet mask for the thermal shock scene, and "
                           "at least one of the three failed to allocate");
    }
    memset(ever_cullet, 0, EVER_CULLET_BYTES);

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_thermal_shock_scene(&s);
    const int painted = sand_count(&s);

    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };

    int d1_steps_nonzero = 0, d2_steps_nonzero = 0;
    int sticky_total_before = 0;
    int third_gain[3] = { 0, 0, 0 };

    for (int step = 1; step <= 10; step++) {
        sand_step(&s, 0, 1000, 0);

        int d1 = 0, d2 = 0;
        for (int y = 0; y < REAL_H; y++) {
            for (int x = 0; x < REAL_W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) != MAT_GLASS) {
                    continue;
                }
                const int v = CELL_VARIANT(c);
                bool near_chiller = false, near_burner = false;
                for (int d = 0; d < 4; d++) {
                    const int nx = x + dx[d], ny = y + dy[d];
                    if ((unsigned)nx >= (unsigned)REAL_W ||
                        (unsigned)ny >= (unsigned)REAL_H) {
                        continue;
                    }
                    const cell_t n = sand_at(&s, nx, ny);
                    if (CELL_IS_EMPTY(n)) {
                        continue;
                    }
                    if (reaction_of(n)->chills != 0)  near_chiller = true;
                    if (cell_is_burning(n))           near_burner  = true;
                }
                if (v >= SAND_SHOCK_HEAT && near_chiller) d1++;
                if (v <= SAND_SHOCK_COLD && near_burner)  d2++;
            }
        }
        if (d1 > 0) d1_steps_nonzero++;
        if (d2 > 0) d2_steps_nonzero++;

        /* Grow the sticky mask, then charge the growth to this step's
         * third of the window - see the comment above for why the mask
         * has to be sticky rather than a live per-step count. The mask
         * only ever grows, so counting each bit's clear-to-set transition
         * right here, as it happens, is exactly equivalent to rescanning
         * the whole mask afterwards and diffing against the previous
         * total - a rescan could only ever find the same bits this loop
         * just set. */
        int sticky_total = sticky_total_before;
        for (int y = 0; y < REAL_H; y++) {
            for (int x = 0; x < REAL_W; x++) {
                const cell_t c = sand_at(&s, x, y);
                if (CELL_MATERIAL(c) == MAT_SAND &&
                    CELL_VARIANT(c) >= SAND_CULLET_BASE) {
                    if (ever_cullet_set(ever_cullet,
                                         (size_t)y * REAL_W + (size_t)x)) {
                        sticky_total++;
                    }
                }
            }
        }
        int third = step - 1;
        third /= 3;
        if (third > 2) third = 2;
        third_gain[third] += sticky_total - sticky_total_before;
        sticky_total_before = sticky_total;
    }

    int cullet_left = 0, cullet_right = 0;
    int water = 0, steam = 0, fire = 0, matx_plant = 0, heat_holders = 0;
    int lava_left = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            const bool left_half = x < REAL_W / 2;
            if (m == MAT_SAND && CELL_VARIANT(c) >= SAND_CULLET_BASE) {
                if (left_half) cullet_left++; else cullet_right++;
            }
            if      (m == MAT_WATER) water++;
            else if (m == MAT_STEAM) steam++;
            else if (m == MAT_FIRE)  fire++;
            else if (m == MAT_LAVA && left_half) lava_left++;
            if (cell_is_extended(c) && CELL_VARIANT(c) == MATX_PLANT) {
                matx_plant++;
            }
            if (!CELL_IS_EMPTY(c) && reaction_of(c)->heat_ramp != 0) {
                heat_holders++;
            }
        }
    }

    /* Distinct TILES (of the 240 per half) that have gained at least one
     * cullet cell over the window - a coarser, per-compartment measure
     * that a handful of very active tiles cannot satisfy on their own,
     * unlike the raw cell counts above. */
    int distinct_left = 0, distinct_right = 0;
    for (int tr = 0; tr < 24; tr++) {
        for (int tc = 0; tc < 20; tc++) {
            bool has_cullet = false;
            for (int ly = 0; ly < 9 && !has_cullet; ly++) {
                for (int lx = 0; lx < 9 && !has_cullet; lx++) {
                    const int x = tc * 9 + lx, y = tr * 9 + ly;
                    if (x >= REAL_W || y >= REAL_H) {
                        continue;
                    }
                    if (ever_cullet_get(ever_cullet,
                                         (size_t)y * REAL_W + (size_t)x)) {
                        has_cullet = true;
                    }
                }
            }
            if (has_cullet) {
                if (tc < 10) distinct_left++; else distinct_right++;
            }
        }
    }

    const int sand_count_now = sand_count(&s);
    const bool temperature_flag  = s.may_have_temperature;
    const bool heat_holder_flag  = s.may_have_heat_holder;

    free(big);
    free(blocks);
    free(ever_cullet);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, d1_steps_nonzero,
        "the cold-onto-hot direction (step_one_cold_cell()'s shock "
        "precondition) must really be firing across most of the window - "
        "if this is low, family H's ice trigger and family C's own cold "
        "payload have stopped reaching hot glass and this scene is no "
        "longer exercising the direction it claims to");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(4, d2_steps_nonzero,
        "the hot-onto-cold direction (try_heat_transform()'s shock "
        "precondition) must really be firing across most of the window - "
        "kept as a SEPARATE assertion from the one above for the same "
        "reason test_heat_arriving_at_frosted_glass_cracks_it is kept "
        "separate from its mirror: the two directions are different code "
        "and break independently");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, cullet_left,
        "the left half of the lattice must be producing cullet in "
        "quantity, not just in one corner of it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, cullet_right,
        "the right half of the lattice must be producing cullet in "
        "quantity, not just in one corner of it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, distinct_left,
        "shattering must be spread across many compartments in the left "
        "half, not concentrated in a few tiles that happen to be "
        "unusually active");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, distinct_right,
        "shattering must be spread across many compartments in the right "
        "half, not concentrated in a few tiles that happen to be "
        "unusually active");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[0],
        "the first third of the window must already be producing new "
        "cullet - see the sticky-mask comment above for why this counts "
        "distinct cells that have EVER been cullet rather than a live "
        "snapshot, which would undercount once fallen shards start "
        "re-fusing near the payload's heat");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[1],
        "the middle third of the window must still be producing new "
        "cullet - shattering has to be staggered across the window "
        "rather than all landing in the first couple of steps");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1500, third_gain[2],
        "the last third of the window must still be producing new "
        "cullet - if this is low while the first third is not, the "
        "lattice went quiet early and the device benchmark beside this "
        "test is measuring a scene that has already settled");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1000, water,
        "meltwater from ice and snow must still be showing at the end of "
        "the window");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(800, steam,
        "steam from meltwater meeting a hot payload must still be "
        "showing at the end of the window");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2000, fire,
        "fire escaping broken compartments must still be showing at the "
        "end of the window");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(painted, sand_count_now,
        "cells must never be net-destroyed - this is deliberately a "
        "FLOOR, not a conservation check: burning wood and lava both "
        "flare fresh MAT_FIRE into empty neighbours, so the live count "
        "is expected to grow past what was painted, exactly as it does "
        "in the lava stress and four-liquid scenes above, neither of "
        "which asserts conservation either");

    /* Model: the lava stress scene's own plant pin above. This scene
     * makes meltwater and has sand about (both plain and cullet), so wet
     * soil is reachable in principle; the plant materials are under
     * active development, and if growth ever starts happening inside
     * this measured window, the frame budget the device benchmark beside
     * this test is pegging from a hardware capture would quietly stop
     * describing the scene it claims to. This assertion is what makes
     * that change announce itself instead of passing silently.
     *
     * Also note this scene cannot reuse the lava stress scene's plain
     * cell_is_extended(c) form: this scene's own payload uses
     * MATX(MATX_ICE), which IS an extended cell, so the pin has to name
     * MATX_PLANT specifically or it would fail on the ice this scene
     * paints on purpose. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, matx_plant,
        "the thermal shock lattice should not be growing any plants - if "
        "it is, the device test's frame budget is no longer measuring "
        "the scene it claims to, and the ice payload means the usual "
        "cell_is_extended() plant pin cannot be reused here as-is");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lava_left,
        "family C's rings (the left half) must not have melted into "
        "lava inside this window - measured, those rings do keep "
        "climbing under their own payload and trigger's heat, and the "
        "first left-half lava cell appears at step 16 (123 of them by "
        "step 40), which is a second, independent reason this window is "
        "kept to 10 steps rather than left to run longer");

    /* step_one_warming_cell()'s call site is gated on three things at
     * once - r->warms, may_have_temperature and may_have_heat_holder -
     * see sand_reactions.c, the branch a previous tuning round added
     * that third flag for. The four assertions below pin all three, plus
     * the physical fact behind the last of them (something on the board
     * really can hold a temperature, not merely a flag saying so).
     * Asserting them together is what proves the warming path is
     * genuinely reachable in this scene rather than skipped by a gate
     * that happens to be shut. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, steam,
        "setup for the warming-gate check below: there must be steam on "
        "the board for the gate to be worth anything");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, heat_holders,
        "setup for the warming-gate check below: there must be cells "
        "that can hold a temperature (glass, here) for the gate to be "
        "worth anything");
    TEST_ASSERT_TRUE_MESSAGE(temperature_flag,
        "may_have_temperature must be armed by this scene, or "
        "step_one_warming_cell()'s call site is never reached at all");
    TEST_ASSERT_TRUE_MESSAGE(heat_holder_flag,
        "may_have_heat_holder must be armed by this scene too - "
        "may_have_temperature alone is not the gate; a previous tuning "
        "round added this second flag specifically because the first one "
        "arms itself the moment anything with a temperature is painted, "
        "which is not the same claim as there being something around "
        "that can actually hold one");
}

/* The boiler from test_the_boiler_end_to_end, scaled from one column to
 * the whole 184x224 grid and run as a SUSTAINED STEADY STATE rather than
 * a transient - the opposite of build_thermal_shock_scene() above,
 * deliberately, so the pair covers both shapes of thermal load this
 * simulation has to handle: a burst of damage that runs its course, and
 * a heat source left running that has to keep producing without either
 * exhausting its fuel or its water.
 *
 * The slab is 11 rows thick, the pour brush's real thickness - the same
 * figure test_the_boiler_end_to_end uses, and for the same reason.
 * conduct_heat() attenuates at roughly 0.86 per cell of depth it has to
 * cross, so slab thickness is the THROTTLE on how fast the basin can
 * boil: eleven rows is what keeps the rate sustainable across the whole
 * measured window instead of exhausting the basin partway through it.
 *
 * TWO BURNERS ON PURPOSE: lava never decays, so it is the steady heat
 * source; wood burns down (burn_decay 24) and is there so the OTHER heat
 * source path - an ember rather than a permanent liquid - is covered by
 * the same scene instead of needing a second one. Measured, all 356 wood
 * cells painted are still lit at the end of the window, and both halves
 * of the basin boil at close to the same rate - see the host test's
 * per-half assertions.
 *
 * WHY THE BOILING RATE IS SELF-SUSTAINING: steam made at the slab is
 * lighter than the water sitting above it, so try_bubble() (sand_gas.c)
 * swaps it upward one cell at a time and water falls back down onto the
 * slab to be boiled in its turn. The basin keeps refilling its own hot
 * face on its own; no extra geometry - chutes, gaps, anything - is
 * needed to make that happen, unlike build_lava_stress_scene() above,
 * which needs its chute for exactly this reason.
 *
 * WHY THE BURNER IS FULLY ENCLOSED (stone side walls the full depth of
 * the basin, a stone slab, the grid floor underneath): so that flare has
 * almost nowhere to put fresh fire, and the cell count therefore says
 * something about the boil rather than about how much empty space
 * happened to be lying around.
 *
 * "Almost" is the honest word, and it is why the host test below asserts
 * a FLOOR on the count rather than an equality. Measured: the count sits
 * exactly at its window-start value for the first twenty steps of the
 * measured window and then starts climbing, reaching 8343 from 8280 by
 * the end of it - the boil has by then opened enough gaps in the water
 * above the slab for flare to reach them. An equality would simply fail
 * here - and it is worth knowing that it held for the shorter settle an
 * earlier draft of this scene used only by a SINGLE step: total step 41
 * is where the count first moves, and that draft stopped at 40. That is
 * not a margin worth building an assertion on. */
static void build_boiler_scene(sand_t *s)
{
    const int burn_h = 4, slab_h = 11, water_h = 30;
    const int burn_top  = REAL_H - burn_h;          /* 220 */
    const int slab_top  = burn_top - slab_h;        /* 209 */
    const int water_top = slab_top - water_h;       /* 179 */

    /* basin walls, full depth */
    for (int y = water_top; y < REAL_H; y++) {
        for (int x = 0; x < 3; x++) {
            sand_set(s, x, y, STONE);
            sand_set(s, REAL_W - 1 - x, y, STONE);
        }
    }
    /* two burners under one slab */
    for (int y = burn_top; y < REAL_H; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, (x < REAL_W / 2) ? CELL_MAKE(MAT_LAVA, MASS_MAX)
                                               : CELL_MAKE(MAT_WOOD, MASS_MAX));
        }
    }
    /* the slab */
    for (int y = slab_top; y < burn_top; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, STONE);
        }
    }
    /* the water */
    for (int y = water_top; y < slab_top; y++) {
        for (int x = 3; x <= REAL_W - 4; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* The boiler above really does keep boiling for the whole window rather
 * than front-loading its output and going quiet, checked the same way
 * the other scenes in this section are: build it through the same
 * function the device test uses, step it the same number of times, and
 * measure.
 *
 * 20 settle steps first - twice the "let it get going" allowance
 * test_four_liquids_reacting_at_once_fits_in_the_frame_budget gives its
 * own scene, because a basin takes longer to reach a steady boil than a
 * liquid stack takes to start mixing: at ten steps the board is still
 * filling with the first flush of steam (295 cells of it), at twenty it
 * is boiling at a rate that then holds for the whole window.
 *
 * Then 30 measured steps, sampled at 0, 7, 15, 22 and 30 steps into the
 * measured window - four intervals, so the per-quarter loss assertions
 * below can catch a basin that boils hard at first and then tails off,
 * which a single before/after comparison could not. Measured, the four
 * quarters lose 206, 211, 215 and 157 cells of water: level enough to
 * call it steady, and the assertions are held at 100 so ordinary
 * quarter-to-quarter variation does not read as a stall. */
static void test_the_boiler_scene_keeps_boiling_across_the_window(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                              ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    const bool have_all = (big != NULL && blocks != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        TEST_FAIL_MESSAGE("need a grid and a block map for the boiler "
                           "scene, and at least one of the two failed to "
                           "allocate");
    }

    sand_t s;
    sand_init(&s, big, REAL_W, REAL_H, 43u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);

    build_boiler_scene(&s);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int water_window_start = 0, steam_window_start = 0;
    int water_left_start = 0, water_right_start = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = CELL_MATERIAL(sand_at(&s, x, y));
            if (m == MAT_WATER) {
                water_window_start++;
                if (x < REAL_W / 2) water_left_start++; else water_right_start++;
            } else if (m == MAT_STEAM) {
                steam_window_start++;
            }
        }
    }
    const int count_at_window_start = sand_count(&s);

    int water_at_checkpoint[5];
    water_at_checkpoint[0] = water_window_start;
    const int checkpoints[4] = { 7, 15, 22, 30 };
    int next_checkpoint = 0;

    for (int i = 1; i <= 30; i++) {
        sand_step(&s, 0, 1000, 0);
        if (next_checkpoint < 4 && i == checkpoints[next_checkpoint]) {
            int w = 0;
            for (int y = 0; y < REAL_H; y++) {
                for (int x = 0; x < REAL_W; x++) {
                    if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_WATER) w++;
                }
            }
            water_at_checkpoint[next_checkpoint + 1] = w;
            next_checkpoint++;
        }
    }

    int water = 0, steam = 0, stone = 0, burning_wood = 0;
    int stone_off_ambient = 0, extended = 0;
    int water_left = 0, water_right = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&s, x, y);
            const int m = CELL_MATERIAL(c);
            const bool left_half = x < REAL_W / 2;
            if (m == MAT_WATER) {
                water++;
                if (left_half) water_left++; else water_right++;
            } else if (m == MAT_STEAM) {
                steam++;
            } else if (m == MAT_STONE) {
                stone++;
                if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT) stone_off_ambient++;
            } else if (m == MAT_WOOD && cell_is_burning(c)) {
                burning_wood++;
            }
            if (cell_is_extended(c)) extended++;
        }
    }

    const int sand_count_now = sand_count(&s);
    const bool temperature_flag = s.may_have_temperature;
    const bool heat_holder_flag = s.may_have_heat_holder;

    free(big);
    free(blocks);

    for (int q = 0; q < 4; q++) {
        const int lost = water_at_checkpoint[q] - water_at_checkpoint[q + 1];
        char why[320];
        snprintf(why, sizeof why,
                 "the boiler must still be boiling in quarter %d of the "
                 "measured window (lost %d cells of water there) - a "
                 "quiet quarter means the basin exhausted its heat or "
                 "its water before the window was over, and this is "
                 "meant to be a STEADY state, not a transient", q, lost);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(100, lost, why);
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(3500, water,
        "the basin must not be exhausted by the end of the window - "
        "measured, 3958 cells of water are left, 83% of what the window "
        "started with and 74% of what was painted, which is what makes "
        "this a steady state rather than another transient like the "
        "thermal shock lattice above");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(800, steam,
        "steam production must be sustained through to the end of the "
        "window");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(steam_window_start, steam,
        "steam must have grown over the measured window, not merely be "
        "present - a count that matches the window's starting steam "
        "would mean production had already stalled by the time "
        "measurement began");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(200, stone_off_ambient,
        "the slab must be genuinely carrying a temperature by the end of "
        "the window - this is the proof that heat is arriving at the "
        "water by conduction THROUGH the slab, not by some other route");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(300, burning_wood,
        "the second burner must still be alight at the end of the "
        "window, or the \"two heat sources\" claim this scene makes only "
        "holds for part of it");

    /* Both halves boil, not just the lava-fed one - the wood-fed half's
     * ember has to be pulling its own weight too. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(200,
        water_left_start - water_left,
        "the left (lava-fed) half of the basin must have lost a real "
        "amount of water over the window");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(200,
        water_right_start - water_right,
        "the right (wood-fed) half of the basin must have lost a real "
        "amount of water over the window - a low loss here would mean "
        "the ember burner is not pulling its share and the \"two "
        "burners\" claim only holds on one side");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(count_at_window_start, sand_count_now,
        "cells must never be net-destroyed here - a FLOOR, not the exact "
        "conservation test_a_screen_of_smoke_and_steam_fits_in_the_frame_"
        "budget makes of its own scene, because this one has a burner in "
        "it. The stone enclosure (side walls, slab, grid floor) leaves "
        "flare almost nowhere to put fresh fire, and measured the count "
        "holds at its window-start 8280 for twenty steps before the boil "
        "opens gaps above the slab and it climbs to 8343 - see "
        "build_boiler_scene()'s comment. Water boiling to steam and steam "
        "condensing back are both one-for-one, so a count BELOW the "
        "window's start means cells went missing, which is a different "
        "and much worse thing than flare adding a few");

    /* Same reasoning as the thermal shock lattice's plant pin above, and
     * the plain cell_is_extended() form works here, unlike there,
     * because this scene paints no extended material at all. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, extended,
        "the boiler scene should not contain any extended cells - if it "
        "does, either the scene changed to paint one on purpose (update "
        "this test) or something is growing that this benchmark was "
        "never meant to measure");

    /* The same check on step_one_warming_cell()'s three-part call-site
     * gate as the thermal shock lattice's host test above - the branch a
     * previous tuning round added may_have_heat_holder for. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, steam,
        "setup for the warming-gate check below: there must be steam on "
        "the board for the gate to be worth anything");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, stone,
        "setup for the warming-gate check below: there must be cells "
        "that can hold a temperature (stone, here) for the gate to be "
        "worth anything");
    TEST_ASSERT_TRUE_MESSAGE(temperature_flag,
        "may_have_temperature must be armed by this scene, or "
        "step_one_warming_cell()'s call site is never reached at all");
    TEST_ASSERT_TRUE_MESSAGE(heat_holder_flag,
        "may_have_heat_holder must be armed by this scene too - see the "
        "thermal shock lattice's host test above for why this second "
        "flag is not redundant with the first");
}

#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* The worst case: every cell on the screen moving at once.
 *
 * Historically this budget was set from a principle (stay well under the
 * blit's ~9.6 ms bus-time ceiling) rather than the measured number, because
 * it had already been raised twice from over-tight measured budgets - see
 * git history. It also carries a documented cross-build risk: the same code
 * has measured a 3.2-3.9 ms swing purely from the ESP32-C6's 32 KB flash
 * cache aligning differently as unrelated code shifts layout.
 *
 * Tightened anyway, past even the ~10% headroom this file's other
 * budgets use, to 6000 - about 3.3% over the real measured 5802 us,
 * exactly reproducible across fresh captures on the current build (fixed
 * RNG seed - see docs/Sand/Architecture.md's "Verifying performance on
 * real hardware"). That thin a margin is a real bet against the
 * documented flash-cache swing above: if a future rebuild reintroduces
 * it and this starts flaking, that is the known, accepted trade-off of
 * tightening this far past the old principle-based number - loosen it
 * again rather than treating one flaky run as a simulation regression.
 *
 * Previously shared with the settled-pile flip test below via one
 * STEP_BUDGET_US constant; split into its own name once the two
 * scenarios needed genuinely different numbers - a checkerboard of
 * falling sand and a full-grid gravity flip are different worst cases
 * and there was never a real reason to hold them to the same ceiling.
 *
 * THE 2026-08-26 RE-BASE, which this and every other frame budget in
 * this file now follows: after the materials wave, the first full
 * capture of the current tree (performance_20260826_150930, reproduced
 * by a second capture to within 4 us on every test) re-measured all
 * thirteen scenes, and every budget was re-set to a UNIFORM REDUCTION
 * TARGET of measured * 0.9, rounded - a deliberate decision to demand
 * 10% improvement across the whole board rather than ratchet any
 * budget up to what the code happens to cost today. Every number
 * stays BELOW its own measured value, so nothing passes by decree;
 * all thirteen fail on the day of the re-base, and each stops failing
 * only when the work is done. Some numbers moved numerically upward
 * from older budgets (water, the fire pair, the every-material flip) -
 * those older budgets were reduction targets pegged to baselines that
 * predate the materials wave, and holding them frozen while the
 * simulation gained temperature, viscosity, drag, percolation and five
 * new scenes would conflate "the feature costs something" with "the
 * code regressed". The measured number beside each assertion is the
 * anchor; the target is a tenth under it.
 *
 * This one: measured 6434 us (was budget 6000, itself 3.3% over an
 * older 5802) -> target 5800. */
#define FULL_STEP_BUDGET_US 5800

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

    TEST_ASSERT_LESS_THAN_MESSAGE(FULL_STEP_BUDGET_US, (int)per_step,
        "the simulation no longer fits in its share of the frame");
}

static void test_a_screen_of_water_fits_in_the_frame_budget(void)
{
    /* Measured separately from sand, because water takes an entirely different
     * path through the step - and the one part of it that is not local, the
     * search across the flow, runs per cell. Something has to watch that. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);

    /* Half a screen of water, dropped in as an uneven slab so it is genuinely
     * flowing rather than already settled - the expensive case. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "water flowing on %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* Water gets a budget of its own, and a larger one, because it genuinely
     * does more: it moves an amount rather than a cell, and it takes a second
     * sweep across the flow - which is the only reason a tilted pool levels at
     * all. The figure is what a screen-wide collapse costs, plus room for the
     * cache to move under it.
     *
     * That case is a transient. Water at rest is 45 us, and even mid-pour only
     * the part that is moving costs anything; a whole screen collapsing at
     * once lasts a fraction of a second. Sustained, this would not be
     * acceptable - and if it ever becomes sustained, this number should be
     * argued down rather than up.
     *
     * Tightened 16000 -> 14000 once the tenth attempt's block-liquid skip
     * landed this at 13288 us, reproduced byte-identically across two
     * captures. A deliberately thin margin: nearby builds of the previous
     * mechanism measured up to 13698 us purely from flash layout, so 14000
     * is ~2% over the worst recent observation - the same knowing bet
     * FULL_STEP_BUDGET_US documents. If a rebuild that does not touch the
     * liquid pass flips this, check the liquid-free benchmarks first (they
     * are the layout controls - see Optimization-Playbook.md) and loosen
     * this rather than misread layout noise as a simulation regression.
     *
     * Re-based 2026-08-26: measured 16043 after the materials wave ->
     * target 14400 (measured * 0.9, rounded) - see FULL_STEP_BUDGET_US's
     * comment for the uniform re-base this is part of. Numerically up
     * from 14000, still well below measured: the 13288 the 14000 was
     * pegged to predates viscosity, drag and percolation existing. */
    TEST_ASSERT_LESS_THAN_MESSAGE(14400, (int)per_step,
        "a screen-wide collapse of water must still land inside a frame or "
        "two - the search across the flow is the thing to suspect");
}

#endif /* DEVICE_BUILD */

#ifdef DEVICE_BUILD
static void test_a_screen_of_settled_sand_costs_almost_nothing(void)
{
    /* The user-visible complaint this answers: adding lots of sand dropped the
     * framerate, even though most of it was just sitting there. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 5u);
    sand_enable_sleeping(&real, blocks);

    /* Every cell full, so nothing can move anywhere. */
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    sand_step(&real, 0, 1, 0);          /* one step to notice it is settled */

    const int64_t start = esp_timer_get_time();
    const int steps = 50;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "settled %dx%d grid: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    const int grains = sand_count(&real);
    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W * REAL_H, grains,
        "and nothing may have moved");
    /* Re-based 2026-08-26: measured 260 -> target 235 (measured * 0.9,
     * rounded) - see FULL_STEP_BUDGET_US's comment for the uniform
     * re-base. Down from 300, which had become pure headroom. */
    TEST_ASSERT_LESS_THAN_MESSAGE(235, (int)per_step,
        "sand that is not moving must cost almost nothing - if this fails, "
        "rows are being examined that had no reason to be");
}

static void test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget(void)
{
    /* The real worst case pouring produces, not a synthetic one: a user
     * pours a big pile, it settles and sleeps (as it does in normal use -
     * sleeping is on here, exactly like app_sand.c runs it), and then the
     * device gets tilted hard enough to reverse gravity outright. Every
     * block must wake at once - this is the scenario the whole block-grid
     * design exists to keep affordable, not the synthetic all-cells-full
     * or checkerboard-falling grids the other frame-budget tests use. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 13u);
    sand_enable_sleeping(&real, blocks);

    /* A big pour: the middle half of the screen's width, filled from the
     * floor up to half the screen's height - wide enough to span many
     * block-columns, deliberately not the whole grid. */
    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    const int grains = sand_count(&real);

    /* Let it fully settle first - every block should go to sleep, the
     * same state a real pile reaches between pours. */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Flip - straight up instead of straight down. */
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip on a %d-grain pile, %dx%d: %lld us "
                             "per step", grains, REAL_W, REAL_H,
             (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real),
        "flipping gravity must conserve grains too");

    free(big);
    free(blocks);

    /* Its own number, split off from the plain full-size-step test's
     * FULL_STEP_BUDGET_US above (see that constant's comment for why they
     * used to share one, and for the 2026-08-26 uniform re-base this
     * follows). Re-based: measured 6529 after the materials wave ->
     * target 5900 (measured * 0.9, rounded; was 6500, pegged when this
     * measured 8996 pre-wave). */
    TEST_ASSERT_LESS_THAN_MESSAGE(5900, (int)per_step,
        "reversing gravity on a settled pile must still fit in a frame or "
        "two - this is the real worst case pouring and tilting produces");
}

static void test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget(void)
{
    /* A harder worst case than the single-material flip above: three
     * materials at once, plus a fixed obstacle, so a settled pile isn't the
     * only thing that has to wake and move together. Sand fills ~30% of the
     * width on the left, water ~30% on the right, both poured from the
     * floor to half height so there's headroom to launch into once flipped
     * - same reasoning as the plain flip test. The remaining ~40% in the
     * middle holds a stone X, floor to ceiling: a fixed obstacle both
     * materials have to route around, not just fall/rise past. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    const int sand_x1  = (REAL_W * 3) / 10;          /* ~30% from the left */
    const int water_x0 = REAL_W - (REAL_W * 3) / 10; /* ~30% from the right */

    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = 0; x < sand_x1; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
        for (int x = water_x0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    /* The X: two diagonals crossing at mid-height, floor to ceiling across
     * the middle band. Two cells thick, clamped to stay inside the band -
     * a single-pixel diagonal staircase is exactly the shape that let fire
     * leak past a corner earlier this session (see
     * test_fire_is_not_smothered_with_a_gap); thickening it is the same
     * fix applied here up front instead of after finding the same leak
     * twice. */
    const int mid_w = water_x0 - sand_x1;
    for (int y = 0; y < REAL_H; y++) {
        const int off = (y * (mid_w - 1)) / (REAL_H - 1);
        const int xa = sand_x1 + off;
        const int xb = water_x0 - 1 - off;
        const int xa2 = (xa + 1 < water_x0) ? xa + 1 : xa;
        const int xb2 = (xb - 1 >= sand_x1) ? xb - 1 : xb;
        sand_set(&real, xa,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xa2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xb,  y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&real, xb2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }

    /* Let it fully settle first - same starting state a real pour-then-
     * pause reaches, stone included (it was never moving, but the pass
     * still has to notice that). */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Flip - straight up instead of straight down. */
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip on a mixed sand/water/stone-X "
                             "scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    /* No grain-conservation check here, unlike the plain flip test above -
     * deliberately, not an oversight. sand_count() counts occupied CELLS,
     * and water's fill-level model can legitimately spread its mass across
     * more or fewer cells while conserving total mass; test_a_screen_of_-
     * water_fits_in_the_frame_budget already skips this same check for the
     * same reason. Asserting it here once failed with a real, misleading
     * "Expected 13206 Was 13348" - not a simulation bug, just this
     * invariant not holding once water is in the scene - and because the
     * assertion sat before the frees below, Unity's longjmp on that
     * failure skipped them and leaked ~41 KB, starving every later
     * malloc()-based test on this device's no-PSRAM heap. Left here as the
     * reason, not just removed quietly. */

    free(big);
    free(blocks);

    /* A deliberate reduction target from the day it was written (12000
     * against a then-measured 15144), never headroom. Re-based
     * 2026-08-26: measured 12999 after the materials wave -> target
     * 11700 (measured * 0.9, rounded) - see FULL_STEP_BUDGET_US's
     * comment for the uniform re-base this is part of. */
    TEST_ASSERT_LESS_THAN_MESSAGE(11700, (int)per_step,
        "reversing gravity over a mixed sand/water/stone scene should come "
        "down to this - a target to optimize toward, not yet the reality");
}

/* Every material at once, then a gravity flip.
 *
 * The other budget tests each isolate one thing - a settled pile, a
 * screen of water, a fire cascade. This one deliberately does not: the
 * board is banded with a share of EVERY material, arranged so the
 * reactive pairs actually touch (fire against wood and gas, acid against
 * sand, lava against water), and then gravity is inverted. That makes
 * every pass in sand_step() do real work in the same step - the main
 * sweep on powders and liquids, sand_step_liquids()' cross-flow,
 * sand_step_gas()' rise and spread, and sand_step_reactions() dispatching
 * both burning and dissolving cells - which no single-material scene
 * does.
 *
 * It is the scene that catches a cost that only appears in combination:
 * a pass that is cheap alone but interacts badly with another's wake
 * pattern, or a per-cell branch that is well predicted in a uniform
 * scene and mispredicted in a mixed one.
 *
 * THE ASSERTION BELOW IS NOT A BUDGET, and must not be treated as one.
 * Every other figure in this file is a measured number with the
 * measurement written beside it; this one was written without access to
 * the device, so it is a deliberately loose SANITY CEILING - wide enough
 * that it cannot pass as tuned, tight enough to catch something
 * catastrophic like an accidental quadratic. Replace it with a real
 * figure from `run_device_tests.sh`, and say what was measured, the first
 * time anyone runs this on hardware. */
static void test_a_gravity_flip_on_every_material_at_once_stays_sane(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    /* The scene is DERIVED from materials[] and laid out by
     * all_pairs_material_at() so that every PAIR of materials touches -
     * not merely every material appearing somewhere. See that function
     * for why bands were not enough, and
     * test_the_mixed_scene_puts_every_material_pair_in_contact, which
     * checks the coverage on the host rather than leaving it a claim in
     * a comment.
     *
     * A share of the board is left empty (EMPTY_SHARE_PERCENT) so the
     * flip has somewhere to launch into - the same reasoning as the
     * other flip tests. */
    const int first = MAT_EMPTY + 1;
    const int n_mats = MAT_COUNT - first;
    const int top = (REAL_H * EMPTY_SHARE_PERCENT) / 100;

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, n_mats,
        "the pattern below needs at least two materials to interleave");

    for (int y = top; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const int m = all_pairs_material_at(x, y, first, n_mats);
            /* sand_spawn() with radius 0 rather than sand_set(): it goes
             * through random_cell(), so a liquid arrives full, a transient
             * arrives at full life and a powder gets a shade - the same
             * cells a real pour produces. */
            sand_spawn(&real, x, y, 0, (material_id_t)m);
        }
    }

    /* Let it get going - long enough for the reactions to be under way and
     * the liquids to have found their levels, so the flip lands on a live
     * scene rather than a freshly painted one. */
    for (int i = 0; i < 120; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "gravity flip with every material at once, "
                             "%dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    /* Freed BEFORE the assertion, deliberately. Unity longjmps out of a
     * failing assert, so a free() after one never runs - which on this
     * device's no-PSRAM heap leaked ~41 KB and starved every later
     * malloc()-based test. That is a real thing that happened to the
     * mixed-scene test above; the fix belongs in every test shaped like
     * this one, not just the one that got caught. */
    free(big);
    free(blocks);

    /* 54000 us: a REDUCTION TARGET, not headroom. Set 10% below a measured
     * 60091 us so this fails today and stops failing only when the code
     * gets faster. That is the same thing the mixed-scene budget above
     * does, and the reason that one went from 26.2% over to 7.0% under
     * without the number ever moving.
     *
     * THE 60091 IS STALE, AND KNOWINGLY SO. It was taken on 2026-08-25
     * against a scene of TWELVE materials covering 66 pairs. There are
     * fourteen now - glass and snow - so the same derived scene covers 91
     * pairs, and the reactions pass has gained a per-cell branch and a
     * second kind of participant (cells with a temperature) since. The
     * scene this number describes no longer exists.
     *
     * The number is deliberately NOT adjusted for that. A reduction target
     * moved to accommodate the code is no longer a target, and this file
     * has already been burned twice by figures that were reasoned about
     * rather than measured - 100000 picked with no hardware, then 300000
     * extrapolated from another scene's ratio, which came out four times
     * too pessimistic. The right correction is a fresh device capture, not
     * an estimate.
     *
     * On the extrapolation that produced 300000: fire measures ~318x host
     * and this scene ~63x, so it predicted 226-244 ms against an actual
     * 60 ms. Worth remembering before anyone extrapolates again - on this
     * chip the ratio is dominated by cache behaviour the host does not
     * model, and it is scene-specific.
     *
     * The staleness warning the paragraphs above carried is RESOLVED:
     * the fresh capture the 2026-08-26 re-base ran on (see
     * FULL_STEP_BUDGET_US's comment) measured the fourteen-material
     * scene at 74911 us, and the target followed the same uniform rule
     * as every other budget: measured * 0.9, rounded -> 67500.
     * Numerically up from the stale 54000, still a tenth below what the
     * current scene actually costs. */
    TEST_ASSERT_LESS_THAN_MESSAGE(67500, (int)per_step,
        "the mixed-material flip is held to 10% below what it measured, "
        "as a reduction target - this failing means the work has not been "
        "done yet, not that something broke");
}

static void test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget(void)
{
    /* The worst case sand_step_reactions() can face, not a synthetic one:
     * sand_reactions.c's own top comment explains that this pass's fixed
     * row-major (top-to-bottom, then left-to-right) scan order lets a
     * cascade continue into any neighbour positioned AHEAD of the scan
     * pointer within the same pass - which is both the right neighbour
     * (same row, not yet scanned) AND the neighbour directly below (a
     * row not yet reached at all). A single spark in the top-left corner
     * of a completely gas-filled grid is therefore the single most
     * expensive case there is: the cascade reaches every one of
     * REAL_W*REAL_H cells in ONE step, each paying a decay tick plus up
     * to eight neighbour lookups. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
    sand_set(&real, 0, 0, FIRE);
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    sand_step(&real, 0, 1000, 0);
    const int64_t elapsed = esp_timer_get_time() - start;

    ESP_LOGI("device_tests", "fire cascading through a full %dx%d screen of "
                             "gas: %lld us for the one step",
             REAL_W, REAL_H, (long long)elapsed);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
        "setup: cells must only ever convert material, never appear or "
        "vanish, across gas igniting into fire");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE,
        CELL_MATERIAL(sand_at(&real, REAL_W - 1, REAL_H - 1)),
        "setup: the cascade must have reached the far corner - the whole "
        "grid must have ignited in this one step, or this is not "
        "actually measuring the worst case it claims to");

    free(big);
    free(blocks);

    /* Measured on device at 321339-321342 us (~321 ms), exactly
     * reproducible across three separate captures (this simulation's
     * fixed RNG seeds mean identical runs give identical timings) -
     * nowhere near the plain-material budgets above, and deliberately not
     * held to them:
     * unlike the flip and water tests above, which model a single
     * realistic user gesture (a pour, then a tilt), an edge-to-edge
     * screen of gas is not something the current pour-brush UI can
     * practically produce - this is a deliberately synthetic worst case
     * (see this test's own top comment), not a claim that a real user
     * could trigger a stall this long. If fire+gas ever needs to
     * support a fully-packed screen at interactive rates, that is the
     * "creeping fire" design explicitly deferred in the plan this was
     * built from, not a bug in this v1.
     *
     * Re-based 2026-08-26: what had been a ~9%-over regression guard
     * became a reduction target like everything else in this file (see
     * FULL_STEP_BUDGET_US's comment). Measured 506666 after the
     * materials wave - the reactions pass gained real chemistry and the
     * gas pass is 60% of a saturated step - -> target 456000
     * (measured * 0.9, rounded; was 350000, pegged to a pre-wave
     * 321339).
     *
     * Re-pegged the same evening from the first watchdog-free capture
     * (performance_20260826_183646): the clean number is 412718 - a
     * 94000 us drop that says the 506666 was itself a contaminated row
     * the fifteenth attempt's survey missed, which this single-step
     * test is unusually exposed to (one ~500 ms window per capture
     * against a 5-second dump cadence, and deterministically so).
     * Same uniform rule, measured * 0.9 rounded -> 371500, back to a
     * deliberately failing reduction target rather than the accidental
     * pass the inflated peg produced. */
    TEST_ASSERT_LESS_THAN_MESSAGE(371500, (int)elapsed,
        "a full-screen cascade must stay in the same ballpark as measured "
        "- a jump here means something got much more expensive, not that "
        "this specific number is a real-time requirement");
}

static void test_a_full_screen_of_fire_fits_in_the_frame_budget(void)
{
    /* The steady-state cost fire's KIND_GAS redesign introduced, not
     * measured by the cascade test above: a full screen that is
     * ALREADY fire pays for BOTH sand_step_gas() (rise+disperse, now
     * that fire shares gas's own pass) AND sand_step_reactions()
     * (decay/extinguish/ignite/smother) on every cell, every step,
     * indefinitely - not just once during ignition. Traced directly:
     * the cascade test's one measured step ignites via
     * sand_step_reactions() alone (sand_step_gas() already ran earlier
     * in that same sand_step() call, before the newly-ignited cells
     * existed), so its own numbers are untouched by this and did not
     * need revisiting - this is genuinely new territory. */
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, FIRE);
        }
    }
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "full %dx%d screen already fire, steady "
                             "state: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
        "setup: a fully packed screen of same-density fire cannot "
        "displace, ignite, or smother anything - the count must not "
        "drift");

    free(big);
    free(blocks);

    /* Measured on device at 230962 us (~231 ms), identical across two
     * separate captures - same "not a real-time promise" reasoning as
     * the cascade test above: an edge-to-edge screen of fire is not
     * something the pour-brush UI can practically sustain, but this
     * catches a real regression if the steady-state cost balloons past
     * what was actually measured. Originally ~8% headroom over 230962,
     * tightened from an initial, untuned 300000 once the number proved
     * exactly reproducible rather than noisy.
     *
     * Re-based 2026-08-26 (see FULL_STEP_BUDGET_US's comment): measured
     * 295533 after the materials wave -> target 266000 (measured * 0.9,
     * rounded; was 250000). A reduction target now, like the rest of the
     * file. */
    TEST_ASSERT_LESS_THAN_MESSAGE(266000, (int)per_step,
        "steady-state cost of a full screen of fire must stay in the "
        "same ballpark as measured - not a real-time promise, but a "
        "real regression guard");
}

/* Every material at once above flips gravity on a settled scene; this one
 * never lets the scene settle in the first place. Four liquids of
 * different density are painted upside down (build_four_liquid_scene()
 * above, shared with test_the_four_liquid_scene_keeps_reacting_after_-
 * settling, which is what proves this really does keep reacting rather
 * than just claiming to) so lava, acid, water and oil spend the whole
 * measured window migrating past each other instead of settling into
 * inert bands - see that function's comment for why upside down is what
 * makes that true.
 *
 * This is also the only benchmark in this file, besides the deliberate-
 * reduction-target gravity-flip-on-every-material test above, that runs
 * with sand_set_mobility(SAND_MOBILITY_PER_MATERIAL) - the setting
 * app_sand.c itself actually calls. A liquid's mobility decides how often
 * it refuses to move at all (oil refuses about two moves in three), and
 * until this test existed nothing in this file was holding the app's own
 * liquid path to any budget - the gravity-flip test above runs at that
 * setting too, but it is graded as a reduction target, not a real ceiling.
 * Four liquids at once, at the app's own mobility, is now that benchmark.
 *
 * First device measurement, 2026-08-26: 125430 us (capture
 * performance_20260826_150930, reproduced to within a microsecond by a
 * second capture). The provisional ceiling this shipped with (150000)
 * was retired the same day - not to the ~9-10%-over guard its draft
 * comment prescribed, but to the uniform reduction target the whole
 * file moved to (see FULL_STEP_BUDGET_US's comment): measured * 0.9,
 * rounded -> 113000, failing by construction until the scene gets 10%
 * faster.
 *
 * Re-pegged the same evening from the first CLEAN capture
 * (performance_20260826_183646, taken after the diag image's task
 * watchdog was turned off): the fifteenth attempt proved the 125430
 * was contaminated - a watchdog register dump's console I/O landed
 * inside this test's timing window and was charged to the simulation.
 * Clean measurement 124336 (the dump was worth ~1100 us here) ->
 * target 112000. */
static void test_four_liquids_reacting_at_once_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_four_liquid_scene(&real);

    /* Settle first - the same "let it get going" step as the every-material
     * flip test above, so the measured window lands on a live scene. */
    for (int i = 0; i < 10; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "four liquids reacting at once, %dx%d: %lld "
                             "us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(112000, (int)per_step,
        "four liquids reacting under the app's own per-material mobility "
        "is held to 10% below its first measured number, as a reduction "
        "target - failing means the work is not done, not that something "
        "broke");
}

/* A lava reservoir, a water roof, and columns of sand, wood and oil between
 * them (build_lava_stress_scene() above, shared with
 * test_the_lava_stress_scene_reaches_every_reaction_it_claims, which
 * proves all six reactions this scene exists for really do fire in it).
 * Lava is the reaction-richest material in the simulation, and this scene
 * puts every reaction it takes part in - quenching, boiling, glassing,
 * igniting wood, igniting oil, flaring - in front of the reactions pass at
 * once, across a scene big enough to keep them going for the whole
 * measured window rather than one that burns out in the first few steps.
 *
 * First device measurement, 2026-08-26: 121377 us (capture
 * performance_20260826_150930, reproduced by a second capture to within
 * a microsecond). Provisional ceiling (150000) retired the same day to
 * the file-wide uniform reduction target (see FULL_STEP_BUDGET_US's
 * comment): measured * 0.9, rounded -> 109000. */
static void test_the_lava_stress_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_lava_stress_scene(&real);

    for (int i = 0; i < 30; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "lava stress scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(109000, (int)per_step,
        "the lava stress scene is held to 10% below its first measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}

/* A full screen of smoke and steam with one spark of fire
 * (build_smoke_and_steam_scene() above, shared with
 * test_the_smoke_and_steam_scene_stays_a_gas_screen, which proves the
 * scene conserves cells and is still a gas screen at the end of the
 * window rather than one that decayed into something else). Smoke and
 * steam are the only materials in this simulation with convection
 * behaviour - they warm what they touch - and no benchmark in this file
 * has ever put either of them on screen in quantity before this one.
 *
 * Ten measured steps, not twenty: the same reason
 * test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget
 * and test_a_full_screen_of_fire_fits_in_the_frame_budget above use ten -
 * a full grid of gas is the most expensive thing this simulation does per
 * step, and a previous round of this project tripped the device's
 * five-second task watchdog with a test that ran too many steps across a
 * full screen. No settling steps either - the scene is the worst case
 * from the moment it is painted.
 *
 * First device measurement, 2026-08-26: 141189 us (capture
 * performance_20260826_150930; the second capture reproduced it at
 * 141187, a 2 us spread). Provisional ceiling (400000) retired the same
 * day to the file-wide uniform reduction target (see
 * FULL_STEP_BUDGET_US's comment): measured * 0.9, rounded -> 127000. */
static void test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    build_smoke_and_steam_scene(&real);
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "screen of smoke and steam, %dx%d: %lld us "
                             "per step",
             REAL_W, REAL_H, (long long)per_step);

    /* Read before the frees below, asserted after - the same fix
     * test_a_gravity_flip_on_every_material_at_once_stays_sane documents:
     * Unity longjmps out of a failing assert, so an assert ahead of
     * free() would skip it and leak ~41 KB on this device's no-PSRAM
     * heap. */
    const int count = sand_count(&real);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, count,
        "setup: cells must only ever convert material, never appear or "
        "vanish, across a screen of smoke and steam");
    TEST_ASSERT_LESS_THAN_MESSAGE(127000, (int)per_step,
        "a full screen of smoke and steam is held to 10% below its first "
        "measured number, as a reduction target - failing means the work "
        "is not done, not that something broke");
}

/* 480 glass compartments (build_thermal_shock_scene() above, shared with
 * test_the_thermal_shock_scene_shatters_in_both_directions, which proves
 * both shock directions really fire across the lattice and that the
 * aftermath - meltwater, steam, escaping fire, falling cullet - is still
 * alive at the end of the window rather than one this scene claims to
 * measure but has actually gone quiet). No settling steps: the lattice is
 * already at its most active the moment it is painted, since every ring
 * starts strictly between the two shock thresholds and every trigger
 * starts touching its ring from step 1.
 *
 * First device measurement, 2026-08-26: 106650 us (capture
 * performance_20260826_150930, reproduced by a second capture).
 * Provisional ceiling (400000) retired the same day to the file-wide
 * uniform reduction target (see FULL_STEP_BUDGET_US's comment):
 * measured * 0.9, rounded -> 96000.
 *
 * Re-pegged the same evening from the first CLEAN capture
 * (performance_20260826_183646, taken after the diag image's task
 * watchdog was turned off): the fifteenth attempt proved the 106650
 * was contaminated - a watchdog register dump's console I/O landed
 * inside this test's timing window and was charged to the simulation,
 * and here the dump was worth fully ~7900 us of the old number. Clean
 * measurement 98738 -> target 89000.
 *
 * The watchdog reasoning here is stronger than it was for the three
 * scenes above: ten steps, and no settling step to spend any of the
 * window on first. Host timing, best-of-5 and interleaved with the other
 * four scenes so nothing is measured while the machine is still warming
 * up, ranks this scene the most expensive of the five - 1199 us/step
 * against 824 for the lava stress scene, 763 for four liquids reacting,
 * 647 for the smoke-and-steam screen and 218 for the boiler. That host
 * number is a RELATIVE signal only, telling us this scene costs more
 * than its siblings on the same machine - it is deliberately NOT
 * extrapolated to a device figure, for the reason given above, and the
 * absolute figures are worth little in any case: host wall-clock on this
 * project drifts up to 40% within a single harness run, which is why
 * they are taken best-of-N with the builds interleaved.
 *
 * The step count is not chosen against those numbers at all. It is fixed
 * at ten by the host guard beside this test - see that test's comment on
 * why the cullet timeline only splits into honest thirds at ten - and
 * the CEILING is then what gets chosen against the watchdog: at the
 * original provisional 400000, ten steps was four seconds against the
 * device's five-second task watchdog; at the re-based 96000 the margin
 * is wide. The two remain one decision - raise the step count without
 * minding the ceiling and the bet needs re-doing. */
static void test_the_thermal_shock_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_thermal_shock_scene(&real);

    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "thermal shock lattice, %dx%d: %lld us per "
                             "step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(89000, (int)per_step,
        "the thermal shock lattice is held to 10% below its first "
        "measured number, as a reduction target - failing means the work "
        "is not done, not that something broke");
}

/* The boiler scaled to the whole grid and run as a sustained steady state
 * (build_boiler_scene() above, shared with test_the_boiler_scene_keeps_-
 * boiling_across_the_window, which proves the basin keeps boiling for
 * the whole measured window, both burners are still contributing at the
 * end of it, and no cells go missing - the enclosed-burner property that
 * host test explains and this one relies on).
 *
 * 20 settle steps, then 30 measured - matching the host test's own
 * window exactly, so what this times is what that one already proved
 * really is a sustained boil rather than a burst that has mostly spent
 * itself by the time the measured window starts.
 *
 * First device measurement, 2026-08-26: 31529 us (capture
 * performance_20260826_150930, reproduced by a second capture).
 * Provisional ceiling (80000) retired the same day to the file-wide
 * uniform reduction target (see FULL_STEP_BUDGET_US's comment):
 * measured * 0.9, rounded -> 28500.
 *
 * The watchdog pairing the provisional ceiling was chosen against (50
 * total steps at 80000 us was four seconds against the five-second task
 * watchdog; an earlier draft's round 100000 was exactly five, which is
 * not a margin) is comfortably looser at the re-based number. Host
 * timing, best-of-5 and interleaved, ranks this scene the CHEAPEST of
 * the five at 218 us/step against 1199 for the thermal shock lattice -
 * which is why it can afford fifty steps where that one is held to
 * ten. */
static void test_the_boiler_scene_fits_in_the_frame_budget(void)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 43u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_boiler_scene(&real);

    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int64_t start = esp_timer_get_time();
    const int steps = 30;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;

    ESP_LOGI("device_tests", "boiler scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    TEST_ASSERT_LESS_THAN_MESSAGE(28500, (int)per_step,
        "the boiler scene is held to 10% below its first measured "
        "number, as a reduction target - failing means the work is not "
        "done, not that something broke");
}
#endif /* DEVICE_BUILD */

/* --- suite -------------------------------------------------------------- */

void run_sand_suite(void)
{
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

    RUN_TEST(test_load_counts_the_grains_stacked_above);
    RUN_TEST(test_load_stops_at_a_gap);
    RUN_TEST(test_open_sky_is_not_load);
    RUN_TEST(test_load_is_measured_against_gravity);
    RUN_TEST(test_a_buried_grain_will_not_slide);
    RUN_TEST(test_a_surface_grain_still_slides);
    RUN_TEST(test_friction_never_stops_a_grain_falling);
    RUN_TEST(test_shaking_overcomes_friction);
    RUN_TEST(test_a_flat_bed_does_not_slide_on_a_slight_tilt);
    RUN_TEST(test_a_steep_tilt_does_pour_the_bed);
    RUN_TEST(test_a_deep_bed_is_harder_to_move_than_a_thin_one);

    RUN_TEST(test_a_settled_grid_reports_nothing_dirty);
    RUN_TEST(test_a_falling_grain_marks_both_rows_it_touched);
    RUN_TEST(test_every_changed_row_is_reported);
    RUN_TEST(test_spawning_marks_the_rows_it_filled);
    RUN_TEST(test_tracking_starts_by_assuming_everything_changed);

    RUN_TEST(test_falling_is_exact_when_scatter_is_off);
    RUN_TEST(test_scatter_spreads_a_falling_stream);
    RUN_TEST(test_scatter_conserves_grains);
    RUN_TEST(test_a_lagging_grain_is_not_left_asleep);

    RUN_TEST(test_sleeping_leaves_nothing_able_to_move);
    RUN_TEST(test_sleeping_leaves_nothing_able_to_move_on_a_slope);
    RUN_TEST(test_sand_poured_onto_a_sleeping_pile_still_falls);
    RUN_TEST(test_undermining_a_sleeping_pile_collapses_it);
    RUN_TEST(test_turning_the_board_wakes_a_sleeping_pile);
    RUN_TEST(test_two_separate_active_spots_in_the_same_block_row_do_not_wake_each_other);
    RUN_TEST(test_a_block_wakes_when_disturbed_diagonally);
    RUN_TEST(test_sideways_tilt_wakes_only_the_disturbed_column);
    RUN_TEST(test_liquid_cross_flow_wakes_only_the_blocks_it_touches_by_range);
    RUN_TEST(test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in);
    RUN_TEST(test_water_falling_into_the_next_block_down_still_spreads);
    RUN_TEST(test_block_indices_stay_in_range_at_the_real_screens_partial_edge_blocks);
    RUN_TEST(test_block_indices_stay_in_range_after_flipping_a_settled_pile_at_the_real_size);
    RUN_TEST(test_block_indices_stay_in_range_for_a_falling_screen_of_water_at_the_real_size);

    RUN_TEST(test_a_cell_carries_both_material_and_variant);
    RUN_TEST(test_stone_never_moves);
    RUN_TEST(test_nothing_displaces_stone);
    RUN_TEST(test_sand_sinks_through_water);
    RUN_TEST(test_water_does_not_sink_through_sand);
    RUN_TEST(test_displacement_conserves_both_materials);
    RUN_TEST(test_water_finds_its_own_level);
    RUN_TEST(test_a_powder_still_holds_a_heap);
    RUN_TEST(test_water_can_be_held_by_a_stone_basin);
    RUN_TEST(test_a_drop_resting_on_a_pool_comes_to_rest);
    RUN_TEST(test_water_under_weight_still_spreads);
    RUN_TEST(test_water_poured_into_a_basin_reaches_both_ends);
    RUN_TEST(test_a_tipped_basin_pours_its_water_out);
    RUN_TEST(test_a_tipped_basin_keeps_its_sand);
    RUN_TEST(test_water_puddles_where_sand_heaps);
    RUN_TEST(test_a_large_body_of_water_levels);
    RUN_TEST(test_a_settled_pool_does_not_flicker);
    RUN_TEST(test_a_pool_settles_at_the_angle_it_is_tilted_to);
    RUN_TEST(test_a_hard_flick_kicks_water_off_the_wall_it_just_hit);
    RUN_TEST(test_a_reversal_without_a_flick_signal_still_does_not_rebound);

    RUN_TEST(test_gas_rises_straight_up_under_ordinary_gravity);
    RUN_TEST(test_gas_falls_when_the_board_is_inverted);
    RUN_TEST(test_gas_rises_diagonally_under_tilted_gravity);
    RUN_TEST(test_gas_is_blocked_by_a_stone_ceiling);
    RUN_TEST(test_gas_disperses_across_a_ceiling);
    RUN_TEST(test_sand_sinks_through_gas);
    RUN_TEST(test_water_sinks_through_gas);
    RUN_TEST(test_gas_grain_count_is_conserved);
    RUN_TEST(test_rising_gas_wakes_the_blocks_it_passes_through);
    RUN_TEST(test_gas_scatter_can_be_disabled);
    RUN_TEST(test_gas_decays_and_disappears_over_time);
    RUN_TEST(test_gas_decaying_away_marks_its_row_dirty);

    RUN_TEST(test_fire_ignites_an_adjacent_flammable_neighbour);
    RUN_TEST(test_extinguishing_wins_over_igniting);
    RUN_TEST(test_fire_burns_out_and_disappears_over_time);
    RUN_TEST(test_fire_rises_and_disperses_like_gas);
    RUN_TEST(test_sand_sinks_through_fire);
    RUN_TEST(test_fire_is_smothered_when_fully_buried);
    RUN_TEST(test_fire_is_not_smothered_with_a_gap);
    RUN_TEST(test_fire_is_not_smothered_by_gas);
    RUN_TEST(test_liquid_wins_over_smothering);
    RUN_TEST(test_igniting_a_neighbour_marks_its_row_dirty);
    RUN_TEST(test_fire_burning_out_marks_its_row_dirty);
    RUN_TEST(test_fire_spreads_through_a_connected_pocket_in_one_step);
    RUN_TEST(test_pouring_stone_never_arms_the_reactions_pass);
    RUN_TEST(test_placing_fire_arms_both_gas_and_fire_passes);

    RUN_TEST(test_wood_does_not_catch_instantly);
    RUN_TEST(test_wood_eventually_catches_and_becomes_an_ember);
    RUN_TEST(test_an_ember_does_not_rise);
    RUN_TEST(test_an_ember_burns_out_over_time);
    RUN_TEST(test_an_ember_flares_fire_into_an_empty_neighbour);
    RUN_TEST(test_quenching_costs_the_water_a_unit_of_mass);
    RUN_TEST(test_steam_rises_and_disperses);
    RUN_TEST(test_creating_steam_arms_the_gas_pass);
    RUN_TEST(test_burnt_out_fire_can_leave_smoke);
    RUN_TEST(test_only_the_exposed_surface_of_an_oil_pool_can_ignite);
    RUN_TEST(test_oil_ignites_with_a_flame_sitting_directly_on_it);
    RUN_TEST(test_oil_does_not_put_fire_out);
    RUN_TEST(test_water_still_puts_fire_out);
    RUN_TEST(test_oil_trapped_under_water_floats_to_the_surface);
    RUN_TEST(test_acid_dissolves_sand);
    RUN_TEST(test_acid_does_not_dissolve_its_container);
    RUN_TEST(test_acid_eats_through_stone);
    RUN_TEST(test_sand_turns_to_glass_under_sustained_heat);
    RUN_TEST(test_glass_conducts_heat_like_stone);
    RUN_TEST(test_glass_banks_heat_rather_than_melting_on_contact);
    RUN_TEST(test_a_fire_held_long_enough_melts_glass_to_lava);
    RUN_TEST(test_glass_forgets_a_fire_that_went_out);
    RUN_TEST(test_glass_cools_on_a_board_with_no_fire_at_all);
    RUN_TEST(test_freshly_fused_glass_starts_cold);
    RUN_TEST(test_snow_shatters_a_glowing_pane_into_sand);
    RUN_TEST(test_one_shock_cracks_the_whole_pane);
    RUN_TEST(test_a_crack_does_not_jump_to_a_separate_pane);
    RUN_TEST(test_cold_glass_is_unharmed_by_snow);
    RUN_TEST(test_snow_frosts_a_resting_pane);
    RUN_TEST(test_lava_one_side_snow_the_other_cracks_the_wall);
    RUN_TEST(test_a_frosted_pane_warms_back_to_room_temperature);
    RUN_TEST(test_heat_arriving_at_frosted_glass_cracks_it);
    RUN_TEST(test_heat_arriving_at_resting_glass_only_warms_it);
    RUN_TEST(test_frost_spreads_beyond_the_snow_touching_it);
    RUN_TEST(test_snow_keeps_on_ordinary_cold_glass);
    RUN_TEST(test_the_shock_threshold_is_exact);
    RUN_TEST(test_glass_looks_different_at_the_shock_threshold);
    RUN_TEST(test_snow_melts_where_it_chills);
    RUN_TEST(test_snow_floats_on_water);
    RUN_TEST(test_heat_through_a_pan_lights_oil_rather_than_boiling_it);
    RUN_TEST(test_a_powder_lands_on_a_powder_but_sinks_in_a_liquid);
    RUN_TEST(test_hot_gas_warms_what_it_touches);
    RUN_TEST(test_hot_gas_does_not_set_fire_to_anything);
    RUN_TEST(test_wet_sand_becomes_dirt_and_spends_the_water);
    RUN_TEST(test_dirt_takes_on_moisture_and_dries_out_again);
    RUN_TEST(test_new_dirt_starts_dry_in_a_random_tone);
    RUN_TEST(test_soil_keeps_its_tone_through_wetting_and_drying);
    RUN_TEST(test_the_two_soil_tones_are_different_colours);
    RUN_TEST(test_soaking_is_off_unless_asked_for);
    RUN_TEST(test_only_water_wets_what_it_touches);
    RUN_TEST(test_the_grain_hash_does_not_stripe);
    RUN_TEST(test_the_air_agrees_about_weight_speed_and_lifetime);
    RUN_TEST(test_the_right_extended_materials_are_speckled);
    RUN_TEST(test_a_tilt_between_two_directions_is_dithered_not_snapped);
    RUN_TEST(test_water_percolates_to_the_bottom_of_a_submerged_pile);
    RUN_TEST(test_water_percolates_diagonally_as_well_as_straight_down);
    RUN_TEST(test_a_plant_drains_standing_water_into_the_soil);
    RUN_TEST(test_a_plant_rooted_on_stone_does_not_drink);
    RUN_TEST(test_a_limb_travels_outward_instead_of_climbing);
    RUN_TEST(test_a_crowned_trunk_buds_and_a_bare_one_does_not);
    RUN_TEST(test_a_hardened_trunk_is_left_with_foliage);
    RUN_TEST(test_a_hardened_trunk_is_thicker_at_the_foot);
    RUN_TEST(test_a_finished_tree_carries_no_green);
    RUN_TEST(test_a_leaf_neither_spreads_nor_falls);
    RUN_TEST(test_a_leaf_with_no_tree_withers_away);
    RUN_TEST(test_a_dying_leaf_yellows_before_it_goes);
    RUN_TEST(test_a_leaf_drains_standing_water_into_the_soil);
    RUN_TEST(test_a_seed_falls_until_it_lands);
    RUN_TEST(test_two_falling_seeds_do_not_hold_each_other_up);
    RUN_TEST(test_a_brushful_of_seeds_does_not_hang_in_the_air);
    RUN_TEST(test_a_seed_in_a_shaft_does_not_stick_to_the_walls);
    RUN_TEST(test_a_settled_plant_keeps_the_reaction_pass_armed);
    RUN_TEST(test_a_growing_tree_does_not_shed_what_it_grows);
    RUN_TEST(test_a_limb_hangs_on_to_a_wooden_trunk);
    RUN_TEST(test_loose_greenery_withers_but_a_tree_keeps_its_leaves);
    RUN_TEST(test_a_tree_grows_wider_than_one_column);
    RUN_TEST(test_a_buried_seed_comes_up_through_the_soil);
    RUN_TEST(test_a_seed_under_stone_stays_put);
    RUN_TEST(test_a_stem_that_wanders_still_hardens);
    RUN_TEST(test_a_bare_trunk_in_wet_ground_buds_again);
    RUN_TEST(test_a_shattered_pane_comes_back_as_cullet);
    RUN_TEST(test_painted_sand_stays_out_of_the_cullet_band);
    RUN_TEST(test_cullet_does_not_look_like_sand);
    RUN_TEST(test_a_wetting_front_spreads_past_the_cells_it_touched);
    RUN_TEST(test_soil_a_wetting_front_converts_is_handed_a_real_share);
    RUN_TEST(test_moisture_is_conserved_as_it_spreads);
    RUN_TEST(test_a_plant_on_wet_soil_grows_upward);
    RUN_TEST(test_a_plant_on_dry_soil_stays_where_it_is);
    RUN_TEST(test_a_tall_plant_hardens_into_wood_that_is_not_alight);
    RUN_TEST(test_every_material_has_a_palette_block);
    RUN_TEST(test_ice_is_its_own_colour);
    RUN_TEST(test_an_extended_material_survives_being_painted);
    RUN_TEST(test_every_extended_material_shares_one_physics_row);
    RUN_TEST(test_extended_materials_get_their_own_reactions);
    RUN_TEST(test_ice_cracks_hot_glass_and_stays_where_it_is_put);
    RUN_TEST(test_stone_heats_up_next_to_lava);
    RUN_TEST(test_stone_never_melts_however_hot);
    RUN_TEST(test_snow_cracks_glass_but_not_stone);
    RUN_TEST(test_an_edge_shows_less_temperature_than_the_body);
    RUN_TEST(test_each_material_is_painted_the_way_it_should_be);
    RUN_TEST(test_glass_grain_is_quieter_than_stone);
    RUN_TEST(test_the_shine_does_not_vary_between_cells);
    RUN_TEST(test_stone_speckles_by_position_at_every_temperature);
    RUN_TEST(test_lava_buried_in_stone_is_not_deleted);
    RUN_TEST(test_lava_is_not_boiled_by_its_own_conducted_heat);
    RUN_TEST(test_the_mixed_scene_puts_every_material_pair_in_contact);
    RUN_TEST(test_the_four_liquid_scene_keeps_reacting_after_settling);
    RUN_TEST(test_the_lava_stress_scene_reaches_every_reaction_it_claims);
    RUN_TEST(test_the_smoke_and_steam_scene_stays_a_gas_screen);
    RUN_TEST(test_the_thermal_shock_scene_shatters_in_both_directions);
    RUN_TEST(test_the_boiler_scene_keeps_boiling_across_the_window);
    RUN_TEST(test_reinitialising_forgets_the_old_board);
    RUN_TEST(test_the_brush_and_the_setter_agree_about_every_material);
    RUN_TEST(test_snow_painted_into_water_melts);
    RUN_TEST(test_snow_melts_in_any_liquid);
    RUN_TEST(test_melting_snow_makes_water_not_more_of_the_liquid);
    RUN_TEST(test_snow_keeps_on_dry_ground);
    RUN_TEST(test_acid_spends_a_unit_of_itself_per_cell_dissolved);
    RUN_TEST(test_acid_fizzes_while_it_eats);
    RUN_TEST(test_the_fizz_rises_out_of_the_acid);
    RUN_TEST(test_a_little_acid_cannot_eat_an_unlimited_amount);
    RUN_TEST(test_every_liquid_declares_a_mobility);
    RUN_TEST(test_water_does_not_drill_into_oil_when_tilted);
    RUN_TEST(test_oil_flows_more_slowly_than_water);
    RUN_TEST(test_lava_does_not_decay_away);
    RUN_TEST(test_water_freezes_lava_into_stone);
    RUN_TEST(test_lava_quenched_into_stone_mid_pass_arms_the_heat_holder_flag);
    RUN_TEST(test_lava_does_not_put_fire_out);
    RUN_TEST(test_steam_bubbles_up_through_standing_water);
    RUN_TEST(test_bubbling_conserves_the_water_it_displaces);
    RUN_TEST(test_plain_gas_bubbles_up_through_water_too);
    RUN_TEST(test_a_bubble_does_not_push_through_a_solid);
    RUN_TEST(test_quenching_makes_steam_but_burning_out_makes_smoke);
    RUN_TEST(test_steam_and_smoke_are_told_apart_by_brightness);
    RUN_TEST(test_stone_conducts_heat_into_water_beyond_it);
    RUN_TEST(test_stone_does_not_conduct_fire_into_empty_space);
    RUN_TEST(test_a_thick_wall_still_conducts);
    RUN_TEST(test_conduction_stops_at_the_reach_cap);
    RUN_TEST(test_a_thick_wall_conducts_more_slowly_than_a_thin_one);
    RUN_TEST(test_boiling_converts_the_cell_nearest_the_heat);
    RUN_TEST(test_the_boiler_end_to_end);
    RUN_TEST(test_wood_and_steam_grain_count_is_conserved);

    RUN_TEST(test_every_cell_change_marks_its_row_dirty);
    RUN_TEST(test_grains_are_never_created_or_destroyed);
    RUN_TEST(test_a_grain_keeps_its_shade_as_it_falls);

    RUN_TEST(test_grains_fall_upward_when_the_board_is_inverted);
    RUN_TEST(test_grains_fall_sideways_when_the_board_is_on_its_edge);
    RUN_TEST(test_a_heap_settles_against_whichever_wall_is_down);

    RUN_TEST(test_spawn_fills_a_disc);
    RUN_TEST(test_spawn_is_clipped_to_the_grid);
    RUN_TEST(test_spawning_onto_existing_grains_does_not_double_count);
    RUN_TEST(test_spawned_grains_use_the_full_range_of_shades);

    RUN_TEST(test_erase_removes_a_disc);
    RUN_TEST(test_erasing_empty_space_removes_nothing);
    RUN_TEST(test_erase_is_clipped_to_the_grid);
    RUN_TEST(test_erase_marks_the_rows_it_emptied);

    RUN_TEST(test_nothing_moves_in_free_fall);
    RUN_TEST(test_shaking_spreads_a_pile_sideways);

#ifdef DEVICE_BUILD
    RUN_TEST(test_a_full_size_step_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_settled_sand_costs_almost_nothing);
    RUN_TEST(test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget);
    RUN_TEST(test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_water_fits_in_the_frame_budget);
    RUN_TEST(test_a_gravity_flip_on_every_material_at_once_stays_sane);
    RUN_TEST(test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_a_full_screen_of_fire_fits_in_the_frame_budget);
    RUN_TEST(test_four_liquids_reacting_at_once_fits_in_the_frame_budget);
    RUN_TEST(test_the_lava_stress_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget);
    RUN_TEST(test_the_thermal_shock_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_boiler_scene_fits_in_the_frame_budget);
#endif
}

SUITE_REGISTER(run_sand_suite);
