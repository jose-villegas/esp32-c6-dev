/*
 * Device-only suite: the graphics layer.
 *
 * Covers what a host cannot: real framebuffer memory, real DMA, real I2C and
 * the actual panel. This suite is compiled into the shipped firmware and runs
 * at boot alongside the portable suites; it is excluded from the host runner
 * because none of it would mean anything on a laptop.
 *
 * Guidance on what belongs here:
 *   - reading back what a draw call actually wrote to memory
 *   - anything involving DMA completion, interrupts or bus timing
 *   - anything whose correctness depends on the target's word size,
 *     endianness or alignment
 *   - resource limits: does the framebuffer actually fit?
 *
 * Anything that is pure logic belongs in the host suite instead. See
 * docs/Testing-Guide.md.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"

static const char* TAG = "device_tests";

/* gfx owns global hardware state and is already initialised by the time this
 * runs - the shipped firmware brings the display up before self-testing. Tests
 * may leave the framebuffer in any state, but must not deinitialise it, and
 * must reset the clip rect since they share it. */
static void
fixture(void) {
    gfx_clear_clip();
    gfx_set_partial_clear(false);
    gfx_invalidate();
}

/* Counts pixels equal to `expect` across the whole framebuffer, which is how
 * most of these tests assert "exactly this region changed and nothing else". */
static int
count_pixels(gfx_color_t expect) {
    const gfx_color_t* fb = gfx_framebuffer();
    int n = 0;
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        if (fb[i] == expect) {
            n++;
        }
    }
    return n;
}

static gfx_color_t
pixel_at(int x, int y) {
    return gfx_framebuffer()[y * GFX_WIDTH + x];
}

/* --- bring-up ----------------------------------------------------------- */

void
test_display_is_up(void) {
    fixture();
    TEST_ASSERT_NOT_NULL_MESSAGE(gfx_framebuffer(),
                                 "the framebuffer should already be allocated by the time tests run");
}

void
test_framebuffer_fits_with_headroom_to_spare(void) {
    fixture();
    /* The framebuffer is 322 KiB of roughly 424 KiB. If this margin ever
     * vanishes, allocations elsewhere start failing in confusing ways, so it
     * is worth asserting rather than discovering later. */
    const size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "free heap after framebuffer: %u bytes", (unsigned)free_heap);
    TEST_ASSERT_GREATER_THAN_UINT32(40 * 1024, free_heap);
}

void
test_touch_controller_is_present(void) {
    fixture();
    /* Confirms the I2C bus works and something answers - the host suite can
     * test what samples mean, but never that the controller exists. */
    const bsp_board_variant_t variant = bsp_board_detect();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(BSP_BOARD_VARIANT_UNKNOWN, variant, "no supported touch controller responded on I2C");
}

/* --- colour packing ----------------------------------------------------- */

void
test_colour_packing_matches_the_panel_format(void) {
    fixture();
    /* RGB565, byte-swapped. Worth checking on the target rather than the host
     * because it depends on the target's endianness and integer promotion.
     *
     * Red 0xFF -> 0b11111 in the top 5 bits -> 0xF800, swapped -> 0x00F8. */
    TEST_ASSERT_EQUAL_HEX16(0x00F8, gfx_rgb(0xFF0000));
    TEST_ASSERT_EQUAL_HEX16(0xE007, gfx_rgb(0x00FF00));
    TEST_ASSERT_EQUAL_HEX16(0x1F00, gfx_rgb(0x0000FF));
    TEST_ASSERT_EQUAL_HEX16(0x0000, gfx_rgb(0x000000));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, gfx_rgb(0xFFFFFF));
}

/* --- primitives, verified by reading the framebuffer back --------------- */

void
test_clear_touches_every_pixel(void) {
    fixture();
    const gfx_color_t c = gfx_rgb(0x123456);
    gfx_clear(c);
    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(c));
}

void
test_partial_clear_erases_only_previous_drawn_region(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_set_partial_clear(true);

    /* Frame 1: first clear is full because partial tracking has no prior frame. */
    gfx_clear(bg);
    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(bg));

    /* Draw a 20x20 rect at (50, 50) and mark it dirty. */
    gfx_fill_rect(50, 50, 20, 20, fg);
    gfx_mark_dirty(50, 50, 20, 20);
    TEST_ASSERT_EQUAL_INT(400, count_pixels(fg));

    gfx_present();

    /* Frame 2: clear should erase only the 20x20 box at (50, 50). */
    gfx_clear(bg);
    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(bg));

    /* Draw a new 10x10 rect at (100, 100) and mark it dirty. */
    gfx_fill_rect(100, 100, 10, 10, fg);
    gfx_mark_dirty(100, 100, 10, 10);
    TEST_ASSERT_EQUAL_INT(100, count_pixels(fg));

    gfx_present();

    /* Frame 3: clear should erase only the (100, 100) 10x10 box. */
    gfx_clear(bg);
    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(bg));
}

void
test_gfx_invalidate_forces_full_clear_in_partial_mode(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_set_partial_clear(true);
    gfx_clear(bg);

    gfx_fill_rect(50, 50, 20, 20, fg);
    gfx_mark_dirty(50, 50, 20, 20);
    gfx_present();

    /* Place rogue pixels elsewhere without marking dirty. */
    gfx_color_t* fb = gfx_framebuffer();
    fb[200 * GFX_WIDTH + 200] = fg;

    /* Invalidation forces next clear to wipe entire screen in full. */
    gfx_invalidate();
    gfx_clear(bg);

    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(bg));
}

void
test_fill_rect_writes_exactly_its_own_area(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    gfx_fill_rect(10, 20, 30, 40, fg);

    TEST_ASSERT_EQUAL_INT_MESSAGE(30 * 40, count_pixels(fg), "a filled rect must cover exactly w*h pixels");

    /* Corners in, neighbours out. */
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(10, 20));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(39, 59));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(9, 20));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(40, 59));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(10, 19));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(39, 60));
}

/* --- dithered fake transparency ------------------------------------------ */

