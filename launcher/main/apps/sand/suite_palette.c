/*=============================================================================
 * Portable suite: palette - grid arithmetic and hit-testing for the material
 * picker overlay.
 *
 * The centred partial last row is the fiddly part and the main reason this
 * module is host-tested at all - get it wrong and the bottom few materials
 * are unhittable or answer to the wrong index. These tests lean on that case
 * specifically, at BRUSH_COUNT's real value (14) and at a couple of others,
 * rather than trusting the arithmetic by eye.
 *===========================================================================*/

#include <stdbool.h>
#include <stddef.h>

#include "unity.h"
#include "suites.h"

#include "palette.h"

/* --- palette_tile_rect / palette_hit agreement, count = 14 (the real brush
 * count: 3 full rows of 4, then a centred row of 2) ---------------------- */

static void test_centre_of_every_tile_hits_its_own_index(void)
{
    const int count = 14;
    for (int i = 0; i < count; i++) {
        int x, y, w, h;
        palette_tile_rect(i, count, &x, &y, &w, &h);

        const int cx = x + w / 2;
        const int cy = y + h / 2;

        TEST_ASSERT_EQUAL_INT_MESSAGE(i, palette_hit(cx, cy, count),
            "the centre of a tile's own rect must hit that tile's index");
    }
}

static void test_hit_round_trips_against_tile_rect_for_every_tile(void)
{
    /* Every corner and the centre of every tile's rect must resolve back
     * to that same tile - not just the middle, which would miss an
     * off-by-one in the rect's edges. The bottom-right corner is
     * exclusive (x+w, y+h lies in the NEXT cell or off the grid), so only
     * x+w-1, y+h-1 is tested there. */
    for (int count = 1; count <= 16; count++) {
        for (int i = 0; i < count; i++) {
            int x, y, w, h;
            palette_tile_rect(i, count, &x, &y, &w, &h);

            const int xs[] = { x, x + w / 2, x + w - 1 };
            const int ys[] = { y, y + h / 2, y + h - 1 };
            for (int xi = 0; xi < 3; xi++) {
                for (int yi = 0; yi < 3; yi++) {
                    TEST_ASSERT_EQUAL_INT_MESSAGE(i,
                        palette_hit(xs[xi], ys[yi], count),
                        "a point inside tile i's own rect must hit i");
                }
            }
        }
    }
}

/* --- the centred partial row: empty space beside it is a genuine miss --- */

static void test_empty_region_beside_centred_partial_row_misses(void)
{
    /* count=14: last row (row 3) holds 2 of 4 tiles, centred - so it
     * occupies [92,276) horizontally (see PALETTE_TILE) and leaves
     * [0,92) and [276,368) of that row empty. A point in either empty
     * strip, at the row's own y, must miss. */
    const int count = 14;
    int x, y, w, h;
    palette_tile_rect(12, count, &x, &y, &w, &h);   /* first tile, last row */
    TEST_ASSERT_EQUAL_INT(92, x);

    const int row_y = y + h / 2;

    TEST_ASSERT_EQUAL_INT(-1, palette_hit(0, row_y, count));
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(x - 1, row_y, count));
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(367, row_y, count));
}

/* --- outside the panel entirely, and negative coordinates -------------- */

static void test_points_outside_the_panel_miss(void)
{
    const int count = 14;
    int px, py, pw, ph;
    palette_panel_rect(count, &px, &py, &pw, &ph);

    TEST_ASSERT_EQUAL_INT(-1, palette_hit(px + pw / 2, py - 1, count));       /* above */
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(px + pw / 2, py + ph, count));      /* below */
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(px - 1, py + ph / 2, count));       /* left */
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(px + pw, py + ph / 2, count));      /* right */
}

static void test_negative_coordinates_miss(void)
{
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(-1, -1, 14));
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(-1, 100, 14));
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(100, -1, 14));
}

/* --- tiles never overlap, and always sit inside the screen ------------- */

static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void test_tile_rects_never_overlap_and_stay_on_screen(void)
{
    for (int count = 1; count <= 16; count++) {
        int rects[16][4];
        for (int i = 0; i < count; i++) {
            int x, y, w, h;
            palette_tile_rect(i, count, &x, &y, &w, &h);
            rects[i][0] = x; rects[i][1] = y;
            rects[i][2] = w; rects[i][3] = h;

            TEST_ASSERT_TRUE_MESSAGE(x >= 0 && y >= 0 &&
                x + w <= PALETTE_SCREEN_W && y + h <= PALETTE_SCREEN_H,
                "a tile rect must stay entirely on screen");
        }
        for (int i = 0; i < count; i++) {
            for (int j = i + 1; j < count; j++) {
                TEST_ASSERT_FALSE_MESSAGE(
                    rects_overlap(rects[i][0], rects[i][1], rects[i][2], rects[i][3],
                                  rects[j][0], rects[j][1], rects[j][2], rects[j][3]),
                    "two tiles in the same panel must never overlap");
            }
        }
    }
}

