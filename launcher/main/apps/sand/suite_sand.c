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

    sand_spawn(&s, 4, 4, 1);

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

static uint8_t sleep_rows[H];

static void settle_with_sleeping(const char *rows[], int count, int steps,
                                 int gx, int gy)
{
    fixture();
    sand_enable_sleeping(&s, sleep_rows);
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
        sand_enable_sleeping(&s, sleep_rows);
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

/* The worst case: every grain on the screen moving at once.
 *
 * The budget was 4 ms when a frame meant an unconditional 17.6 ms blit and the
 * simulation had to squeeze in beside it. Partial updates changed that - a
 * frame now usually sends one or two bands and costs 1-3 ms - so the honest
 * worst-case frame is step + full redraw + full blit, about 15 ms, or 64 fps
 * while the entire screen is in motion. 6 ms of that is a fair share.
 *
 * Measured between 3.2 and 3.9 ms across builds that did not touch this path,
 * which is the ESP32-C6's 32 KB flash cache moving under code-layout changes
 * rather than anything in the algorithm. The margin here has to accommodate
 * that, or the test becomes a coin toss. */
#define STEP_BUDGET_US 6000

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

#ifdef DEVICE_BUILD
static void test_a_screen_of_settled_sand_costs_almost_nothing(void)
{
    /* The user-visible complaint this answers: adding lots of sand dropped the
     * framerate, even though most of it was just sitting there. */
    uint8_t *big  = malloc(REAL_W * REAL_H);
    uint8_t *rows = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(rows);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 5u);
    sand_enable_sleeping(&real, rows);

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
    free(rows);

    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W * REAL_H, grains,
        "and nothing may have moved");
    TEST_ASSERT_LESS_THAN_MESSAGE(300, (int)per_step,
        "sand that is not moving must cost almost nothing - if this fails, "
        "rows are being examined that had no reason to be");
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
#endif
}

SUITE_REGISTER(run_sand_suite);