void
test_dither_at_alpha_zero_draws_nothing(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    gfx_fill_rect_dither(10, 10, 40, 40, fg, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_pixels(fg), "alpha 0 should draw nothing at all - not even one dither cell");
}

/* BOOT_ANIM_TITLE_SHADOW_ALPHA defaults to 255, so alpha 255 must stay
 * byte-for-byte the plain gfx_fill_rect() every other caller draws. Full
 * coverage plus corner spot checks rather than a framebuffer snapshot
 * compare: a second copy of the 322 KiB framebuffer does not fit (see
 * docs/notes/Board-and-Memory.md). */
void
test_dither_at_alpha_255_matches_a_solid_fill_exactly(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    gfx_fill_rect_dither(10, 10, 40, 40, fg, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(40 * 40, count_pixels(fg),
                                  "alpha 255 should cover every pixel of the rect, same as a solid "
                                  "gfx_fill_rect() would");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(10, 10));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(49, 49));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(9, 10));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(50, 49));
}

void
test_dither_coverage_is_monotonic_and_graduated(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);
    const int area = 40 * 40;

    int prev = -1;
    for (int a = 0; a <= 255; a += 17) {
        gfx_clear(bg);
        gfx_fill_rect_dither(10, 10, 40, 40, fg, (uint8_t)a);
        const int n = count_pixels(fg);
        TEST_ASSERT_TRUE_MESSAGE(n >= prev, "coverage should never decrease as alpha climbs");
        TEST_ASSERT_TRUE_MESSAGE(n <= area, "a dithered fill can never draw MORE pixels than a solid one "
                                            "would across the same area");
        prev = n;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(area, prev, "the sweep's own last step (255) should reach full coverage");
}

/* The dither is keyed to each pixel's ABSOLUTE panel position, not one
 * local to whichever call drew it, so two abutting dithered rects read as
 * one continuous texture instead of each restarting the pattern at its own
 * corner. Sampled at the seam across all four dither phase-rows - the Bayer
 * table's own period. */
void
test_dither_stays_in_phase_across_separate_calls(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    gfx_fill_rect_dither(10, 10, 80, 40, fg, 128);

    gfx_color_t one_call[4][8];
    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            one_call[dy][dx] = pixel_at(46 + dx, 10 + dy);
        }
    }

    gfx_clear(bg);
    gfx_fill_rect_dither(10, 10, 40, 40, fg, 128);
    gfx_fill_rect_dither(50, 10, 40, 40, fg, 128);

    for (int dy = 0; dy < 4; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(one_call[dy][dx], pixel_at(46 + dx, 10 + dy),
                                            "the seam between two abutting dithered rects should read "
                                            "identically to one continuous dithered rect at the same "
                                            "spot - a mismatch here means the dither restarted its "
                                            "own pattern at the second rect's own corner");
        }
    }
}

/* --- gfx_blit_dither: image-over-live-content compositing ---------------- */

/* One synthetic source pixel per index - every value distinct from its
 * neighbours and never equal to the black background (the | 1), so a blit
 * writing the WRONG source pixel reads as a value mismatch, not a
 * coincidental pass. Heap, not the 3584-byte main-task stack, and not
 * static: permanent .bss is the scarcest budget on this board. */
#define BLIT_SRC_STRIDE 70
#define BLIT_SRC_ROWS   40

static gfx_color_t
blit_src_pixel(int i) {
    return (gfx_color_t)(((unsigned)i * 7u) | 1u);
}

static gfx_color_t*
make_blit_src(void) {
    gfx_color_t* src = malloc(sizeof(gfx_color_t) * BLIT_SRC_STRIDE * BLIT_SRC_ROWS);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "need a heap source image for the blit tests");
    for (int i = 0; i < BLIT_SRC_STRIDE * BLIT_SRC_ROWS; i++) {
        src[i] = blit_src_pixel(i);
    }
    return src;
}

void
test_blit_dither_at_alpha_zero_changes_nothing(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    gfx_color_t* src = make_blit_src();

    gfx_clear(bg);
    gfx_blit_dither(13, 10, 61, BLIT_SRC_ROWS, src, BLIT_SRC_STRIDE, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_WIDTH * GFX_HEIGHT, count_pixels(bg),
                                  "alpha 0 should write nothing at all - not even one dither cell");
    free(src);
}

void
test_blit_dither_at_alpha_255_matches_the_source_exactly(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    gfx_color_t* src = make_blit_src();
    const int x = 13, y = 10, w = 61, h = BLIT_SRC_ROWS;

    gfx_clear(bg);
    gfx_blit_dither(x, y, w, h, src, BLIT_SRC_STRIDE, 255);

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(blit_src_pixel(row * BLIT_SRC_STRIDE + col), pixel_at(x + col, y + row),
                                            "alpha 255 must reproduce the source rect exactly, stride "
                                            "and all - same guarantee gfx_fill_rect_dither() makes");
        }
    }
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(x - 1, y), "a blit must not touch pixels left of its own rect");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(x + w, y + h - 1),
                                    "a blit must not touch pixels right of its own rect");
    free(src);
}

/* The primitive's whole correctness claim (see its comment in gfx.c): the
 * row-pattern walk, fringes and memcpy shortcuts included, is bit-identical
 * to asking gfx_dither_covers() at every pixel. Checked at a deliberately
 * UNALIGNED rect (x = 13, so the unrolled group loop's prologue and
 * epilogue both actually run) across alphas that exercise a sparse, a
 * half, and a dense pattern. */
void
test_blit_dither_matches_per_pixel_covers_reference(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    gfx_color_t* src = make_blit_src();
    const int x = 13, y = 10, w = 61, h = BLIT_SRC_ROWS;
    const uint8_t alphas[3] = {32, 128, 200};

    for (int a = 0; a < 3; a++) {
        gfx_clear(bg);
        gfx_blit_dither(x, y, w, h, src, BLIT_SRC_STRIDE, alphas[a]);

        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                const gfx_color_t want =
                    gfx_dither_covers(x + col, y + row, alphas[a]) ? blit_src_pixel(row * BLIT_SRC_STRIDE + col) : bg;
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, pixel_at(x + col, y + row),
                                                "gfx_blit_dither() must agree with gfx_dither_covers() "
                                                "at every pixel - fringes and full-row shortcuts "
                                                "included");
            }
        }
    }
    free(src);
}

