/*=============================================================================
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
 *===========================================================================*/

#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "suites.h"

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"

static const char *TAG = "device_tests";

/* gfx owns global hardware state and is already initialised by the time this
 * runs - the shipped firmware brings the display up before self-testing. Tests
 * may leave the framebuffer in any state, but must not deinitialise it, and
 * must reset the clip rect since they share it. */
static void fixture(void)
{
    gfx_clear_clip();
}

/* Counts pixels equal to `expect` across the whole framebuffer, which is how
 * most of these tests assert "exactly this region changed and nothing else". */
static int count_pixels(gfx_color_t expect)
{
    const gfx_color_t *fb = gfx_framebuffer();
    int n = 0;
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        if (fb[i] == expect) { n++; }
    }
    return n;
}

static gfx_color_t pixel_at(int x, int y)
{
    return gfx_framebuffer()[y * GFX_WIDTH + x];
}

/* --- bring-up ----------------------------------------------------------- */

void test_display_is_up(void)
{
    fixture();
    TEST_ASSERT_NOT_NULL_MESSAGE(gfx_framebuffer(),
        "the framebuffer should already be allocated by the time tests run");
}

void test_framebuffer_fits_with_headroom_to_spare(void)
{
    fixture();
    /* The framebuffer is 322 KiB of roughly 424 KiB. If this margin ever
     * vanishes, allocations elsewhere start failing in confusing ways, so it
     * is worth asserting rather than discovering later. */
    const size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "free heap after framebuffer: %u bytes", (unsigned)free_heap);
    TEST_ASSERT_GREATER_THAN_UINT32(40 * 1024, free_heap);
}

void test_touch_controller_is_present(void)
{
    fixture();
    /* Confirms the I2C bus works and something answers - the host suite can
     * test what samples mean, but never that the controller exists. */
    const bsp_board_variant_t variant = bsp_board_detect();
    TEST_ASSERT_NOT_EQUAL_MESSAGE(BSP_BOARD_VARIANT_UNKNOWN, variant,
        "no supported touch controller responded on I2C");
}

/* --- colour packing ----------------------------------------------------- */

void test_colour_packing_matches_the_panel_format(void)
{
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

void test_clear_touches_every_pixel(void)
{
    fixture();
    const gfx_color_t c = gfx_rgb(0x123456);
    gfx_clear(c);
    TEST_ASSERT_EQUAL_INT(GFX_WIDTH * GFX_HEIGHT, count_pixels(c));
}

void test_fill_rect_writes_exactly_its_own_area(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    gfx_fill_rect(10, 20, 30, 40, fg);

    TEST_ASSERT_EQUAL_INT_MESSAGE(30 * 40, count_pixels(fg),
        "a filled rect must cover exactly w*h pixels");

    /* Corners in, neighbours out. */
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(10, 20));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(39, 59));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(9, 20));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(40, 59));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(10, 19));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(39, 60));
}

/* --- lines -------------------------------------------------------------- */

void test_a_horizontal_line_covers_both_endpoints(void)
{
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

void test_a_line_is_the_same_line_drawn_backwards(void)
{
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(56, forward,
        "a line should be one pixel per step along its longer axis");
}

void test_a_single_point_line_draws_one_pixel(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x00FFFF);

    gfx_clear(bg);
    gfx_line(100, 100, 100, 100, fg);

    TEST_ASSERT_EQUAL_INT(1, count_pixels(fg));
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(100, 100));
}

void test_a_line_is_clipped_rather_than_wrapped(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF00FF);

    gfx_clear(bg);
    /* Starts off the left edge and off the top, ends on screen. The failure
     * this guards against is not a crash but a wrap: an unclipped write at
     * x = -1 lands at the far end of the previous row. */
    gfx_line(-50, -20, 20, 15, fg);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(fg, pixel_at(20, 15),
        "the on-screen end of the line should still be drawn");
    for (int y = 0; y < GFX_HEIGHT; y++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(GFX_WIDTH - 1, y),
            "a clipped line wrapped onto the opposite edge");
    }
}

