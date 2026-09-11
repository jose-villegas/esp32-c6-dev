/*
 * Portable suite: the falling-sand automaton - 2D block locality and scatter.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 */
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

/* --- 2D block locality -----------------------------------------------------
 *
 * A row-shaped settled bit cannot fix a pour keeping a long-settled row
 * awake, even in principle: wake propagation only ever reached vertically.
 * These exercise 2D locality whichever way gravity points, on a 3x3-block
 * grid, which is the smallest that gives "far apart" a meaning. */
#define LOC_W_CAP (((SAND_BLOCK_W + 2) > 128) ? (SAND_BLOCK_W + 2) : 128)
/* Capped, and malloc'd per test rather than `static`: this file also
 * compiles into the device build, where a `static` array is permanent BSS
 * for the whole boot. A block-size tuning experiment (SAND_BLOCK_H=64) once
 * grew a `static loc_cells` from 2304 to 9216 bytes, and the already-tight
 * device heap (framebuffer alone claims 322 of ~424 KiB) could not spare it
 * for the rest of that boot. */
#define LOC_H_CAP (((SAND_BLOCK_H + 2) > 128) ? (SAND_BLOCK_H + 2) : 128)
/* The cap can't be a flat 128 independent of block size:
 * test_a_block_wakes_when_disturbed_diagonally() needs LOC_H >=
 * SAND_BLOCK_H + 2 to exist on the grid at all, and sand_set() on an
 * out-of-range cell is a silent no-op, so a flat cap below that would fail
 * the test on unrelated grounds once SAND_BLOCK_H passed ~42. */
#define LOC_W (((SAND_BLOCK_W * 3) < LOC_W_CAP) ? (SAND_BLOCK_W * 3) : LOC_W_CAP)
#define LOC_H (((SAND_BLOCK_H * 3) < LOC_H_CAP) ? (SAND_BLOCK_H * 3) : LOC_H_CAP)
#define LOC_BLOCK_COLS ((LOC_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define LOC_BLOCK_ROWS ((LOC_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
static uint8_t *loc_cells;
static uint8_t *loc_sleep_blocks;

/* Mallocs the three buffers above, fresh, every call - loc_free() below
 * must be called before the test returns, or the next call here leaks the
 * previous allocation. */