/* Compared one ROW at a time, re-rendering both glyphs per row, rather than
 * snapshotting the whole 40x40 box into a stack array: these tests run on
 * the ESP-IDF main task's 3584-byte stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE)
 * and already several frames deep. The box would be 3.2 KB of that in one
 * local; a 40-wide row is 80 bytes. */
void
test_dithered_text_at_alpha_255_matches_solid_text_exactly(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFFFF);
    const gfx_font_t* font = gfx_font_ui();
    const int box = 8 * 5; /* cell_w/cell_h (8) * scale (5) */

    for (int row = 0; row < box; row++) {
        gfx_color_t solid_row[8 * 5];

        gfx_clear(bg);
        gfx_text_font(20, 20, "A", fg, 5, 0, font);
        for (int col = 0; col < box; col++) {
            solid_row[col] = pixel_at(20 + col, 20 + row);
        }

        gfx_clear(bg);
        gfx_text_font_dither(20, 20, "A", fg, 5, 0, font, 255);

        for (int col = 0; col < box; col++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(solid_row[col], pixel_at(20 + col, 20 + row),
                                            "gfx_text_font_dither() at alpha 255 must match "
                                            "gfx_text_font()'s own solid output pixel for pixel");
        }
    }
}

void
test_dithered_text_at_low_alpha_draws_fewer_pixels_than_solid(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFFFF);
    const gfx_font_t* font = gfx_font_ui();

    gfx_clear(bg);
    gfx_text_font(20, 20, "A", fg, 5, 0, font);
    const int solid_n = count_pixels(fg);

    gfx_clear(bg);
    gfx_text_font_dither(20, 20, "A", fg, 5, 0, font, 64);
    const int dim_n = count_pixels(fg);

    TEST_ASSERT_TRUE_MESSAGE(dim_n > 0 && dim_n < solid_n,
                             "a dim shadow should draw some but fewer pixels than a solid glyph");
}

/* --- lines -------------------------------------------------------------- */

void
test_a_horizontal_line_covers_both_endpoints(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x00FF00);

    gfx_clear(bg);
    gfx_line(10, 30, 40, 30, fg);

    /* Inclusive at both ends: 40 - 10 + 1. A line that quietly drops its last
     * pixel leaves a gap at every joint of a polyline, which is what a curve
     * is made of. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(31, count_pixels(fg),
                                  "a horizontal line should cover both of its endpoints and nothing "
                                  "else");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(10, 30));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(40, 30));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(9, 30));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(41, 30));
}

void
test_a_line_is_the_same_line_drawn_backwards(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF8800);

    /* Same two points, opposite order. Bresenham breaks ties by the direction
     * it steps in, so the two passes can differ - but only where they are
     * already adjacent, never by a pixel's worth of coverage. */
    gfx_clear(bg);
    gfx_line(5, 7, 60, 33, fg);
    const int forward = count_pixels(fg);

    gfx_clear(bg);
    gfx_line(60, 33, 5, 7, fg);
    const int backward = count_pixels(fg);

    TEST_ASSERT_EQUAL_INT_MESSAGE(forward, backward,
                                  "drawing a line end-to-start covered a different number of pixels");
    TEST_ASSERT_EQUAL_INT_MESSAGE(56, forward, "a line should be one pixel per step along its longer axis");
}

void
test_a_single_point_line_draws_one_pixel(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x00FFFF);

    gfx_clear(bg);
    gfx_line(100, 100, 100, 100, fg);

    TEST_ASSERT_EQUAL_INT(1, count_pixels(fg));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(100, 100));
}

void
test_a_line_is_clipped_rather_than_wrapped(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    /* Starts off the left edge and off the top, ends on screen. The failure
     * this guards against is not a crash but a wrap: an unclipped write at
     * x = -1 lands at the far end of the previous row. */
    gfx_line(-50, -20, 20, 15, fg);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(fg, pixel_at(20, 15), "the on-screen end of the line should still be drawn");
    for (int y = 0; y < GFX_HEIGHT; y++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(GFX_WIDTH - 1, y),
                                        "a clipped line wrapped onto the opposite edge");
    }
}

void
test_a_line_entirely_off_screen_draws_nothing(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFF00);

    gfx_clear(bg);
    gfx_line(-100, -100, -10, -40, fg);
    gfx_line(GFX_WIDTH + 5, 10, GFX_WIDTH + 90, 200, fg);

    TEST_ASSERT_EQUAL_INT(0, count_pixels(fg));
}

void
test_a_line_honours_the_clip_rect(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x8888FF);

    gfx_clear(bg);
    gfx_set_clip(20, 20, 10, 10);
    gfx_line(0, 25, GFX_WIDTH - 1, 25, fg);
    gfx_clear_clip();

    TEST_ASSERT_EQUAL_INT_MESSAGE(10, count_pixels(fg),
                                  "only the part of the line inside the clip rect should be drawn");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(20, 25));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(29, 25));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(19, 25));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(30, 25));
}

void
test_an_additive_line_brightens_where_it_crosses_itself(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t red = gfx_rgb(0xFF0000);
    const gfx_color_t grn = gfx_rgb(0x00FF00);

    gfx_clear(bg);
    gfx_line_ex(10, 100, 60, 100, red, GFX_LINE_ADD);
    gfx_line_ex(35, 80, 35, 120, grn, GFX_LINE_ADD);

    /* Where they cross, both channels are lit; where they do not, only one
     * is. Flat writes would have put green over red at the crossing. */
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(gfx_rgb(0xFFFF00), pixel_at(35, 100),
                                    "the crossing should be the sum of the two strokes");
    TEST_ASSERT_EQUAL_HEX16(red, pixel_at(20, 100));
    TEST_ASSERT_EQUAL_HEX16(grn, pixel_at(35, 90));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(70, 100));
}