void test_a_line_entirely_off_screen_draws_nothing(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFF00);

    gfx_clear(bg);
    gfx_line(-100, -100, -10, -40, fg);
    gfx_line(GFX_WIDTH + 5, 10, GFX_WIDTH + 90, 200, fg);

    TEST_ASSERT_EQUAL_INT(0, count_pixels(fg));
}

void test_a_line_honours_the_clip_rect(void)
{
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

void test_an_additive_line_brightens_where_it_crosses_itself(void)
{
    fixture();
    const gfx_color_t bg  = gfx_rgb(0x000000);
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

void test_an_open_line_leaves_its_first_pixel_alone(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x004000);

    gfx_clear(bg);
    gfx_line_ex(10, 40, 20, 40, fg, GFX_LINE_ADD | GFX_LINE_OPEN);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(bg, pixel_at(10, 40),
        "an open line must not draw its starting pixel");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(11, 40));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(fg, pixel_at(20, 40),
        "an open line still draws its END pixel");
    TEST_ASSERT_EQUAL_INT(10, count_pixels(fg));
}

/* The reason GFX_LINE_OPEN exists. A polyline drawn as closed segments
 * adds the shared pixel at each joint twice, which under additive blending is
 * a brighter dot at every joint - a curve made of a few hundred segments
 * comes out visibly beaded. Chaining open segments puts exactly one
 * contribution on every pixel. */
void test_chained_open_segments_do_not_double_their_joints(void)
{
    fixture();
    const gfx_color_t bg   = gfx_rgb(0x000000);
    const gfx_color_t step = gfx_rgb(0x002000);

    gfx_clear(bg);
    gfx_line_ex(10, 60, 20, 60, step, GFX_LINE_ADD);
    gfx_line_ex(20, 60, 30, 60, step, GFX_LINE_ADD | GFX_LINE_OPEN);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(step, pixel_at(20, 60),
        "the joint should carry exactly one stroke's worth of light");
    TEST_ASSERT_EQUAL_INT_MESSAGE(21, count_pixels(step),
        "every pixel of the chain should be lit exactly once");
}

void test_fill_rect_is_clipped_to_the_screen(void)
{
    fixture();
    /* Straddling every edge. If clipping were wrong this would corrupt memory
     * around the framebuffer rather than fail politely, so it is worth having. */
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0x00FF00);

    gfx_clear(bg);
    gfx_fill_rect(-50, -50, 100, 100, fg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(50 * 50, count_pixels(fg),
        "only the on-screen quarter should be drawn");

    gfx_clear(bg);
    gfx_fill_rect(GFX_WIDTH - 10, GFX_HEIGHT - 10, 100, 100, fg);
    TEST_ASSERT_EQUAL_INT(10 * 10, count_pixels(fg));
}

void test_fill_rect_entirely_off_screen_draws_nothing(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF0000);

    gfx_clear(bg);
    gfx_fill_rect(-200, -200, 50, 50, fg);
    gfx_fill_rect(GFX_WIDTH + 10, 0, 50, 50, fg);
    gfx_fill_rect(0, GFX_HEIGHT + 10, 50, 50, fg);

    TEST_ASSERT_EQUAL_INT(0, count_pixels(fg));
}

void test_clip_rect_restricts_drawing(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFFFF00);

    gfx_clear(bg);
    gfx_set_clip(100, 100, 50, 50);
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, fg);   /* try to cover everything */
    gfx_clear_clip();

    TEST_ASSERT_EQUAL_INT_MESSAGE(50 * 50, count_pixels(fg),
        "drawing must be confined to the clip rect");
    TEST_ASSERT_EQUAL_HEX16(fg, pixel_at(100, 100));
    TEST_ASSERT_EQUAL_HEX16(bg, pixel_at(99, 100));
}

