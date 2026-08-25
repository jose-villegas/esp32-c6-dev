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

    for (int i = 0; i < 30; i++) {
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
    sand_set(&loc, gx,     gy + 1, CELL_MAKE(MAT_STONE, 8));  /* blocks the fall */
    sand_set(&loc, gx + 1, gy + 1, CELL_MAKE(MAT_STONE, 8));  /* blocks down-right */
    sand_set(&loc, gx - 1, gy + 1, CELL_MAKE(MAT_STONE, 8));  /* blocks down-left */
    /* Once the down-left slide is freed and the grain lands there, it
     * must stop - otherwise it keeps sliding on its own three legal moves
     * from its new position, and the test would be checking the wrong
     * cell. */
    sand_set(&loc, gx - 1, gy + 2, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, gx - 2, gy + 2, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, gx,     gy + 2, CELL_MAKE(MAT_STONE, 8));

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
    sand_set(&loc, 3, 10, CELL_MAKE(MAT_STONE, 8));    /* blocks the fall */
    sand_set(&loc, 3, 11, CELL_MAKE(MAT_STONE, 8));    /* blocks down-right slide */
    sand_set(&loc, 3, 9,  CELL_MAKE(MAT_STONE, 8));    /* blocks up-right slide */
    /* Once (3,10) is freed and the grain moves there, it must stop, or it
     * keeps sliding diagonally from its new position and the test would
     * be checking the wrong cell. */
    sand_set(&loc, 4, 10, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, 4, 11, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, 4, 9,  CELL_MAKE(MAT_STONE, 8));

    sand_set(&loc, 18, 10, SAND_FIRST_SHADE);
    sand_set(&loc, 19, 10, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, 19, 11, CELL_MAKE(MAT_STONE, 8));
    sand_set(&loc, 19, 9,  CELL_MAKE(MAT_STONE, 8));

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
        sand_set(&pool, 0, y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&pool, POOL_W - 1, y, CELL_MAKE(MAT_STONE, 8));
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
    for (int i = 0; i < 40; i++) {
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
        sand_set(&g, x, SAND_BLOCK_H + 1, CELL_MAKE(MAT_STONE, 8));
    }
    /* One full cell of water, in the last row of the UPPER block. */
    sand_set(&g, 5, SAND_BLOCK_H - 1, CELL_MAKE(MAT_WATER, MASS_MAX));

    for (int i = 0; i < 40; i++) {
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
#define STONE CELL_MAKE(MAT_STONE, 8)
#define SAND  CELL_MAKE(MAT_SAND,  8)
#define GAS   CELL_MAKE(MAT_GAS,   8)
#define FIRE  CELL_MAKE(MAT_FIRE,  8)
#define WOOD  CELL_MAKE(MAT_WOOD,  8)
#define STEAM CELL_MAKE(MAT_STEAM, 8)
#define EMBER CELL_MAKE(MAT_EMBER, 8)

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
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, 8));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, 8));
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

    /* Nearly all of it, rather than every last unit. A few cells of film can
     * cling in the corners, and demanding a perfectly dry basin would be
     * pinning an exact outcome rather than testing that it pours. */
    const long left = mass_in_basin();
    TEST_ASSERT_LESS_THAN_MESSAGE((int)(held / 10), (int)left,
        "held on its side, a basin of water must almost entirely empty - "
        "water has no angle of repose, so a lump left behind is it behaving "
        "like a powder");
}