/* --- a full grid (16, no partial row) leaves no gap --------------------- */

static void test_full_grid_of_sixteen_leaves_no_gap(void)
{
    const int count = 16;
    int px, py, pw, ph;
    palette_panel_rect(count, &px, &py, &pw, &ph);

    TEST_ASSERT_EQUAL_INT(PALETTE_COLS * PALETTE_TILE, pw);
    TEST_ASSERT_EQUAL_INT(PALETTE_ROWS(count) * PALETTE_TILE, ph);

    /* Every pixel of the panel belongs to exactly one tile - sample the
     * centre of every PALETTE_TILE x PALETTE_TILE cell in the panel and
     * confirm each one hits a distinct, valid index. */
    bool seen[16] = { false };
    for (int row = 0; row < PALETTE_ROWS(count); row++) {
        for (int col = 0; col < PALETTE_COLS; col++) {
            const int cx = px + col * PALETTE_TILE + PALETTE_TILE / 2;
            const int cy = py + row * PALETTE_TILE + PALETTE_TILE / 2;
            const int hit = palette_hit(cx, cy, count);

            TEST_ASSERT_TRUE_MESSAGE(hit >= 0 && hit < count,
                "every cell of a full grid must hit a valid tile");
            TEST_ASSERT_FALSE_MESSAGE(seen[hit],
                "a full grid must not have two cells hitting the same tile");
            seen[hit] = true;
        }
    }
}

/* --- a single tile still lands somewhere sensible ----------------------- */

static void test_count_of_one_lands_sensibly(void)
{
    int x, y, w, h;
    palette_tile_rect(0, 1, &x, &y, &w, &h);

    TEST_ASSERT_EQUAL_INT(PALETTE_TILE, w);
    TEST_ASSERT_EQUAL_INT(PALETTE_TILE, h);
    TEST_ASSERT_TRUE(x >= 0 && x + w <= PALETTE_SCREEN_W);
    TEST_ASSERT_TRUE(y >= 0 && y + h <= PALETTE_SCREEN_H);

    /* A single tile is its own (trivially centred) partial row, so its
     * centre must hit it and its own top-left corner must too. */
    TEST_ASSERT_EQUAL_INT(0, palette_hit(x + w / 2, y + h / 2, 1));
    TEST_ASSERT_EQUAL_INT(0, palette_hit(x, y, 1));
    TEST_ASSERT_EQUAL_INT(-1, palette_hit(x - 1, y, 1));
}

/* --- palette_label_origin(): the label lands centred at every quarter turn
 * --------------------------------------------------------------------------
 *
 * gfx_text_turned()'s origin is the FIRST GLYPH's cell, not any corner of
 * the string as it appears once drawn, so the origin VALUE is not itself
 * the meaningful thing to check - it differs by turn even when the text
 * ends up in the same place (see the turn-0-vs-turn-2 test below). What has
 * to be true is the box the string actually occupies once drawn: centred in
 * the tile at every turn, and swapped in shape at turns 1/3 versus 0/2.
 *
 * label_bbox() below walks the origin the same way gfx_text_turned() does -
 * PALETTE_CHAR_W per glyph, in the direction each turn implies (see
 * palette_label_origin()'s own comment in palette.h for the four
 * corner/direction pairs) - to reconstruct that box from an origin, so the
 * tests can assert on the box rather than have to re-derive gfx_text_turned's
 * walk inline in every test. */

static void label_bbox(int ox, int oy, int len, int turn,
                       int *bx, int *by, int *bw, int *bh)
{
    switch (turn) {
    case 0:                                    /* upright: walks +x from ox,oy */
        *bx = ox;
        *by = oy;
        *bw = len * PALETTE_CHAR_W;
        *bh = PALETTE_CHAR_H;
        break;
    case 2:                                    /* upside down: walks -x from ox,oy */
        *bx = ox - (len - 1) * PALETTE_CHAR_W;
        *by = oy;
        *bw = len * PALETTE_CHAR_W;
        *bh = PALETTE_CHAR_H;
        break;
    case 1:                                    /* top-to-bottom: walks +y from ox,oy */
        *bx = ox;
        *by = oy;
        *bw = PALETTE_CHAR_H;
        *bh = len * PALETTE_CHAR_W;
        break;
    default:                                   /* turn 3, bottom-to-top: walks -y */
        *bx = ox;
        *by = oy - (len - 1) * PALETTE_CHAR_W;
        *bw = PALETTE_CHAR_H;
        *bh = len * PALETTE_CHAR_W;
        break;
    }
}