void test_pixel_outside_the_screen_is_ignored(void)
{
    fixture();
    const gfx_color_t bg = gfx_rgb(0x000000);
    const gfx_color_t fg = gfx_rgb(0xFF0000);

    gfx_clear(bg);
    gfx_pixel(-1, 0, fg);
    gfx_pixel(0, -1, fg);
    gfx_pixel(GFX_WIDTH, 0, fg);
    gfx_pixel(0, GFX_HEIGHT, fg);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_pixels(fg),
        "out-of-bounds pixels must be dropped, not wrapped");
}

void test_text_draws_and_advances(void)
{
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
            if (pixel_at(x, y) == fg) { second_cell++; }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, second_cell,
        "the second character must be drawn one cell to the right");
}

void test_text_metrics_agree_with_what_is_drawn(void)
{
    fixture();
    /* The UI layer lays out from these numbers, so they must match the
     * renderer or every label is subtly misplaced. */
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_W * 5, gfx_text_width("hello", -1));
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_W * 2, gfx_text_width("hello", 2));
    TEST_ASSERT_EQUAL_INT(GFX_CHAR_H, gfx_text_height());
}

/* --- DMA ---------------------------------------------------------------- */

void test_present_completes(void)
{
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
}

void test_repeated_presents_stay_in_sync(void)
{
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
 * showing what it last received. These verify the saving is real and measured
 * on the bus, not merely assumed from the flag bookkeeping. */

static int64_t time_present(void)
{
    const int64_t start = esp_timer_get_time();
    gfx_present();
    return esp_timer_get_time() - start;
}

static void test_an_unchanged_frame_costs_almost_nothing(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    const int64_t full = time_present();       /* clear marks everything */

    const int64_t unchanged = time_present();  /* nothing touched since */

    ESP_LOGI(TAG, "present: full %lld us, unchanged %lld us",
             (long long)full, (long long)unchanged);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)(full / 10), (int)unchanged,
        "a frame in which nothing changed must skip the bus entirely, not "
        "resend 322 KiB of identical pixels");
}

/* Decomposes a full-screen gfx_present() into raw QSPI bus time versus
 * everything gfx_present() itself adds on top of it - the question a
 * documented figure in suite_sand.c (the comment above FULL_STEP_BUDGET_US)
 * has stood on without ever having measured it directly: a "~9.6 ms
 * bus-time ceiling", stated there as a principle rather than a capture,
 * that this frame's budget was historically set to stay under. The
 * synthetic test above measures a full gfx_present() at ~17,900 us -
 * nearly double that figure - and nothing before this test isolated how
 * much of the gap is genuinely the bus versus gfx_present()'s own
 * bookkeeping - the seven-strip loop, dirty_row_is_dirty() checks,
 * collect_dirty_runs(), leaf refinement and the gather-vs-full-band
 * choice, all of which still run even when the whole screen is one
 * full-band send.
 *
 * gfx_present_raw_full_frame_for_test() (gfx.c, CONFIG_LAUNCHER_DEVELOPMENT
 * only) is the bus-time side of the comparison: one esp_lcd_panel_draw_-
 * bitmap() call over the whole framebuffer, none of the above involved at
 * all - see its own comment for why waiting on exactly one completion is
 * still correct despite the SPI driver chunking the transfer internally. */