void
test_an_open_line_leaves_its_first_pixel_alone(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x004000);

    gfx_clear(bg);
    gfx_line_ex(10, 40, 20, 40, fg, GFX_LINE_ADD | GFX_LINE_OPEN);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(10, 40), "an open line must not draw its starting pixel");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(11, 40));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(fg, pixel_at(20, 40), "an open line still draws its END pixel");
    TEST_ASSERT_EQUAL_INT(10, count_pixels(fg));
}

/* The reason GFX_LINE_OPEN exists. A polyline drawn as closed segments
 * adds the shared pixel at each joint twice, which under additive blending is
 * a brighter dot at every joint - a curve made of a few hundred segments
 * comes out visibly beaded. Chaining open segments puts exactly one
 * contribution on every pixel. */
void
test_chained_open_segments_do_not_double_their_joints(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t step = gfx_rgb(0x002000);

    gfx_clear(bg);
    gfx_line_ex(10, 60, 20, 60, step, GFX_LINE_ADD);
    gfx_line_ex(20, 60, 30, 60, step, GFX_LINE_ADD | GFX_LINE_OPEN);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(step, pixel_at(20, 60),
                                    "the joint should carry exactly one stroke's worth of light");
    TEST_ASSERT_EQUAL_INT_MESSAGE(21, count_pixels(step), "every pixel of the chain should be lit exactly once");
}

void
test_fill_rect_is_clipped_to_the_screen(void) {
    fixture();
    /* Straddling every edge. If clipping were wrong this would corrupt memory
     * around the framebuffer rather than fail politely, so it is worth having. */
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x00FF00);

    gfx_clear(bg);
    gfx_fill_rect(-50, -50, 100, 100, fg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(50 * 50, count_pixels(fg), "only the on-screen quarter should be drawn");

    gfx_clear(bg);
    gfx_fill_rect(GFX_WIDTH - 10, GFX_HEIGHT - 10, 100, 100, fg);
    TEST_ASSERT_EQUAL_INT(10 * 10, count_pixels(fg));
}

void
test_fill_rect_entirely_off_screen_draws_nothing(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF0000);

    gfx_clear(bg);
    gfx_fill_rect(-200, -200, 50, 50, fg);
    gfx_fill_rect(GFX_WIDTH + 10, 0, 50, 50, fg);
    gfx_fill_rect(0, GFX_HEIGHT + 10, 50, 50, fg);

    TEST_ASSERT_EQUAL_INT(0, count_pixels(fg));
}

void
test_clip_rect_restricts_drawing(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFF00);

    gfx_clear(bg);
    gfx_set_clip(100, 100, 50, 50);
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, fg); /* try to cover everything */
    gfx_clear_clip();

    TEST_ASSERT_EQUAL_INT_MESSAGE(50 * 50, count_pixels(fg), "drawing must be confined to the clip rect");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(100, 100));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(99, 100));
}

void
test_pixel_outside_the_screen_is_ignored(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF0000);

    gfx_clear(bg);
    gfx_pixel(-1, 0, fg);
    gfx_pixel(0, -1, fg);
    gfx_pixel(GFX_WIDTH, 0, fg);
    gfx_pixel(0, GFX_HEIGHT, fg);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_pixels(fg), "out-of-bounds pixels must be dropped, not wrapped");
}

void
test_text_draws_and_advances(void) {
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFFFF);

    gfx_clear(bg);
    gfx_text(0, 0, "II", fg);

    const int drawn = count_pixels(fg);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, drawn, "text drew nothing at all");

    /* Both glyphs rendered, so ink appears in the second cell too. */
    int second_cell = 0;
    for (int y = 0; y < GFX_CHAR_H; y++) {
        for (int x = GFX_CHAR_W; x < GFX_CHAR_W * 2; x++) {
            if (pixel_at(x, y) == fg) {
                second_cell++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, second_cell, "the second character must be drawn one cell to the right");
}

void
test_text_metrics_agree_with_what_is_drawn(void) {
    fixture();
    /* The UI layer lays out from these numbers, so they must match the
     * renderer or every label is subtly misplaced. */
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_W * 5, gfx_text_width("hello", -1));
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_W * 2, gfx_text_width("hello", 2));
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_H, gfx_text_height());
}

/* --- DMA ---------------------------------------------------------------- */

void
test_present_completes(void) {
    fixture();
    /* This is the regression guard for a real deadlock: the frame is queued as
     * seven strip transfers before any is awaited, and waiting on a binary
     * semaphore silently dropped the extra completions, hanging on the second
     * take. Nothing about that is reproducible off-device.
     *
     * If it ever regresses this call never returns and the harness reports a
     * timeout, which is the correct outcome. */
    gfx_clear(gfx_rgb(0x001020));

    const int64_t started = esp_timer_get_time();
    gfx_present();
    const int64_t elapsed_us = esp_timer_get_time() - started;

    ESP_LOGI(TAG, "gfx_present() took %lld us", (long long)elapsed_us);

    /* Sanity bounds rather than a benchmark: a full frame over QSPI cannot be
     * instant, and should not take anywhere near a second. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1000, (int)elapsed_us,
                                         "present returned implausibly fast - did it actually wait for the DMA?");
    TEST_ASSERT_LESS_THAN_INT(500000, (int)elapsed_us);

    /* A full seven-band frame measured between 18,094 and 18,444 us across
     * four device captures, under 2% apart. 19,500 leaves about 5.7% over
     * the observed maximum - past scheduling jitter, still tight enough to
     * catch the bus clock regressing or the bands dropping out of
     * pipelining. */
    TEST_ASSERT_LESS_THAN_MESSAGE(19500, (int)elapsed_us,
                                  "a full-frame present cost more than its observed price - the bus "
                                  "clock may have regressed, or the seven bands stopped pipelining");
}

void
test_repeated_presents_stay_in_sync(void) {
    fixture();
    /* Each frame must consume exactly as many completions as it queued. If the
     * accounting drifted, this would deadlock within a few iterations rather
     * than after hours of running. */
    for (int i = 0; i < 10; i++) {
        gfx_clear(gfx_rgb(i * 0x101010));
        gfx_present();
    }
    TEST_PASS();
}

