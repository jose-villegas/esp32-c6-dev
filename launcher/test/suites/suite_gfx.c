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

#include "gfx.h"

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
     * See docs/ESP32-C6-AMOLED-Notes.md. */
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
    RUN_TEST(test_fill_rect_entirely_off_screen_draws_nothing);
    RUN_TEST(test_clip_rect_restricts_drawing);
    RUN_TEST(test_pixel_outside_the_screen_is_ignored);
    RUN_TEST(test_text_draws_and_advances);
    RUN_TEST(test_text_metrics_agree_with_what_is_drawn);

    RUN_TEST(test_present_completes);
    RUN_TEST(test_repeated_presents_stay_in_sync);

    RUN_TEST(test_an_unchanged_frame_costs_almost_nothing);
    RUN_TEST(test_a_partial_change_costs_less_than_a_full_frame);
    RUN_TEST(test_drawing_marks_what_it_touched);

    RUN_TEST(test_the_framebuffer_survives_a_suspend);
    RUN_TEST(test_the_panel_still_works_after_a_resume);
    RUN_TEST(test_a_suspend_resume_round_trip_costs_under_a_frame);
}

SUITE_REGISTER(run_gfx_suite);