static void test_full_present_cost_splits_into_bus_time_and_overhead(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x102030));           /* marks the whole screen dirty */
    const int64_t present_us = time_present();

    const int64_t raw_start = esp_timer_get_time();
    gfx_present_raw_full_frame_for_test();
    const int64_t raw_us = esp_timer_get_time() - raw_start;

    const int64_t overhead_us = present_us - raw_us;

    ESP_LOGI(TAG, "present decompose: gfx_present() full %lld us, raw blit "
                  "%lld us, overhead %lld us",
             (long long)present_us, (long long)raw_us, (long long)overhead_us);

    /* Sanity bounds only, the same shape test_present_completes uses above -
     * a full frame over QSPI cannot be instant and should not take anywhere
     * near a second. The interesting numbers are the two logged above and
     * their difference; this test exists to produce and log them, not to
     * hold either to a tuned ceiling. PROVISIONAL: no device capture of
     * this split exists yet, so nothing tighter is asserted - re-peg (or
     * replace with a real ceiling on the overhead specifically) from the
     * first device capture, the same convention suite_sand.c's
     * FULL_STEP_BUDGET_US comment documents for a newly-added measurement. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1000, (int)raw_us,
        "the raw blit returned implausibly fast - did it actually wait for "
        "the DMA?");
    TEST_ASSERT_LESS_THAN_INT(500000, (int)raw_us);
}

static void test_a_partial_change_costs_less_than_a_full_frame(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    const int64_t full = time_present();

    /* One band of seven. */
    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x204060));
    const int64_t one_band = time_present();

    ESP_LOGI(TAG, "present: full %lld us, one band %lld us",
             (long long)full, (long long)one_band);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)(full / 2), (int)one_band,
        "sending one band of seven must cost far less than sending all of "
        "them - this is the whole point of dirty tracking");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)one_band,
        "but it must still actually send something");
}

/* Every ratio test from here through test_two_far_corners_cost_less_than_
 * a_full_band measures a full band - one gfx_fill_rect() over the whole
 * 368x64 strip, presented alone via time_present() with nothing else
 * queued - as its reference cost. Because nothing else is in flight, that
 * reference is the UN-PIPELINED price: measured in isolation, a band
 * costs 3,405 us.
 *
 * That is not what a band costs inside a real frame. send_full_row()
 * (gfx.c) queues its draw_bitmap without waiting, and gfx_present()
 * drains every queued band together at the end, so later bands' DMA
 * overlaps earlier bands' CPU-side setup. Seven bands sent in a real
 * frame come to 18,147 us, not 7 x 3,405 = 23,835 - that pipelined price
 * is what run_present_against_scene() in suite_sand.c measures, with its
 * three present-cost tests. Sanity-checking one of those numbers against
 * the other by multiplying is not valid; the two measure different
 * things, and both are correct for what they measure. */

/* PROTOTYPE: measures the gather-copy path in gfx_present() - a strip whose
 * real dirty width is only a fraction of the band, written directly (not
 * through gfx_fill_rect(), which always claims the whole band via
 * mark_band() regardless of what it drew - see its comment). See
 * docs/Notes/Display-and-Rendering.md's "Still untapped" for why this
 * exists. */
static void test_a_narrow_change_costs_less_than_a_full_band(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();   /* drain: everything now clean */

    /* One full band, the existing fast path. */
    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x204060));
    const int64_t full_band = time_present();

    /* A narrow strip within a band, written directly and marked with its
     * real bounds - what draw_dirty_rows() actually does for a small pour. */
    gfx_color_t *fb = gfx_framebuffer();
    const int w = 20;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x204060);
        }
    }
    gfx_mark_dirty(0, 0, w, 64);
    const int64_t narrow = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, %d px wide (gathered) %lld us",
             (long long)full_band, w, (long long)narrow);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)narrow,
        "a strip a fraction of the band's width must cost less than "
        "claiming the whole band, or the gather-copy path is not paying "
        "for itself");
}

/* The box is bounded by area, not width alone, specifically so a
 * wide-but-short change gathers as cheaply as a narrow-but-tall one - tilt
 * the board so gravity points sideways and a falling stream is wide and
 * short instead of narrow and tall, and a width-only bound would give it no
 * benefit at all. See docs/Notes/Display-and-Rendering.md's "Still
 * untapped". */
static void test_a_short_wide_change_costs_less_than_a_full_band(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x602040));
    const int64_t full_band = time_present();

    /* Wide but short: most of the band's width, a sliver of its height -
     * the shape a sideways-falling stream leaves behind. */
    gfx_color_t *fb = gfx_framebuffer();
    const int w = 300;
    const int h = 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x602040);
        }
    }
    gfx_mark_dirty(0, 0, w, h);
    const int64_t wide = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, %dx%d px (gathered) %lld us",
             (long long)full_band, w, h, (long long)wide);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)wide,
        "a box short enough in height must cost less than claiming the "
        "whole band, even at most of its width - orientation must not "
        "matter to whether gathering pays off");
}