/* --- partial presents --------------------------------------------------- */

/* The panel refreshes from its own GRAM, so a band that is not sent keeps
 * showing what it last received.
 *
 * Most tests below assert a ratio against a reference measured in the same
 * run AND an absolute budget: the ratio proves the mechanism but is blind
 * to a uniform slowdown. Absolutes are affordable because these tests are
 * bus-bound - the full-band reference measured 3,405/3,405/3,404/3,406 us
 * across four captures, a 0.06% spread. */

static int64_t
time_present(void) {
    const int64_t start = esp_timer_get_time();
    gfx_present();
    return esp_timer_get_time() - start;
}

static void
test_an_unchanged_frame_costs_almost_nothing(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    const int64_t full = time_present(); /* clear marks everything */

    const int64_t unchanged = time_present(); /* nothing touched since */

    ESP_LOGI(TAG, "present: full %lld us, unchanged %lld us", (long long)full, (long long)unchanged);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)(full / 10), (int)unchanged,
                                  "a frame in which nothing changed must skip the bus entirely, not "
                                  "resend 322 KiB of identical pixels");

    /* 3-4 us across four device captures: seven dirty_row_is_dirty() checks,
     * all clean. 50 us is generous on purpose - the regression it guards
     * against, an unchanged frame sending pixels again, lands in the
     * thousands rather than 10% over. */
    TEST_ASSERT_LESS_THAN_MESSAGE(50, (int)unchanged,
                                  "an unchanged frame's cost grew past what a clean dirty-check should "
                                  "ever take - did it start touching the bus?");
}

/* Splits a full-screen gfx_present() into raw QSPI bus time versus
 * everything gfx_present() adds on top: the seven-strip loop,
 * dirty_row_is_dirty() checks, collect_dirty_runs(), leaf refinement and
 * the gather-vs-full-band choice, all of which run even when the whole
 * screen goes out as full-band sends.
 * gfx_present_raw_full_frame_for_test() (gfx.c, CONFIG_LAUNCHER_DEVELOPMENT
 * only) is the bus-time side: one draw-bitmap call over the whole
 * framebuffer, with none of that involved. */
static void
test_full_present_cost_splits_into_bus_time_and_overhead(void) {
    fixture();

    gfx_clear(gfx_rgb(0x102030)); /* marks the whole screen dirty */
    const int64_t present_us = time_present();

    const int64_t raw_start = esp_timer_get_time();
    gfx_present_raw_full_frame_for_test();
    const int64_t raw_us = esp_timer_get_time() - raw_start;

    const int64_t overhead_us = present_us - raw_us;

    ESP_LOGI(TAG,
             "present decompose: gfx_present() full %lld us, raw blit "
             "%lld us, overhead %lld us",
             (long long)present_us, (long long)raw_us, (long long)overhead_us);

    /* Sanity bounds only: this test exists to log the split, not to hold it
     * to a ceiling. PROVISIONAL - no device capture of the split exists yet;
     * peg a real budget on the overhead from the first one. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1000, (int)raw_us,
                                         "the raw blit returned implausibly fast - did it actually wait for "
                                         "the DMA?");
    TEST_ASSERT_LESS_THAN_INT(500000, (int)raw_us);
}

static void
test_a_partial_change_costs_less_than_a_full_frame(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    const int64_t full = time_present();

    /* One band of seven. */
    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x204060));
    const int64_t one_band = time_present();

    ESP_LOGI(TAG, "present: full %lld us, one band %lld us", (long long)full, (long long)one_band);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)(full / 2), (int)one_band,
                                  "sending one band of seven must cost far less than sending all of "
                                  "them - this is the whole point of dirty tracking");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)one_band, "but it must still actually send something");

    /* One band, un-pipelined - the same reference every ratio test below
     * this one measures, and the tightest of the lot: 3,400 / 3,398 / 3,398
     * / 3,399 us across four captures, a 0.06% spread. 3,550 us leaves
     * about 4.4% over the observed maximum, tight because the reference
     * itself is this stable - a looser margin here would just be slack that
     * a real regression could hide in. */
    TEST_ASSERT_LESS_THAN_MESSAGE(3550, (int)one_band,
                                  "one band alone cost more than its stable observed price - the bus "
                                  "clock or the QSPI setup may have regressed");
}

/* The ratio tests below take a band presented alone as their reference,
 * which is the UN-PIPELINED price: 3,405 us. Inside a real frame
 * send_full_row() (gfx.c) queues without waiting and gfx_present() drains
 * every band at the end, so seven bands come to 18,147 us, not 7 x 3,405.
 * Sanity-checking one figure against the other by multiplying is not
 * valid. */

/* PROTOTYPE: measures the gather-copy path in gfx_present() - a strip whose
 * real dirty width is only a fraction of the band, written directly (not
 * through gfx_fill_rect(), which always claims the whole band via
 * mark_band() regardless of what it drew - see its comment). See
 * docs/notes/Display-and-Rendering.md's "Still untapped" for why this
 * exists. */
static void
test_a_narrow_change_costs_less_than_a_full_band(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present(); /* drain: everything now clean */

    /* One full band, the existing fast path. */
    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x204060));
    const int64_t full_band = time_present();

    /* A narrow strip within a band, written directly and marked with its
     * real bounds - what draw_dirty_rows() actually does for a small pour. */
    gfx_color_t* fb = gfx_framebuffer();
    const int w = 20;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x204060);
        }
    }
    gfx_mark_dirty(0, 0, w, 64);
    const int64_t narrow = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, %d px wide (gathered) %lld us", (long long)full_band, w,
             (long long)narrow);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)narrow,
                                  "a strip a fraction of the band's width must cost less than "
                                  "claiming the whole band, or the gather-copy path is not paying "
                                  "for itself");

    /* 757 / 750 / 766 / 743 us across four captures - a 3% spread, wider
     * than the full-band reference because this path does a memcpy into
     * gather_buf on top of the same DMA wait, and that copy is what varies.
     * 850 us leaves about 11% over the observed maximum: room for that
     * spread plus some, without being loose enough to miss the gather path
     * regressing back towards full-band cost. */
    TEST_ASSERT_LESS_THAN_MESSAGE(850, (int)narrow,
                                  "the gathered narrow strip cost more than its observed price - the "
                                  "gather-copy path may have regressed");
}