static void test_a_tipped_basin_keeps_its_sand(void)
{
    /* The control, and the reason the last test means anything: sand tipped
     * the same way must NOT all run out. If both emptied, the test above would
     * be measuring gravity rather than the difference between a liquid and a
     * powder. */
    sand_init(&pour, pour_cells, POUR_W, POUR_H, 9u);
    for (int y = POUR_H - 6; y < POUR_H; y++) {
        sand_set(&pour, 5,  y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&pour, 12, y, CELL_MAKE(MAT_STONE, 8));
    }
    for (int x = 5; x < 13; x++) {
        sand_set(&pour, x, POUR_H - 1, CELL_MAKE(MAT_STONE, 8));
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

    for (int i = 0; i < 30; i++) {
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
    for (int i = 0; i < 30; i++) {
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

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_EMBER, CELL_MATERIAL(sand_at(&s, 4, 3)),
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

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_EMBER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "an ember is KIND_STATIC, unlike fire - it must stay exactly "
        "where it was placed rather than rising the way fire (KIND_GAS) "
        "does");
}

static void test_an_ember_burns_out_over_time(void)
{
    fixture();
    sand_set_decay(&s, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 3, 0, MAT_EMBER),
        "setup: exactly one ember cell placed");

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
     * MAT_EMBER's decay figure and test_an_ember_burns_out_over_time
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
    sand_set(&s, wall + 1, face, SNOW);

    int cracked = 0;
    for (int k = 0; k < 30 && !cracked; k++) {
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

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_EMBER, CELL_MATERIAL(sand_at(&wide, x, wood_y)),
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
 * and there was never a real reason to hold them to the same ceiling. */
#define FULL_STEP_BUDGET_US 6000

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
     * this rather than misread layout noise as a simulation regression. */
    TEST_ASSERT_LESS_THAN_MESSAGE(14000, (int)per_step,
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
    TEST_ASSERT_LESS_THAN_MESSAGE(300, (int)per_step,
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

    /* 6500us: its own number now, split off from the plain full-size-step
     * test's FULL_STEP_BUDGET_US above (see that constant's comment for
     * why they used to share one). Measured ~8996 us, well over this -
     * same deliberate-reduction-target framing as the mixed-scene and
     * water tests: a real regression would be this number jumping
     * further, not the fact that it currently fails at all. */
    TEST_ASSERT_LESS_THAN_MESSAGE(6500, (int)per_step,
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
        sand_set(&real, xa,  y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&real, xa2, y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&real, xb,  y, CELL_MAKE(MAT_STONE, 8));
        sand_set(&real, xb2, y, CELL_MAKE(MAT_STONE, 8));
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

    /* 12000us: NOT headroom over the real measured 15144us - a deliberate
     * reduction target below it (about 21% down), same as this file's
     * other two currently-failing budgets (the plain settled-pile flip and
     * the water test just above). Those exist to keep pressure on real
     * optimization work rather than quietly ratchet up to whatever the
     * code happens to cost today; this one joins them on purpose, not as a
     * mistake - SELFTEST_COMPLETE's accepted baseline moves from
     * failures=2 to failures=3 the moment this lands (see
     * docs/Sand/Architecture.md's device-tests table, which needs the same
     * update). */
    TEST_ASSERT_LESS_THAN_MESSAGE(12000, (int)per_step,
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
     * So read a failure here carefully: it may be the regression it always
     * meant, or it may be two extra materials in a harder scene. Re-measure
     * before concluding either.
     *
     * On the extrapolation that produced 300000: fire measures ~318x host
     * and this scene ~63x, so it predicted 226-244 ms against an actual
     * 60 ms. Worth remembering before anyone extrapolates again - on this
     * chip the ratio is dominated by cache behaviour the host does not
     * model, and it is scene-specific. */
    TEST_ASSERT_LESS_THAN_MESSAGE(54000, (int)per_step,
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
     * built from, not a bug in this v1. The budget below is ~9% over
     * the measured number - tight enough to catch a real regression,
     * loose enough to absorb ordinary flash-layout noise (the
     * ~2-5% this project has already characterised elsewhere), not a
     * frame-rate promise. */
    TEST_ASSERT_LESS_THAN_MESSAGE(350000, (int)elapsed,
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
     * what was actually measured. ~8% headroom, tightened from an
     * initial, untuned 300000 once the number proved exactly
     * reproducible rather than noisy. */
    TEST_ASSERT_LESS_THAN_MESSAGE(250000, (int)per_step,
        "steady-state cost of a full screen of fire must stay in the "
        "same ballpark as measured - not a real-time promise, but a "
        "real regression guard");
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
    RUN_TEST(test_the_mixed_scene_puts_every_material_pair_in_contact);
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
#endif
}

SUITE_REGISTER(run_sand_suite);