/* Full width, most of a band's height: 368x48 is many times
 * GATHER_MAX_PIXELS, far too big to gather - and yet a box at the full
 * panel width is already contiguous in the framebuffer, row-major,
 * GFX_WIDTH stride, so it needs no gather buffer at all. This is the
 * case send_partial_band() exists for (see gfx.c): the same one
 * transaction as a whole band, just not rounded up to the band's own 64
 * rows.
 *
 * 90% is the threshold, not 75%, on purpose. 48 of a band's 64 rows is
 * 75% of the band's pixels, and a present against this panel is ~94% bus
 * time (see gfx.h and test_full_present_cost_splits_into_bus_time_and_
 * overhead), so the honest floor - once the fixed per-transaction cost
 * that does not shrink with the row count is counted - is around 78%.
 * 90% sits comfortably inside that margin without being loose enough to
 * pass by accident.
 *
 * And it really cannot pass by accident: before send_partial_band()
 * existed, a full-width box took the identical code path as a whole
 * band - the same single esp_lcd_panel_draw_bitmap() of the same
 * 368x64 pixels out of fb - so `partial` and `full_band` were the same
 * transaction and no ratio under 1.0 was reachable at all, let alone one
 * under 0.9.
 *
 * Both halves are measured in this same run, like every other test in
 * this file, so this is a ratio rather than an absolute number - it does
 * not need re-pegging when the panel clock or the build's layout
 * moves. */
static void test_a_full_width_partial_height_change_costs_less_than_a_band(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x406020));
    const int64_t full_band = time_present();

    /* Full width, 48 of the band's 64 rows - the shape a wide pour
     * leaves that has not yet grown to fill its whole strip. */
    gfx_color_t *fb = gfx_framebuffer();
    const int h = 48;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            fb[y * GFX_WIDTH + x] = gfx_rgb(0x406020);
        }
    }
    gfx_mark_dirty(0, 0, GFX_WIDTH, h);
    const int64_t partial = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, full width x %d px (partial "
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
 * docs/Notes/Display-and-Rendering.md's "Still untapped". */
static void test_two_far_corners_cost_less_than_a_full_band(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x206020));
    const int64_t full_band = time_present();

    gfx_color_t *fb = gfx_framebuffer();
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

    ESP_LOGI(TAG, "present: full band %lld us, two %dx%d corners %lld us",
             (long long)full_band, size, size, (long long)two_corners);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)two_corners,
        "two small, far-apart clusters sent independently together must "
        "still cost less than the whole band");
}

/* Three separated marks, not two - one more than ROW_MAX_RUNS/
 * LEAF_REFINE_MAX_RUNS (row_runs.h, gfx_dirty.h) currently track - placed
 * to actually exercise that cap, which test_two_far_corners_cost_less_
 * than_a_full_band does not: collect_dirty_runs() finds cell-level runs
 * with a cap of its own (GRID_COLS, effectively unlimited at 4 cells - see
 * its own comment), not ROW_MAX_RUNS/LEAF_REFINE_MAX_RUNS at all. A mark
 * in a genuinely separate, non-adjacent cell - like the two-far-corners
 * test's opposite corners - is handled entirely at that cell level and
 * never touches this cap, no matter how many marks there are. Cells 0-2
 * are adjacent, though, so collect_dirty_runs() merges them into ONE
 * coarse run before leaf refinement (refine_run()/plan_run()) ever gets
 * involved - splitting THAT merged run into its three real gaps is what
 * LEAF_REFINE_MAX_RUNS caps.
 *
 * At the shipped cap of 2, refine_run() gives up (three isolated leaf
 * bits, cap 2 - see collect_runs_from_mask()'s own "too fragmented" case)
 * and plan_run() falls back to run_box()'s coarse union instead - NOT the
 * whole band, and not even the whole 3-cell span: run_box() unions each
 * cell's own already-tight cell_x0/x1 box, so the fallback here is one
 * ~240x64 send spanning just the marks' own extent, still skipping the
 * untouched cell to its right entirely. That single wider transaction
 * measured cheaper than the full band by a wide margin even at cap 2 -
 * given the ~118us fixed cost of a QSPI transaction (see "The blit is
 * bus-bound" in Display-and-Rendering.md), three separate small sends
 * under a raised cap are not guaranteed to beat one merged fallback send;
 * that is exactly the open question a sweep of this cap is for, not
 * something to assume going in. */