/* The box is bounded by area, not width alone, specifically so a
 * wide-but-short change gathers as cheaply as a narrow-but-tall one - tilt
 * the board so gravity points sideways and a falling stream is wide and
 * short instead of narrow and tall, and a width-only bound would give it no
 * benefit at all. See docs/notes/Display-and-Rendering.md's "Still
 * untapped". */
static void
test_a_short_wide_change_costs_less_than_a_full_band(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x602040));
    const int64_t full_band = time_present();

    /* Wide but short: most of the band's width, a sliver of its height -
     * the shape a sideways-falling stream leaves behind. */
    gfx_color_t* fb = gfx_framebuffer();
    const int w = 300;
    const int h = 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x602040);
        }
    }
    gfx_mark_dirty(0, 0, w, h);
    const int64_t wide = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, %dx%d px (gathered) %lld us", (long long)full_band, w, h,
             (long long)wide);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)wide,
                                  "a box short enough in height must cost less than claiming the "
                                  "whole band, even at most of its width - orientation must not "
                                  "matter to whether gathering pays off");

    /* 562 / 605 / 576 / 591 us across four captures - the widest spread of
     * any gathered-piece test here, about 7.6%, from the same memcpy-plus-
     * DMA-wait shape as the narrow strip above but at a different aspect
     * ratio. 700 us leaves about 16% over the observed maximum, wider than
     * the narrow strip's margin because this test's own captures already
     * moved twice as much - the margin tracks the spread it is guarding,
     * not a fixed percentage. */
    TEST_ASSERT_LESS_THAN_MESSAGE(700, (int)wide,
                                  "the gathered wide-short box cost more than its observed price - "
                                  "the gather-copy path may have regressed");
}

/* Full width, most of a band's height: 368x48 is far over GATHER_MAX_PIXELS,
 * yet a full-width box is already contiguous in the framebuffer and needs no
 * gather buffer at all - the case send_partial_band() (gfx.c) exists for.
 *
 * 90% is the threshold, not 75%, on purpose: 48 of a band's 64 rows is 75%
 * of its pixels, and a present is ~94% bus time (gfx.h), so once the fixed
 * per-transaction cost is counted the honest floor is around 78%. */
static void
test_a_full_width_partial_height_change_costs_less_than_a_band(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x406020));
    const int64_t full_band = time_present();

    /* Full width, 48 of the band's 64 rows - the shape a wide pour
     * leaves that has not yet grown to fill its whole strip. */
    gfx_color_t* fb = gfx_framebuffer();
    const int h = 48;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x406020);
        }
    }
    gfx_mark_dirty(0, 0, GFX_WIDTH, h);
    const int64_t partial = time_present();

    ESP_LOGI(TAG,
             "present: full band %lld us, full width x %d px (partial "
             "band) %lld us",
             (long long)full_band, h, (long long)partial);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)(full_band * 9 / 10), (int)partial,
                                  "a full-width box shorter than a whole band must send fewer rows "
                                  "and cost less than claiming the whole band - see "
                                  "send_partial_band() in gfx.c");
}

/* Two small clusters in the same band but opposite corners - each cheap
 * enough on its own to gather independently, which is the point: this is
 * the case a single adaptive box per strip cannot help with at all, since
 * a box spanning both would cover nearly the whole band for no reason. Two
 * separate pools settling in the same horizontal band, say. See
 * docs/notes/Display-and-Rendering.md's "Still untapped". */
static void
test_two_far_corners_cost_less_than_a_full_band(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x206020));
    const int64_t full_band = time_present();

    gfx_color_t* fb = gfx_framebuffer();
    const int size = 15;

    /* Top-left corner. */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x206020);
        }
    }
    gfx_mark_dirty(0, 0, size, size);

    /* Bottom-right corner - a different row range within the same band, a
     * different column, nothing in between touched. */
    const int y0 = 64 - size;
    const int x0 = GFX_WIDTH - size;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fb[(y0 + y) * GFX_WIDTH + x0 + x] = gfx_rgb(0x206020);
        }
    }
    gfx_mark_dirty(x0, y0, size, size);

    const int64_t two_corners = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, two %dx%d corners %lld us", (long long)full_band, size, size,
             (long long)two_corners);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)two_corners,
                                  "two small, far-apart clusters sent independently together must "
                                  "still cost less than the whole band");

    /* 1,914 / 1,917 / 1,916 / 1,914 us across four captures - a 0.16%
     * spread, nearly as tight as the full-band reference itself, because
     * two independent gather-and-waits dominated by DMA time leave little
     * room for the copy-side jitter the single-piece gathers above show.
     * 2,000 us leaves about 4.3% over the observed maximum - tight, to
     * match how tight the reference is. */
    TEST_ASSERT_LESS_THAN_MESSAGE(2000, (int)two_corners,
                                  "two far corners cost more than their observed price - one of the "
                                  "two independent gathers may have regressed");
}

/* Three separated marks, one more than LEAF_REFINE_MAX_RUNS (gfx_dirty.h)
 * tracks. Cells 0-2 are adjacent, so collect_dirty_runs() merges them into
 * one coarse run, and splitting that run into its three gaps is what the cap
 * governs - marks in non-adjacent cells never reach it. At the shipped cap
 * of 2 plan_run() falls back to run_box()'s coarse union, one ~240x64 send
 * that a raised cap's three small sends are not guaranteed to beat. */