/* Every rect/len combination below is exercised at all four turns - a rect
 * that is not itself square (unlike a real palette tile) so a formula that
 * only happens to work when w == h cannot pass by accident. */

static void test_label_centred_in_rect_at_every_turn(void)
{
    struct { int x, y, w, h, len; } cases[] = {
        {   0,  40,  92,  92, 5 },   /* a real tile: 5-char name, 92x92 */
        {   0,  40,  92,  92, 1 },   /* shortest real name */
        { 100, 200, 120,  80, 3 },   /* non-square rect */
        {  10,  10,  61,  74, 4 },   /* odd dimensions - VERY LOW quality's cell math shows up elsewhere as odd numbers too */
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const int x = cases[c].x, y = cases[c].y;
        const int w = cases[c].w, h = cases[c].h, len = cases[c].len;

        for (int turn = 0; turn < 4; turn++) {
            int ox, oy;
            palette_label_origin(x, y, w, h, len, turn, &ox, &oy);

            int bx, by, bw, bh;
            label_bbox(ox, oy, len, turn, &bx, &by, &bw, &bh);

            TEST_ASSERT_EQUAL_INT_MESSAGE(x + (w - bw) / 2, bx,
                "label box must be horizontally centred in the rect");
            TEST_ASSERT_EQUAL_INT_MESSAGE(y + (h - bh) / 2, by,
                "label box must be vertically centred in the rect");
        }
    }
}

static void test_label_box_swaps_dimensions_at_turns_one_and_three(void)
{
    const int len = 5;
    int ox, oy, bx, by, bw, bh;

    palette_label_origin(0, 40, 92, 92, len, 1, &ox, &oy);
    label_bbox(ox, oy, len, 1, &bx, &by, &bw, &bh);
    TEST_ASSERT_EQUAL_INT(PALETTE_CHAR_H, bw);
    TEST_ASSERT_EQUAL_INT(len * PALETTE_CHAR_W, bh);

    palette_label_origin(0, 40, 92, 92, len, 3, &ox, &oy);
    label_bbox(ox, oy, len, 3, &bx, &by, &bw, &bh);
    TEST_ASSERT_EQUAL_INT(PALETTE_CHAR_H, bw);
    TEST_ASSERT_EQUAL_INT(len * PALETTE_CHAR_W, bh);
}

static void test_turn_zero_and_two_share_a_box_but_not_an_origin(void)
{
    const int len = 5;
    int ox0, oy0, ox2, oy2;

    palette_label_origin(0, 40, 92, 92, len, 0, &ox0, &oy0);
    palette_label_origin(0, 40, 92, 92, len, 2, &ox2, &oy2);

    /* The origins are genuinely different points - turn 2 starts drawing
     * from the box's other end - which is exactly why the origin itself
     * cannot be the thing under test above. */
    TEST_ASSERT_NOT_EQUAL_INT(ox0, ox2);

    int bx0, by0, bw0, bh0, bx2, by2, bw2, bh2;
    label_bbox(ox0, oy0, len, 0, &bx0, &by0, &bw0, &bh0);
    label_bbox(ox2, oy2, len, 2, &bx2, &by2, &bw2, &bh2);

    TEST_ASSERT_EQUAL_INT(bx0, bx2);
    TEST_ASSERT_EQUAL_INT(by0, by2);
    TEST_ASSERT_EQUAL_INT(bw0, bw2);
    TEST_ASSERT_EQUAL_INT(bh0, bh2);
}

void run_palette_suite(void)
{
    RUN_TEST(test_centre_of_every_tile_hits_its_own_index);
    RUN_TEST(test_hit_round_trips_against_tile_rect_for_every_tile);
    RUN_TEST(test_empty_region_beside_centred_partial_row_misses);
    RUN_TEST(test_points_outside_the_panel_miss);
    RUN_TEST(test_negative_coordinates_miss);
    RUN_TEST(test_tile_rects_never_overlap_and_stay_on_screen);
    RUN_TEST(test_full_grid_of_sixteen_leaves_no_gap);
    RUN_TEST(test_count_of_one_lands_sensibly);
    RUN_TEST(test_label_centred_in_rect_at_every_turn);
    RUN_TEST(test_label_box_swaps_dimensions_at_turns_one_and_three);
    RUN_TEST(test_turn_zero_and_two_share_a_box_but_not_an_origin);
}

SUITE_REGISTER(run_palette_suite);