static void test_three_far_apart_marks_falls_back_at_the_current_cap(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x602060));
    const int64_t full_band = time_present();

    gfx_color_t *fb = gfx_framebuffer();
    const int size = 15;
    /* Cells 0, 1 and 2 (COL_WIDTH=92 each) - adjacent, so these merge
     * into one 276px-wide coarse run, not three separate cell-level ones.
     * Each mark sits inside its own cell with real room either side, so
     * the gaps are genuine at the leaf level, not an artifact of landing
     * right on a cell boundary. */
    const int xs[3] = { 5, 115, 230 };

    for (int i = 0; i < 3; i++) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                fb[y * GFX_WIDTH + xs[i] + x] = gfx_rgb(0x602060);
            }
        }
        gfx_mark_dirty(xs[i], 0, size, size);
    }

    const int64_t three_marks = time_present();

    ESP_LOGI(TAG, "present: full band %lld us, three %dx%d marks %lld us",
             (long long)full_band, size, size, (long long)three_marks);
}

/* A small mark plus a wide one, in the same coarse run, sized to put the
 * wide mark's own leaf-refined piece right where GATHER_MAX_PIXELS
 * (gfx_dirty.h, 8192 shipped) decides whether it gets gathered at all -
 * not near-zero like every other gathered-piece test here, and not so
 * far over that it stays rejected everywhere a sweep of this budget would
 * plausibly try. This file cannot reference GATHER_MAX_PIXELS directly:
 * gfx_dirty.h is header-only, static, deliberately included only by
 * gfx.c in the real firmware (see its own top comment) - a second real-
 * firmware include here would silently duplicate its dirty-tracking
 * state into a second, disconnected copy this test never touches, not
 * just pull in a constant. Literal numbers instead, chosen for the
 * candidates this project has actually swept (4096, 6144, 8192, 9216):
 *
 * The wide mark spans x=[48,158) - inside leaf columns 2 through 6
 * (LEAF_W=23, COL_WIDTH=92, so leaf 1 ends at 46 and leaf 7 starts at
 * 161 - clear of both). refine_run() reports leaf-refined pieces at
 * whole leaf-column granularity, so this becomes a 5-leaf, 115px-wide
 * piece regardless of the mark's own exact width - 115 * STRIP_HEIGHT
 * (64) = 7360px. That is over budget at 4096 and 6144 (falls back to
 * the coarse box, same fallback test_three_far_apart_marks_falls_back_
 * at_the_current_cap already measured), and under budget at 8192 (the
 * shipped default) and 9216 (gathers as two pieces instead). Whether
 * that crossing actually helps or hurts is exactly what a sweep of this
 * budget is for - not assumed here. */
static void test_a_near_budget_split_crosses_the_gather_threshold(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x206040));
    const int64_t full_band = time_present();

    gfx_color_t *fb = gfx_framebuffer();
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

    ESP_LOGI(TAG, "present: full band %lld us, near-budget split %lld us",
             (long long)full_band, (long long)near_budget);
}