static void
test_three_far_apart_marks_falls_back_at_the_current_cap(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x602060));
    const int64_t full_band = time_present();

    gfx_color_t* fb = gfx_framebuffer();
    const int size = 15;
    /* Cells 0, 1 and 2 (COL_WIDTH=92 each) - adjacent, so these merge
     * into one 276px-wide coarse run, not three separate cell-level ones.
     * Each mark sits inside its own cell with real room either side, so
     * the gaps are genuine at the leaf level, not an artifact of landing
     * right on a cell boundary. */
    const int xs[3] = {5, 115, 230};

    for (int i = 0; i < 3; i++) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                fb[y * GFX_WIDTH + xs[i] + x] = gfx_rgb(0x602060);
            }
        }
        gfx_mark_dirty(xs[i], 0, size, size);
    }

    const int64_t three_marks = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, three %dx%d marks %lld us", (long long)full_band, size, size,
             (long long)three_marks);

    /* No ratio against full_band on purpose: whether the fallback beats a
     * full band is the open question this test measures. The absolute is
     * safe to peg - 869/882/875/877 us across four captures, a 1.5% spread;
     * 980 leaves about 11% over, room for a different fallback shape. */
    TEST_ASSERT_LESS_THAN_MESSAGE(980, (int)three_marks,
                                  "three far-apart marks' fallback send cost more than its observed "
                                  "price");
}

/* A small mark plus a wide one in the same coarse run, sized to land the
 * wide mark's leaf-refined piece right where GATHER_MAX_PIXELS decides
 * whether it gets gathered. Literal numbers because gfx_dirty.h is
 * header-only and static - a second include would duplicate its
 * dirty-tracking state. The wide mark covers leaf columns 2-6, so
 * refine_run() reports a 5-leaf 115px piece: 115 * 64 = 7360 px - over
 * budget at 4096 and 6144, under it at the shipped 8192. */
static void
test_a_near_budget_split_crosses_the_gather_threshold(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x206040));
    const int64_t full_band = time_present();

    gfx_color_t* fb = gfx_framebuffer();
    const int small_size = 15;
    const int wide_x = 48, wide_w = 110, wide_h = 64;

    for (int y = 0; y < small_size; y++) {
        for (int x = 0; x < small_size; x++) {
            fb[y * GFX_WIDTH + 2 + x] = gfx_rgb(0x206040);
        }
    }
    gfx_mark_dirty(2, 0, small_size, small_size);

    for (int y = 0; y < wide_h; y++) {
        for (int x = 0; x < wide_w; x++) {
            fb[y * GFX_WIDTH + wide_x + x] = gfx_rgb(0x206040);
        }
    }
    gfx_mark_dirty(wide_x, 0, wide_w, wide_h);

    const int64_t near_budget = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, near-budget split %lld us", (long long)full_band,
             (long long)near_budget);

    /* No ratio on purpose: whether gathering at this size helps is the open
     * question a sweep of GATHER_MAX_PIXELS is for. 1,671/1,800/1,715/1,754
     * us across four captures, a 7.7% spread - two independent sends, so
     * wider than the single-piece gathers above; 2,050 leaves ~14% over. */
    TEST_ASSERT_LESS_THAN_MESSAGE(2050, (int)near_budget, "the near-budget split cost more than its observed price");
}

/* Two small marks inside the SAME 92px cell, far enough apart to leave a
 * real gap between them - the shape only leaf refinement can split on.
 * test_two_far_corners above lands in different CELLS, which
 * collect_dirty_runs() alone already separates without any help from the
 * leaf layer; this test is the one that actually exercises it. See
 * docs/notes/Display-and-Rendering.md's "Still untapped". */
static void
test_two_marks_in_one_cell_cost_less_than_the_coarse_box(void) {
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x406020));
    const int64_t full_band = time_present();

    gfx_color_t* fb = gfx_framebuffer();
    const int size = 10;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fb[y * GFX_WIDTH + (5 + x)] = gfx_rgb(0x406020);
        }
    }
    gfx_mark_dirty(5, 0, size, size);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            fb[y * GFX_WIDTH + (70 + x)] = gfx_rgb(0x406020);
        }
    }
    gfx_mark_dirty(70, 0, size, size);

    const int64_t two_marks = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, two marks in one cell %lld us", (long long)full_band,
             (long long)two_marks);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)two_marks,
                                  "two small marks separated by a real gap inside one cell must cost "
                                  "less than sending the coarse box spanning both");

    /* 1,958 / 1,960 / 1,959 / 1,959 us across four captures - a 0.1%
     * spread, the same shape as two-far-corners above and for the same
     * reason: two independent gather-and-waits, DMA-dominated, with little
     * room for copy-side jitter. 2,050 us leaves about 4.6% over the
     * observed maximum, tight to match. */
    TEST_ASSERT_LESS_THAN_MESSAGE(2050, (int)two_marks,
                                  "two marks in one cell cost more than their observed price - the "
                                  "leaf-refined split may have regressed");
}

static void
test_drawing_marks_what_it_touched(void) {
    fixture();
    gfx_clear(gfx_rgb(0x000000));
    gfx_present(); /* everything now clean */

    TEST_ASSERT_FALSE_MESSAGE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT),
                              "a present must clear the dirty state, or every frame sends the whole "
                              "screen for ever");

    gfx_fill_rect(0, 0, 8, 8, gfx_rgb(0xFFFFFF));

    TEST_ASSERT_TRUE_MESSAGE(gfx_region_dirty(0, 0, GFX_WIDTH, 64), "the band that was drawn into must be marked");
    TEST_ASSERT_FALSE_MESSAGE(gfx_region_dirty(0, GFX_HEIGHT - 64, GFX_WIDTH, 64),
                              "a band nowhere near the drawing must not be");
}

/* --- suspending the display to share SPI2 ------------------------------- */

/* The display and the SD card are wired to different pins on the one SPI2
 * controller, so reaching the card means releasing the panel. These cover the
 * round trip. They can only run on hardware: the whole point is whether real
 * bus teardown and rebuild leave the driver in a usable state. */