static void loc_fixture(void)
{
    loc_cells        = malloc((size_t)LOC_W * LOC_H);
    loc_sleep_blocks = malloc((size_t)LOC_BLOCK_COLS * LOC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(loc_cells);
    TEST_ASSERT_NOT_NULL(loc_sleep_blocks);

    sand_init(&fx.loc, loc_cells, LOC_W, LOC_H, 555u);
    sand_enable_sleeping(&fx.loc, loc_sleep_blocks);
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
        sand_set(&fx.loc, x, LOC_H - 1, SAND_FIRST_SHADE);
    }
    for (int i = 0; i < 100; i++) {
        sand_step(&fx.loc, 0, 1000, 0);
    }

    /* Heap, not a stack array: at the shipped SAND_BLOCK_W (16) this is a
     * harmless 2 KB, but it scales with the tunable (see loc_fixture()'s
     * own comment on SAND_BLOCK_W being worth retuning) and the device's
     * main task stack is only 3.5 KB total (CONFIG_ESP_MAIN_TASK_STACK_
     * SIZE) - a wider block size alone is enough to blow it, with a real
     * stack-protection panic on device that a host run cannot reproduce
     * (the host stack is megabytes). */
    uint8_t *left_before = malloc((size_t)SAND_BLOCK_W * LOC_H);
    TEST_ASSERT_NOT_NULL(left_before);
    for (int y = 0; y < LOC_H; y++) {
        for (int x = 0; x < SAND_BLOCK_W; x++) {
            left_before[y * SAND_BLOCK_W + x] = sand_at(&fx.loc, x, y);
        }
    }

    /* A stream, falling in the rightmost block-column - two block-columns
     * away, but landing in the SAME row the heap rests in. A row-shaped
     * scheme could not tell these apart: the whole row would be forced
     * awake by the stream, heap included. LOC_H steps is enough for a
     * fresh grain to fall the full height of the grid, whatever
     * SAND_BLOCK_H currently is. */
    for (int i = 0; i < LOC_H; i++) {
        sand_set(&fx.loc, LOC_W - 1, 0, SAND_FIRST_SHADE);
        sand_step(&fx.loc, 0, 1000, 0);
    }

    bool left_unchanged = true;
    for (int y = 0; y < LOC_H && left_unchanged; y++) {
        for (int x = 0; x < SAND_BLOCK_W; x++) {
            if (sand_at(&fx.loc, x, y) != left_before[y * SAND_BLOCK_W + x]) {
                left_unchanged = false;
                break;
            }
        }
    }
    const cell_t stream_landed = sand_at(&fx.loc, LOC_W - 1, LOC_H - 1);

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
    sand_set(&fx.loc, gx, gy, SAND_FIRST_SHADE);
    sand_set(&fx.loc, gx,     gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks the fall */
    sand_set(&fx.loc, gx + 1, gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks down-right */
    sand_set(&fx.loc, gx - 1, gy + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));  /* blocks down-left */
    /* Once the down-left slide is freed and the grain lands there, it
     * must stop - otherwise it keeps sliding on its own three legal moves
     * from its new position, and the test would be checking the wrong
     * cell. */
    sand_set(&fx.loc, gx - 1, gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, gx - 2, gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, gx,     gy + 2, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    for (int i = 0; i < 100; i++) {
        sand_step(&fx.loc, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_FIRST_SHADE, sand_at(&fx.loc, gx, gy),
        "the grain must still be boxed in and asleep before the test begins,"
        " or freeing the down-left slide below proves nothing");

    /* Free the down-left slide - differs from the grain's own block in
     * BOTH x and y, a true diagonal neighbour, not the orthogonal case an
     * easy bug could special-case by mistake. */
    sand_erase(&fx.loc, gx - 1, gy + 1, 0);

    for (int i = 0; i < 20; i++) {
        sand_step(&fx.loc, 0, 1000, 0);
    }

    const cell_t old_cell = sand_at(&fx.loc, gx, gy);
    const cell_t new_cell = sand_at(&fx.loc, gx - 1, gy + 1);

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
    sand_set(&fx.loc, 2, 10, SAND_FIRST_SHADE);
    sand_set(&fx.loc, 3, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks the fall */
    sand_set(&fx.loc, 3, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks down-right slide */
    sand_set(&fx.loc, 3, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));    /* blocks up-right slide */
    /* Once (3,10) is freed and the grain moves there, it must stop, or it
     * keeps sliding diagonally from its new position and the test would
     * be checking the wrong cell. */
    sand_set(&fx.loc, 4, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, 4, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, 4, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    sand_set(&fx.loc, 18, 10, SAND_FIRST_SHADE);
    sand_set(&fx.loc, 19, 10, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, 19, 11, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_set(&fx.loc, 19, 9,  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    for (int i = 0; i < 100; i++) {
        sand_step(&fx.loc, 1000, 0, 0);
    }

    uint8_t right_before[8 * 4];   /* a small window around the right grain */
    for (int y = 8; y < 12; y++) {
        for (int x = 16; x < 24; x++) {
            right_before[(y - 8) * 8 + (x - 16)] = sand_at(&fx.loc, x, y);
        }
    }

    /* Free only the left grain's fall. */
    sand_erase(&fx.loc, 3, 10, 0);
    for (int i = 0; i < 20; i++) {
        sand_step(&fx.loc, 1000, 0, 0);
    }

    const cell_t left_moved = sand_at(&fx.loc, 3, 10);

    bool right_unchanged = true;
    for (int y = 8; y < 12 && right_unchanged; y++) {
        for (int x = 16; x < 24; x++) {
            if (sand_at(&fx.loc, x, y) != right_before[(y - 8) * 8 + (x - 16)]) {
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

/* A basin wide enough to span several block-columns, so the pour and far
 * wall are genuinely in different blocks - the regression guard for
 * equalise_one_row()'s touched-x range accumulation. Both the pool and
 * its water source scale with SAND_BLOCK_W: a fixed-width source can't
 * supply enough water mass to cross a wider floor within
 * SAND_LIQUID_SIGHT's per-step reach. The source widens with the pool
 * and fills its vertical room; POOL_H grows with POOL_W too, keeping
 * proportions constant. */
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
 * relocates liquid: move_to() is a SWAP, so sand entering water sends that
 * water UP into a row that had none a moment earlier. Written against
 * ROW_NO_LIQUID, a per-row dry cache since deleted for costing more than it
 * saved - kept anyway, since any future row-skip optimization needs exactly
 * this scenario checked against it. */
static void test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    sand_clear(&s);

    /* The setup is built so ONLY cross-flow can spread the displaced water:
     * the pool is full (MASS_MAX), so once pushed up it has nowhere below or
     * down either slope for move_liquid_grain() to use. Dropped from three
     * rows up rather than placed on the surface, since an external
     * sand_set() would itself announce the landing row and mask the case
     * under test. */
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


/* BLOCK_LIQUID_NEAR (sand_priv.h) is expanded to a block's 8 neighbours
 * because liquid can ARRIVE in a block the sweep already walked and found
 * dry. Two block-ROWS and a single cell of water are both the point: the
 * sweep walks the lower row first, and one cell empties its source block,
 * so nothing else covers the destination. Un-expanded, the pass finds no
 * liquid and switches itself off with the water still on screen.
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

/* The cross-flow block skip proves a block cannot flow and does not walk
 * it. Rays reach one cell past the block each side, so the span checked
 * is wider than the block - [lo, hi) alone reads a full block as settled
 * while the cell just outside is where the water was about to go. A
 * sealed one-cell channel is the fixture: the main sweep's diagonal-down
 * slides would spread an open-topped column sideways regardless of
 * cross-flow, but stone above/below leaves no diagonal, so only this
 * pass can move it. */
#define XSPAN_W (SAND_BLOCK_W * 2)
#define XSPAN_H 80
#define XSPAN_COLS ((XSPAN_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define XSPAN_ROWS ((XSPAN_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

static void test_water_crosses_a_block_boundary_sideways(void)
{
    uint8_t *cells  = malloc((size_t)XSPAN_W * XSPAN_H);
    uint8_t *blocks = malloc((size_t)XSPAN_COLS * XSPAN_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t g;
    sand_init(&g, cells, XSPAN_W, XSPAN_H, 5u);
    sand_enable_sleeping(&g, blocks);

    const int channel_y = 40;
    for (int x = 0; x < XSPAN_W; x++) {
        sand_set(&g, x, channel_y - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(&g, x, channel_y + 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    /* Every cell full, filling the first block column exactly - so the block
     * holds no empty cell and no partly-filled liquid, and the skip's own test
     * passes on everything except the one cell beyond its edge. */
    for (int x = 0; x < SAND_BLOCK_W; x++) {
        sand_set(&g, x, channel_y, CELL_MAKE(MAT_WATER, MASS_MAX));
    }

    for (int i = 0; i < 120; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int crossed = 0;
    for (int x = SAND_BLOCK_W; x < XSPAN_W; x++) {
        const cell_t c = sand_at(&g, x, channel_y);
        if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
            crossed++;
        }
    }

    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, crossed,
        "a full block column of water beside an empty one must still spread "
        "into it - the cross-flow block skip has to check one cell past each "
        "edge of the block, because that is how far its rays reach");
}

/* At the real screen size, SAND_BLOCK_W/H (16x64) do NOT evenly divide
 * 184x224: the last block-column is 8 cells wide and the last block-row 32
 * tall. Every other test here picks exact multiples to keep its ASCII
 * fixtures small, and that gap let a division-free block-index rewrite pass
 * all of them while still producing bad indices at the screen's true edges.
 * This drives grains into every block edge, the two partial ones included,
 * under every gravity direction the dithering can produce. */
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

    /* Cycling all eight ring directions is what reaches the corners: the
     * dithering already mixes two per call, but only a requested direction
     * brings both slide directions and the fall direction to x=183 and
     * y=223. Kept short deliberately - a full checkerboard is the most
     * expensive occupancy shape there is, and 30 steps per direction
     * tripped the device's 5s task watchdog. */
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

void run_sand_locality_suite(void)
{
    RUN_TEST(test_two_separate_active_spots_in_the_same_block_row_do_not_wake_each_other);
    RUN_TEST(test_a_block_wakes_when_disturbed_diagonally);
    RUN_TEST(test_sideways_tilt_wakes_only_the_disturbed_column);
    RUN_TEST(test_liquid_cross_flow_wakes_only_the_blocks_it_touches_by_range);
    RUN_TEST(test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in);
    RUN_TEST(test_water_falling_into_the_next_block_down_still_spreads);
    RUN_TEST(test_water_crosses_a_block_boundary_sideways);
    RUN_TEST(test_block_indices_stay_in_range_at_the_real_screens_partial_edge_blocks);
    RUN_TEST(test_block_indices_stay_in_range_after_flipping_a_settled_pile_at_the_real_size);
    RUN_TEST(test_block_indices_stay_in_range_for_a_falling_screen_of_water_at_the_real_size);
    RUN_TEST(test_falling_is_exact_when_scatter_is_off);
    RUN_TEST(test_scatter_spreads_a_falling_stream);
    RUN_TEST(test_scatter_conserves_grains);
    RUN_TEST(test_a_lagging_grain_is_not_left_asleep);
}

SUITE_REGISTER(run_sand_locality_suite);