/* Two small marks inside the SAME 92px cell, far enough apart to leave a
 * real gap between them - the shape only leaf refinement can split on.
 * test_two_far_corners above lands in different CELLS, which
 * collect_dirty_runs() alone already separates without any help from the
 * leaf layer; this test is the one that actually exercises it. See
 * docs/Notes/Display-and-Rendering.md's "Still untapped". */
static void test_two_marks_in_one_cell_cost_less_than_the_coarse_box(void)
{
    fixture();

    gfx_clear(gfx_rgb(0x000000));
    (void)time_present();

    gfx_fill_rect(0, 0, GFX_WIDTH, 64, gfx_rgb(0x406020));
    const int64_t full_band = time_present();

    gfx_color_t *fb = gfx_framebuffer();
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

    ESP_LOGI(TAG, "present: full band %lld us, two marks in one cell %lld us",
             (long long)full_band, (long long)two_marks);

    TEST_ASSERT_LESS_THAN_MESSAGE((int)full_band, (int)two_marks,
        "two small marks separated by a real gap inside one cell must cost "
        "less than sending the coarse box spanning both");
}

static void test_drawing_marks_what_it_touched(void)
{
    fixture();
    gfx_clear(gfx_rgb(0x000000));
    gfx_present();                 /* everything now clean */

    TEST_ASSERT_FALSE_MESSAGE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT),
        "a present must clear the dirty state, or every frame sends the whole "
        "screen for ever");

    gfx_fill_rect(0, 0, 8, 8, gfx_rgb(0xFFFFFF));

    TEST_ASSERT_TRUE_MESSAGE(gfx_region_dirty(0, 0, GFX_WIDTH, 64),
        "the band that was drawn into must be marked");
    TEST_ASSERT_FALSE_MESSAGE(
        gfx_region_dirty(0, GFX_HEIGHT - 64, GFX_WIDTH, 64),
        "a band nowhere near the drawing must not be");
}

/* --- suspending the display to share SPI2 ------------------------------- */

/* The display and the SD card are wired to different pins on the one SPI2
 * controller, so reaching the card means releasing the panel. These cover the
 * round trip. They can only run on hardware: the whole point is whether real
 * bus teardown and rebuild leave the driver in a usable state. */

static void test_the_framebuffer_survives_a_suspend(void)
{
    fixture();

    /* A recognisable pattern, so this fails loudly if resume were ever to
     * reallocate rather than reattach. */
    gfx_color_t *before = gfx_framebuffer();
    const gfx_color_t marker = gfx_rgb(0x8040C0);
    gfx_clear(marker);

    TEST_ASSERT_TRUE_MESSAGE(gfx_suspend(), "suspend must succeed");
    TEST_ASSERT_TRUE_MESSAGE(gfx_resume(false), "resume must bring the panel back");

    TEST_ASSERT_EQUAL_PTR_MESSAGE(before, gfx_framebuffer(),
        "the framebuffer is plain RAM and must not move across a suspend");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(marker, gfx_framebuffer()[0],
        "suspending must not disturb framebuffer contents");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(marker,
        gfx_framebuffer()[GFX_WIDTH * GFX_HEIGHT - 1],
        "suspending must not disturb framebuffer contents");
}

static void test_the_panel_still_works_after_a_resume(void)
{
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

static void test_a_suspend_resume_round_trip_costs_under_a_frame(void)
{
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
     * See docs/Notes/Board-and-Memory.md's "Time-multiplexing the bus". */
    TEST_ASSERT_LESS_THAN_MESSAGE(25000, (int)elapsed,
        "a round trip must fit inside one frame - did the init sequence "
        "get re-sent?");
}

/* --- suite ------------------------------------------------------------- */

void run_gfx_suite(void)
{
    RUN_TEST(test_display_is_up);
    RUN_TEST(test_framebuffer_fits_with_headroom_to_spare);
    RUN_TEST(test_touch_controller_is_present);

    RUN_TEST(test_colour_packing_matches_the_panel_format);

    RUN_TEST(test_clear_touches_every_pixel);
    RUN_TEST(test_fill_rect_writes_exactly_its_own_area);
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