static void
test_the_framebuffer_survives_a_suspend(void) {
    fixture();

    /* A recognisable pattern, so this fails loudly if resume were ever to
     * reallocate rather than reattach. */
    gfx_color_t* before = gfx_framebuffer();
    const gfx_color_t marker = gfx_rgb(0x8040C0);
    gfx_clear(marker);

    TEST_ASSERT_TRUE_MESSAGE(gfx_suspend(), "suspend must succeed");
    TEST_ASSERT_TRUE_MESSAGE(gfx_resume(false), "resume must bring the panel back");

    TEST_ASSERT_EQUAL_PTR_MESSAGE(before, gfx_framebuffer(),
                                  "the framebuffer is plain RAM and must not move across a suspend");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(marker, gfx_framebuffer()[0], "suspending must not disturb framebuffer contents");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(marker, gfx_framebuffer()[GFX_WIDTH * GFX_HEIGHT - 1],
                                    "suspending must not disturb framebuffer contents");
}

static void
test_the_panel_still_works_after_a_resume(void) {
    fixture();

    TEST_ASSERT_TRUE(gfx_suspend());
    TEST_ASSERT_TRUE(gfx_resume(false));

    /* If resume left the IO handle or the transfer-done callback unregistered,
     * this never returns and the board hangs here - which is the correct, loud
     * outcome rather than a silently dead display. */
    gfx_clear(gfx_rgb(0x101018));
    gfx_present();
    gfx_present();
    TEST_PASS();
}

static void
test_a_suspend_resume_round_trip_costs_under_a_frame(void) {
    fixture();

    const int64_t start = esp_timer_get_time();
    TEST_ASSERT_TRUE(gfx_suspend());
    TEST_ASSERT_TRUE(gfx_resume(false));
    const int64_t elapsed = esp_timer_get_time() - start;

    ESP_LOGI(TAG, "suspend/resume round trip: %lld us", (long long)elapsed);

    /* Measures ~715 us on hardware. The budget is one 25 ms frame at 40 fps:
     * the point is not the exact figure but that nobody reintroduces the panel
     * init sequence. Verified by flipping the argument to gfx_resume(true),
     * which takes 230 ms and fails this by 321x.
     * See docs/notes/Board-and-Memory.md's "Time-multiplexing the bus". */
    TEST_ASSERT_LESS_THAN_MESSAGE(25000, (int)elapsed,
                                  "a round trip must fit inside one frame - did the init sequence "
                                  "get re-sent?");
}

/* --- suite ------------------------------------------------------------- */

void
run_gfx_suite(void) {
    RUN_TEST(test_display_is_up);
    RUN_TEST(test_framebuffer_fits_with_headroom_to_spare);
    RUN_TEST(test_touch_controller_is_present);

    RUN_TEST(test_colour_packing_matches_the_panel_format);

    RUN_TEST(test_clear_touches_every_pixel);
    RUN_TEST(test_partial_clear_erases_only_previous_drawn_region);
    RUN_TEST(test_gfx_invalidate_forces_full_clear_in_partial_mode);
    RUN_TEST(test_fill_rect_writes_exactly_its_own_area);
    RUN_TEST(test_dither_at_alpha_zero_draws_nothing);
    RUN_TEST(test_dither_at_alpha_255_matches_a_solid_fill_exactly);
    RUN_TEST(test_dither_coverage_is_monotonic_and_graduated);
    RUN_TEST(test_dither_stays_in_phase_across_separate_calls);
    RUN_TEST(test_blit_dither_at_alpha_zero_changes_nothing);
    RUN_TEST(test_blit_dither_at_alpha_255_matches_the_source_exactly);
    RUN_TEST(test_blit_dither_matches_per_pixel_covers_reference);
    RUN_TEST(test_dithered_text_at_alpha_255_matches_solid_text_exactly);
    RUN_TEST(test_dithered_text_at_low_alpha_draws_fewer_pixels_than_solid);
    RUN_TEST(test_fill_rect_is_clipped_to_the_screen);
    RUN_TEST(test_a_horizontal_line_covers_both_endpoints);
    RUN_TEST(test_a_line_is_the_same_line_drawn_backwards);
    RUN_TEST(test_a_single_point_line_draws_one_pixel);
    RUN_TEST(test_a_line_is_clipped_rather_than_wrapped);
    RUN_TEST(test_a_line_entirely_off_screen_draws_nothing);
    RUN_TEST(test_a_line_honours_the_clip_rect);
    RUN_TEST(test_an_additive_line_brightens_where_it_crosses_itself);
    RUN_TEST(test_an_open_line_leaves_its_first_pixel_alone);
    RUN_TEST(test_chained_open_segments_do_not_double_their_joints);
    RUN_TEST(test_fill_rect_entirely_off_screen_draws_nothing);
    RUN_TEST(test_clip_rect_restricts_drawing);
    RUN_TEST(test_pixel_outside_the_screen_is_ignored);
    RUN_TEST(test_text_draws_and_advances);
    RUN_TEST(test_text_metrics_agree_with_what_is_drawn);

    RUN_TEST(test_present_completes);
    RUN_TEST(test_repeated_presents_stay_in_sync);

    RUN_TEST(test_an_unchanged_frame_costs_almost_nothing);
    RUN_TEST(test_full_present_cost_splits_into_bus_time_and_overhead);
    RUN_TEST(test_a_partial_change_costs_less_than_a_full_frame);
    RUN_TEST(test_a_narrow_change_costs_less_than_a_full_band);
    RUN_TEST(test_a_short_wide_change_costs_less_than_a_full_band);
    RUN_TEST(test_a_full_width_partial_height_change_costs_less_than_a_band);
    RUN_TEST(test_two_far_corners_cost_less_than_a_full_band);
    RUN_TEST(test_three_far_apart_marks_falls_back_at_the_current_cap);
    RUN_TEST(test_a_near_budget_split_crosses_the_gather_threshold);
    RUN_TEST(test_two_marks_in_one_cell_cost_less_than_the_coarse_box);
    RUN_TEST(test_drawing_marks_what_it_touched);

    RUN_TEST(test_the_framebuffer_survives_a_suspend);
    RUN_TEST(test_the_panel_still_works_after_a_resume);
    RUN_TEST(test_a_suspend_resume_round_trip_costs_under_a_frame);
}

SUITE_REGISTER(run_gfx_suite);
